"""기능 테스트: 실제 mini-redis 프로세스를 띄워 TCP/RESP 로 외부 사용자처럼 검증한다."""
import os
import shutil
import socket
import tempfile
import threading
import time
import unittest

from helpers import Server, free_port, read_exact, read_until_close, run_expect_exit
from miniredis_client import MiniRedis, RedisError


class FunctionalTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.srv = Server()

    @classmethod
    def tearDownClass(cls):
        cls.srv.stop()

    def setUp(self):
        self.c = self.srv.client()
        self.c.flushall()

    def tearDown(self):
        self.c.close()

    # ---- 기본 명령 ----------------------------------------------------------
    def test_ping_echo(self):
        self.assertEqual(self.c.execute("PING"), "PONG")
        self.assertEqual(self.c.execute("PING", "hi"), b"hi")
        self.assertEqual(self.c.execute("ECHO", "안녕"), "안녕".encode())

    def test_set_get_del_exists(self):
        self.assertIsNone(self.c.get("a"))
        self.assertEqual(self.c.set("a", "1"), "OK")
        self.assertEqual(self.c.get("a"), b"1")
        self.assertEqual(self.c.set("a", "2"), "OK")  # 덮어쓰기
        self.assertEqual(self.c.get("a"), b"2")
        self.assertEqual(self.c.exists("a"), 1)
        self.assertEqual(self.c.delete("a", "nope"), 1)
        self.assertEqual(self.c.exists("a"), 0)
        self.assertIsNone(self.c.get("a"))

    def test_binary_safe_values_and_keys(self):
        value = b"line1\r\nline2\x00\xff\xfe tail"
        key = b"k e y\r\n\x00"
        self.assertEqual(self.c.execute("SET", key, value), "OK")
        self.assertEqual(self.c.execute("GET", key), value)
        self.assertEqual(self.c.execute("SET", "한글", "값"), "OK")
        self.assertEqual(self.c.get("한글"), "값".encode())
        self.assertEqual(self.c.execute("SET", "empty", ""), "OK")
        self.assertEqual(self.c.get("empty"), b"")

    def test_large_value(self):
        value = os.urandom(4 * 1024 * 1024)
        self.assertEqual(self.c.execute("SET", "big", value), "OK")
        self.assertEqual(self.c.execute("GET", "big"), value)

    def test_set_nx_xx(self):
        self.assertEqual(self.c.set("k", "1", nx=True), "OK")
        self.assertIsNone(self.c.set("k", "2", nx=True))
        self.assertEqual(self.c.get("k"), b"1")
        self.assertIsNone(self.c.set("none", "1", xx=True))
        self.assertEqual(self.c.set("k", "3", xx=True), "OK")
        self.assertEqual(self.c.get("k"), b"3")

    def test_keys_dbsize_flushall(self):
        for i in range(5):
            self.c.set("user:%d" % i, i)
        self.c.set("other", 1)
        self.assertEqual(self.c.dbsize(), 6)
        self.assertEqual(sorted(self.c.keys("user:*")), [b"user:%d" % i for i in range(5)])
        self.assertEqual(self.c.keys("nomatch"), [])
        self.assertEqual(self.c.flushall(), "OK")
        self.assertEqual(self.c.dbsize(), 0)

    def test_errors_do_not_break_the_connection(self):
        with self.assertRaisesRegex(RedisError, "unknown command"):
            self.c.execute("NOSUCHCMD")
        with self.assertRaisesRegex(RedisError, "wrong number of arguments"):
            self.c.execute("GET")
        with self.assertRaisesRegex(RedisError, "syntax error"):
            self.c.execute("SET", "a", "b", "BOGUS")
        self.assertEqual(self.c.ping(), "PONG")  # 같은 연결이 계속 쓰인다

    def test_quit_closes_connection(self):
        self.assertEqual(self.c.execute("QUIT"), "OK")
        with self.assertRaises((ConnectionError, OSError)):
            self.c.execute("PING")

    # ---- 만료 ---------------------------------------------------------------
    def test_lazy_expiry(self):
        self.c.set("k", "v", px=100)
        self.assertEqual(self.c.get("k"), b"v")
        time.sleep(0.25)
        self.assertIsNone(self.c.get("k"))  # 만료된 값은 절대 돌려주지 않는다
        self.assertEqual(self.c.ttl("k"), -2)
        self.assertEqual(self.c.exists("k"), 0)

    def test_active_expiry_removes_untouched_keys(self):
        for i in range(50):
            self.c.set("t:%d" % i, i, px=100)
        self.c.set("keep", "x")
        time.sleep(0.6)
        # 아무도 조회하지 않아도 주기 삭제가 메모리에서 치운다 (DBSIZE 는 치우지 않은 만료 키도 센다)
        self.assertEqual(self.c.dbsize(), 1)

    def test_expire_ttl_persist(self):
        self.c.set("k", "v")
        self.assertEqual(self.c.ttl("k"), -1)
        self.assertEqual(self.c.expire("k", 100), 1)
        self.assertTrue(98 <= self.c.ttl("k") <= 100)
        self.assertEqual(self.c.execute("PERSIST", "k"), 1)
        self.assertEqual(self.c.ttl("k"), -1)
        self.assertEqual(self.c.expire("missing", 10), 0)
        self.assertEqual(self.c.execute("PEXPIRE", "k", 100), 1)
        time.sleep(0.25)
        self.assertIsNone(self.c.get("k"))

    def test_plain_set_clears_ttl(self):
        self.c.set("k", "v", ex=100)
        self.c.set("k", "v2")
        self.assertEqual(self.c.ttl("k"), -1)

    def test_invalidation_by_del_and_ttl(self):
        self.c.set("cache:1", "old", ex=100)
        self.assertEqual(self.c.delete("cache:1"), 1)  # 명시적 무효화
        self.assertIsNone(self.c.get("cache:1"))

    # ---- 동시성 -------------------------------------------------------------
    def test_incr_is_atomic_across_clients(self):
        threads, per = 8, 500

        def work():
            c = self.srv.client()
            for _ in range(per):
                c.incr("counter")
            c.close()

        ts = [threading.Thread(target=work) for _ in range(threads)]
        [t.start() for t in ts]
        [t.join() for t in ts]
        self.assertEqual(self.c.get("counter"), str(threads * per).encode())  # 하나도 유실되지 않는다

    def test_many_clients_do_not_interfere(self):
        errors = []

        def work(n):
            try:
                c = self.srv.client()
                for i in range(50):
                    key = "c%d:%d" % (n, i)
                    c.set(key, "v%d" % n)
                    if c.get(key) != b"v%d" % n:
                        errors.append(key)
                c.close()
            except Exception as e:  # noqa: BLE001
                errors.append(repr(e))

        ts = [threading.Thread(target=work, args=(n,)) for n in range(30)]
        [t.start() for t in ts]
        [t.join() for t in ts]
        self.assertEqual(errors, [])
        self.assertEqual(self.c.dbsize(), 30 * 50)

    def test_many_simultaneous_connections(self):
        socks = [self.srv.raw() for _ in range(200)]
        try:
            for s in socks:
                s.sendall(b"*1\r\n$4\r\nPING\r\n")
            for s in socks:
                self.assertEqual(read_exact(s, 7), b"+PONG\r\n")
        finally:
            for s in socks:
                s.close()

    def test_stalled_client_does_not_block_others(self):
        stalled = self.srv.raw()
        stalled.sendall(b"*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$100\r\nonly-part")  # 명령을 끝내지 않는다
        try:
            self.assertEqual(self.c.ping(), "PONG")
            self.assertEqual(self.c.set("other", "1"), "OK")
        finally:
            stalled.close()

    # ---- 스트림 처리 (엣지 케이스) -------------------------------------------
    def test_pipeline(self):
        cmds = [("SET", "k%d" % i, i) for i in range(1000)] + [("GET", "k999"), ("DBSIZE",)]
        replies = self.c.pipeline(cmds)
        self.assertEqual(replies[:1000], ["OK"] * 1000)
        self.assertEqual(replies[1000], b"999")
        self.assertEqual(replies[1001], 1000)

    def test_command_arrives_in_tiny_fragments(self):
        s = self.srv.raw()
        try:
            data = MiniRedis.encode("SET", "frag", "value-" * 20)
            for b in data:
                s.sendall(bytes([b]))
                time.sleep(0.0005)
            self.assertEqual(read_exact(s, 5), b"+OK\r\n")
            s.sendall(MiniRedis.encode("GET", "frag"))
            expect = b"$%d\r\n%s\r\n" % (120, b"value-" * 20)
            self.assertEqual(read_exact(s, len(expect)), expect)
        finally:
            s.close()

    def test_huge_pipeline_with_slow_reader(self):
        """응답이 소켓 버퍼보다 훨씬 커도(부분 쓰기) 모든 응답이 순서대로 온다."""
        n = 100000
        s = self.srv.raw(timeout=30)
        payload = b"*1\r\n$4\r\nPING\r\n" * n

        def send():
            s.sendall(payload)

        t = threading.Thread(target=send)
        t.start()
        try:
            got = read_exact(s, 7 * n, timeout=30)
            self.assertEqual(got, b"+PONG\r\n" * n)
        finally:
            t.join()
            s.close()

    def test_inline_command(self):
        s = self.srv.raw()
        try:
            s.sendall(b"PING\r\n")
            self.assertEqual(read_exact(s, 7), b"+PONG\r\n")
            s.sendall(b"SET inline works\r\nGET inline\r\n")
            expect = b"+OK\r\n$5\r\nworks\r\n"
            self.assertEqual(read_exact(s, len(expect)), expect)
            s.sendall(b"\r\n\r\nPING\r\n")  # 빈 줄은 무시
            self.assertEqual(read_exact(s, 7), b"+PONG\r\n")
        finally:
            s.close()

    def test_protocol_errors_close_the_connection(self):
        for bad in (b"*abc\r\n", b"*1\r\n$99999999999\r\n", b"*1\r\n:5\r\n", b"*2000\r\n", b"*1\r\n$-1\r\n"):
            s = self.srv.raw()
            try:
                s.sendall(bad)
                reply = read_until_close(s)
                self.assertTrue(reply.startswith(b"-ERR Protocol error"), (bad, reply))
            finally:
                s.close()
        self.assertEqual(self.c.ping(), "PONG")  # 서버는 멀쩡하다

    def test_oversized_bulk_is_not_allocated(self):
        """16MB 보다 큰 값을 선언하기만 하는 공격에 메모리를 내주지 않는다."""
        s = self.srv.raw()
        try:
            s.sendall(b"*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1000000000\r\n")
            reply = read_until_close(s)
            self.assertTrue(reply.startswith(b"-ERR Protocol error"), reply)
        finally:
            s.close()

    def test_client_disconnect_in_the_middle_of_a_command(self):
        for _ in range(20):
            s = self.srv.raw()
            s.sendall(b"*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$10\r\nabc")
            s.close()
        self.assertEqual(self.c.ping(), "PONG")
        self.assertIsNone(self.c.get("k"))  # 끝나지 않은 명령은 실행되지 않았다

    def test_client_disconnect_with_unread_replies(self):
        s = self.srv.raw()
        s.sendall(b"*1\r\n$4\r\nPING\r\n" * 50000)
        s.close()  # 응답을 하나도 읽지 않고 끊는다
        time.sleep(0.2)
        self.assertEqual(self.c.ping(), "PONG")


class PersistenceTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.aof = os.path.join(self.dir, "appendonly.aof")
        self.srv = None

    def tearDown(self):
        if self.srv:
            self.srv.kill()
        shutil.rmtree(self.dir, ignore_errors=True)

    def start(self, **kw):
        self.srv = Server(aof=self.aof, **kw)
        return self.srv.client()

    def test_data_survives_kill_9(self):
        c = self.start()
        c.set("a", "1")
        c.set("b", "2", ex=100)
        c.incr("n")
        c.incr("n")
        c.set("c", "3")
        c.delete("c")
        c.set("t", "v")
        c.expire("t", 50)
        c.close()
        self.srv.kill()  # 서버가 갑자기 죽는다 (정리 코드가 돌 기회가 없다)

        c = self.start()
        self.assertEqual(c.get("a"), b"1")
        self.assertEqual(c.get("b"), b"2")
        self.assertEqual(c.get("n"), b"2")
        self.assertIsNone(c.get("c"))
        self.assertTrue(90 <= c.ttl("b") <= 100)  # 남은 시간이 이어진다 (100 초로 리셋되지 않는다)
        self.assertTrue(40 <= c.ttl("t") <= 50)
        c.close()

    def test_key_that_expired_while_server_was_down_is_gone(self):
        c = self.start()
        c.set("short", "v", px=300)
        c.set("long", "v")
        c.close()
        self.srv.kill()
        time.sleep(0.5)
        c = self.start()
        self.assertIsNone(c.get("short"))
        self.assertEqual(c.get("long"), b"v")
        c.close()

    def test_flushall_and_overwrite_are_replayed_in_order(self):
        c = self.start()
        c.set("a", "1")
        c.flushall()
        c.set("b", "2")
        c.set("b", "3")
        c.close()
        self.srv.kill()
        c = self.start()
        self.assertIsNone(c.get("a"))
        self.assertEqual(c.get("b"), b"3")
        c.close()

    def test_graceful_stop_and_fsync_always(self):
        c = self.start(fsync="always")
        c.set("a", "1")
        c.close()
        self.srv.stop()
        c = self.start(fsync="no")
        self.assertEqual(c.get("a"), b"1")
        c.close()

    def test_truncated_tail_is_repaired(self):
        c = self.start()
        c.set("a", "1")
        c.close()
        self.srv.kill()
        with open(self.aof, "ab") as f:  # 쓰다가 죽은 흔적
            f.write(b"*3\r\n$3\r\nSET\r\n$1\r\nx")
        c = self.start()
        self.assertEqual(c.get("a"), b"1")
        self.assertIsNone(c.get("x"))
        c.set("after", "ok")
        c.close()
        self.srv.kill()
        c = self.start()  # 잘라 낸 뒤에 이어 쓴 기록이 다음 시작에서도 읽힌다
        self.assertEqual(c.get("a"), b"1")
        self.assertEqual(c.get("after"), b"ok")
        c.close()

    def test_corrupt_aof_refuses_to_start(self):
        with open(self.aof, "wb") as f:
            f.write(b"*1\r\n$4\r\nPING\r\nGARBAGE\r\n*1\r\n$4\r\nPING\r\n")
        # 포트가 정상이므로, 비정상 종료의 원인은 깨진 AOF 뿐이다
        self.assertNotEqual(run_expect_exit(["--port", str(free_port()), "--aof", self.aof]), 0)


class StartupTest(unittest.TestCase):
    def test_bad_arguments_exit_nonzero(self):
        self.assertNotEqual(run_expect_exit(["--port", "99999"]), 0)
        self.assertNotEqual(run_expect_exit(["--port", "abc"]), 0)
        self.assertNotEqual(run_expect_exit(["--bogus"]), 0)
        self.assertNotEqual(run_expect_exit(["--appendfsync", "sometimes"]), 0)

    def test_second_server_on_same_port_fails(self):
        a = Server()
        try:
            self.assertNotEqual(run_expect_exit(["--port", str(a.port)]), 0)
            c = a.client()
            self.assertEqual(c.ping(), "PONG")  # 먼저 뜬 서버는 영향이 없다
            c.close()
        finally:
            a.stop()

    def test_listens_on_loopback_by_default(self):
        a = Server()
        try:
            out = socket.create_connection(("127.0.0.1", a.port), timeout=2)
            out.close()
        finally:
            a.stop()


if __name__ == "__main__":
    unittest.main(verbosity=2)
