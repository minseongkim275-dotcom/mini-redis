#include "testutil.h"

#include <stdarg.h>
#include <stdlib.h>

#include "commands/commands.h"

static store_t *G;
static aof_t *GA;
static char g_out[1 << 16];
static int g_close;

/* 명령을 한 번 실행하고 응답 원문을 돌려준다. 인자 목록은 NULL 로 끝낸다. */
static const char *run_(long long now, ...) {
    const char *argv[32];
    size_t argl[32];
    int argc = 0;
    va_list ap;
    va_start(ap, now);
    const char *a;
    while ((a = va_arg(ap, const char *)) != NULL && argc < 32) {
        argv[argc] = a;
        argl[argc] = strlen(a);
        argc++;
    }
    va_end(ap);
    resp_cmd cmd = {argc, argv, argl};
    buf_t out;
    buf_init(&out);
    g_close = cmd_execute(G, GA, &cmd, &out, now);
    snprintf(g_out, sizeof g_out, "%s", out.data ? out.data : "");
    buf_free(&out);
    return g_out;
}
#define R(now, ...) run_((now), __VA_ARGS__, (const char *)NULL)

static void setup(void) {
    if (G) store_destroy(G);
    G = store_create();
    GA = NULL;
}

static void test_ping_echo(void) {
    setup();
    CHECK_STR(R(0, "PING"), "+PONG\r\n");
    CHECK_STR(R(0, "ping"), "+PONG\r\n"); /* 명령 이름은 대소문자 무관 */
    CHECK_STR(R(0, "PING", "hi"), "$2\r\nhi\r\n");
    CHECK_STR(R(0, "ECHO", "hello"), "$5\r\nhello\r\n");
    CHECK_STR(R(0, "ECHO"), "-ERR wrong number of arguments for 'echo' command\r\n");
}

static void test_set_get_del_exists(void) {
    setup();
    CHECK_STR(R(0, "GET", "a"), "$-1\r\n");
    CHECK_STR(R(0, "SET", "a", "1"), "+OK\r\n");
    CHECK_STR(R(0, "GET", "a"), "$1\r\n1\r\n");
    CHECK_STR(R(0, "set", "a", "hello"), "+OK\r\n");
    CHECK_STR(R(0, "GET", "a"), "$5\r\nhello\r\n");
    CHECK_STR(R(0, "EXISTS", "a"), ":1\r\n");
    CHECK_STR(R(0, "EXISTS", "a", "a", "zz"), ":2\r\n"); /* 중복 키도 각각 센다 */
    CHECK_STR(R(0, "SET", "b", "2"), "+OK\r\n");
    CHECK_STR(R(0, "DEL", "a", "b", "nope"), ":2\r\n");
    CHECK_STR(R(0, "EXISTS", "a"), ":0\r\n");
    CHECK_STR(R(0, "GET", "b"), "$-1\r\n");
    CHECK_STR(R(0, "DEL", "a"), ":0\r\n");
}

static void test_arity_and_unknown(void) {
    setup();
    CHECK_STR(R(0, "GET"), "-ERR wrong number of arguments for 'get' command\r\n");
    CHECK_STR(R(0, "GET", "a", "b"), "-ERR wrong number of arguments for 'get' command\r\n");
    CHECK_STR(R(0, "SET", "a"), "-ERR wrong number of arguments for 'set' command\r\n");
    CHECK_STR(R(0, "DEL"), "-ERR wrong number of arguments for 'del' command\r\n");
    CHECK_STR(R(0, "foo"), "-ERR unknown command 'foo'\r\n");

    /* 이름에 CRLF 가 들어 있어도 응답 형식이 깨지지 않는다 (응답이 정확히 한 줄) */
    const char *r = R(0, "a\r\nb");
    CHECK(r[0] == '-');
    const char *first = strstr(r, "\r\n");
    CHECK(first != NULL && strstr(first + 2, "\r\n") == NULL && first[2] == '\0');
}

