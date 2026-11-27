# toy-btrfs
#   make          build everything into build/
#   make test     run the automated tests
#   make demo     run the narrated demo (good for a screen recording)
#   make shell    interactive shell on disk.img
#   make clean

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -g -O1
INC     := -Icore -Ifeatures/checkpoint -Ifeatures/duplication -Ifeatures/recovery
SRC     := $(wildcard core/*.c) $(wildcard features/*/*.c)
HDR     := $(wildcard core/*.h) $(wildcard features/*/*.h)

all: build/tests build/demo build/btrfs-shell

build/tests: tests/tests.c $(SRC) $(HDR) | build
	$(CC) $(CFLAGS) $(INC) -o $@ $(SRC) $<

build/demo: demo/demo.c $(SRC) $(HDR) | build
	$(CC) $(CFLAGS) $(INC) -o $@ $(SRC) $<

build/btrfs-shell: tools/btrfs-shell.c $(SRC) $(HDR) | build
	$(CC) $(CFLAGS) $(INC) -o $@ $(SRC) $<

build:
	mkdir -p build

test: build/tests
	./build/tests

demo: build/demo
	./build/demo

shell: build/btrfs-shell
	./build/btrfs-shell disk.img

clean:
	rm -rf build disk.img

.PHONY: all test demo shell clean
