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

# Phase 2 builds independently of the Phase 1 server and its libraries.
M2_BUILD_DIR ?= build/m2
M2_CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -g -Iinclude -pthread $(M2_SANITIZERS)
M2_LDFLAGS = -pthread -rdynamic $(M2_SANITIZERS)
M2_MODULES = agentos_lockdep agentos_pip agentos_tx agentos_mutex harness_sched echo9_crisis_sim
M2_LIB_OBJECTS = $(addprefix $(M2_BUILD_DIR)/,$(addsuffix .o,$(M2_MODULES)))
M2_OBJECTS = $(M2_LIB_OBJECTS) $(M2_BUILD_DIR)/test_atv_runner.o $(M2_BUILD_DIR)/echo9_sim_main.o
M2_LIB = $(M2_BUILD_DIR)/libagentos_sync.a

.PHONY: m2 test-m2 sim-m2 asan-m2 tsan-m2

m2: $(M2_BUILD_DIR)/test_atv $(M2_BUILD_DIR)/echo9_sim

test-m2: m2
	./$(M2_BUILD_DIR)/test_atv

sim-m2: $(M2_BUILD_DIR)/echo9_sim
	./$(M2_BUILD_DIR)/echo9_sim

asan-m2:
	$(MAKE) M2_BUILD_DIR=build/m2-asan M2_SANITIZERS=-fsanitize=address,leak test-m2

tsan-m2:
	$(MAKE) M2_BUILD_DIR=build/m2-tsan M2_SANITIZERS=-fsanitize=thread test-m2

$(M2_BUILD_DIR):
	mkdir -p $@

$(M2_BUILD_DIR)/%.o: src/%.c Makefile | $(M2_BUILD_DIR)
	$(CC) $(M2_CFLAGS) -MMD -MP -c $< -o $@

$(M2_BUILD_DIR)/test_atv_runner.o: tests/test_atv_runner.c Makefile | $(M2_BUILD_DIR)
	$(CC) $(M2_CFLAGS) -MMD -MP -c $< -o $@

$(M2_LIB): $(M2_LIB_OBJECTS)
	$(AR) rcs $@ $^

$(M2_BUILD_DIR)/test_atv: $(M2_BUILD_DIR)/test_atv_runner.o $(M2_LIB)
	$(CC) $(M2_LDFLAGS) $^ -o $@

$(M2_BUILD_DIR)/echo9_sim: $(M2_BUILD_DIR)/echo9_sim_main.o $(M2_LIB)
	$(CC) $(M2_LDFLAGS) $^ -o $@

-include $(M2_OBJECTS:.o=.d)