static void test_set_options(void) {
    setup();
    CHECK_STR(R(0, "SET", "k", "1", "NX"), "+OK\r\n");
    CHECK_STR(R(0, "SET", "k", "2", "NX"), "$-1\r\n"); /* 이미 있으면 실패 */
    CHECK_STR(R(0, "GET", "k"), "$1\r\n1\r\n");
    CHECK_STR(R(0, "SET", "z", "1", "XX"), "$-1\r\n"); /* 없으면 실패 */
    CHECK_STR(R(0, "GET", "z"), "$-1\r\n");
    CHECK_STR(R(0, "SET", "k", "3", "XX"), "+OK\r\n");
    CHECK_STR(R(0, "GET", "k"), "$1\r\n3\r\n");

    CHECK_STR(R(0, "SET", "a", "b", "NX", "XX"), "-ERR syntax error\r\n");
    CHECK_STR(R(0, "SET", "a", "b", "EX"), "-ERR syntax error\r\n");
    CHECK_STR(R(0, "SET", "a", "b", "BOGUS"), "-ERR syntax error\r\n");
    CHECK_STR(R(0, "SET", "a", "b", "EX", "10", "PX", "100"), "-ERR syntax error\r\n");
    CHECK_STR(R(0, "SET", "a", "b", "EX", "abc"), "-ERR value is not an integer or out of range\r\n");
    CHECK_STR(R(0, "SET", "a", "b", "EX", "0"), "-ERR invalid expire time in 'set' command\r\n");
    CHECK_STR(R(0, "SET", "a", "b", "EX", "-5"), "-ERR invalid expire time in 'set' command\r\n");
    CHECK_STR(R(0, "SET", "a", "b", "EX", "9223372036854775807"),
              "-ERR invalid expire time in 'set' command\r\n"); /* 곱셈 오버플로 */
    CHECK_STR(R(1000, "SET", "a", "b", "PX", "9223372036854775807"),
              "-ERR invalid expire time in 'set' command\r\n"); /* 현재 시각을 더하면 오버플로 */
    CHECK_STR(R(1000, "GET", "a"), "$-1\r\n"); /* 오류난 SET 은 아무것도 저장하지 않는다 */
}

static void test_expiry_via_set(void) {
    setup();
    CHECK_STR(R(1000, "SET", "e", "v", "EX", "10"), "+OK\r\n");
    CHECK_STR(R(1000, "TTL", "e"), ":10\r\n");
    CHECK_STR(R(1000, "PTTL", "e"), ":10000\r\n");
    CHECK_STR(R(5000, "TTL", "e"), ":6\r\n");
    CHECK_STR(R(10999, "GET", "e"), "$1\r\nv\r\n");
    CHECK_STR(R(11000, "GET", "e"), "$-1\r\n"); /* 만료 시각이 되면 없는 키 */
    CHECK_STR(R(11000, "TTL", "e"), ":-2\r\n");
    CHECK_STR(R(11000, "EXISTS", "e"), ":0\r\n");

    CHECK_STR(R(0, "SET", "p", "v", "PX", "1500"), "+OK\r\n");
    CHECK_STR(R(0, "PTTL", "p"), ":1500\r\n");
    CHECK_STR(R(0, "TTL", "p"), ":2\r\n"); /* 반올림 */
    CHECK_STR(R(1500, "GET", "p"), "$-1\r\n");

    CHECK_STR(R(0, "SET", "x", "v", "PXAT", "3000"), "+OK\r\n");
    CHECK_STR(R(2999, "GET", "x"), "$1\r\nv\r\n");
    CHECK_STR(R(3000, "GET", "x"), "$-1\r\n");
    CHECK_STR(R(0, "SET", "y", "v", "EXAT", "3"), "+OK\r\n");
    CHECK_STR(R(3000, "GET", "y"), "$-1\r\n");

    /* 이미 지난 시각을 줘도 오류가 아니라 즉시 만료 */
    CHECK_STR(R(5000, "SET", "old", "v", "PXAT", "100"), "+OK\r\n");
    CHECK_STR(R(5000, "GET", "old"), "$-1\r\n");

    /* 일반 SET 은 기존 만료를 지우고, KEEPTTL 은 유지한다 */
    CHECK_STR(R(0, "SET", "t", "1", "EX", "100"), "+OK\r\n");
    CHECK_STR(R(0, "SET", "t", "2", "KEEPTTL"), "+OK\r\n");
    CHECK_STR(R(0, "TTL", "t"), ":100\r\n");
    CHECK_STR(R(0, "SET", "t", "3"), "+OK\r\n");
    CHECK_STR(R(0, "TTL", "t"), ":-1\r\n");

    /* 만료된 키에 NX 는 성공한다 (없는 키로 취급) */
    CHECK_STR(R(0, "SET", "n", "1", "PX", "10"), "+OK\r\n");
    CHECK_STR(R(100, "SET", "n", "2", "NX"), "+OK\r\n");
    CHECK_STR(R(100, "GET", "n"), "$1\r\n2\r\n");
}

