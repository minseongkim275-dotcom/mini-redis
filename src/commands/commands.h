#ifndef COMMANDS_H
#define COMMANDS_H

#include "aof/aof.h"
#include "resp/resp.h"
#include "store/store.h"

/*
 * 명령 하나를 실행해 응답을 out 에 쓴다. 쓰기 명령이 성공하면 aof 에도 기록한다(aof 가 NULL 이면 생략).
 * now 는 호출자가 정해 주는 현재 시각(ms) — 테스트에서 시간을 흘려보내기 위해 주입한다.
 * 반환값 1 이면 응답을 보낸 뒤 연결을 닫아야 한다(QUIT).
 */
int cmd_execute(store_t *s, aof_t *aof, const resp_cmd *cmd, buf_t *out, long long now);

#endif
