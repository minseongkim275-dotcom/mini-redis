#ifndef SERVER_H
#define SERVER_H

#include "aof/aof.h"

typedef struct {
    const char *bind_addr; /* 기본 127.0.0.1 (인증이 없으므로 바깥에 열려면 명시해야 한다) */
    int port;
    const char *aof_path;  /* NULL 이면 영속성 없음 */
    aof_policy_t fsync_policy;
} server_config_t;

/* 서버를 실행한다. SIGINT/SIGTERM 으로 멈추면 0, 시작하지 못하면 1. */
int server_run(const server_config_t *cfg);

#endif
