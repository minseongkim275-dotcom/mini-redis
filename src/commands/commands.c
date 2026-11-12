#include "commands/commands.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 명령 처리. 모든 명령은 이벤트 루프 스레드 하나에서 한 번에 하나씩 끝까지 실행된다.
 * 그래서 "읽고-고치고-쓰기"(INCR 등)에 락이 필요 없고, 중간에 다른 명령이 끼어들지 못한다.
 */

typedef struct {
    store_t *s;
    aof_t *aof;
    long long now;
    buf_t *out;
    int close;
    char name[33]; /* 소문자로 바꾼 명령 이름 (오류 메시지용) */
} ctx_t;

typedef void (*handler_t)(ctx_t *c, int argc, const char **argv, const size_t *argl);

typedef struct {
    const char *name;
    int arity; /* 양수: 정확히 N 개 / 음수: 최소 -N 개 (명령 이름 포함) */
    handler_t fn;
} cmd_def;

#define ERR_NOT_INT "ERR value is not an integer or out of range"
#define ERR_SYNTAX "ERR syntax error"

static void err_fmt(ctx_t *c, const char *fmt, const char *name) {
    char msg[128];
    snprintf(msg, sizeof msg, fmt, name);
    resp_error(c->out, msg);
}

static int eq_ci(const char *a, size_t al, const char *lit) {
    size_t n = strlen(lit);
    if (al != n) return 0;
    for (size_t i = 0; i < n; i++) {
        if (toupper((unsigned char)a[i]) != lit[i]) return 0;
    }
    return 1;
}

static void log_cmd(ctx_t *c, int argc, const char **argv, const size_t *argl) {
    if (c->aof) aof_append(c->aof, argc, argv, argl);
}

/* ---- 연결/기본 ------------------------------------------------------------ */

static void cmd_ping(ctx_t *c, int argc, const char **argv, const size_t *argl) {
    if (argc > 2) return err_fmt(c, "ERR wrong number of arguments for '%s' command", c->name);
    if (argc == 2) resp_bulk(c->out, argv[1], argl[1]);
    else resp_simple(c->out, "PONG");
}

static void cmd_echo(ctx_t *c, int argc, const char **argv, const size_t *argl) {
    (void)argc;
    resp_bulk(c->out, argv[1], argl[1]);
}

static void cmd_quit(ctx_t *c, int argc, const char **argv, const size_t *argl) {
    (void)argc;
    (void)argv;
    (void)argl;
    resp_simple(c->out, "OK");
    c->close = 1;
}

static void cmd_command(ctx_t *c, int argc, const char **argv, const size_t *argl) {
    (void)argc;
    (void)argv;
    (void)argl;
    resp_array_header(c->out, 0); /* redis-cli 가 접속하면서 물어보는 명령. 빈 목록으로 답한다. */
}

/* ---- SET / GET / DEL / EXISTS ---------------------------------------------- */

static void cmd_get(ctx_t *c, int argc, const char **argv, const size_t *argl) {
    (void)argc;
    entry_t *e = store_get(c->s, argv[1], argl[1], c->now);
    if (e) resp_bulk(c->out, e->val, e->vlen);
    else resp_null(c->out);
}

