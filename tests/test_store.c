#include "testutil.h"

#include <stdlib.h>

#include "store/store.h"

static int g_visited;
static void count_cb(const entry_t *e, void *ud) {
    (void)e;
    (void)ud;
    g_visited++;
}

static void test_set_get_overwrite_del(void) {
    store_t *s = store_create();
    CHECK(s != NULL);
    CHECK(store_get(s, "a", 1, 0) == NULL);

    CHECK(store_set(s, "a", 1, "1", 1, STORE_NO_EXPIRE, 0) == 0);
    entry_t *e = store_get(s, "a", 1, 0);
    CHECK(e != NULL && e->vlen == 1 && memcmp(e->val, "1", 1) == 0);
    CHECK(store_size(s) == 1);

    CHECK(store_set(s, "a", 1, "twenty", 6, STORE_NO_EXPIRE, 0) == 0);
    e = store_get(s, "a", 1, 0);
    CHECK(e != NULL && e->vlen == 6 && memcmp(e->val, "twenty", 6) == 0);
    CHECK(store_size(s) == 1); /* 덮어쓰기는 항목을 늘리지 않는다 */

    CHECK(store_del(s, "a", 1, 0) == 1);
    CHECK(store_del(s, "a", 1, 0) == 0);
    CHECK(store_get(s, "a", 1, 0) == NULL);
    CHECK(store_size(s) == 0);
    store_destroy(s);
}

static void test_binary_safe_and_empty(void) {
    store_t *s = store_create();
    /* NUL 이 들어 있어도 길이로 구분한다 */
    CHECK(store_set(s, "a\0b", 3, "x", 1, STORE_NO_EXPIRE, 0) == 0);
    CHECK(store_set(s, "a\0c", 3, "y", 1, STORE_NO_EXPIRE, 0) == 0);
    CHECK(store_size(s) == 2);
    entry_t *e = store_get(s, "a\0c", 3, 0);
    CHECK(e != NULL && e->val[0] == 'y');
    CHECK(store_get(s, "a", 1, 0) == NULL);

    /* 빈 키, 빈 값 */
    CHECK(store_set(s, "", 0, "", 0, STORE_NO_EXPIRE, 0) == 0);
    e = store_get(s, "", 0, 0);
    CHECK(e != NULL && e->vlen == 0 && e->val[0] == '\0');
    store_destroy(s);
}

static void test_resize_keeps_every_key(void) {
    store_t *s = store_create();
    size_t initial = store_bucket_count(s);
    char k[32], v[32];
    for (int i = 0; i < 20000; i++) {
        int kl = snprintf(k, sizeof k, "key:%d", i);
        int vl = snprintf(v, sizeof v, "val:%d", i * 7);
        CHECK(store_set(s, k, (size_t)kl, v, (size_t)vl, STORE_NO_EXPIRE, 0) == 0);
    }
    CHECK(store_size(s) == 20000);
    CHECK(store_bucket_count(s) > initial);
    CHECK((store_bucket_count(s) & (store_bucket_count(s) - 1)) == 0); /* 2의 거듭제곱 */

    int bad = 0;
    for (int i = 0; i < 20000; i++) {
        int kl = snprintf(k, sizeof k, "key:%d", i);
        int vl = snprintf(v, sizeof v, "val:%d", i * 7);
        entry_t *e = store_get(s, k, (size_t)kl, 0);
        if (!e || e->vlen != (size_t)vl || memcmp(e->val, v, (size_t)vl) != 0) bad++;
    }
    CHECK(bad == 0);

    /* 절반을 지워도 나머지는 그대로 */
    for (int i = 0; i < 20000; i += 2) {
        int kl = snprintf(k, sizeof k, "key:%d", i);
        CHECK(store_del(s, k, (size_t)kl, 0) == 1);
    }
    CHECK(store_size(s) == 10000);
    bad = 0;
    for (int i = 0; i < 20000; i++) {
        int kl = snprintf(k, sizeof k, "key:%d", i);
        entry_t *e = store_get(s, k, (size_t)kl, 0);
        if ((i % 2 == 0) != (e == NULL)) bad++;
    }
    CHECK(bad == 0);
    store_destroy(s);
}

