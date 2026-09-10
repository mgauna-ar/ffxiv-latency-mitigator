CXX ?= clang++
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Wpedantic -Werror -Iinclude -Isrc -Itests

SRCS = \
	src/core/rolling_rtt.cpp \
	src/core/sequence_tracker.cpp \
	src/core/cast_tracker.cpp \
	src/core/animation_lock.cpp \
	src/core/ipc_protocol.cpp \
	src/core/sigscan.cpp \
	src/loader/ui_renderer.cpp \
	tests/test_main.cpp \
	tests/test_rolling_rtt.cpp \
	tests/test_sequence_tracker.cpp \
	tests/test_cast_tracker.cpp \
	tests/test_animation_lock.cpp \
	tests/test_ipc_protocol.cpp \
	tests/test_sigscan.cpp \
	tests/test_ui_renderer.cpp

BIN = test_runner

.PHONY: all test clean

all: $(BIN)

$(BIN): $(SRCS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(BIN)

test: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
