CXX      ?= clang++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Isrc
SRC      := $(shell find src -name '*.cpp')
LIB_OBJ  := $(patsubst %.cpp,build/%.o,$(filter-out src/main.cpp,$(SRC)))

minisql: build/src/main.o $(LIB_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

build/test_basic: build/tests/test_basic.o $(LIB_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

build/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

test: build/test_basic
	./build/test_basic

clean:
	rm -rf build minisql

.PHONY: clean test