static void test_lazy_expiry(void) {
    store_t *s = store_create();
    CHECK(store_set(s, "k", 1, "v", 1, 100, 0) == 0);
    CHECK(store_get(s, "k", 1, 99) != NULL);   /* 만료 직전까지는 보인다 */
    CHECK(store_get(s, "k", 1, 100) == NULL);  /* 만료 시각이 되면 없는 키 */
    CHECK(store_size(s) == 0);                 /* 조회하면서 지워졌다 */

    /* 만료된 키를 덮어쓰면 새 키처럼 취급: KEEP_TTL 을 줘도 옛 만료를 물려받지 않는다 */
    CHECK(store_set(s, "k", 1, "v", 1, 100, 0) == 0);
    CHECK(store_set(s, "k", 1, "w", 1, STORE_KEEP_TTL, 500) == 0);
    entry_t *e = store_get(s, "k", 1, 500);
    CHECK(e != NULL && e->expire_at == STORE_NO_EXPIRE);

    /* 만료된 키의 삭제는 "지운 것이 없음" */
    CHECK(store_set(s, "x", 1, "v", 1, 100, 0) == 0);
    CHECK(store_del(s, "x", 1, 200) == 0);
    store_destroy(s);
}

static void test_keep_ttl(void) {
    store_t *s = store_create();
    CHECK(store_set(s, "k", 1, "v", 1, 1000, 0) == 0);
    CHECK(store_set(s, "k", 1, "w", 1, STORE_KEEP_TTL, 0) == 0);
    entry_t *e = store_get(s, "k", 1, 0);
    CHECK(e != NULL && e->expire_at == 1000);
    CHECK(store_set(s, "k", 1, "z", 1, STORE_NO_EXPIRE, 0) == 0);
    e = store_get(s, "k", 1, 0);
    CHECK(e != NULL && e->expire_at == STORE_NO_EXPIRE);
    store_destroy(s);
}

static void test_active_expire_cycle(void) {
    store_t *s = store_create();
    char k[32];
    for (int i = 0; i < 100; i++) {
        int kl = snprintf(k, sizeof k, "ttl:%d", i);
        CHECK(store_set(s, k, (size_t)kl, "v", 1, 100, 0) == 0);
        kl = snprintf(k, sizeof k, "keep:%d", i);
        CHECK(store_set(s, k, (size_t)kl, "v", 1, STORE_NO_EXPIRE, 0) == 0);
    }
    CHECK(store_expire_cycle(s, 50, 1000000) == 0); /* 아직 아무도 만료 전 */
    CHECK(store_size(s) == 200);

    /* 예산이 작으면 한 번에 다 지우지 않는다 (이벤트 루프를 오래 막지 않기 위해) */
    size_t first = store_expire_cycle(s, 200, 1);
    CHECK(first < 100);

    /* 여러 번 돌리면 결국 만료 키만 모두 사라진다 */
    size_t total = first;
    for (size_t i = 0; i < store_bucket_count(s) * 2; i++) total += store_expire_cycle(s, 200, 1);
    CHECK(total == 100);
    CHECK(store_size(s) == 100);
    CHECK(store_get(s, "keep:5", 6, 200) != NULL);
    store_destroy(s);
}

static void test_clear_and_foreach(void) {
    store_t *s = store_create();
    CHECK(store_set(s, "a", 1, "1", 1, STORE_NO_EXPIRE, 0) == 0);
    CHECK(store_set(s, "b", 1, "2", 1, 100, 0) == 0);
    CHECK(store_set(s, "c", 1, "3", 1, STORE_NO_EXPIRE, 0) == 0);
    g_visited = 0;
    store_foreach_live(s, 50, count_cb, NULL);
    CHECK(g_visited == 3);
    g_visited = 0;
    store_foreach_live(s, 150, count_cb, NULL); /* 만료된 b 는 건너뛴다 */
    CHECK(g_visited == 2);

    store_clear(s);
    CHECK(store_size(s) == 0);
    CHECK(store_get(s, "a", 1, 0) == NULL);
    CHECK(store_set(s, "a", 1, "again", 5, STORE_NO_EXPIRE, 0) == 0); /* 비운 뒤에도 쓸 수 있다 */
    CHECK(store_size(s) == 1);
    store_destroy(s);
}

static void test_now_is_sane(void) {
    long long a = store_now_ms();
    long long b = store_now_ms();
    CHECK(a > 1700000000000LL); /* 2023 년 이후의 epoch ms */
    CHECK(b >= a);
}

int main(void) {
    test_set_get_overwrite_del();
    test_binary_safe_and_empty();
    test_resize_keeps_every_key();
    test_lazy_expiry();
    test_keep_ttl();
    test_active_expire_cycle();
    test_clear_and_foreach();
    test_now_is_sane();
    return t_report("test_store");
}
