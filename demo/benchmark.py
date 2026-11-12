"""캐시 사용 / 미사용 성능 비교.

미니 Redis 와 데모 API 를 띄우고, 같은 요청 묶음을 두 경로로 보내 지연 시간과 처리량을 잰다.
  - 캐시 미사용: /nocache/users/<id>  (매번 느린 원본)
  - 캐시 사용  : /users/<id>          (첫 요청만 원본, 이후는 미니 Redis)
요청은 일부 id 에 몰리도록(핫 키) 뽑는다. 결과는 이 머신, 이 설정에서 잰 값이다.

    make bench
    python3 demo/benchmark.py --requests 400 --threads 16 --origin-delay 0.1 --keys 20
"""
import argparse
import http.client
import os
import random
import statistics
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from api_server import make_server  # noqa: E402
from miniredis_client import MiniRedis  # noqa: E402


def pick_port():
    import socket
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def run_load(port, paths, threads):
    """paths 를 threads 개 스레드로 나눠 요청하고 (요청별 지연, 전체 시간, 헤더 집계) 를 돌려준다."""
    local = threading.local()
    lat, states, lock = [], {}, threading.Lock()

    def one(path):
        conn = getattr(local, "c", None)
        if conn is None:
            conn = local.c = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
        t = time.perf_counter()
        conn.request("GET", path)
        r = conn.getresponse()
        r.read()
        dt = time.perf_counter() - t
        with lock:
            lat.append(dt)
            st = r.getheader("X-Cache")
            states[st] = states.get(st, 0) + 1

    t0 = time.perf_counter()
    with ThreadPoolExecutor(max_workers=threads) as ex:
        list(ex.map(one, paths))
    return lat, time.perf_counter() - t0, states


def pct(sorted_vals, p):
    return sorted_vals[min(len(sorted_vals) - 1, int(len(sorted_vals) * p))]


def report(name, lat, total, states):
    lat = sorted(lat)
    ms = lambda x: "%8.2f ms" % (x * 1000)  # noqa: E731
    print("%-14s 요청 %4d | 평균 %s | p50 %s | p95 %s | 처리량 %7.1f req/s | X-Cache %s" % (
        name, len(lat), ms(statistics.mean(lat)), ms(pct(lat, 0.5)), ms(pct(lat, 0.95)),
        len(lat) / total, states))
    return statistics.mean(lat), len(lat) / total


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--requests", type=int, default=400)
    ap.add_argument("--threads", type=int, default=16)
    ap.add_argument("--origin-delay", type=float, default=0.1, help="원본 조회 지연(초)")
    ap.add_argument("--keys", type=int, default=20, help="서로 다른 id 개수")
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()

    binary = os.path.join(ROOT, "mini-redis")
    if not os.path.exists(binary):
        sys.exit("mini-redis 가 없다. 먼저 make 를 실행해.")
    rport = pick_port()
    proc = subprocess.Popen([binary, "--port", str(rport)], stdout=subprocess.DEVNULL)
    for _ in range(100):
        try:
            c = MiniRedis("127.0.0.1", rport)
            break
        except OSError:
            time.sleep(0.05)
    else:
        proc.kill()
        sys.exit("mini-redis 가 뜨지 않았다")

    httpd = make_server("127.0.0.1", 0, "127.0.0.1", rport, origin_delay=args.origin_delay, ttl=300)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    api_port = httpd.server_address[1]

    rnd = random.Random(args.seed)
    ids = [rnd.randrange(args.keys) for _ in range(args.requests)]
    print("설정: 요청 %d, 스레드 %d, 원본 지연 %.0f ms, 서로 다른 id %d, 캐시 TTL 300 s\n" % (
        args.requests, args.threads, args.origin_delay * 1000, args.keys))

    try:
        # 워밍업(연결/스레드 준비)은 캐시와 무관한 /health 로
        run_load(api_port, ["/health"] * 50, args.threads)

        base = report("캐시 미사용", *run_load(api_port, ["/nocache/users/%d" % i for i in ids], args.threads))
        c.flushall()  # 캐시가 빈 상태에서 시작: 첫 요청은 MISS 로 비용을 그대로 낸다
        cached = report("캐시 사용", *run_load(api_port, ["/users/%d" % i for i in ids], args.threads))

        print("\n평균 지연: %.1f 배 빠름 | 처리량: %.1f 배" % (base[0] / cached[0], cached[1] / base[1]))
        print("(캐시 사용 경로의 MISS 는 서로 다른 id 개수만큼 + 동시에 같은 id 를 요청한 중복분)")

        # 미니 Redis 자체의 지연: 캐시 적중 한 번이 얼마나 드는가
        c.set("bench", "x" * 200)
        t = time.perf_counter()
        n = 5000
        for _ in range(n):
            c.get("bench")
        per = (time.perf_counter() - t) / n
        print("\n미니 Redis GET 왕복(파이썬 클라이언트, 순차 %d 회): 평균 %.3f ms" % (n, per * 1000))
    finally:
        httpd.shutdown()
        c.close()
        proc.terminate()
        proc.wait()


if __name__ == "__main__":
    main()