static void cmd_set(ctx_t *c, int argc, const char **argv, const size_t *argl) {
    int nx = 0, xx = 0, keepttl = 0;
    int kind = 0; /* 0 없음, 1 EX, 2 PX, 3 EXAT, 4 PXAT */
    long long ev = 0;

    for (int i = 3; i < argc; i++) {
        if (eq_ci(argv[i], argl[i], "NX") && !xx) nx = 1;
        else if (eq_ci(argv[i], argl[i], "XX") && !nx) xx = 1;
        else if (eq_ci(argv[i], argl[i], "KEEPTTL") && !kind) keepttl = 1;
        else if (!kind && !keepttl && i + 1 < argc &&
                 (eq_ci(argv[i], argl[i], "EX") || eq_ci(argv[i], argl[i], "PX") ||
                  eq_ci(argv[i], argl[i], "EXAT") || eq_ci(argv[i], argl[i], "PXAT"))) {
            kind = eq_ci(argv[i], argl[i], "EX") ? 1 : eq_ci(argv[i], argl[i], "PX") ? 2
                   : eq_ci(argv[i], argl[i], "EXAT") ? 3 : 4;
            if (!resp_parse_ll(argv[i + 1], argl[i + 1], &ev)) return resp_error(c->out, ERR_NOT_INT);
            i++;
        } else {
            return resp_error(c->out, ERR_SYNTAX);
        }
    }

    long long abs_at = STORE_NO_EXPIRE; /* 절대 만료 시각(ms) */
    if (kind) {
        if (ev <= 0) return err_fmt(c, "ERR invalid expire time in '%s' command", c->name);
        int bad = 0;
        if (kind == 1 || kind == 3) { /* 초 → ms: 곱셈 오버플로 */
            if (ev > LLONG_MAX / 1000) bad = 1;
            else abs_at = ev * 1000;
        } else {
            abs_at = ev;
        }
        if (!bad && (kind == 1 || kind == 2)) { /* 상대 시간 → 절대 시각: 덧셈 오버플로 */
            if (abs_at > LLONG_MAX - c->now) bad = 1;
            else abs_at += c->now;
        }
        if (bad) return err_fmt(c, "ERR invalid expire time in '%s' command", c->name);
    }

    if (nx || xx) {
        int exists = store_get(c->s, argv[1], argl[1], c->now) != NULL;
        if ((nx && exists) || (xx && !exists)) return resp_null(c->out);
    }
    long long stored = kind ? abs_at : (keepttl ? STORE_KEEP_TTL : STORE_NO_EXPIRE);
    if (store_set(c->s, argv[1], argl[1], argv[2], argl[2], stored, c->now) < 0)
        return resp_error(c->out, "ERR out of memory");
    resp_simple(c->out, "OK");

    /* AOF 에는 재시작 후에도 같은 결과가 나오는 형태로 남긴다: 상대 시간을 절대 시각으로 바꾸고,
       조건부(NX/XX)는 이미 통과했으니 뗀다. */
    const char *la[5] = {"SET", argv[1], argv[2], NULL, NULL};
    size_t ll[5] = {3, argl[1], argl[2], 0, 0};
    int n = 3;
    char num[32];
    if (kind) {
        snprintf(num, sizeof num, "%lld", abs_at);
        la[3] = "PXAT";
        ll[3] = 4;
        la[4] = num;
        ll[4] = strlen(num);
        n = 5;
    } else if (keepttl) {
        la[3] = "KEEPTTL";
        ll[3] = 7;
        n = 4;
    }
    log_cmd(c, n, la, ll);
}

static void cmd_del(ctx_t *c, int argc, const char **argv, const size_t *argl) {
    long long n = 0;
    for (int i = 1; i < argc; i++) n += store_del(c->s, argv[i], argl[i], c->now);
    resp_int(c->out, n);
    if (n > 0) log_cmd(c, argc, argv, argl);
}

static void cmd_exists(ctx_t *c, int argc, const char **argv, const size_t *argl) {
    long long n = 0;
    for (int i = 1; i < argc; i++) n += store_get(c->s, argv[i], argl[i], c->now) != NULL;
    resp_int(c->out, n);
}

/* ---- 만료 ----------------------------------------------------------------- */

