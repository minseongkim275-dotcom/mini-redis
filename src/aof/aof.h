#ifndef AOF_H
#define AOF_H

#include <stddef.h>

#include "store/store.h"

typedef struct aof aof_t;

typedef enum {
    AOF_FSYNC_NO = 0,       /* OS 에 맡긴다 */
    AOF_FSYNC_EVERYSEC = 1, /* 1초마다 디스크에 강제 기록 (기본) */
    AOF_FSYNC_ALWAYS = 2    /* 명령마다 강제 기록 */
} aof_policy_t;

/* 추가 모드로 연다. 실패하면 NULL. */
aof_t *aof_open(const char *path, aof_policy_t policy);
/* 명령 하나를 RESP 형식 그대로 파일 끝에 덧붙인다. */
void aof_append(aof_t *a, int argc, const char **argv, const size_t *argl);
/* 이벤트 루프가 주기적으로 호출: everysec 정책의 fsync 를 수행한다. */
void aof_cron(aof_t *a, long long now);
void aof_close(aof_t *a);

/*
 * 파일을 처음부터 다시 실행해 s 를 복원한다. 파일이 없으면 0(빈 상태).
 * 끝이 잘린 명령(쓰다가 죽은 흔적)은 파일에서 잘라내고 계속한다.
 * 중간이 깨졌으면 -1. *replayed 에 실행한 명령 수.
 */
int aof_load(const char *path, store_t *s, size_t *replayed);

#endif
