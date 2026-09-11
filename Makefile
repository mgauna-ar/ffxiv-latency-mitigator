CXX ?= clang++
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Wpedantic -Werror -Iinclude -Isrc -Itests -Isrc/third_party/ftxui/include -Isrc/third_party/ftxui/src

SRCS = \
	src/core/rolling_rtt.cpp \
	src/core/sequence_tracker.cpp \
	src/core/cast_tracker.cpp \
	src/core/animation_lock.cpp \
	src/core/ipc_protocol.cpp \
	src/core/sigscan.cpp \
	src/core/config_manager.cpp \
	src/loader/ui_renderer.cpp \
	tests/test_main.cpp \
	tests/test_rolling_rtt.cpp \
	tests/test_sequence_tracker.cpp \
	tests/test_cast_tracker.cpp \
	tests/test_animation_lock.cpp \
	tests/test_ipc_protocol.cpp \
	tests/test_sigscan.cpp \
	tests/test_ui_renderer.cpp \
	tests/test_config_manager.cpp

LIBS = libftxui.a
BIN = test_runner

.PHONY: all test clean

all: $(BIN)

$(BIN): $(SRCS) $(LIBS)
	$(CXX) $(CXXFLAGS) $(SRCS) $(LIBS) -o $(BIN)

test: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN) test_mitigator_config_temp.json
