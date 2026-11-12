#ifndef RESP_H
#define RESP_H

#include <stddef.h>

#define RESP_MAX_ARGS 1024
#define RESP_MAX_BULK (16 * 1024 * 1024)
#define RESP_MAX_INLINE 65536

/* 파싱된 명령. argv 는 입력 버퍼 안을 가리키므로(복사 없음) 버퍼보다 오래 쓰면 안 된다. */
typedef struct {
    int argc;
    const char **argv;
    size_t *argl;
} resp_cmd;

/*
 * 버퍼 맨 앞에서 명령 하나를 파싱한다.
 *   1  : 완성. *consumed 바이트를 소비했다 (argc == 0 이면 빈 명령이라 무시하면 된다)
 *   0  : 데이터가 더 필요하다 (TCP 는 스트림이라 조각나서 올 수 있다)
 *  -1  : 프로토콜 오류. *err 에 사유.
 * 1 을 반환했으면 resp_cmd_free 로 해제해야 한다.
 */
int resp_parse(const char *buf, size_t len, resp_cmd *cmd, size_t *consumed, const char **err);
void resp_cmd_free(resp_cmd *cmd);

/* 엄격한 정수 파싱: 부호 앞 '+', 앞자리 0, 공백, 오버플로를 모두 거부한다. 성공 1. */
int resp_parse_ll(const char *s, size_t n, long long *out);

/* 응답 버퍼 */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
    int failed; /* 메모리 부족이 한 번이라도 있었으면 1 */
} buf_t;

void buf_init(buf_t *b);
void buf_free(buf_t *b);
int buf_append(buf_t *b, const void *p, size_t n);

void resp_simple(buf_t *b, const char *s);         /* +OK\r\n */
void resp_error(buf_t *b, const char *msg);        /* -ERR ...\r\n  (msg 는 접두어 포함) */
void resp_int(buf_t *b, long long v);              /* :1\r\n */
void resp_bulk(buf_t *b, const char *p, size_t n); /* $3\r\nfoo\r\n */
void resp_null(buf_t *b);                          /* $-1\r\n */
void resp_array_header(buf_t *b, long long n);     /* *2\r\n */

#endif
