"""문장 임베딩 서비스 - C 서버(src/embed.c)가 부르는 딥러닝 모델.

사전학습된 다국어 트랜스포머(기본 intfloat/multilingual-e5-small, 384차원)로
글을 벡터로 바꾼다. 같은 글은 SQLite 캐시에서 바로 돌려준다.

    ml\\.venv\\Scripts\\python.exe ml\\embed_server.py [--port 8001]

  GET  /health                         {"model": ..., "dim": 384}
  POST /embed  {"kind": "passage"|"query", "texts": [...]}
               -> {"model": ..., "dim": 384, "n": N, "data": base64(float32 LE, N x dim)}

ml/models/finetuned 가 있으면 (finetune.py 결과) 그 모델을 쓴다.
환경변수 SKU_EMBED_MODEL 로 다른 모델을 고를 수 있다.
"""
# ---------------------------------------------------------------------------
# 구조 요약
#   - Embedder : 모델을 한 번 불러 두고, 글 목록을 벡터 배열로 바꾼다 (캐시 포함).
#   - Handler  : 표준 라이브러리 http.server 로 만든 아주 작은 HTTP 서버의 요청 처리기.
#   - main     : 127.0.0.1 에만 열어 같은 PC 의 C 서버만 접속할 수 있게 한다.
#
# 왜 C 서버와 따로 두는가?
#   트랜스포머 모델은 PyTorch 가 필요해 C 로 직접 돌리기 어렵다. 그래서 Python 프로세스로
#   띄워 HTTP 로 주고받는다. 이 서비스가 꺼져 있어도 C 서버는 TF-IDF 만으로 동작한다.
# ---------------------------------------------------------------------------
import argparse            # 명령줄 인자(--port) 해석
import base64              # 벡터 바이트를 JSON 에 넣기 위한 base64 인코딩
import hashlib             # 캐시 키(SHA-1 해시) 생성
import json
import os
import sqlite3             # 임베딩 캐시 저장소 (파일 하나짜리 내장 DB)
import sys
import threading           # 동시 요청 사이에서 모델/캐시를 보호하는 잠금
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import numpy as np                                     # 벡터 배열 처리
from sentence_transformers import SentenceTransformer  # 사전학습 문장 임베딩 모델 라이브러리

HERE = os.path.dirname(os.path.abspath(__file__))          # 이 파일이 있는 폴더 (ml/)
DEFAULT_MODEL = "intfloat/multilingual-e5-small"           # Hugging Face 모델 이름 (한국어 포함 다국어)
FINETUNED_DIR = os.path.join(HERE, "models", "finetuned")  # finetune.py 가 저장하는 위치
CACHE_PATH = os.path.join(HERE, "cache", "embeddings.sqlite")
MAX_TEXTS = 5000                # 한 요청에 받을 최대 글 수
MAX_BODY = 32 * 1024 * 1024     # 요청 본문 최대 32MB


def pick_model():
    """(불러올 경로나 이름, 캐시 키에 쓸 이름, e5 접두어 필요 여부)"""
    # 우선순위: 환경변수 SKU_EMBED_MODEL > 미세조정 모델 폴더 > 기본 모델
    name = os.environ.get("SKU_EMBED_MODEL")
    if not name and os.path.isdir(FINETUNED_DIR):
        name = FINETUNED_DIR
    name = name or DEFAULT_MODEL

    base = name   # 원래 사전학습 모델 이름 (e5 계열인지 판단용)
    if os.path.isdir(name):
        # 로컬 폴더 모델이면 finetune.py 가 남긴 메타 정보에서 원래 모델 이름을 읽는다
        meta = os.path.join(name, "sku_meta.json")
        if os.path.exists(meta):
            with open(meta, encoding="utf-8") as f:
                base = json.load(f).get("base", name)
        # 다시 미세조정하면 같은 경로라도 캐시가 갈리도록 수정 시각을 붙인다.
        key = "finetuned:%s@%d" % (base, int(os.path.getmtime(name)))
    else:
        key = name
    # E5 모델은 입력 앞에 "query: " / "passage: " 접두어를 붙이도록 학습되었다
    return name, key, "e5" in base.lower()


