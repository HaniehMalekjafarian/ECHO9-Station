# AgentOS M1: incremental C build and reproducible instructor checks.
CC = gcc
PYTHON = python3
PKG_CONFIG = pkg-config

CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -g -Iinclude
CFLAGS += $(shell $(PKG_CONFIG) --cflags json-c libcurl)
LDLIBS = $(shell $(PKG_CONFIG) --libs json-c libcurl)

TARGET = build/agentos
SOURCES = src/main.c src/protocol.c src/backend.c src/ollama_backend.c src/request_dispatcher.c src/process_manager.c src/agentos_runtime.c src/control_plane.c
OBJECTS = $(patsubst src/%.c,build/%.o,$(SOURCES))
DEPFILES = $(OBJECTS:.o=.d)

.PHONY: all run clean doctor test

all: $(TARGET)

# The Makefile prerequisite also rebuilds on compiler-flag changes.
$(TARGET): $(OBJECTS) Makefile
	$(CC) $(CFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

build/%.o: src/%.c Makefile | build
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

build:
	mkdir -p $@

# Missing .d files are expected on the very first build.
-include $(DEPFILES)

run: all
	./$(TARGET)

# Check the tools used by Dockerfile and the two C libraries.
doctor:
	@command -v $(CC) >/dev/null || { echo "Missing C compiler: $(CC)" >&2; exit 1; }
	@command -v $(PYTHON) >/dev/null || { echo "Missing Python: $(PYTHON)" >&2; exit 1; }
	@command -v $(PKG_CONFIG) >/dev/null || { echo "Missing pkg-config" >&2; exit 1; }
	@$(PKG_CONFIG) --exists json-c libcurl || { echo "Missing json-c/libcurl development packages" >&2; exit 1; }
	@echo "PASS: C compiler, Python, json-c and libcurl available"

test: all
	$(PYTHON) tests/run_all.py

clean:
	rm -rf build
