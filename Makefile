# Host build and tests for the portable core. The Switch build lives in nx/ (platform owner).
CC       ?= cc
SAN      := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
WARN     := -std=c11 -Wall -Wextra -Wpedantic -Werror
INC      := -Icore/include -Icore/src -Itests -Itests/fakes
CFLAGS   := $(WARN) -g -O1 $(SAN) $(INC)
B        := build/host

CORE_SRC := $(wildcard core/src/*.c)
CORE_OBJ := $(CORE_SRC:core/src/%.c=$(B)/core/%.o)
FAKE_OBJ := $(patsubst tests/%.c,$(B)/tests/%.o,$(wildcard tests/fakes/*.c) tests/rig.c)
TESTS    := util decode tracker jsonl state watcher faults journal_durability diag crash prop replay
TEST_BIN := $(TESTS:%=$(B)/test_%)

DOCKER_IMAGE := devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282

.PHONY: all test replay core-arm clean
.SECONDARY:
all: test

test: $(TEST_BIN) $(B)/replay
	@set -e; for t in $(TEST_BIN); do echo "== $$t"; $$t; done; \
	echo "== replay cli on tests/fixtures/synthetic-diag.jsonl"; \
	$(B)/replay tests/fixtures/synthetic-diag.jsonl > $(B)/cli.out 2> $(B)/cli.err; \
	diff -u tests/fixtures/synthetic-diag.expected $(B)/cli.out; echo "  cli output matches"; \
	echo "== replay reset cli fixture"; \
	$(B)/replay tests/fixtures/replay-reset-diag.jsonl > $(B)/cli-reset.out 2> $(B)/cli-reset.err; \
	diff -u tests/fixtures/replay-reset-diag.expected $(B)/cli-reset.out; \
	echo "  reset keys match ordered golden output"; \
	if $(B)/replay tests/fixtures/replay-reset-diag.jsonl tests/fixtures/replay-reset-diag.jsonl > $(B)/cli-multi.out 2> $(B)/cli-multi.err; \
	then echo "multiple inputs unexpectedly accepted"; exit 1; else test "$$?" -eq 2; fi; \
	test ! -s $(B)/cli-multi.out; echo "  multiple inputs rejected before output"; \
	echo "ALL TESTS PASSED"

replay: $(B)/replay

$(B)/core/%.o: core/src/%.c $(wildcard core/src/*.h) $(wildcard core/include/*.h)
	@mkdir -p $(@D)
	@echo CC $<
	@$(CC) $(CFLAGS) -c $< -o $@

$(B)/tests/%.o: tests/%.c $(wildcard tests/*.h tests/fakes/*.h core/src/*.h core/include/*.h)
	@mkdir -p $(@D)
	@echo CC $<
	@$(CC) $(CFLAGS) -c $< -o $@

$(B)/tools/replay.o: tools/replay.c $(wildcard core/src/*.h core/include/*.h)
	@mkdir -p $(@D)
	@echo CC $<
	@$(CC) $(CFLAGS) -c $< -o $@

$(B)/tools/replay_lib.o: tools/replay.c $(wildcard core/src/*.h core/include/*.h)
	@mkdir -p $(@D)
	@echo "CC $< (lib)"
	@$(CC) $(CFLAGS) -DREPLAY_NO_MAIN -c $< -o $@

$(B)/replay: $(B)/tools/replay.o $(CORE_OBJ)
	@echo LD $@
	@$(CC) $(CFLAGS) $^ -o $@

$(B)/test_replay: $(B)/tests/test_replay.o $(B)/tools/replay_lib.o $(CORE_OBJ) $(FAKE_OBJ)
	@echo LD $@
	@$(CC) $(CFLAGS) $^ -o $@

$(B)/test_%: $(B)/tests/test_%.o $(CORE_OBJ) $(FAKE_OBJ)
	@echo LD $@
	@$(CC) $(CFLAGS) $^ -o $@

core-arm:
	docker run --rm -v "$(CURDIR)":/w -w /w --user "$$(id -u):$$(id -g)" $(DOCKER_IMAGE) bash -c '\
	  export PATH=$$DEVKITPRO/devkitA64/bin:$$PATH; \
	  for f in core/src/*.c; do \
	    aarch64-none-elf-gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -march=armv8-a+crc+crypto -mtp=soft -fPIE \
	      -fstack-usage -Wstack-usage=1024 -Icore/include -Icore/src -c $$f -o /tmp/$$(basename $$f .c).o || exit 1; \
	    echo "compiled $$f"; done; cat /tmp/*.su | sort -k2 -n -r | head -5; \
	  aarch64-none-elf-size /tmp/*.o'

clean:
	rm -rf build
