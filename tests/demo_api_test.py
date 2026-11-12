"""데모 API 기능 테스트: 느린 원본의 응답을 미니 Redis 에 캐시하는 HTTP API 를 외부 사용자처럼 호출한다."""
import json
import threading
import time
import unittest
import urllib.error
import urllib.request

from helpers import Server, free_port
from api_server import make_server

DELAY = 0.05


def call(base, path, method="GET"):
    req = urllib.request.Request(base + path, method=method)
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, dict(r.headers), json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), json.loads(e.read() or b"{}")


def stop_api(httpd):
    httpd.shutdown()
    httpd.server_close()  # 리슨 소켓
    httpd.cache.close()   # 캐시 연결 풀


def serve(redis_port, ttl=30):
    httpd = make_server("127.0.0.1", 0, "127.0.0.1", redis_port, origin_delay=DELAY, ttl=ttl)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd, "http://127.0.0.1:%d" % httpd.server_address[1]


class DemoApiTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.redis = Server()
        cls.httpd, cls.base = serve(cls.redis.port, ttl=1)

    @classmethod
    def tearDownClass(cls):
        stop_api(cls.httpd)
        cls.redis.stop()

    def setUp(self):
        c = self.redis.client()
        c.flushall()
        c.close()

    def test_first_call_misses_then_hits_with_same_payload(self):
        s1, h1, b1 = call(self.base, "/users/1")
        s2, h2, b2 = call(self.base, "/users/1")
        self.assertEqual((s1, h1["X-Cache"]), (200, "MISS"))
        self.assertEqual((s2, h2["X-Cache"]), (200, "HIT"))
        self.assertEqual(b1, b2)  # 캐시에서 온 응답이 원본 응답과 같다 (generated_at 까지)
        self.assertEqual(b1["id"], 1)

    def test_hit_is_faster_than_miss(self):
        t = time.perf_counter()
        call(self.base, "/users/2")
        miss = time.perf_counter() - t
        t = time.perf_counter()
        call(self.base, "/users/2")
        hit = time.perf_counter() - t
        self.assertGreaterEqual(miss, DELAY)
        self.assertLess(hit, miss)

    def test_cache_entry_is_stored_in_mini_redis_with_ttl(self):
        call(self.base, "/users/3")
        c = self.redis.client()
        try:
            self.assertEqual(c.keys("user:*"), [b"user:3"])
            self.assertIn(c.ttl("user:3"), (0, 1))
        finally:
            c.close()

    def test_entry_expires_and_is_refetched(self):
        _, _, first = call(self.base, "/users/4")
        time.sleep(1.2)  # TTL 1 초
        _, h, second = call(self.base, "/users/4")
        self.assertEqual(h["X-Cache"], "MISS")
        self.assertGreater(second["generated_at"], first["generated_at"])

    def test_delete_invalidates_cache(self):
        _, _, first = call(self.base, "/users/5")
        s, _, body = call(self.base, "/users/5", method="DELETE")
        self.assertEqual((s, body), (200, {"deleted": 1}))
        _, h, second = call(self.base, "/users/5")
        self.assertEqual(h["X-Cache"], "MISS")
        self.assertGreater(second["generated_at"], first["generated_at"])
        self.assertEqual(call(self.base, "/users/5", method="DELETE")[2], {"deleted": 1})
        self.assertEqual(call(self.base, "/users/404404", method="DELETE")[2], {"deleted": 0})

    def test_nocache_endpoint_bypasses(self):
        call(self.base, "/users/6")
        _, h, _ = call(self.base, "/nocache/users/6")
        self.assertEqual(h["X-Cache"], "BYPASS")

    def test_different_ids_do_not_collide(self):
        _, _, a = call(self.base, "/users/7")
        _, _, b = call(self.base, "/users/8")
        self.assertNotEqual(a["id"], b["id"])
        self.assertEqual(call(self.base, "/users/7")[2], a)

    def test_bad_requests(self):
        self.assertEqual(call(self.base, "/users/abc")[0], 400)
        self.assertEqual(call(self.base, "/users/1;DEL%20x")[0], 400)  # 키 주입 시도
        self.assertEqual(call(self.base, "/users/" + "9" * 40)[0], 400)
        self.assertEqual(call(self.base, "/nothing")[0], 404)
        self.assertEqual(call(self.base, "/users/1", method="POST")[0], 405)
        self.assertEqual(call(self.base, "/health")[0], 200)

    def test_concurrent_requests_all_succeed(self):
        results = []

        def work(i):
            results.append(call(self.base, "/users/%d" % (i % 5))[0])

        ts = [threading.Thread(target=work, args=(i,)) for i in range(40)]
        [t.start() for t in ts]
        [t.join() for t in ts]
        self.assertEqual(results, [200] * 40)

    def test_api_survives_when_cache_is_down(self):
        dead_port = free_port()  # 아무도 듣지 않는 포트
        httpd, base = serve(dead_port)
        try:
            s, h, body = call(base, "/users/9")
            self.assertEqual(s, 200)  # 캐시 장애가 API 장애로 번지지 않는다
            self.assertEqual(h["X-Cache"], "ERROR")
            self.assertEqual(body["id"], 9)
        finally:
            stop_api(httpd)

    def test_api_recovers_after_cache_restart(self):
        redis = Server()
        httpd, base = serve(redis.port)
        try:
            self.assertEqual(call(base, "/users/10")[1]["X-Cache"], "MISS")
            self.assertEqual(call(base, "/users/10")[1]["X-Cache"], "HIT")
            redis.kill()
            self.assertEqual(call(base, "/users/10")[1]["X-Cache"], "ERROR")
            redis2 = Server(port=redis.port)
            try:
                call(base, "/users/10")  # 끊어진 연결을 버리는 호출 (ERROR 또는 MISS)
                self.assertEqual(call(base, "/users/10")[1]["X-Cache"], "HIT")
            finally:
                redis2.stop()
        finally:
            stop_api(httpd)
            redis.kill()


if __name__ == "__main__":
    unittest.main(verbosity=2)
