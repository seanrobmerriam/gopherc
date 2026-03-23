CC ?= cc
CPPFLAGS += -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Iinclude
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
LDFLAGS ?=

BIN := gopherd
OBJS := src/main.o src/server.o src/request.o src/response.o src/util.o

.PHONY: all clean test

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

src/main.o: src/main.c include/gopherd.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c src/main.c -o src/main.o

src/server.o: src/server.c include/gopherd.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c src/server.c -o src/server.o

src/request.o: src/request.c include/gopherd.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c src/request.c -o src/request.o

src/response.o: src/response.c include/gopherd.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c src/response.c -o src/response.o

src/util.o: src/util.c include/gopherd.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c src/util.c -o src/util.o

clean:
	rm -f $(OBJS) $(BIN)

test: $(BIN)
	python3 tests/regression.py ./$(BIN)
