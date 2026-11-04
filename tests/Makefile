# RetroFusion host-layer tests. Runs on the build machine (x86_64), no NDK needed.
#
#   make -C tests            # build + run
#   make -C tests clean
#
# The same translation units are compiled for Android by native/CMakeLists.txt,
# so a green run here is a real signal, not a smoke test.

CXX      ?= g++
CC       ?= gcc
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter
INCLUDES := -I../vendor -I../native -I../native/corehost -I.
BUILD    := build

HOST_SRC := ../native/corehost/libretro_host.cpp \
            ../native/corehost/core_options.cpp \
            ../native/corehost/core_registry.cpp \
            ../native/corehost/content_sniff.cpp \
            ../native/corehost/path_util.cpp

.PHONY: all run clean
all: run

$(BUILD)/dummy_core.so: dummy_core.c dummy_core.h
	@mkdir -p $(BUILD)
	$(CC) -shared -fPIC -O1 -I../vendor -I. dummy_core.c -o $@

$(BUILD)/host_test: host_test.cpp $(HOST_SRC) dummy_core.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) host_test.cpp $(HOST_SRC) -o $@ -ldl

run: $(BUILD)/dummy_core.so $(BUILD)/host_test
	@./$(BUILD)/host_test $(BUILD)/dummy_core.so

clean:
	rm -rf $(BUILD)
