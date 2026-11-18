#include "net/server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "commands/commands.h"
#include "resp/resp.h"
#include "store/store.h"

/*
 * 단일 스레드 이벤트 루프.
 *   select() 로 모든 소켓을 감시하다가 준비된 소켓만 처리한다. 명령은 한 번에 하나씩, 끝까지 실행된다.
 *   → 저장소를 만지는 스레드가 하나뿐이라 락이 필요 없고, 경쟁 조건이 생길 수 없다.
 */

#define MAX_CLIENTS 1000       /* select() 의 FD_SETSIZE(1024) 안에서 */
#define READ_CHUNK 16384
#define MAX_INBUF (RESP_MAX_BULK + 1024 * 1024) /* 명령 하나가 이보다 커질 수 없다 */
#define SHRINK_ABOVE (1024 * 1024)
#define CRON_INTERVAL_MS 100
#define EXPIRE_BUCKETS_PER_CRON 1000

typedef struct client {
    int fd;
    char *in;
    size_t in_len, in_cap;
    buf_t out;
    size_t out_pos; /* out 에서 이미 보낸 바이트 */
    int closing;    /* 응답을 다 보내고 닫는다 */
    struct client *next;
} client_t;

static volatile sig_atomic_t g_stop;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static size_t pending(const client_t *c) {
    return c->out.len - c->out_pos;
}

static void client_free(client_t *c) {
    close(c->fd);
    free(c->in);
    buf_free(&c->out);
    free(c);
}

