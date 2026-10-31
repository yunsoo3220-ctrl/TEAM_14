"""임베딩 모델 미세조정 + 평가.

DB 의 정답 쌍 (관리자가 체크한 대상 학과 1.0, 그 학과 학생이 댓글 단 글 0.5) 으로
사전학습 모델을 대조학습(MultipleNegativesRankingLoss)한다.

  ml\\.venv\\Scripts\\python.exe ml\\finetune.py --eval-only   지금 모델 성능만 잰다
  ml\\.venv\\Scripts\\python.exe ml\\finetune.py               미세조정 후 좋아졌을 때만 저장

평가: 게시물의 20% 를 떼어 두고, 나머지 연결로 서버와 같은 Rocchio 학과 벡터를 만든 뒤
떼어 둔 게시물의 정답 학과가 상위 5개 안에 드는 비율(Recall@5)과 MRR 을 잰다.
저장하면 ml/models/finetuned 에 남고, 임베딩 서비스를 다시 띄우면 그 모델을 쓴다.

정답 쌍이 수백 개 미만이면 미세조정은 과적합되기 쉽다. 그래서 떼어 둔 데이터에서
나아졌을 때만 저장한다.
"""
# ---------------------------------------------------------------------------
# 용어 정리
#   대조학습(contrastive learning): "맞는 짝(학과, 게시물)은 가깝게, 틀린 짝은 멀게" 벡터를 조정한다.
#   MultipleNegativesRankingLoss: 한 배치 안에서 자기 짝이 아닌 나머지 게시물을 모두 오답으로 쓰는 손실.
#       별도의 오답 데이터를 만들 필요가 없어 정답 쌍만 있으면 학습할 수 있다.
#   Recall@5: 정답 학과 중 상위 5개 안에 들어온 비율 (1.0 이면 모두 맞힘)
#   MRR(Mean Reciprocal Rank): 첫 정답의 순위 r 에 대해 1/r 의 평균 (1등이면 1, 2등이면 0.5 ...)
#   홀드아웃(holdout): 평가용으로 학습에 쓰지 않고 떼어 둔 데이터. 과적합 여부를 공정하게 잰다.
# ---------------------------------------------------------------------------
import argparse
import json
import os
import random
import sys
from collections import defaultdict

import numpy as np
import pymysql                                          # MySQL 접속 (순수 Python 드라이버)
from sentence_transformers import InputExample, SentenceTransformer
try:                                    # sentence-transformers 6 이후 경로
    from sentence_transformers.sentence_transformer import losses
    from sentence_transformers.sentence_transformer.datasets import NoDuplicatesDataLoader
except ImportError:
    # 예전 버전에서는 모듈 위치가 다르다
    from sentence_transformers import losses
    from sentence_transformers.datasets import NoDuplicatesDataLoader

HERE = os.path.dirname(os.path.abspath(__file__))
BASE_MODEL = os.environ.get("SKU_EMBED_MODEL", "intfloat/multilingual-e5-small")   # 미세조정의 출발점
OUT_DIR = os.path.join(HERE, "models", "finetuned")                                # 결과 저장 위치

ROCCHIO_BETA = 0.75     # src/ml.c 와 같은 값
SHRINK_K = 1.5          # src/ml.c 와 같은 값 (연결이 적은 학과의 반영을 줄임)
TOP_K = 5               # Recall@K 의 K