static void test_expire_family(void) {
    setup();
    CHECK_STR(R(0, "EXPIRE", "none", "10"), ":0\r\n");
    CHECK_STR(R(0, "TTL", "none"), ":-2\r\n");
    CHECK_STR(R(0, "SET", "k", "v"), "+OK\r\n");
    CHECK_STR(R(0, "TTL", "k"), ":-1\r\n");
    CHECK_STR(R(0, "EXPIRE", "k", "10"), ":1\r\n");
    CHECK_STR(R(0, "TTL", "k"), ":10\r\n");
    CHECK_STR(R(0, "PERSIST", "k"), ":1\r\n");
    CHECK_STR(R(0, "TTL", "k"), ":-1\r\n");
    CHECK_STR(R(0, "PERSIST", "k"), ":0\r\n");
    CHECK_STR(R(0, "PEXPIRE", "k", "1500"), ":1\r\n");
    CHECK_STR(R(0, "PTTL", "k"), ":1500\r\n");
    CHECK_STR(R(0, "PEXPIREAT", "k", "9000"), ":1\r\n");
    CHECK_STR(R(1000, "PTTL", "k"), ":8000\r\n");

    /* 0 이하의 만료는 키를 바로 지운다 */
    CHECK_STR(R(0, "EXPIRE", "k", "-1"), ":1\r\n");
    CHECK_STR(R(0, "GET", "k"), "$-1\r\n");

    CHECK_STR(R(0, "SET", "k", "v"), "+OK\r\n");
    CHECK_STR(R(0, "EXPIRE", "k", "abc"), "-ERR value is not an integer or out of range\r\n");
    CHECK_STR(R(0, "EXPIRE", "k", "9223372036854775807"),
              "-ERR invalid expire time in 'expire' command\r\n");
    CHECK_STR(R(0, "GET", "k"), "$1\r\nv\r\n"); /* 오류난 EXPIRE 가 키를 건드리지 않는다 */

    /* 이미 만료된 키는 없는 키 */
    CHECK_STR(R(0, "SET", "g", "v", "PX", "10"), "+OK\r\n");
    CHECK_STR(R(50, "EXPIRE", "g", "100"), ":0\r\n");
    CHECK_STR(R(50, "PERSIST", "g"), ":0\r\n");
}

static void test_incr_family(void) {
    setup();
    CHECK_STR(R(0, "INCR", "n"), ":1\r\n"); /* 없는 키는 0 으로 시작 */
    CHECK_STR(R(0, "INCR", "n"), ":2\r\n");
    CHECK_STR(R(0, "DECR", "n"), ":1\r\n");
    CHECK_STR(R(0, "INCRBY", "n", "10"), ":11\r\n");
    CHECK_STR(R(0, "DECRBY", "n", "20"), ":-9\r\n");
    CHECK_STR(R(0, "GET", "n"), "$2\r\n-9\r\n");

    CHECK_STR(R(0, "SET", "s", "abc"), "+OK\r\n");
    CHECK_STR(R(0, "INCR", "s"), "-ERR value is not an integer or out of range\r\n");
    CHECK_STR(R(0, "GET", "s"), "$3\r\nabc\r\n"); /* 실패해도 값은 그대로 */
    CHECK_STR(R(0, "SET", "z", "007"), "+OK\r\n");
    CHECK_STR(R(0, "INCR", "z"), "-ERR value is not an integer or out of range\r\n");
    CHECK_STR(R(0, "INCRBY", "n", "1.5"), "-ERR value is not an integer or out of range\r\n");

    CHECK_STR(R(0, "SET", "max", "9223372036854775807"), "+OK\r\n");
    CHECK_STR(R(0, "INCR", "max"), "-ERR increment or decrement would overflow\r\n");
    CHECK_STR(R(0, "SET", "min", "-9223372036854775808"), "+OK\r\n");
    CHECK_STR(R(0, "DECR", "min"), "-ERR increment or decrement would overflow\r\n");
    CHECK_STR(R(0, "DECRBY", "n", "-9223372036854775808"), "-ERR decrement would overflow\r\n"); /* 실제 Redis 와 같은 문구 */

    /* INCR 은 만료 시각을 유지한다 */
    CHECK_STR(R(0, "SET", "t", "5", "EX", "100"), "+OK\r\n");
    CHECK_STR(R(0, "INCR", "t"), ":6\r\n");
    CHECK_STR(R(0, "TTL", "t"), ":100\r\n");

    /* 만료된 키에 INCR 은 새 키처럼 1 부터 */
    CHECK_STR(R(0, "SET", "x", "5", "PX", "10"), "+OK\r\n");
    CHECK_STR(R(1000, "INCR", "x"), ":1\r\n");
    CHECK_STR(R(1000, "TTL", "x"), ":-1\r\n");
}

