#ifndef STORE_H
#define STORE_H

#include <stddef.h>

/* expire_at 값: 0 이상이면 만료 시각(Unix epoch 밀리초) */
#define STORE_NO_EXPIRE (-1LL) /* 만료 없음 */
#define STORE_KEEP_TTL (-2LL)  /* store_set 에서만: 기존 만료 시각 유지 */

typedef struct entry {
    char *key;
    size_t klen;
    char *val;
    size_t vlen;
    long long expire_at;
    unsigned long long hash;
    struct entry *next; /* 같은 버킷의 다음 항목 (체이닝) */
} entry_t;

typedef struct store store_t;
typedef void (*store_visit_fn)(const entry_t *e, void *ud);

long long store_now_ms(void);

store_t *store_create(void);
void store_destroy(store_t *s);

/*
 * 저장(덮어쓰기 포함). 성공 0, 메모리 부족 -1.
 * 메모리 소유: 키와 값은 store 가 복사해 소유한다. 호출자의 버퍼는 반환 후 자유롭게 써도 된다.
 */
int store_set(store_t *s, const char *k, size_t kl, const char *v, size_t vl,
              long long expire_at, long long now);
/*
 * 조회. 만료된 키는 이 자리에서 지우고(지연 삭제) NULL 을 돌려준다.
 * 반환된 entry 는 store 가 소유하며, 다음 store_* 호출 전까지만 유효하다.
 */
entry_t *store_get(store_t *s, const char *k, size_t kl, long long now);
/* 삭제. 살아 있는 키를 지웠으면 1, 없거나 이미 만료였으면 0. */
int store_del(store_t *s, const char *k, size_t kl, long long now);

/* 주기 삭제: 버킷을 최대 budget 개 훑어 만료된 항목을 지운다. 지운 개수를 반환. */
size_t store_expire_cycle(store_t *s, long long now, size_t budget);

void store_clear(store_t *s);
size_t store_size(const store_t *s); /* 아직 지워지지 않은 만료 키를 포함한다 */
size_t store_bucket_count(const store_t *s);
void store_foreach_live(store_t *s, long long now, store_visit_fn fn, void *ud);

#endif
