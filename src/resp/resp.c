#include "resp/resp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NOT_FOUND ((size_t)-1)
#define MAX_HEADER_LINE 32 /* "*1024" / "$16777216" 같은 머리줄은 이보다 짧다 */

/* ---- 정수 ---------------------------------------------------------------- */

int resp_parse_ll(const char *s, size_t n, long long *out) {
    if (n == 0 || n > 20) return 0;
    size_t i = 0;
    int neg = 0;
    if (s[0] == '-') {
        neg = 1;
        i = 1;
        if (n == 1) return 0;
    }
    if (s[i] == '0') { /* "0" 만 허용: "01", "-0" 은 거부 */
        if (n - i != 1 || neg) return 0;
        *out = 0;
        return 1;
    }
    unsigned long long limit = neg ? 9223372036854775808ULL : 9223372036854775807ULL;
    unsigned long long v = 0;
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        unsigned d = (unsigned)(s[i] - '0');
        if (v > (limit - d) / 10) return 0; /* v*10+d 가 limit 을 넘는다 */
        v = v * 10 + d;
    }
    *out = neg ? -(long long)(v - 1) - 1 : (long long)v;
    return 1;
}

/* ---- 파서 ---------------------------------------------------------------- */

static size_t find_crlf(const char *buf, size_t from, size_t len) {
    for (size_t i = from; i + 1 < len; i++) {
        const char *p = memchr(buf + i, '\r', len - 1 - i);
        if (!p) return NOT_FOUND;
        i = (size_t)(p - buf);
        if (buf[i + 1] == '\n') return i;
    }
    return NOT_FOUND;
}

/* start 에서 시작하는 "숫자\r\n" 머리줄을 읽는다. 1 성공 / 0 더 필요 / -1 오류 */
static int read_header(const char *buf, size_t len, size_t start, long long *val, size_t *next,
                       const char **err, const char *bad) {
    size_t e = find_crlf(buf, start, len);
    if (e == NOT_FOUND) {
        if (len - start > MAX_HEADER_LINE) { /* 끝없이 이어지는 머리줄을 기다리지 않는다 */
            *err = "too big header line";
            return -1;
        }
        return 0;
    }
    if (!resp_parse_ll(buf + start, e - start, val)) {
        *err = bad;
        return -1;
    }
    *next = e + 2;
    return 1;
}

static int is_space(char c) {
    return c == ' ' || c == '\t';
}

/* telnet 처럼 한 줄로 보내는 명령: "SET foo bar\r\n" */
static int parse_inline(const char *buf, size_t len, resp_cmd *cmd, size_t *consumed, const char **err) {
    const char *nl = memchr(buf, '\n', len);
    if (!nl) {
        if (len > RESP_MAX_INLINE) {
            *err = "too big inline request";
            return -1;
        }
        return 0;
    }
    size_t line = (size_t)(nl - buf);
    *consumed = line + 1;
    if (line > 0 && buf[line - 1] == '\r') line--;
    if (line > RESP_MAX_INLINE) {
        *err = "too big inline request";
        return -1;
    }

    int cnt = 0;
    size_t i = 0;
    while (i < line) {
        while (i < line && is_space(buf[i])) i++;
        if (i >= line) break;
        cnt++;
        while (i < line && !is_space(buf[i])) i++;
    }
    if (cnt == 0) return 1; /* 빈 줄 */
    if (cnt > RESP_MAX_ARGS) {
        *err = "too many arguments";
        return -1;
    }

    const char **argv = malloc(sizeof *argv * (size_t)cnt);
    size_t *argl = malloc(sizeof *argl * (size_t)cnt);
    if (!argv || !argl) {
        free(argv);
        free(argl);
        *err = "out of memory";
        return -1;
    }
    int k = 0;
    i = 0;
    while (i < line) {
        while (i < line && is_space(buf[i])) i++;
        if (i >= line) break;
        size_t st = i;
        while (i < line && !is_space(buf[i])) i++;
        argv[k] = buf + st;
        argl[k] = i - st;
        k++;
    }
    cmd->argc = cnt;
    cmd->argv = argv;
    cmd->argl = argl;
    return 1;
}

