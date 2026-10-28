# Dependency-free build used for fast local verification (CMake/Bazel are the
# production build systems). Usage: make [test|bench|sanitize|clean] [CXX=...]
CXX      ?= g++
SAN      ?=
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Werror -Iinclude $(SAN)
LDLIBS   ?= -pthread -lrt
OUT      := build

LIB_SRC := $(wildcard src/*.cpp) src/platform/posix/posix_platform.cpp
LIB_OBJ := $(LIB_SRC:%.cpp=$(OUT)/%.o)

# Default: tests, benchmark and demo.
all: $(OUT)/rtsdk_tests $(OUT)/rtsdk_bench $(OUT)/example_adas

# Any header change rebuilds everything (project is small; avoids stale objects).
$(OUT)/%.o: %.cpp $(wildcard include/rtsdk/*.hpp) $(wildcard tests/*.hpp)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/rtsdk_tests: $(LIB_OBJ) $(patsubst %.cpp,$(OUT)/%.o,$(wildcard tests/*.cpp))
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(OUT)/rtsdk_bench: $(LIB_OBJ) $(OUT)/bench/bench.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(OUT)/example_adas: $(LIB_OBJ) $(OUT)/examples/adas_demo.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

# Unit tests against ManualClock / SimPlatform plus POSIX smoke tests.
test: $(OUT)/rtsdk_tests
	$(OUT)/rtsdk_tests

# Prints dispatch cost, ring/mailbox throughput and start-latency percentiles.
bench: $(OUT)/rtsdk_bench
	$(OUT)/rtsdk_bench

# Sanitizer runs use separate build dirs so objects are never mixed.
sanitize:
	$(MAKE) OUT=build-asan SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" CXXFLAGS="-std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror -Iinclude -fsanitize=address,undefined -fno-sanitize-recover=all" build-asan/rtsdk_tests
	build-asan/rtsdk_tests
	$(MAKE) OUT=build-tsan CXXFLAGS="-std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror -Iinclude -fsanitize=thread" build-tsan/rtsdk_tests
	build-tsan/rtsdk_tests

# Compiles the FreeRTOS backend against declaration-only stubs (no kernel linked).
freertos-syntax:
	$(CXX) -std=c++17 -Wall -Wextra -Wpedantic -Werror -Iinclude -Itests/stubs -fsyntax-only src/platform/freertos/freertos_platform.cpp

# Everything a CI gate should run: tests, sanitizers, perf budget, FreeRTOS compile check.
quality: test sanitize bench-check freertos-syntax

# Fails when a performance budget in bench/bench.cpp is exceeded.
bench-check: $(OUT)/rtsdk_bench
	$(OUT)/rtsdk_bench --check

clean:
	rm -rf build build-asan build-tsan

.PHONY: all test bench sanitize clean freertos-syntax quality bench-check
