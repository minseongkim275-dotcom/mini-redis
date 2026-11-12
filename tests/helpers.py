"""기능 테스트 공용 도우미: 실제 mini-redis 프로세스를 띄우고 TCP 로 붙는다."""
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "mini-redis")
sys.path.insert(0, os.path.join(ROOT, "demo"))

from miniredis_client import MiniRedis  # noqa: E402


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Server:
    """mini-redis 프로세스 하나."""

    def __init__(self, aof=None, fsync=None, port=None):
        self.port = port or free_port()
        args = [BIN, "--port", str(self.port)]
        if aof:
            args += ["--aof", aof]
        if fsync:
            args += ["--appendfsync", fsync]
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self._wait_ready()

    def _wait_ready(self):
        for _ in range(200):
            if self.proc.poll() is not None:
                raise RuntimeError("서버가 바로 종료됐다 (exit %s)" % self.proc.returncode)
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.2).close()
                return
            except OSError:
                time.sleep(0.025)
        self.kill()
        raise RuntimeError("서버가 5 초 안에 뜨지 않았다")

    def client(self, timeout=5.0):
        return MiniRedis("127.0.0.1", self.port, timeout)

    def raw(self, timeout=5.0):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=timeout)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        return s

    def stop(self):
        """SIGTERM: 정상 종료(AOF 를 비우고 닫는다)."""
        if self.proc.poll() is None:
            self.proc.terminate()
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()

    def kill(self):
        """SIGKILL: 서버가 갑자기 죽는 상황."""
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait()


def run_expect_exit(args, timeout=5):
    """서버가 뜨지 못하고 종료해야 하는 경우의 종료 코드를 돌려준다."""
    p = subprocess.run([BIN] + args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=timeout)
    return p.returncode


def read_until_close(sock, timeout=5):
    """상대가 연결을 닫을 때까지 읽는다."""
    sock.settimeout(timeout)
    data = b""
    while True:
        chunk = sock.recv(65536)
        if not chunk:
            return data
        data += chunk


def read_exact(sock, n, timeout=10):
    sock.settimeout(timeout)
    data = b""
    while len(data) < n:
        chunk = sock.recv(min(65536, n - len(data)))
        if not chunk:
            raise ConnectionError("연결이 닫혔다 (%d/%d 바이트)" % (len(data), n))
        data += chunk
    return data