static void test_keys_dbsize_flush(void) {
    setup();
    R(0, "SET", "user:1", "a");
    R(0, "SET", "user:2", "b");
    R(0, "SET", "order:1", "c");
    R(0, "SET", "tmp", "d", "PX", "10");
    CHECK_STR(R(5, "DBSIZE"), ":4\r\n");
    const char *r = R(100, "KEYS", "*"); /* 순서는 해시에 달려 있으니 내용만 본다 */
    CHECK(strncmp(r, "*3\r\n", 4) == 0); /* 만료된 tmp 는 빠진다 */
    CHECK(strstr(r, "user:1") && strstr(r, "user:2") && strstr(r, "order:1") && !strstr(r, "tmp"));

    r = R(100, "KEYS", "user:*");
    CHECK(strncmp(r, "*2\r\n", 4) == 0);
    r = R(100, "KEYS", "user:?");
    CHECK(strncmp(r, "*2\r\n", 4) == 0);
    r = R(100, "KEYS", "[uo]*");
    CHECK(strncmp(r, "*3\r\n", 4) == 0);
    r = R(100, "KEYS", "user:[12]");
    CHECK(strncmp(r, "*2\r\n", 4) == 0);
    r = R(100, "KEYS", "user:[^1]");
    CHECK(strncmp(r, "*1\r\n", 4) == 0 && strstr(r, "user:2"));
    CHECK_STR(R(100, "KEYS", "nomatch*"), "*0\r\n");
    CHECK_STR(R(100, "KEYS", "user:1"), "*1\r\n$6\r\nuser:1\r\n");
    CHECK_STR(R(100, "KEYS", "*a*a*a*a*a*a*a*a*b"), "*0\r\n"); /* 병적인 패턴에서도 빨리 끝난다 */

    CHECK_STR(R(100, "FLUSHALL"), "+OK\r\n");
    CHECK_STR(R(100, "DBSIZE"), ":0\r\n");
    CHECK_STR(R(100, "KEYS", "*"), "*0\r\n");
    CHECK_STR(R(100, "SET", "after", "1"), "+OK\r\n");
}

static void test_quit_and_command(void) {
    setup();
    CHECK_STR(R(0, "QUIT"), "+OK\r\n");
    CHECK(g_close == 1);
    R(0, "PING");
    CHECK(g_close == 0);
    CHECK_STR(R(0, "COMMAND"), "*0\r\n");
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (len) *len = got;
    return buf;
}

static void append_file(const char *path, const char *data) {
    FILE *f = fopen(path, "ab");
    fputs(data, f);
    fclose(f);
}

