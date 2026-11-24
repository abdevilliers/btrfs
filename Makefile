# btrfs - a btrfs-style copy-on-write storage project.
#
#   make            build every library, test and demo into build/
#   make test       run core tests, then every feature's tests
#   make demo       run every feature's demo
#   make shell      the filesystem shell (tools/fsh.c), on fsh.img
#   make clean
#
# Features are found automatically: any folder features/<name>/ with
#   include/*.h   public header(s)       src/*.c   implementation
#   tests/*.c     test programs           demo/*.c  demo programs
# is built and linked. Nothing in this file needs to change to add one.

CC     ?= cc
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion

BUILD    := build
FEATURES := $(notdir $(wildcard features/*))

CORE_SRC := $(wildcard core/src/*.c)
FEAT_SRC := $(foreach f,$(FEATURES),$(wildcard features/$(f)/src/*.c))
OBJS     := $(patsubst %.c,$(BUILD)/%.o,$(CORE_SRC) $(FEAT_SRC))
LIB      := $(BUILD)/libbtrfs.a

# Public headers only: features see core/include and each other's include/.
INCLUDES := -Icore/include $(foreach f,$(FEATURES),-Ifeatures/$(f)/include)
HEADERS  := $(wildcard core/include/*.h core/src/*.h features/*/include/*.h)

TEST_SRC := $(wildcard core/tests/*.c) $(foreach f,$(FEATURES),$(wildcard features/$(f)/tests/*.c))
DEMO_SRC := $(foreach f,$(FEATURES),$(wildcard features/$(f)/demo/*.c))
TOOL_SRC := $(wildcard tools/*.c)
TESTS    := $(addprefix $(BUILD)/bin/,$(basename $(notdir $(TEST_SRC))))
DEMOS    := $(addprefix $(BUILD)/bin/,$(basename $(notdir $(DEMO_SRC))))
TOOLS    := $(addprefix $(BUILD)/bin/,$(basename $(notdir $(TOOL_SRC))))

vpath %.c $(sort $(dir $(TEST_SRC) $(DEMO_SRC) $(TOOL_SRC)))

all: $(LIB) $(TESTS) $(DEMOS) $(TOOLS)

# Core may also use its private headers in core/src (e.g. disk_impl.h).
$(BUILD)/core/%.o: core/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -Icore/src -c $< -o $@

# Features get public headers only.
$(BUILD)/features/%.o: features/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(LIB): $(OBJS)
	@mkdir -p $(dir $@)
	ar rcs $@ $^

$(BUILD)/bin/%: %.c $(LIB) $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(LIB) -o $@

test: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; ./$$t; done

demo: $(DEMOS)
	@set -e; for d in $(DEMOS); do echo "== $$d"; ./$$d; done

# The filesystem shell: an interactive session on fsh.img.
shell: $(BUILD)/bin/fsh
	./$(BUILD)/bin/fsh

clean:
	rm -rf $(BUILD)

.PHONY: all test demo shell clean