static int set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int listen_on(const char *addr, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on); /* 재시작 때 TIME_WAIT 로 막히지 않게 */
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
        fprintf(stderr, "잘못된 bind 주소: %s\n", addr);
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(fd, 511) < 0 || set_nonblocking(fd) < 0) {
        fprintf(stderr, "%s:%d 에 열 수 없다: %s\n", addr, port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* 보낼 수 있는 만큼 보낸다. 연결에 문제가 있으면 -1. */
static int flush_client(client_t *c) {
    while (pending(c) > 0) {
        ssize_t n = send(c->fd, c->out.data + c->out_pos, pending(c), 0);
        if (n > 0) {
            c->out_pos += (size_t)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0; /* 소켓 버퍼가 가득: 쓸 수 있게 되면 select 가 알려 준다 */
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    c->out.len = 0;
    c->out_pos = 0;
    if (c->out.cap > SHRINK_ABOVE) buf_free(&c->out); /* 큰 응답 뒤에 메모리를 붙들고 있지 않는다 */
    return 0;
}

/* 입력 버퍼에서 완성된 명령을 모두(파이프라이닝) 꺼내 실행한다. */
static void process_input(client_t *c, store_t *s, aof_t *aof, long long now) {
    size_t pos = 0;
    while (pos < c->in_len && !c->closing) {
        resp_cmd cmd;
        size_t used;
        const char *err;
        int r = resp_parse(c->in + pos, c->in_len - pos, &cmd, &used, &err);
        if (r == 0) break; /* 명령이 아직 덜 왔다: 다음 recv 를 기다린다 */
        if (r < 0) {
            char msg[96];
            snprintf(msg, sizeof msg, "ERR Protocol error: %s", err);
            resp_error(&c->out, msg);
            c->closing = 1;
            break;
        }
        if (cmd.argc > 0 && cmd_execute(s, aof, &cmd, &c->out, now)) c->closing = 1;
        resp_cmd_free(&cmd);
        pos += used;
        if (c->out.failed) c->closing = 1;
    }
    if (pos > 0) {
        memmove(c->in, c->in + pos, c->in_len - pos);
        c->in_len -= pos;
    }
    if (c->in_len == 0 && c->in_cap > SHRINK_ABOVE) {
        free(c->in);
        c->in = NULL;
        c->in_cap = 0;
    }
}

/* 읽는다. 연결을 닫아야 하면 -1. */
static int read_client(client_t *c, store_t *s, aof_t *aof, long long now) {
    if (c->in_len >= MAX_INBUF) return -1;
    if (c->in_cap - c->in_len < READ_CHUNK) {
        size_t nc = c->in_cap ? c->in_cap * 2 : 4096;
        while (nc - c->in_len < READ_CHUNK) nc *= 2;
        char *p = realloc(c->in, nc);
        if (!p) return -1;
        c->in = p;
        c->in_cap = nc;
    }
    ssize_t n = recv(c->fd, c->in + c->in_len, c->in_cap - c->in_len, 0);
    if (n == 0) return -1; /* 상대가 닫았다 */
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
    c->in_len += (size_t)n;
    process_input(c, s, aof, now);
    return flush_client(c); /* 응답을 바로 보내 지연을 줄인다. 못 보낸 것은 select 가 이어 준다 */
}

int server_run(const server_config_t *cfg) {
    store_t *store = store_create();
    if (!store) {
        fprintf(stderr, "메모리가 부족하다\n");
        return 1;
    }

    aof_t *aof = NULL;
    if (cfg->aof_path) {
        size_t n = 0;
        if (aof_load(cfg->aof_path, store, &n) < 0) {
            fprintf(stderr, "AOF 파일이 깨져 있어 시작하지 않는다: %s\n", cfg->aof_path);
            store_destroy(store);
            return 1;
        }
        printf("AOF 에서 명령 %zu 개를 복구했다 (키 %zu 개)\n", n, store_size(store));
        aof = aof_open(cfg->aof_path, cfg->fsync_policy);
        if (!aof) {
            fprintf(stderr, "AOF 파일을 열 수 없다: %s\n", cfg->aof_path);
            store_destroy(store);
            return 1;
        }
    }

    int lfd = listen_on(cfg->bind_addr, cfg->port);
    if (lfd < 0) {
        aof_close(aof);
        store_destroy(store);
        return 1;
    }

    g_stop = 0;
    signal(SIGPIPE, SIG_IGN); /* 끊긴 소켓에 쓰면 프로세스가 죽는 대신 send 가 오류를 돌려준다 */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    printf("mini-redis 가 %s:%d 에서 기다린다\n", cfg->bind_addr, cfg->port);
    fflush(stdout);

    client_t *clients = NULL;
    int nclients = 0;
    long long last_cron = store_now_ms();

    while (!g_stop) {
        fd_set rd, wr;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        FD_SET(lfd, &rd);
        int maxfd = lfd;
        for (client_t *c = clients; c; c = c->next) {
            if (!c->closing) FD_SET(c->fd, &rd);
            if (pending(c) > 0) FD_SET(c->fd, &wr);
            if (c->fd > maxfd) maxfd = c->fd;
        }

        struct timeval tv = {0, CRON_INTERVAL_MS * 1000};
        int ready = select(maxfd + 1, &rd, &wr, NULL, &tv);
        if (ready < 0 && errno != EINTR) {
            fprintf(stderr, "select 실패: %s\n", strerror(errno));
            break;
        }
        long long now = store_now_ms();

        if (ready > 0) {
            if (FD_ISSET(lfd, &rd)) {
                for (int i = 0; i < 128; i++) {
                    int fd = accept(lfd, NULL, NULL);
                    if (fd < 0) break;
                    if (nclients >= MAX_CLIENTS || fd >= FD_SETSIZE || set_nonblocking(fd) < 0) {
                        static const char busy[] = "-ERR max number of clients reached\r\n";
                        if (send(fd, busy, sizeof busy - 1, 0) < 0) { /* 최선을 다할 뿐 */ }
                        close(fd);
                        continue;
                    }
                    int on = 1;
                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
                    client_t *c = calloc(1, sizeof *c);
                    if (!c) {
                        close(fd);
                        continue;
                    }
                    c->fd = fd;
                    buf_init(&c->out);
                    c->next = clients;
                    clients = c;
                    nclients++;
                }
            }

            client_t **pp = &clients;
            while (*pp) {
                client_t *c = *pp;
                int dead = 0;
                if (FD_ISSET(c->fd, &rd) && !c->closing && read_client(c, store, aof, now) < 0) dead = 1;
                if (!dead && FD_ISSET(c->fd, &wr) && flush_client(c) < 0) dead = 1;
                if (!dead && c->closing && pending(c) == 0) dead = 1; /* 오류/QUIT 응답까지 보냈으면 닫는다 */
                if (dead) {
                    *pp = c->next;
                    client_free(c);
                    nclients--;
                } else {
                    pp = &c->next;
                }
            }
        }

        if (now - last_cron >= CRON_INTERVAL_MS) {
            store_expire_cycle(store, now, EXPIRE_BUCKETS_PER_CRON); /* 주기 삭제 */
            aof_cron(aof, now);
            last_cron = now;
        }
    }

    printf("종료한다\n");
    while (clients) {
        client_t *next = clients->next;
        client_free(clients);
        clients = next;
    }
    close(lfd);
    aof_close(aof); /* 남은 기록을 디스크에 내린다 */
    store_destroy(store);
    return 0;
}
