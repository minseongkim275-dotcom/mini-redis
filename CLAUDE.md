# mini-redis

크래프톤 정글 13기 팀 프로젝트. C로 구현하는 미니 Redis (TCP 서버 + RESP + 인메모리 저장소).

## 빌드와 테스트
- 환경: Linux 또는 WSL(`Ubuntu-22.04`)의 gcc·make. Windows의 gcc(MinGW)로는 빌드하지 않는다.
- 전체 검사: `bash .claude/check.sh` (작업을 마치기 전에 반드시 통과해야 함. Stop 훅과 CI가 같은 파일을 실행함. 경고는 오류로 다룸)
- WSL 안에서 직접: `make`, `make test`
- Makefile은 `CFLAGS`를 `?=`와 `+=`로 받는다. `check.sh`가 `CFLAGS`로 `-Werror`를 넘기기 때문이다.
- 서버 실행: `./mini-redis --port 6380`, 확인: `redis-cli -p 6380 PING`
- 캐시 사용/미사용 성능 비교: `make bench` (미니 Redis와 데모 API를 띄워 직접 잰다)
- `make test`는 C 단위 테스트 → 서버를 띄우는 기능 테스트(`tests/functional_test.py`) → 데모 API 테스트(`tests/demo_api_test.py`) 순서로 돈다. 테스트 서버는 빈 포트를 골라 쓰고 끝나면 정리한다.

## 문서
- 과제 요구사항 원문: `docs/requirements.md` (대화에 다시 붙여넣지 않는다)
- 고치지 않기로 한 지적: `docs/known-limits.md` (리뷰의 "선택" 항목을 한 줄씩 기록. 없으면 처음 기록할 때 만든다)

## 디렉터리
- `src/net/`: 소켓, 단일 스레드 이벤트 루프(select)
- `src/resp/`: RESP 파서·직렬화
- `src/store/`: 해시 테이블(체이닝), 만료(expire)
- `src/commands/`: 명령 표와 처리 (SET, GET, EXPIRE, INCR, KEYS …)
- `src/aof/`: 쓰기 명령 기록(AOF)과 시작 시 복구
- `tests/`: C 단위 테스트(`test_*.c`)와 Python 기능 테스트(`*_test.py`)
- `demo/`: 미니 Redis에 응답을 캐시하는 데모 HTTP API, 파이썬 클라이언트, 성능 비교

## 도메인 규칙
규칙마다 그것을 검사하는 테스트를 적는다. "검사 없음"은 리뷰어가 직접 봐야 하는 항목이고, 테스트로 옮길 후보다.

| 규칙 | 검사하는 곳 |
|---|---|
| 응답은 RESP 형식이고 실제 Redis와 같다 | `tests/test_commands.c`(응답 원문 비교), `tests/test_resp.c`. 실제 `redis-cli` 접속은 검사 없음(이 환경에 `redis-cli`가 없어 확인하지 못함) |
| 값의 메모리는 저장소(`src/store/`)가 소유한다. 넘겨받은 쪽이 해제하는 경우는 함수 주석에 적는다 | 검사 없음 (ASan/UBSan 빌드로 `make test`를 돌려 수동 확인) |
| 외부 라이브러리를 쓰지 않는다 (C 표준 라이브러리와 POSIX API만) | 검사 없음 (Makefile이 링크하는 것이 없음) |
| 만료된 키는 조회·덮어쓰기·삭제·카운터에서 모두 "없는 키"다 | `tests/test_store.c`(`test_lazy_expiry`), `tests/test_commands.c`, `tests/functional_test.py`(`test_lazy_expiry`) |
| 명령은 이벤트 루프 스레드 하나에서만 실행한다 (저장소에 락이 없다) | `tests/functional_test.py`(`test_incr_is_atomic_across_clients`) |
| AOF에는 상대 시간(EX, EXPIRE)이 아니라 절대 시각을 기록한다 | `tests/test_commands.c`(`test_aof_replay`), `tests/functional_test.py`(`test_data_survives_kill_9`) |
| 클라이언트가 선언한 길이(배열 개수, bulk 길이)로 검사 없이 메모리를 잡지 않는다 | `tests/test_resp.c`(`test_protocol_errors`), `tests/functional_test.py`(`test_oversized_bulk_is_not_allocated`) |

<!-- TODO(human): 코드만 읽어서는 Claude가 알 수 없는 팀의 약속이 더 있으면 위 표에 적어 줘.
     위 세 줄은 예전 하네스(리뷰어·조사 에이전트 지시)에 있던 내용을 옮긴 것이다. 팀에서 정한 것과 다르면 고쳐 줘. -->

## 팀 규칙
@.claude/team-rules.md