def load_data():
    # DB 에서 (게시물 글, 학과 글, 정답 쌍) 을 읽는다. 접속 정보는 서버와 같은 환경변수를 쓴다.
    con = pymysql.connect(host=os.environ.get("SKU_DB_HOST", "127.0.0.1"),
                          user=os.environ.get("SKU_DB_USER", "root"),
                          password=os.environ.get("SKU_DB_PASS", ""),
                          database=os.environ.get("SKU_DB_NAME", "sku_contest"),
                          charset="utf8mb4")
    with con.cursor() as c:
        # 게시물: {id: "제목\n본문\n"}  (src/ml.c 의 join3 과 같은 모양 → 같은 임베딩 캐시를 공유)
        c.execute("SELECT id, title, body FROM posts ORDER BY id")
        posts = {pid: "%s\n%s\n" % (t, b or "") for pid, t, b in c.fetchall()}
        # 학과: {id: "학과이름\n키워드\n"}
        c.execute("SELECT d.id, d.name, IFNULL(p.keywords, '') FROM departments d "
                  "LEFT JOIN dept_profiles p ON p.department_id = d.id ORDER BY d.id")
        depts = {did: "%s\n%s\n" % (n, k) for did, n, k in c.fetchall()}
        # 정답 쌍: src/ml.c 의 train() 과 똑같은 질의 (관리자 체크 1.0, 댓글 0.5, ♥ 0.5)
        c.execute("SELECT post_id, department_id, MAX(w) FROM ("
                  "  SELECT post_id, department_id, 1.0 AS w FROM post_departments "
                  "  UNION ALL "
                  "  SELECT c.post_id, u.department_id, 0.5 FROM comments c "
                  "  JOIN users u ON u.id = c.author_id "
                  "  WHERE u.role = 'student' AND u.department_id IS NOT NULL "
                  "  UNION ALL "
                  "  SELECT r.post_id, u.department_id, 0.5 FROM recruit_likes l "
                  "  JOIN recruits r ON r.id = l.recruit_id JOIN users u ON u.id = l.user_id "
                  "  WHERE r.post_id IS NOT NULL AND u.role = 'student' AND u.department_id IS NOT NULL"
                  ") x GROUP BY post_id, department_id")
        # [(게시물 id, 학과 id, 가중치), ...] - 위에서 읽은 목록에 실제로 있는 것만
        labels = [(p, d, float(w)) for p, d, w in c.fetchall() if p in posts and d in depts]
    con.close()
    return posts, depts, labels


def encode(model, texts, kind):
    # 글 목록을 정규화된 벡터 배열로. E5 모델이면 서버와 똑같이 "query: "/"passage: " 를 붙인다.
    prefix = (kind + ": ") if "e5" in BASE_MODEL.lower() else ""
    return model.encode([prefix + t for t in texts], batch_size=16,
                        normalize_embeddings=True, convert_to_numpy=True, show_progress_bar=False)


