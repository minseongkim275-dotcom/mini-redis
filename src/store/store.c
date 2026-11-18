#include "store/store.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * 해시 테이블 (체이닝)
 *  - 키를 FNV-1a 로 64비트 해시 → 버킷 수가 2의 거듭제곱이라 `hash & (size-1)` 로 버킷을 고른다.
 *  - 같은 버킷에 떨어진 항목은 연결 리스트로 잇는다.
 *  - 항목 수가 버킷 수를 넘으면(적재율 > 1) 버킷을 2배로 늘려 전부 다시 배치한다.
 *  - 만료 시각은 항목마다 절대 시각(ms)으로 저장한다. 시간이 흘러도 갱신할 필요가 없다.
 */

#define INITIAL_BUCKETS 16
#define MAX_BUCKETS ((size_t)1 << 30)

struct store {
    entry_t **buckets;
    size_t size;   /* 버킷 수 (항상 2의 거듭제곱) */
    size_t count;  /* 항목 수 */
    size_t cursor; /* 주기 삭제가 다음에 훑을 버킷 */
};

long long store_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static unsigned long long hash_bytes(const char *p, size_t n) {
    unsigned long long h = 14695981039346656037ULL; /* FNV-1a 64비트 */
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* 길이로 복사하고 끝에 NUL 을 하나 더 붙인다 (빈 값도 항상 유효한 포인터가 되도록) */
static char *dup_bytes(const char *p, size_t n) {
    char *d = malloc(n + 1);
    if (!d) return NULL;
    if (n) memcpy(d, p, n);
    d[n] = '\0';
    return d;
}

static int is_expired(const entry_t *e, long long now) {
    return e->expire_at >= 0 && e->expire_at <= now;
}

static void free_entry(entry_t *e) {
    free(e->key);
    free(e->val);
    free(e);
}

store_t *store_create(void) {
    store_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->buckets = calloc(INITIAL_BUCKETS, sizeof *s->buckets);
    if (!s->buckets) {
        free(s);
        return NULL;
    }
    s->size = INITIAL_BUCKETS;
    return s;
}

static void free_all_entries(store_t *s) {
    for (size_t i = 0; i < s->size; i++) {
        entry_t *e = s->buckets[i];
        while (e) {
            entry_t *next = e->next;
            free_entry(e);
            e = next;
        }
        s->buckets[i] = NULL;
    }
    s->count = 0;
}

void store_destroy(store_t *s) {
    if (!s) return;
    free_all_entries(s);
    free(s->buckets);
    free(s);
}

/* 키를 찾는다. 찾으면 그 항목을 가리키는 링크(이전 항목의 next 또는 버킷 머리)를 *link 로 돌려준다. */
static entry_t *find(store_t *s, const char *k, size_t kl, unsigned long long h, entry_t ***link) {
    entry_t **pp = &s->buckets[h & (s->size - 1)];
    while (*pp) {
        entry_t *e = *pp;
        if (e->hash == h && e->klen == kl && (kl == 0 || memcmp(e->key, k, kl) == 0)) {
            *link = pp;
            return e;
        }
        pp = &e->next;
    }
    *link = NULL;
    return NULL;
}

static void unlink_and_free(store_t *s, entry_t **link) {
    entry_t *e = *link;
    *link = e->next;
    free_entry(e);
    s->count--;
}

/* 버킷 수를 늘려 모든 항목을 새 위치로 옮긴다. 메모리가 모자라면 그대로 둔다(느려질 뿐 틀리진 않는다). */
static void resize(store_t *s, size_t new_size) {
    entry_t **nb = calloc(new_size, sizeof *nb);
    if (!nb) return;
    for (size_t i = 0; i < s->size; i++) {
        entry_t *e = s->buckets[i];
        while (e) {
            entry_t *next = e->next;
            size_t idx = e->hash & (new_size - 1);
            e->next = nb[idx];
            nb[idx] = e;
            e = next;
        }
    }
    free(s->buckets);
    s->buckets = nb;
    s->size = new_size;
    s->cursor = 0;
}

int store_set(store_t *s, const char *k, size_t kl, const char *v, size_t vl,
              long long expire_at, long long now) {
    unsigned long long h = hash_bytes(k, kl);
    entry_t **link;
    entry_t *e = find(s, k, kl, h, &link);

    if (e && is_expired(e, now)) { /* 만료된 키는 없는 키다: 옛 값과 옛 만료를 물려받지 않는다 */
        unlink_and_free(s, link);
        e = NULL;
    }

    if (e) { /* 덮어쓰기 */
        char *nv = dup_bytes(v, vl);
        if (!nv) return -1;
        free(e->val);
        e->val = nv;
        e->vlen = vl;
        if (expire_at != STORE_KEEP_TTL) e->expire_at = expire_at;
        return 0;
    }

    e = calloc(1, sizeof *e);
    if (!e) return -1;
    e->key = dup_bytes(k, kl);
    e->val = dup_bytes(v, vl);
    if (!e->key || !e->val) {
        free_entry(e);
        return -1;
    }
    e->klen = kl;
    e->vlen = vl;
    e->hash = h;
    e->expire_at = (expire_at == STORE_KEEP_TTL) ? STORE_NO_EXPIRE : expire_at;
    size_t idx = h & (s->size - 1);
    e->next = s->buckets[idx];
    s->buckets[idx] = e;
    s->count++;

    if (s->count > s->size && s->size < MAX_BUCKETS) resize(s, s->size * 2);
    return 0;
}

entry_t *store_get(store_t *s, const char *k, size_t kl, long long now) {
    entry_t **link;
    entry_t *e = find(s, k, kl, hash_bytes(k, kl), &link);
    if (!e) return NULL;
    if (is_expired(e, now)) { /* 지연 삭제: 만료된 값은 절대 돌려주지 않는다 */
        unlink_and_free(s, link);
        return NULL;
    }
    return e;
}

int store_del(store_t *s, const char *k, size_t kl, long long now) {
    entry_t **link;
    entry_t *e = find(s, k, kl, hash_bytes(k, kl), &link);
    if (!e) return 0;
    int live = !is_expired(e, now);
    unlink_and_free(s, link);
    return live;
}

size_t store_expire_cycle(store_t *s, long long now, size_t budget) {
    size_t removed = 0;
    size_t n = budget < s->size ? budget : s->size;
    for (size_t i = 0; i < n; i++) {
        entry_t **pp = &s->buckets[s->cursor];
        while (*pp) {
            if (is_expired(*pp, now)) {
                unlink_and_free(s, pp);
                removed++;
            } else {
                pp = &(*pp)->next;
            }
        }
        s->cursor = (s->cursor + 1) & (s->size - 1);
    }
    return removed;
}

void store_clear(store_t *s) {
    free_all_entries(s);
    if (s->size > INITIAL_BUCKETS) { /* 커졌던 테이블은 처음 크기로 돌려 메모리를 반납한다 */
        entry_t **nb = calloc(INITIAL_BUCKETS, sizeof *nb);
        if (nb) {
            free(s->buckets);
            s->buckets = nb;
            s->size = INITIAL_BUCKETS;
        }
    }
    s->cursor = 0;
}

size_t store_size(const store_t *s) {
    return s->count;
}

size_t store_bucket_count(const store_t *s) {
    return s->size;
}

void store_foreach_live(store_t *s, long long now, store_visit_fn fn, void *ud) {
    for (size_t i = 0; i < s->size; i++) {
        for (entry_t *e = s->buckets[i]; e; e = e->next) {
            if (!is_expired(e, now)) fn(e, ud);
        }
    }
}
