#include "aof/aof.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "commands/commands.h"
#include "resp/resp.h"

/*
 * AOF(Append Only File): 쓰기 명령을 RESP 형식 그대로 파일 끝에 덧붙인다.
 * 서버를 시작할 때 파일을 처음부터 다시 실행하면 죽기 직전의 상태가 복원된다.
 * 명령이 성공한 뒤에 기록하므로, 실패한 명령은 파일에 없다.
 */

struct aof {
    FILE *f;
    aof_policy_t policy;
    long long last_sync;
    int dirty;     /* 마지막 fsync 이후 쓴 것이 있는가 */
    int warned;    /* 쓰기 오류를 이미 알렸는가 */
};

aof_t *aof_open(const char *path, aof_policy_t policy) {
    FILE *f = fopen(path, "ab");
    if (!f) return NULL;
    aof_t *a = calloc(1, sizeof *a);
    if (!a) {
        fclose(f);
        return NULL;
    }
    a->f = f;
    a->policy = policy;
    a->last_sync = store_now_ms();
    return a;
}

static void warn_once(aof_t *a, const char *what) {
    if (a->warned) return;
    a->warned = 1;
    fprintf(stderr, "AOF: %s 실패: %s (이후 쓰기는 디스크에 남지 않을 수 있다)\n", what, strerror(errno));
}

static void sync_now(aof_t *a, long long now) {
    if (fflush(a->f) != 0) warn_once(a, "fflush");
    if (fsync(fileno(a->f)) != 0) warn_once(a, "fsync");
    a->dirty = 0;
    a->last_sync = now;
}

void aof_append(aof_t *a, int argc, const char **argv, const size_t *argl) {
    if (!a) return;
    int ok = fprintf(a->f, "*%d\r\n", argc) > 0;
    for (int i = 0; i < argc && ok; i++) {
        ok = fprintf(a->f, "$%zu\r\n", argl[i]) > 0;
        if (ok && argl[i]) ok = fwrite(argv[i], 1, argl[i], a->f) == argl[i];
        if (ok) ok = fwrite("\r\n", 1, 2, a->f) == 2;
    }
    /* 프로세스가 죽어도 OS 가 가진 데이터는 남도록 매번 커널로 넘기고, 디스크 강제 기록은 정책에 따른다 */
    if (!ok || fflush(a->f) != 0) warn_once(a, "write");
    a->dirty = 1;
    if (a->policy == AOF_FSYNC_ALWAYS) sync_now(a, store_now_ms());
}

void aof_cron(aof_t *a, long long now) {
    if (!a || a->policy != AOF_FSYNC_EVERYSEC || !a->dirty) return;
    if (now - a->last_sync >= 1000) sync_now(a, now);
}

void aof_close(aof_t *a) {
    if (!a) return;
    if (a->policy != AOF_FSYNC_NO) sync_now(a, store_now_ms());
    fclose(a->f);
    free(a);
}

int aof_load(const char *path, store_t *s, size_t *replayed) {
    *replayed = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return errno == ENOENT ? 0 : -1;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fclose(f);
        return -1;
    }
    char *data = malloc((size_t)size + 1);
    if (!data) {
        fclose(f);
        return -1;
    }
    size_t got = fread(data, 1, (size_t)size, f);
    fclose(f);

    size_t pos = 0;
    int rc = 0;
    buf_t sink; /* 복구 중에는 응답을 버린다 */
    buf_init(&sink);
    while (pos < got) {
        if (data[pos] != '*') { /* 인라인 형식은 AOF 에 없다: 깨진 파일 */
            rc = -1;
            break;
        }
        resp_cmd cmd;
        size_t used;
        const char *err;
        int r = resp_parse(data + pos, got - pos, &cmd, &used, &err);
        if (r == 0) break; /* 끝이 잘렸다 (쓰다가 죽은 흔적) */
        if (r < 0) {
            rc = -1;
            break;
        }
        if (cmd.argc > 0) {
            sink.len = 0;
            cmd_execute(s, NULL, &cmd, &sink, store_now_ms()); /* aof=NULL: 복구하면서 다시 기록하지 않는다 */
            (*replayed)++;
        }
        resp_cmd_free(&cmd);
        pos += used;
    }
    buf_free(&sink);
    free(data);

    if (rc == 0 && pos < got) {
        /* 잘린 꼬리를 파일에서도 지운다. 안 그러면 이어 쓰는 기록이 깨진 조각 뒤에 붙어 다음 시작에 파일 전체가 깨진다. */
        fprintf(stderr, "AOF: 끝이 잘린 명령 %zu 바이트를 버리고 복구한다\n", got - pos);
        if (truncate(path, (off_t)pos) != 0) rc = -1;
    }
    return rc;
}