static void test_aof_replay(void) {
    const char *path = "test_aof.tmp";
    remove(path);
    setup();
    GA = aof_open(path, AOF_FSYNC_NO);
    CHECK(GA != NULL);
    long long now = store_now_ms();

    R(now, "SET", "a", "1");
    R(now, "SET", "b", "2", "EX", "100");
    R(now, "INCR", "n");
    R(now, "INCR", "n");
    R(now, "SET", "c", "3");
    R(now, "DEL", "c");
    R(now, "SET", "t", "v");
    R(now, "EXPIRE", "t", "50");
    R(now, "SET", "nx", "1", "NX");
    R(now, "SET", "nx", "2", "NX"); /* 실패한 쓰기 */
    R(now, "GET", "a");             /* 읽기 */
    R(now, "DEL", "ghost");         /* 지운 게 없는 DEL */
    R(now, "SET", "d", "1");
    R(now, "EXPIRE", "d", "-5");    /* 즉시 삭제 */
    aof_close(GA);
    GA = NULL;

    size_t flen = 0;
    char *content = read_file(path, &flen);
    CHECK(content != NULL);
    /* 상대 시간(EX/EXPIRE)이 그대로 기록되면 재시작 때 TTL 이 다시 시작된다 → 절대 시각으로 기록 */
    CHECK(strstr(content, "PXAT") != NULL);
    CHECK(strstr(content, "PEXPIREAT") != NULL);
    CHECK(strstr(content, "\r\nEX\r\n") == NULL);
    CHECK(strstr(content, "\r\nEXPIRE\r\n") == NULL);
    CHECK(strstr(content, "GET") == NULL);   /* 읽기는 기록하지 않는다 */
    CHECK(strstr(content, "ghost") == NULL); /* 효과 없는 명령도 기록하지 않는다 */
    free(content);

    store_t *s2 = store_create();
    size_t n = 0;
    CHECK(aof_load(path, s2, &n) == 0);
    CHECK(n == 11);
    store_destroy(G);
    G = s2;
    GA = NULL;
    long long t2 = store_now_ms();
    CHECK_STR(R(t2, "GET", "a"), "$1\r\n1\r\n");
    CHECK_STR(R(t2, "GET", "b"), "$1\r\n2\r\n");
    CHECK_STR(R(t2, "GET", "n"), "$1\r\n2\r\n");
    CHECK_STR(R(t2, "GET", "c"), "$-1\r\n");
    CHECK_STR(R(t2, "GET", "d"), "$-1\r\n");
    CHECK_STR(R(t2, "GET", "nx"), "$1\r\n1\r\n");
    long long ttl_b = atoll(R(t2, "PTTL", "b") + 1);
    CHECK(ttl_b > 90000 && ttl_b <= 100000); /* 남은 시간이 이어진다 (100 초로 리셋되지 않는다) */
    long long ttl_t = atoll(R(t2, "PTTL", "t") + 1);
    CHECK(ttl_t > 40000 && ttl_t <= 50000);

    /* 쓰다 죽어서 끝이 잘린 파일: 잘린 부분만 버리고 복구한다 */
    size_t good = 0;
    free(read_file(path, &good));
    append_file(path, "*3\r\n$3\r\nSET\r\n$1\r\nx");
    store_t *s3 = store_create();
    CHECK(aof_load(path, s3, &n) == 0);
    CHECK(n == 11);
    size_t after = 0;
    free(read_file(path, &after));
    CHECK(after == good); /* 파일에서도 잘려, 이어 쓰는 기록이 깨진 조각 뒤에 붙지 않는다 */
    store_destroy(s3);

    remove(path);
}

static void test_aof_edge_files(void) {
    const char *path = "test_aof_edge.tmp";
    store_t *s = store_create();
    size_t n = 99;

    remove(path);
    CHECK(aof_load(path, s, &n) == 0 && n == 0); /* 파일이 없으면 빈 상태로 시작 */

    FILE *f = fopen(path, "wb"); /* 이미 지난 만료 시각이 기록된 키는 복구돼도 없다 */
    fputs("*5\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n$4\r\nPXAT\r\n$1\r\n1\r\n", f);
    fclose(f);
    CHECK(aof_load(path, s, &n) == 0 && n == 1);
    store_destroy(G);
    G = s;
    GA = NULL;
    CHECK_STR(R(store_now_ms(), "GET", "k"), "$-1\r\n");

    f = fopen(path, "wb"); /* 중간이 깨진 파일은 조용히 넘기지 않고 오류 */
    fputs("*1\r\n$4\r\nPING\r\nGARBAGE\r\n*1\r\n$4\r\nPING\r\n", f);
    fclose(f);
    store_t *s2 = store_create();
    CHECK(aof_load(path, s2, &n) == -1);
    store_destroy(s2);

    f = fopen(path, "wb"); /* '*' 로 시작하지 않는 줄(인라인 명령)도 AOF 에서는 허용하지 않는다 */
    fputs("SET a 1\r\n", f);
    fclose(f);
    s2 = store_create();
    CHECK(aof_load(path, s2, &n) == -1);
    store_destroy(s2);

    remove(path);
}

int main(void) {
    test_ping_echo();
    test_set_get_del_exists();
    test_arity_and_unknown();
    test_set_options();
    test_expiry_via_set();
    test_expire_family();
    test_incr_family();
    test_keys_dbsize_flush();
    test_quit_and_command();
    test_aof_replay();
    test_aof_edge_files();
    if (G) store_destroy(G);
    return t_report("test_commands");
}
