# Host build and tests for the portable sync client. Switch build lives in switch/ (platform owner).
CC     ?= cc
SAN    := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
WARN   := -std=c11 -Wall -Wextra -Wpedantic -Werror
INC    := -Iinclude -Isrc -I../../core/src -Itests
CFLAGS := $(WARN) -g -O1 $(SAN) $(INC)
B      := ../../build/addon-sync

LIB_SRC := $(wildcard src/*.c) ../../core/src/util.c
LIB_OBJ := $(patsubst %.c,$(B)/%.o,$(subst ../../,,$(LIB_SRC)))

.PHONY: all test sync_host clean
all: test sync_host

$(B)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/core/src/util.o: ../../core/src/util.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/test_sync: $(LIB_OBJ) tests/test_sync.c tests/fakes.c tests/fakes.h
	$(CC) $(CFLAGS) tests/test_sync.c tests/fakes.c $(LIB_OBJ) -o $@

$(B)/sync_host: $(LIB_OBJ) tests/sync_host.c tests/posix_plat.c tests/posix_plat.h
	$(CC) $(CFLAGS) tests/sync_host.c tests/posix_plat.c $(LIB_OBJ) -o $@

sync_host: $(B)/sync_host

test: $(B)/test_sync $(B)/sync_host
	$(B)/test_sync

clean:
	rm -rf $(B)