/* EXPIRE / PEXPIRE / PEXPIREAT. mult: 단위(초면 1000), absolute: 절대 시각인가 */
static void expire_generic(ctx_t *c, const char **argv, const size_t *argl, long long mult, int absolute) {
    long long v;
    if (!resp_parse_ll(argv[2], argl[2], &v)) return resp_error(c->out, ERR_NOT_INT);
    if ((v > 0 && v > LLONG_MAX / mult) || (v < 0 && v < LLONG_MIN / mult))
        return err_fmt(c, "ERR invalid expire time in '%s' command", c->name);
    long long at = v * mult;
    if (!absolute) {
        if (at > 0 && at > LLONG_MAX - c->now)
            return err_fmt(c, "ERR invalid expire time in '%s' command", c->name);
        at += c->now;
    }

    entry_t *e = store_get(c->s, argv[1], argl[1], c->now);
    if (!e) return resp_int(c->out, 0);

    if (at <= c->now) { /* 이미 지난 시각: 키를 바로 지운다 */
        store_del(c->s, argv[1], argl[1], c->now);
        resp_int(c->out, 1);
        const char *la[2] = {"DEL", argv[1]};
        size_t ll[2] = {3, argl[1]};
        log_cmd(c, 2, la, ll);
        return;
    }
    e->expire_at = at;
    resp_int(c->out, 1);
    char num[32];
    snprintf(num, sizeof num, "%lld", at);
    const char *la[3] = {"PEXPIREAT", argv[1], num}; /* 상대 → 절대 */
    size_t ll[3] = {9, argl[1], strlen(num)};
    log_cmd(c, 3, la, ll);
}

static void cmd_expire(ctx_t *c, int argc, const char **argv, const size_t *argl) {
    (void)argc;
    expire_generic(c, argv, argl, 1000, 0);
}
static void cmd_pexpire(ctx_t *c, int argc, const char **argv, const size_t *argl) {
    (void)argc;
    expire_generic(c, argv, argl, 1, 0);
}
static void cmd_pexpireat(ctx_t *c, int argc, const char **argv, const size_t *argl) {
    (void)argc;
    expire_generic(c, argv, argl, 1, 1);
}

static void ttl_generic(ctx_t *c, const char **argv, const size_t *argl, int ms) {
    entry_t *e = store_get(c->s, argv[1], argl[1], c->now);
    if (!e) return resp_int(c->out, -2);
    if (e->expire_at < 0) return resp_int(c->out, -1);
    long long left = e->expire_at - c->now;
    if (left < 0) left = 0;
    resp_int(c->out, ms ? left : (left + 500) / 1000);
}
static void cmd_ttl(ctx_t *c, int argc, const char **argv, const size_t *argl) {
    (void)argc;
    ttl_generic(c, argv, argl, 0);
}
static void cmd_pttl(ctx_t *c, int argc, const char **argv, const size_t *argl) {
    (void)argc;
    ttl_generic(c, argv, argl, 1);
}

static void cmd_persist(ctx_t *c, int argc, const char **argv, const size_t *argl) {
    entry_t *e = store_get(c->s, argv[1], argl[1], c->now);
    if (!e || e->expire_at < 0) return resp_int(c->out, 0);
    e->expire_at = STORE_NO_EXPIRE;
    resp_int(c->out, 1);
    log_cmd(c, argc, argv, argl);
}

/* ---- 카운터 --------------------------------------------------------------- */

static void incr_generic(ctx_t *c, int argc, const char **argv, const size_t *argl, long long delta) {
    entry_t *e = store_get(c->s, argv[1], argl[1], c->now);
    long long cur = 0;
    if (e && !resp_parse_ll(e->val, e->vlen, &cur)) return resp_error(c->out, ERR_NOT_INT);
    if ((delta > 0 && cur > LLONG_MAX - delta) || (delta < 0 && cur < LLONG_MIN - delta))
        return resp_error(c->out, "ERR increment or decrement would overflow");
    cur += delta;
    char num[32];
    int n = snprintf(num, sizeof num, "%lld", cur);
    if (store_set(c->s, argv[1], argl[1], num, (size_t)n, STORE_KEEP_TTL, c->now) < 0)
        return resp_error(c->out, "ERR out of memory");
    resp_int(c->out, cur);
    log_cmd(c, argc, argv, argl); /* 같은 명령을 다시 실행하면 같은 값이 되므로 원문 그대로 남긴다 */
}

