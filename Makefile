# Linux/WSL 전용. check.sh 가 CFLAGS 로 -Werror 를 넘기므로 `=` 가 아니라 `?=` 와 `+=` 로 받는다.
CC = gcc
CFLAGS ?= -O2
CFLAGS += -std=gnu99 -Wall -Wextra -Isrc -MMD -MP
PY = python3

CORE_OBJS = src/store/store.o src/resp/resp.o src/commands/commands.o src/aof/aof.o
SERVER_OBJS = $(CORE_OBJS) src/net/server.o src/main.o
TEST_BINS = tests/test_store tests/test_resp tests/test_commands

all: mini-redis

mini-redis: $(SERVER_OBJS)
	$(CC) -o $@ $^

tests/test_store: tests/test_store.o src/store/store.o
	$(CC) -o $@ $^

tests/test_resp: tests/test_resp.o src/resp/resp.o
	$(CC) -o $@ $^

tests/test_commands: tests/test_commands.o $(CORE_OBJS)
	$(CC) -o $@ $^

# 단위 테스트(C) → 기능 테스트(실제 서버를 띄워 TCP 로 검증) → 데모 API 테스트
test: mini-redis $(TEST_BINS)
	./tests/test_store
	./tests/test_resp
	./tests/test_commands
	$(PY) tests/functional_test.py
	$(PY) tests/demo_api_test.py

# 캐시 사용/미사용 성능 비교
bench: mini-redis
	$(PY) demo/benchmark.py

clean:
	rm -f mini-redis src/*.o src/*/*.o src/*.d src/*/*.d tests/*.o tests/*.d $(TEST_BINS)

.PHONY: all test bench clean

-include $(wildcard src/*.d src/*/*.d tests/*.d)
