#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net/server.h"
#include "resp/resp.h"

static void usage(FILE *out) {
    fprintf(out,
            "사용법: mini-redis [옵션]\n"
            "  --port <N>              기다릴 포트 (기본 6379)\n"
            "  --bind <주소>           기다릴 IPv4 주소 (기본 127.0.0.1)\n"
            "  --aof <파일>            쓰기 명령을 이 파일에 기록하고, 시작할 때 복구한다\n"
            "  --appendfsync <정책>    always | everysec(기본) | no\n"
            "  --help\n");
}

int main(int argc, char **argv) {
    server_config_t cfg = {"127.0.0.1", 6379, NULL, AOF_FSYNC_EVERYSEC};

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0) {
            usage(stdout);
            return 0;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "%s 에 값이 없다\n", a);
            usage(stderr);
            return 2;
        }
        const char *v = argv[++i];
        if (strcmp(a, "--port") == 0) {
            long long p;
            if (!resp_parse_ll(v, strlen(v), &p) || p < 1 || p > 65535) {
                fprintf(stderr, "잘못된 포트: %s (1~65535)\n", v);
                return 2;
            }
            cfg.port = (int)p;
        } else if (strcmp(a, "--bind") == 0) {
            cfg.bind_addr = v;
        } else if (strcmp(a, "--aof") == 0) {
            cfg.aof_path = v;
        } else if (strcmp(a, "--appendfsync") == 0) {
            if (strcmp(v, "always") == 0) cfg.fsync_policy = AOF_FSYNC_ALWAYS;
            else if (strcmp(v, "everysec") == 0) cfg.fsync_policy = AOF_FSYNC_EVERYSEC;
            else if (strcmp(v, "no") == 0) cfg.fsync_policy = AOF_FSYNC_NO;
            else {
                fprintf(stderr, "잘못된 appendfsync: %s (always | everysec | no)\n", v);
                return 2;
            }
        } else {
            fprintf(stderr, "알 수 없는 옵션: %s\n", a);
            usage(stderr);
            return 2;
        }
    }
    return server_run(&cfg);
}
