CXX      ?= clang++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Isrc
SRC      := $(shell find src -name '*.cpp')
OBJ      := $(SRC:%.cpp=build/%.o)

minisql: $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

build/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf build minisql

.PHONY: clean
