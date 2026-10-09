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
import argparse
import base64
import hashlib
import json
import os
import sqlite3
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import numpy as np
from sentence_transformers import SentenceTransformer

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_MODEL = "intfloat/multilingual-e5-small"
FINETUNED_DIR = os.path.join(HERE, "models", "finetuned")
CACHE_PATH = os.path.join(HERE, "cache", "embeddings.sqlite")
MAX_TEXTS = 5000
MAX_BODY = 32 * 1024 * 1024


def pick_model():
    """(불러올 경로나 이름, 캐시 키에 쓸 이름, e5 접두어 필요 여부)"""
    name = os.environ.get("SKU_EMBED_MODEL")
    if not name and os.path.isdir(FINETUNED_DIR):
        name = FINETUNED_DIR
    name = name or DEFAULT_MODEL

    base = name
    if os.path.isdir(name):
        meta = os.path.join(name, "sku_meta.json")
        if os.path.exists(meta):
            with open(meta, encoding="utf-8") as f:
                base = json.load(f).get("base", name)
        # 다시 미세조정하면 같은 경로라도 캐시가 갈리도록 수정 시각을 붙인다.
        key = "finetuned:%s@%d" % (base, int(os.path.getmtime(name)))
    else:
        key = name
    return name, key, "e5" in base.lower()


class Embedder:
    def __init__(self):
        self.path, self.key, self.e5 = pick_model()
        print("모델 불러오는 중: %s" % self.path, flush=True)
        self.model = SentenceTransformer(self.path, device="cpu")
        get_dim = getattr(self.model, "get_embedding_dimension", None) or \
            self.model.get_sentence_embedding_dimension          # 예전 버전 이름
        self.dim = get_dim()
        self.lock = threading.Lock()            # 모델과 캐시 연결을 한 스레드씩 쓴다
        os.makedirs(os.path.dirname(CACHE_PATH), exist_ok=True)
        self.db = sqlite3.connect(CACHE_PATH, check_same_thread=False)
        self.db.execute("CREATE TABLE IF NOT EXISTS emb (h TEXT PRIMARY KEY, v BLOB)")
        print("준비 완료: %s (%d차원, e5 접두어 %s)" % (self.key, self.dim, "사용" if self.e5 else "없음"),
              flush=True)

    def _hash(self, kind, text):
        return hashlib.sha1(("%s\0%s\0%s" % (self.key, kind, text)).encode("utf-8")).hexdigest()

    def embed(self, kind, texts):
        hashes = [self._hash(kind, t) for t in texts]
        out = np.zeros((len(texts), self.dim), dtype="<f4")
        with self.lock:
            missing = []
            for i, h in enumerate(hashes):
                row = self.db.execute("SELECT v FROM emb WHERE h = ?", (h,)).fetchone()
                if row and len(row[0]) == self.dim * 4:
                    out[i] = np.frombuffer(row[0], dtype="<f4")
                else:
                    missing.append(i)
            if missing:
                prefix = (kind + ": ") if self.e5 else ""
                vecs = self.model.encode([prefix + texts[i] for i in missing], batch_size=16,
                                         normalize_embeddings=True, convert_to_numpy=True)
                for i, v in zip(missing, vecs):
                    out[i] = v
                    self.db.execute("INSERT OR REPLACE INTO emb VALUES (?, ?)",
                                    (hashes[i], v.astype("<f4").tobytes()))
                self.db.commit()
        return out, len(missing)


class Handler(BaseHTTPRequestHandler):
    embedder = None

    def _send(self, status, obj):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/health":
            e = self.embedder
            self._send(200, {"model": e.key, "dim": e.dim})
        else:
            self._send(404, {"error": "not found"})

    def do_POST(self):
        if self.path != "/embed":
            self._send(404, {"error": "not found"})
            return
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
            self._send(400, {"error": str(ex)})
            return

        vecs, computed = self.embedder.embed(kind, texts)
        if computed:
            print("임베딩 %d건 계산 (요청 %d건)" % (computed, len(texts)), flush=True)
        self._send(200, {
            "model": self.embedder.key,
            "dim": self.embedder.dim,
            "n": len(texts),
            "data": base64.b64encode(vecs.tobytes()).decode("ascii"),
        })

    def log_message(self, fmt, *args):     # 요청마다 찍지 않는다
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=int(os.environ.get("SKU_EMBED_PORT", "8001")))
    args = ap.parse_args()

    Handler.embedder = Embedder()
    srv = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print("임베딩 서비스: http://127.0.0.1:%d" % args.port, flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
