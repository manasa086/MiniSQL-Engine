CXX      ?= clang++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Isrc -Itests
SRC      := $(shell find src -name '*.cpp')
LIB_OBJ  := $(patsubst %.cpp,build/%.o,$(filter-out src/main.cpp,$(SRC)))
TESTS    := $(patsubst tests/%.cpp,build/%,$(wildcard tests/test_*.cpp))

minisql: build/src/main.o $(LIB_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

build/test_%: build/tests/test_%.o $(LIB_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

build/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

test: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

clean:
	rm -rf build minisql

.PHONY: clean test
