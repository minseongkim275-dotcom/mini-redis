#include "testutil.h"

#include <stdlib.h>

#include "resp/resp.h"

static const char *SET_CMD = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";

static void test_parse_complete(void) {
    resp_cmd c;
    size_t used = 0;
    const char *err = NULL;
    CHECK(resp_parse(SET_CMD, strlen(SET_CMD), &c, &used, &err) == 1);
    CHECK(used == strlen(SET_CMD));
    CHECK(c.argc == 3);
    CHECK(c.argl[0] == 3 && memcmp(c.argv[0], "SET", 3) == 0);
    CHECK(c.argl[1] == 3 && memcmp(c.argv[1], "foo", 3) == 0);
    CHECK(c.argl[2] == 3 && memcmp(c.argv[2], "bar", 3) == 0);
    resp_cmd_free(&c);
}

/* TCP 는 스트림이라 어느 바이트에서 끊겨 와도 "더 필요함(0)"이어야 한다 */
static void test_every_prefix_needs_more(void) {
    size_t total = strlen(SET_CMD);
    for (size_t n = 0; n < total; n++) {
        resp_cmd c;
        size_t used = 123;
        const char *err = NULL;
        int r = resp_parse(SET_CMD, n, &c, &used, &err);
        CHECK(r == 0);
        if (r == 1) resp_cmd_free(&c);
    }
}

static void test_pipelined_commands(void) {
    const char *two = "*1\r\n$4\r\nPING\r\n*2\r\n$3\r\nGET\r\n$1\r\nk\r\n";
    size_t first = strlen("*1\r\n$4\r\nPING\r\n");
    resp_cmd c;
    size_t used;
    const char *err;
    CHECK(resp_parse(two, strlen(two), &c, &used, &err) == 1);
    CHECK(used == first); /* 첫 명령까지만 소비 */
    CHECK(c.argc == 1 && memcmp(c.argv[0], "PING", 4) == 0);
    resp_cmd_free(&c);
    CHECK(resp_parse(two + used, strlen(two) - used, &c, &used, &err) == 1);
    CHECK(c.argc == 2 && memcmp(c.argv[1], "k", 1) == 0);
    resp_cmd_free(&c);
}

static void test_binary_payload(void) {
    /* 값 안에 \r\n 과 NUL 이 있어도 길이로 읽는다 */
    const char msg[] = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$6\r\na\r\nb\0c\r\n";
    resp_cmd c;
    size_t used;
    const char *err;
    CHECK(resp_parse(msg, sizeof msg - 1, &c, &used, &err) == 1);
    CHECK(c.argl[2] == 6 && memcmp(c.argv[2], "a\r\nb\0c", 6) == 0);
    resp_cmd_free(&c);

    /* 빈 bulk */
    const char *empty = "*2\r\n$3\r\nGET\r\n$0\r\n\r\n";
    CHECK(resp_parse(empty, strlen(empty), &c, &used, &err) == 1);
    CHECK(c.argc == 2 && c.argl[1] == 0);
    resp_cmd_free(&c);
}

static void test_inline_command(void) {
    const char *line = "SET  foo\tbar\r\n";
    resp_cmd c;
    size_t used;
    const char *err;
    CHECK(resp_parse(line, strlen(line), &c, &used, &err) == 1);
    CHECK(used == strlen(line));
    CHECK(c.argc == 3 && memcmp(c.argv[1], "foo", 3) == 0 && memcmp(c.argv[2], "bar", 3) == 0);
    resp_cmd_free(&c);

    CHECK(resp_parse("PING", 4, &c, &used, &err) == 0); /* 줄이 끝나지 않았다 */

    const char *blank = "\r\n"; /* 빈 줄은 무시할 명령(argc 0) */
    CHECK(resp_parse(blank, 2, &c, &used, &err) == 1);
    CHECK(c.argc == 0 && used == 2);
    resp_cmd_free(&c);
}

static void test_empty_array(void) {
    resp_cmd c;
    size_t used;
    const char *err;
    CHECK(resp_parse("*0\r\n", 4, &c, &used, &err) == 1);
    CHECK(c.argc == 0 && used == 4);
    resp_cmd_free(&c);
    CHECK(resp_parse("*-1\r\n", 5, &c, &used, &err) == 1);
    CHECK(c.argc == 0 && used == 5);
    resp_cmd_free(&c);
}

