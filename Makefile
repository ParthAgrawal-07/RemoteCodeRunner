CC      ?= gcc
CFLAGS  ?= -std=gnu11 -O2 -g -Wall -Wextra -Wpedantic -pthread
LDFLAGS ?= -pthread

all: server client

server: server.o executor.o logger.o protocol.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

client: client.o protocol.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c server.h protocol.h
	$(CC) $(CFLAGS) -c $< -o $@

# Build with AddressSanitizer + UBSan, then run the test-suite against that build
asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Wpedantic -pthread -fsanitize=address,undefined -fno-omit-frame-pointer" \
	        LDFLAGS="-pthread -fsanitize=address,undefined"

test: all
	python3 tests/test_all.py

clean:
	rm -f *.o server client

.PHONY: all asan test clean