static void cmd_incr(ctx_t *c, int argc, const char **argv, const size_t *argl) {
    incr_generic(c, argc, argv, argl, 1);
}
static void cmd_decr(ctx_t *c, int argc, const char **argv, const size_t *argl) {
    incr_generic(c, argc, argv, argl, -1);
}
static void cmd_incrby(ctx_t *c, int argc, const char **argv, const size_t *argl) {
    long long d;
    if (!resp_parse_ll(argv[2], argl[2], &d)) return resp_error(c->out, ERR_NOT_INT);
    incr_generic(c, argc, argv, argl, d);
}
static void cmd_decrby(ctx_t *c, int argc, const char **argv, const size_t *argl) {
    long long d;
    if (!resp_parse_ll(argv[2], argl[2], &d)) return resp_error(c->out, ERR_NOT_INT);
    if (d == LLONG_MIN) return resp_error(c->out, "ERR decrement would overflow");
    incr_generic(c, argc, argv, argl, -d);
}

/* ---- KEYS / DBSIZE / FLUSHALL ---------------------------------------------- */

/* glob: * ? [abc] [a-z] [^a] \x. '*' 는 되짚기를 한 지점만 기억하므로 병적인 패턴에서도 O(n*m). */
static int elem_match(const char *p, size_t pl, size_t pi, char ch, size_t *next) {
    if (p[pi] == '?') {
        *next = pi + 1;
        return 1;
    }
    if (p[pi] == '\\' && pi + 1 < pl) {
        *next = pi + 2;
        return p[pi + 1] == ch;
    }
    if (p[pi] == '[') {
        size_t i = pi + 1;
        int neg = 0, hit = 0;
        if (i < pl && p[i] == '^') {
            neg = 1;
            i++;
        }
        while (i < pl && p[i] != ']') {
            if (p[i] == '\\' && i + 1 < pl) {
                i++;
                if (p[i] == ch) hit = 1;
            } else if (i + 2 < pl && p[i + 1] == '-' && p[i + 2] != ']') {
                char lo = p[i], hi = p[i + 2];
                if (lo > hi) {
                    char t = lo;
                    lo = hi;
                    hi = t;
                }
                if (ch >= lo && ch <= hi) hit = 1;
                i += 2;
            } else if (p[i] == ch) {
                hit = 1;
            }
            i++;
        }
        *next = i < pl ? i + 1 : i; /* 닫는 ']' 가 없으면 끝까지가 한 덩어리 */
        return neg ? !hit : hit;
    }
    *next = pi + 1;
    return p[pi] == ch;
}

static int glob_match(const char *p, size_t pl, const char *s, size_t sl) {
    size_t pi = 0, si = 0, star_pi = (size_t)-1, star_si = 0;
    while (si < sl) {
        if (pi < pl && p[pi] == '*') {
            star_pi = pi++;
            star_si = si;
            continue;
        }
        size_t nx;
        if (pi < pl && elem_match(p, pl, pi, s[si], &nx)) {
            pi = nx;
            si++;
            continue;
        }
        if (star_pi != (size_t)-1) { /* 직전 '*' 가 글자 하나를 더 먹게 한다 */
            pi = star_pi + 1;
            si = ++star_si;
            continue;
        }
        return 0;
    }
    while (pi < pl && p[pi] == '*') pi++;
    return pi == pl;
}

typedef struct {
    const char *pat;
    size_t patl;
    buf_t *out;
    long long count;
} keys_ud;

static void keys_count_cb(const entry_t *e, void *ud) {
    keys_ud *k = ud;
    if (glob_match(k->pat, k->patl, e->key, e->klen)) k->count++;
}
static void keys_emit_cb(const entry_t *e, void *ud) {
    keys_ud *k = ud;
    if (glob_match(k->pat, k->patl, e->key, e->klen)) resp_bulk(k->out, e->key, e->klen);
}