static void expect_error(const char *input) {
    resp_cmd c;
    size_t used = 0;
    const char *err = NULL;
    int r = resp_parse(input, strlen(input), &c, &used, &err);
    CHECK(r == -1);
    CHECK(err != NULL);
    if (r == 1) resp_cmd_free(&c);
}

static void test_protocol_errors(void) {
    expect_error("*abc\r\n");                              /* 배열 길이가 숫자가 아님 */
    expect_error("*2000\r\n");                             /* 인자 개수 상한 초과 */
    expect_error("*1\r\n:5\r\n");                          /* bulk 가 아님 */
    expect_error("*1\r\n$abc\r\n");                        /* bulk 길이가 숫자가 아님 */
    expect_error("*1\r\n$-1\r\n");                         /* 명령 인자는 nil 일 수 없다 */
    expect_error("*1\r\n$99999999999\r\n");                /* bulk 길이 상한 초과 (할당 방지) */
    expect_error("*1\r\n$3\r\nabcXY");                     /* bulk 뒤가 CRLF 가 아님 */
    expect_error("*01\r\n$1\r\nx\r\n");                    /* 앞자리 0 거부 */

    /* 머리줄이 끝없이 이어지는 입력은 무한정 기다리지 않고 오류 */
    char longline[200];
    memset(longline, '9', sizeof longline);
    longline[0] = '*';
    resp_cmd c;
    size_t used;
    const char *err;
    CHECK(resp_parse(longline, sizeof longline, &c, &used, &err) == -1);
}

static void test_parse_ll(void) {
    long long v = 0;
    CHECK(resp_parse_ll("0", 1, &v) == 1 && v == 0);
    CHECK(resp_parse_ll("123", 3, &v) == 1 && v == 123);
    CHECK(resp_parse_ll("-45", 3, &v) == 1 && v == -45);
    CHECK(resp_parse_ll("9223372036854775807", 19, &v) == 1 && v == 9223372036854775807LL);
    CHECK(resp_parse_ll("-9223372036854775808", 20, &v) == 1 && v == (-9223372036854775807LL - 1));
    CHECK(resp_parse_ll("9223372036854775808", 19, &v) == 0);   /* 오버플로 */
    CHECK(resp_parse_ll("-9223372036854775809", 20, &v) == 0);
    CHECK(resp_parse_ll("", 0, &v) == 0);
    CHECK(resp_parse_ll("-", 1, &v) == 0);
    CHECK(resp_parse_ll("+1", 2, &v) == 0);
    CHECK(resp_parse_ll("01", 2, &v) == 0);
    CHECK(resp_parse_ll("-0", 2, &v) == 0);
    CHECK(resp_parse_ll("1 ", 2, &v) == 0);
    CHECK(resp_parse_ll("1.5", 3, &v) == 0);
    CHECK(resp_parse_ll("abc", 3, &v) == 0);
}

static void test_writers(void) {
    buf_t b;
    buf_init(&b);
    resp_simple(&b, "OK");
    CHECK_STR(b.data, "+OK\r\n");
    b.len = 0;
    resp_error(&b, "ERR boom");
    CHECK_STR(b.data, "-ERR boom\r\n");
    b.len = 0;
    resp_int(&b, -42);
    CHECK_STR(b.data, ":-42\r\n");
    b.len = 0;
    resp_bulk(&b, "foo", 3);
    CHECK_STR(b.data, "$3\r\nfoo\r\n");
    b.len = 0;
    resp_bulk(&b, "", 0);
    CHECK_STR(b.data, "$0\r\n\r\n");
    b.len = 0;
    resp_null(&b);
    CHECK_STR(b.data, "$-1\r\n");
    b.len = 0;
    resp_array_header(&b, 2);
    CHECK_STR(b.data, "*2\r\n");
    CHECK(!b.failed);

    /* 큰 응답도 자란다 */
    b.len = 0;
    char *big = malloc(1 << 20);
    memset(big, 'x', 1 << 20);
    resp_bulk(&b, big, 1 << 20);
    CHECK(b.len == strlen("$1048576\r\n") + (1u << 20) + 2);
    free(big);
    buf_free(&b);
}

int main(void) {
    test_parse_complete();
    test_every_prefix_needs_more();
    test_pipelined_commands();
    test_binary_payload();
    test_inline_command();
    test_empty_array();
    test_protocol_errors();
    test_parse_ll();
    test_writers();
    return t_report("test_resp");
}