int resp_parse(const char *buf, size_t len, resp_cmd *cmd, size_t *consumed, const char **err) {
    cmd->argc = 0;
    cmd->argv = NULL;
    cmd->argl = NULL;
    *consumed = 0;
    *err = NULL;
    if (len == 0) return 0;
    if (buf[0] != '*') return parse_inline(buf, len, cmd, consumed, err);

    long long n;
    size_t pos;
    int r = read_header(buf, len, 1, &n, &pos, err, "invalid multibulk length");
    if (r <= 0) return r;
    if (n <= 0) { /* "*0" 과 "*-1" 은 빈 명령 */
        *consumed = pos;
        return 1;
    }
    if (n > RESP_MAX_ARGS) { /* 클라이언트가 보낸 개수로 곧바로 메모리를 잡지 않는다 */
        *err = "invalid multibulk length";
        return -1;
    }

    int argc = (int)n;
    const char **argv = malloc(sizeof *argv * (size_t)argc);
    size_t *argl = malloc(sizeof *argl * (size_t)argc);
    if (!argv || !argl) {
        free(argv);
        free(argl);
        *err = "out of memory";
        return -1;
    }

    for (int i = 0; i < argc; i++) {
        if (pos >= len) goto need_more;
        if (buf[pos] != '$') {
            *err = "expected '$'";
            goto fail;
        }
        long long bl;
        r = read_header(buf, len, pos + 1, &bl, &pos, err, "invalid bulk length");
        if (r == 0) goto need_more;
        if (r < 0) goto fail;
        if (bl < 0 || bl > RESP_MAX_BULK) {
            *err = "invalid bulk length";
            goto fail;
        }
        size_t b = (size_t)bl;
        if (len - pos < b + 2) goto need_more; /* 본문이 아직 다 오지 않았다 */
        if (buf[pos + b] != '\r' || buf[pos + b + 1] != '\n') {
            *err = "bulk is not terminated by CRLF";
            goto fail;
        }
        argv[i] = buf + pos;
        argl[i] = b;
        pos += b + 2;
    }
    cmd->argc = argc;
    cmd->argv = argv;
    cmd->argl = argl;
    *consumed = pos;
    return 1;

need_more:
    free(argv);
    free(argl);
    return 0;
fail:
    free(argv);
    free(argl);
    return -1;
}

void resp_cmd_free(resp_cmd *cmd) {
    free(cmd->argv);
    free(cmd->argl);
    cmd->argv = NULL;
    cmd->argl = NULL;
    cmd->argc = 0;
}

/* ---- 응답 버퍼 ------------------------------------------------------------ */

void buf_init(buf_t *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->failed = 0;
}

void buf_free(buf_t *b) {
    free(b->data);
    buf_init(b);
}

static int buf_reserve(buf_t *b, size_t add) {
    if (b->failed) return -1;
    if (b->len + add + 1 <= b->cap) return 0; /* +1: 끝의 NUL */
    size_t nc = b->cap ? b->cap : 256;
    while (nc < b->len + add + 1) nc *= 2;
    char *p = realloc(b->data, nc);
    if (!p) {
        b->failed = 1;
        return -1;
    }
    b->data = p;
    b->cap = nc;
    return 0;
}

int buf_append(buf_t *b, const void *p, size_t n) {
    if (buf_reserve(b, n) < 0) return -1;
    if (n) memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

static void append_str(buf_t *b, const char *s) {
    buf_append(b, s, strlen(s));
}

void resp_simple(buf_t *b, const char *s) {
    buf_append(b, "+", 1);
    append_str(b, s);
    buf_append(b, "\r\n", 2);
}

void resp_error(buf_t *b, const char *msg) {
    buf_append(b, "-", 1);
    append_str(b, msg);
    buf_append(b, "\r\n", 2);
}

void resp_int(buf_t *b, long long v) {
    char tmp[32];
    int n = snprintf(tmp, sizeof tmp, ":%lld\r\n", v);
    buf_append(b, tmp, (size_t)n);
}

void resp_bulk(buf_t *b, const char *p, size_t n) {
    char tmp[32];
    int h = snprintf(tmp, sizeof tmp, "$%zu\r\n", n);
    buf_append(b, tmp, (size_t)h);
    buf_append(b, p, n);
    buf_append(b, "\r\n", 2);
}

void resp_null(buf_t *b) {
    append_str(b, "$-1\r\n");
}

void resp_array_header(buf_t *b, long long n) {
    char tmp[32];
    int h = snprintf(tmp, sizeof tmp, "*%lld\r\n", n);
    buf_append(b, tmp, (size_t)h);
}
