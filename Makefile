CC ?= cc
CPPFLAGS += -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Iinclude
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
LDFLAGS ?=

BIN := gopherd
SRCS := src/main.c src/server.c src/request.c src/response.c src/util.c
OBJS := $(SRCS:.c=.o)

.PHONY: all clean test

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

src/%.o: src/%.c include/gopherd.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(BIN)

test: $(BIN)
	python3 tests/regression.py ./$(BIN)