class Embedder:
    # 모델과 캐시를 들고 있는 객체. 서버가 뜰 때 한 번만 만든다 (모델 로딩은 수 초 걸림).
    def __init__(self):
        self.path, self.key, self.e5 = pick_model()
        print("모델 불러오는 중: %s" % self.path, flush=True)
        self.model = SentenceTransformer(self.path, device="cpu")   # GPU 없이 CPU 로 실행
        # 라이브러리 버전에 따라 차원 조회 메서드 이름이 다르므로 둘 다 시도한다
        get_dim = getattr(self.model, "get_embedding_dimension", None) or \
            self.model.get_sentence_embedding_dimension          # 예전 버전 이름
        self.dim = get_dim()
        self.lock = threading.Lock()            # 모델과 캐시 연결을 한 스레드씩 쓴다
        os.makedirs(os.path.dirname(CACHE_PATH), exist_ok=True)
        # check_same_thread=False: 여러 스레드에서 같은 연결을 쓰겠다는 뜻 (대신 위 lock 으로 보호)
        self.db = sqlite3.connect(CACHE_PATH, check_same_thread=False)
        # 캐시 테이블: h = 해시(모델+종류+글), v = float32 벡터 바이트
        self.db.execute("CREATE TABLE IF NOT EXISTS emb (h TEXT PRIMARY KEY, v BLOB)")
        print("준비 완료: %s (%d차원, e5 접두어 %s)" % (self.key, self.dim, "사용" if self.e5 else "없음"),
              flush=True)

    def _hash(self, kind, text):
        # 캐시 키 = SHA-1(모델 키 \0 종류 \0 글). 모델이나 종류가 다르면 다른 벡터이므로 함께 섞는다.
        # \0 구분자는 "ab"+"c" 와 "a"+"bc" 같은 경계 혼동을 막는다.
        return hashlib.sha1(("%s\0%s\0%s" % (self.key, kind, text)).encode("utf-8")).hexdigest()

    def embed(self, kind, texts):
        # 글 목록 → (N x dim) float32 배열, 그리고 새로 계산한 개수
        hashes = [self._hash(kind, t) for t in texts]
        out = np.zeros((len(texts), self.dim), dtype="<f4")   # "<f4" = 리틀엔디언 float32 (C 쪽과 약속)
        with self.lock:
            # 1) 캐시에 있는 것은 바로 채우고, 없는 것의 위치만 모은다
            missing = []
            for i, h in enumerate(hashes):
                row = self.db.execute("SELECT v FROM emb WHERE h = ?", (h,)).fetchone()
                if row and len(row[0]) == self.dim * 4:   # 차원이 맞는지까지 확인 (float32 = 4바이트)
                    out[i] = np.frombuffer(row[0], dtype="<f4")
                else:
                    missing.append(i)
            # 2) 없는 것만 모델로 한꺼번에 계산 (배치 16개씩)
            if missing:
                prefix = (kind + ": ") if self.e5 else ""
                vecs = self.model.encode([prefix + texts[i] for i in missing], batch_size=16,
                                         normalize_embeddings=True, convert_to_numpy=True)
                # normalize_embeddings=True: 길이 1 로 맞춰 내적 = 코사인 유사도
                for i, v in zip(missing, vecs):
                    out[i] = v
                    self.db.execute("INSERT OR REPLACE INTO emb VALUES (?, ?)",
                                    (hashes[i], v.astype("<f4").tobytes()))
                self.db.commit()
        return out, len(missing)


class Handler(BaseHTTPRequestHandler):
    # 요청 하나마다 이 클래스의 인스턴스가 만들어진다. 공유할 Embedder 는 클래스 변수로 둔다.
    embedder = None

    def _send(self, status, obj):
        # obj 를 JSON 으로 직렬화해 응답한다. ensure_ascii=False 면 한글을 \uXXXX 없이 그대로 보낸다.
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        # GET /health : C 서버(embed_available)가 서비스 생존 여부를 확인할 때 부른다
        if self.path == "/health":
            e = self.embedder
            self._send(200, {"model": e.key, "dim": e.dim})
        else:
            self._send(404, {"error": "not found"})

    def do_POST(self):
        # POST /embed : 글 목록을 벡터로
        if self.path != "/embed":
            self._send(404, {"error": "not found"})
            return
        # 입력 검증 - 형식이 틀리면 400
        try:
            n = int(self.headers.get("Content-Length", "0"))
            if n <= 0 or n > MAX_BODY:
                raise ValueError("본문 크기가 올바르지 않습니다")
            req = json.loads(self.rfile.read(n).decode("utf-8"))
            kind = req.get("kind", "passage")
            texts = req.get("texts")
            if kind not in ("passage", "query"):
                raise ValueError("kind 는 passage 또는 query")
            if not isinstance(texts, list) or len(texts) > MAX_TEXTS or \
                    not all(isinstance(t, str) for t in texts):
                raise ValueError("texts 는 문자열 배열")
        except (ValueError, UnicodeDecodeError) as ex:
            # json.JSONDecodeError 는 ValueError 의 하위 클래스라 여기서 함께 잡힌다
            self._send(400, {"error": str(ex)})
            return

        vecs, computed = self.embedder.embed(kind, texts)
        if computed:
            print("임베딩 %d건 계산 (요청 %d건)" % (computed, len(texts)), flush=True)
        # 벡터 배열을 바이트로 이어 붙여 base64 로 (src/embed.c 의 b64decode 가 그대로 float 배열로 푼다)
        self._send(200, {
            "model": self.embedder.key,
            "dim": self.embedder.dim,
            "n": len(texts),
            "data": base64.b64encode(vecs.tobytes()).decode("ascii"),
        })

    def log_message(self, fmt, *args):     # 요청마다 찍지 않는다
        # 기본 구현은 요청마다 stderr 에 한 줄씩 남기므로 덮어써서 끈다
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=int(os.environ.get("SKU_EMBED_PORT", "8001")))
    args = ap.parse_args()

    Handler.embedder = Embedder()
    # ThreadingHTTPServer: 요청마다 스레드를 만들어 동시에 처리한다 (모델 계산은 lock 으로 한 번에 하나)
    # 127.0.0.1 에만 바인드 → 외부 컴퓨터에서는 접속할 수 없다
    srv = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print("임베딩 서비스: http://127.0.0.1:%d" % args.port, flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:      # Ctrl+C 로 조용히 종료
        pass


if __name__ == "__main__":
    # Windows 콘솔에서 한글 출력이 깨지지 않도록 표준 출력 인코딩을 UTF-8 로
    sys.stdout.reconfigure(encoding="utf-8")
    main()
