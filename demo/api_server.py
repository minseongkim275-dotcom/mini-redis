"""데모 HTTP API: 느린 원본(DB 를 흉내 낸 것)의 응답을 미니 Redis 에 캐시한다 (cache-aside).

    GET    /users/<id>            캐시를 먼저 보고, 없으면 원본에서 가져와 캐시에 넣는다
                                  응답 헤더 X-Cache: HIT | MISS | ERROR(캐시 장애, 원본으로 응답)
    GET    /nocache/users/<id>    캐시를 쓰지 않고 항상 원본 (X-Cache: BYPASS) — 성능 비교용
    DELETE /users/<id>            캐시 무효화 (원본이 바뀌었을 때)
    GET    /health

실행:  python3 demo/api_server.py --port 8000 --redis-port 6380
"""
import argparse
import json
import os
import re
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from miniredis_client import ClientPool, RedisError  # noqa: E402

ID_RE = re.compile(r"^[0-9]{1,12}$")  # 키로 쓰는 값이므로 숫자만 받는다 (키 주입 방지)


def fetch_origin(user_id, delay):
    """느린 원본 조회를 흉내 낸다 (DB 쿼리, 외부 API 호출 등)."""
    time.sleep(delay)
    return {"id": user_id, "name": "user-%d" % user_id, "generated_at": time.time()}


def make_server(host, port, redis_host, redis_port, origin_delay=0.1, ttl=30):
    cache = ClientPool(redis_host, redis_port, timeout=2.0)

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        # 기본값(0)은 헤더와 본문을 따로 write 해서 Nagle + delayed ACK 로 응답마다 약 40ms 가 붙는다.
        # 모아서 한 번에 보낸다.
        wbufsize = 64 * 1024

        def log_message(self, *args):  # 요청마다 로그를 찍으면 성능 비교가 흐려진다
            pass

        def _send(self, status, body, cache_state=None):
            data = json.dumps(body).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            if cache_state:
                self.send_header("X-Cache", cache_state)
            self.end_headers()
            self.wfile.write(data)

        def _route(self):
            path = self.path.split("?", 1)[0]
            m = re.match(r"^/(nocache/)?users/([^/]*)$", path)
            if not m:
                return None, None
            return bool(m.group(1)), m.group(2)

        def do_GET(self):
            if self.path == "/health":
                return self._send(200, {"status": "ok"})
            bypass, raw_id = self._route()
            if raw_id is None:
                return self._send(404, {"error": "not found"})
            if not ID_RE.match(raw_id):
                return self._send(400, {"error": "id must be digits"})
            user_id = int(raw_id)
            if bypass:
                return self._send(200, fetch_origin(user_id, origin_delay), "BYPASS")

            key = "user:%d" % user_id
            try:
                cached = cache.execute("GET", key)
            except (OSError, ConnectionError, RedisError):
                # 캐시가 죽어도 API 는 산다: 원본으로 답한다
                return self._send(200, fetch_origin(user_id, origin_delay), "ERROR")
            if cached is not None:
                data = cached
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.send_header("X-Cache", "HIT")
                self.end_headers()
                self.wfile.write(data)
                return

            body = fetch_origin(user_id, origin_delay)
            try:
                cache.execute("SET", key, json.dumps(body), "EX", ttl)
            except (OSError, ConnectionError, RedisError):
                pass  # 저장 실패는 응답을 막지 않는다
            return self._send(200, body, "MISS")

        def do_DELETE(self):
            bypass, raw_id = self._route()
            if raw_id is None or bypass:
                return self._send(404, {"error": "not found"})
            if not ID_RE.match(raw_id):
                return self._send(400, {"error": "id must be digits"})
            try:
                n = cache.execute("DEL", "user:%d" % int(raw_id))
            except (OSError, ConnectionError, RedisError):
                return self._send(503, {"error": "cache unavailable"})
            return self._send(200, {"deleted": n})

        def _not_allowed(self):
            self._send(405, {"error": "method not allowed"})

        do_POST = do_PUT = do_PATCH = _not_allowed

    server = ThreadingHTTPServer((host, port), Handler)
    server.daemon_threads = True
    server.cache = cache  # 종료할 때 server.cache.close() 로 연결을 정리한다
    return server


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--redis-host", default="127.0.0.1")
    ap.add_argument("--redis-port", type=int, default=6379)
    ap.add_argument("--origin-delay", type=float, default=0.1, help="원본 조회 지연(초)")
    ap.add_argument("--ttl", type=int, default=30, help="캐시 수명(초)")
    args = ap.parse_args()
    srv = make_server(args.host, args.port, args.redis_host, args.redis_port, args.origin_delay, args.ttl)
    print("API: http://%s:%d  (캐시: %s:%d)" % (args.host, args.port, args.redis_host, args.redis_port))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