def evaluate(model, posts, depts, train_labels, test_labels):
    """서버와 같은 Rocchio 학과 벡터로 떼어 둔 게시물의 정답 학과 순위를 잰다."""
    pids, dids = list(posts), list(depts)
    pidx = {p: i for i, p in enumerate(pids)}        # 게시물 id → 행 번호
    P = encode(model, [posts[p] for p in pids], "passage")   # (게시물 수 x 차원)
    D = encode(model, [depts[d] for d in dids], "query")     # (학과 수 x 차원)

    # Rocchio: 학습용 정답 쌍으로만 학과 벡터를 보정 (평가용 쌍은 쓰지 않는다 - 정답 누설 방지)
    cent = np.zeros_like(D)          # 학과별 연결된 게시물 벡터 합
    cw = np.zeros(len(dids))         # 학과별 가중치 합
    for p, d, w in train_labels:
        j = dids.index(d)
        cent[j] += w * P[pidx[p]]
        cw[j] += w
    for j in range(len(dids)):
        if cw[j] > 0:
            c = cent[j] / (np.linalg.norm(cent[j]) or 1)    # 방향만 (길이 1)
            D[j] = D[j] + ROCCHIO_BETA * cw[j] / (cw[j] + SHRINK_K) * c
        D[j] /= np.linalg.norm(D[j]) or 1                   # 다시 길이 1 로

    # 평가 게시물마다 정답 학과 집합
    truth = defaultdict(set)
    for p, d, _ in test_labels:
        truth[p].add(d)
    recall, mrr = [], []
    for p, ds in truth.items():
        # D @ P[i] = 모든 학과와의 코사인 유사도 → 내림차순 학과 순서
        order = [dids[j] for j in np.argsort(-(D @ P[pidx[p]]))]
        # 정답이 5개보다 많으면 분모를 5로 (상위 5칸에 다 넣는 것이 최선이므로)
        recall.append(len(ds & set(order[:TOP_K])) / min(len(ds), TOP_K))
        # 첫 번째 정답의 순위 r(0부터) → 1/(r+1)
        mrr.append(next(1.0 / (r + 1) for r, d in enumerate(order) if d in ds))
    return float(np.mean(recall)) if recall else 0.0, float(np.mean(mrr)) if mrr else 0.0, len(truth)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--eval-only", action="store_true")                  # 평가만 하고 학습은 안 함
    ap.add_argument("--epochs", type=int, default=3)                     # 학습 데이터를 몇 번 반복할지
    ap.add_argument("--batch", type=int, default=16)                     # 한 번에 묶는 쌍 수 (= 오답 후보 수 + 1)
    ap.add_argument("--holdout", type=float, default=0.2)                # 평가용으로 떼어 둘 게시물 비율
    ap.add_argument("--save-anyway", action="store_true", help="나빠져도 저장")
    args = ap.parse_args()

    posts, depts, labels = load_data()
    labeled = sorted({p for p, _, _ in labels})      # 정답이 하나라도 있는 게시물 id
    print("게시물 %d, 학과 %d, 정답 쌍 %d (게시물 %d건에 걸침)" %
          (len(posts), len(depts), len(labels), len(labeled)))
    if len(labeled) < 5:
        print("정답 쌍이 너무 적어 평가할 수 없습니다. 관리자 화면에서 대상 학과를 더 체크하세요.")
        return 1

    # 게시물 단위로 나눈다 (같은 게시물의 쌍이 학습/평가 양쪽에 섞이면 평가가 부풀려진다).
    # 시드 42 로 고정해 실행할 때마다 같은 분할이 나오게 한다 (결과 비교 가능).
    random.Random(42).shuffle(labeled)
    test_posts = set(labeled[:max(1, int(len(labeled) * args.holdout))])
    train_labels = [x for x in labels if x[0] not in test_posts]
    test_labels = [x for x in labels if x[0] in test_posts]

    # 1) 기준선: 사전학습 모델 그대로의 성능
    base = SentenceTransformer(BASE_MODEL, device="cpu")
    r0, m0 = evaluate(base, posts, depts, train_labels, test_labels)[:2]
    print("[사전학습 %s] Recall@%d %.3f, MRR %.3f (평가 게시물 %d건)" %
          (BASE_MODEL, TOP_K, r0, m0, len(test_posts)))
    if args.eval_only:
        return 0

    if len(train_labels) < 200:
        print("주의: 학습 쌍이 %d개뿐이라 과적합되기 쉽습니다. 결과를 꼭 비교하세요." % len(train_labels))

    # 2) 학습 데이터: (학과 글, 게시물 글) 짝. 서버가 쓰는 접두어와 똑같이 맞춘다.
    prefix_q, prefix_p = ("query: ", "passage: ") if "e5" in BASE_MODEL.lower() else ("", "")
    examples = [InputExample(texts=[prefix_q + depts[d], prefix_p + posts[p]])
                for p, d, _ in train_labels]
    # 같은 배치 안의 다른 게시물을 오답으로 쓴다. 같은 학과가 한 배치에 두 번 들어가지 않게 한다.
    # (같은 학과가 두 번 있으면, 실제로는 정답인 게시물이 오답으로 취급되는 문제가 생긴다)
    loader = NoDuplicatesDataLoader(examples, batch_size=min(args.batch, len(examples)))
    model = SentenceTransformer(BASE_MODEL, device="cpu")
    # warmup_steps: 처음 10% 단계 동안 학습률을 서서히 올려 초반의 큰 흔들림을 막는다
    model.fit(train_objectives=[(loader, losses.MultipleNegativesRankingLoss(model))],
              epochs=args.epochs, warmup_steps=max(1, len(loader) // 10), show_progress_bar=True)

    # 3) 미세조정 모델 평가 (같은 평가 데이터)
    r1, m1 = evaluate(model, posts, depts, train_labels, test_labels)[:2]
    print("[미세조정]           Recall@%d %.3f, MRR %.3f" % (TOP_K, r1, m1))

    # 4) 튜플 비교: Recall 이 우선, 같으면 MRR 로 비교 → 더 나아졌을 때만 저장
    if (r1, m1) > (r0, m0) or args.save_anyway:
        # 최종 모델은 떼어 둔 쌍까지 모두 써서 다시 학습할 수도 있지만, 데이터가 적을 때는
        # 평가한 모델을 그대로 두는 편이 안전하다.
        model.save(OUT_DIR)
        # embed_server.py 의 pick_model 이 읽는 메타 정보 (원래 모델 이름 → e5 접두어 판단)
        with open(os.path.join(OUT_DIR, "sku_meta.json"), "w", encoding="utf-8") as f:
            json.dump({"base": BASE_MODEL, "recall_at_5": r1, "mrr": m1,
                       "pairs": len(train_labels)}, f, ensure_ascii=False)
        print("저장: %s  (임베딩 서비스를 다시 띄우면 적용됩니다)" % OUT_DIR)
    else:
        print("사전학습 모델보다 나아지지 않아 저장하지 않았습니다.")
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")   # Windows 콘솔 한글 출력
    sys.exit(main())                            # main 의 반환값을 프로세스 종료 코드로