static void cmd_keys(ctx_t *c, int argc, const char **argv, const size_t *argl) {
    (void)argc;
    keys_ud ud = {argv[1], argl[1], c->out, 0};
    store_foreach_live(c->s, c->now, keys_count_cb, &ud);
    resp_array_header(c->out, ud.count);
    store_foreach_live(c->s, c->now, keys_emit_cb, &ud);
}

static void cmd_dbsize(ctx_t *c, int argc, const char **argv, const size_t *argl) {
    (void)argc;
    (void)argv;
    (void)argl;
    resp_int(c->out, (long long)store_size(c->s));
}

static void cmd_flushall(ctx_t *c, int argc, const char **argv, const size_t *argl) {
    (void)argc;
    (void)argv;
    (void)argl;
    store_clear(c->s);
    resp_simple(c->out, "OK");
    const char *la[1] = {"FLUSHALL"};
    size_t ll[1] = {8};
    log_cmd(c, 1, la, ll);
}

/* ---- 디스패치 -------------------------------------------------------------- */

static const cmd_def TABLE[] = {
    {"ping", -1, cmd_ping},         {"echo", 2, cmd_echo},       {"quit", -1, cmd_quit},
    {"command", -1, cmd_command},   {"get", 2, cmd_get},         {"set", -3, cmd_set},
    {"del", -2, cmd_del},           {"exists", -2, cmd_exists},  {"expire", 3, cmd_expire},
    {"pexpire", 3, cmd_pexpire},    {"pexpireat", 3, cmd_pexpireat}, {"ttl", 2, cmd_ttl},
    {"pttl", 2, cmd_pttl},          {"persist", 2, cmd_persist}, {"incr", 2, cmd_incr},
    {"decr", 2, cmd_decr},          {"incrby", 3, cmd_incrby},   {"decrby", 3, cmd_decrby},
    {"keys", 2, cmd_keys},          {"dbsize", 1, cmd_dbsize},   {"flushall", -1, cmd_flushall},
    {"flushdb", -1, cmd_flushall},
};

int cmd_execute(store_t *s, aof_t *aof, const resp_cmd *cmd, buf_t *out, long long now) {
    if (cmd->argc == 0) return 0;
    ctx_t c = {s, aof, now, out, 0, {0}};

    /* 이름을 소문자로 (32 자까지). 더 길면 어차피 모르는 명령이다. */
    const cmd_def *def = NULL;
    if (cmd->argl[0] <= 32) {
        for (size_t i = 0; i < cmd->argl[0]; i++) c.name[i] = (char)tolower((unsigned char)cmd->argv[0][i]);
        for (size_t i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++) {
            if (strcmp(TABLE[i].name, c.name) == 0) {
                def = &TABLE[i];
                break;
            }
        }
    }
    if (!def) {
        /* 사용자 입력을 응답에 싣기 전에 제어 문자를 지운다: 줄바꿈이 섞이면 응답 형식이 깨진다 */
        char shown[48];
        size_t n = cmd->argl[0] < sizeof shown - 1 ? cmd->argl[0] : sizeof shown - 1;
        for (size_t i = 0; i < n; i++) {
            unsigned char ch = (unsigned char)cmd->argv[0][i];
            shown[i] = (ch < 32 || ch == 127) ? ' ' : (char)ch;
        }
        shown[n] = '\0';
        err_fmt(&c, "ERR unknown command '%s'", shown);
        return 0;
    }

    int argc = cmd->argc;
    if ((def->arity > 0 && argc != def->arity) || (def->arity < 0 && argc < -def->arity)) {
        err_fmt(&c, "ERR wrong number of arguments for '%s' command", def->name);
        return 0;
    }
    def->fn(&c, argc, cmd->argv, cmd->argl);
    return c.close;
}
