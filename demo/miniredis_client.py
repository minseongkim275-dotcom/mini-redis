"""미니 Redis 용 최소 클라이언트 (표준 라이브러리만 사용).

RESP 로 말하므로 같은 프로토콜을 쓰는 어떤 Redis 클라이언트로도 붙을 수 있다.
이 파일은 데모 API 와 테스트가 쓰는 작은 구현이다.

    r = MiniRedis("127.0.0.1", 6380)
    r.set("greeting", "hello", ex=60)
    r.get("greeting")   # b"hello"
    r.delete("greeting")
"""
import socket
import threading


class RedisError(Exception):
    """서버가 '-ERR ...' 로 응답했을 때."""


class MiniRedis:
    def __init__(self, host="127.0.0.1", port=6379, timeout=5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._rf = self.sock.makefile("rb")

    def close(self):
        try:
            self._rf.close()
        finally:
            self.sock.close()

    # ---- RESP 인코딩/디코딩 -------------------------------------------------
    @staticmethod
    def encode(*args):
        parts = [b"*%d\r\n" % len(args)]
        for a in args:
            if isinstance(a, str):
                a = a.encode()
            elif isinstance(a, (int, float)):
                a = str(a).encode()
            parts.append(b"$%d\r\n%s\r\n" % (len(a), a))
        return b"".join(parts)

    def _read(self):
        line = self._rf.readline()
        if not line:
            raise ConnectionError("서버가 연결을 닫았다")
        kind, body = line[:1], line[1:-2]
        if kind == b"+":
            return body.decode()
        if kind == b"-":
            raise RedisError(body.decode())
        if kind == b":":
            return int(body)
        if kind == b"$":
            n = int(body)
            if n == -1:
                return None
            data = self._rf.read(n + 2)
            return data[:-2]
        if kind == b"*":
            n = int(body)
            return None if n == -1 else [self._read() for _ in range(n)]
        raise ValueError("알 수 없는 응답: %r" % line)

    # ---- 명령 -------------------------------------------------------------
    def execute(self, *args):
        self.sock.sendall(self.encode(*args))
        return self._read()

    def pipeline(self, commands):
        """명령 여러 개를 한 번에 보내고 응답을 모아서 돌려준다. 오류 응답은 예외 객체로 담긴다."""
        self.sock.sendall(b"".join(self.encode(*c) for c in commands))
        out = []
        for _ in commands:
            try:
                out.append(self._read())
            except RedisError as e:
                out.append(e)
        return out

    def ping(self):
        return self.execute("PING")

    def get(self, key):
        return self.execute("GET", key)

    def set(self, key, value, ex=None, px=None, nx=False, xx=False):
        args = ["SET", key, value]
        if ex is not None:
            args += ["EX", ex]
        if px is not None:
            args += ["PX", px]
        if nx:
            args.append("NX")
        if xx:
            args.append("XX")
        return self.execute(*args)

    def delete(self, *keys):
        return self.execute("DEL", *keys)

    def exists(self, *keys):
        return self.execute("EXISTS", *keys)

    def expire(self, key, seconds):
        return self.execute("EXPIRE", key, seconds)

    def ttl(self, key):
        return self.execute("TTL", key)

    def incr(self, key):
        return self.execute("INCR", key)

    def keys(self, pattern="*"):
        return self.execute("KEYS", pattern)

    def dbsize(self):
        return self.execute("DBSIZE")

    def flushall(self):
        return self.execute("FLUSHALL")


class ClientPool:
    """연결 풀: HTTP 서버처럼 스레드가 계속 생기고 사라지는 곳에서 연결이 새지 않게 한다.

    execute() 마다 풀에서 연결 하나를 빌려 쓰고 돌려준다. 오류가 난 연결은 버리고 예외를 그대로 올린다
    (다음 호출이 새 연결을 맺는다). 풀 크기를 넘는 연결은 돌려받을 때 닫는다.
    """

    def __init__(self, host, port, timeout=2.0, max_idle=16):
        self._host, self._port, self._timeout, self._max_idle = host, port, timeout, max_idle
        self._idle = []
        self._lock = threading.Lock()

    def execute(self, *args):
        with self._lock:
            conn = self._idle.pop() if self._idle else None
        if conn is None:
            conn = MiniRedis(self._host, self._port, self._timeout)
        try:
            result = conn.execute(*args)
        except RedisError:
            self._release(conn)  # 서버가 오류로 답했을 뿐 연결은 멀쩡하다
            raise
        except BaseException:
            try:
                conn.close()
            except OSError:
                pass
            raise
        self._release(conn)
        return result

    def _release(self, conn):
        with self._lock:
            if len(self._idle) < self._max_idle:
                self._idle.append(conn)
                return
        conn.close()

    def close(self):
        with self._lock:
            idle, self._idle = self._idle, []
        for c in idle:
            c.close()
