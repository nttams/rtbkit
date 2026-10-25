CXX      ?= clang++
CXXFLAGS := -std=c++23 -Wall -Wextra -O2 -Isrc -Ithird_party

BUILD := build
LIB_OBJS := $(BUILD)/static_filters.o $(BUILD)/filter_pool.o
HEADERS := $(wildcard src/*.h)

all: $(BUILD)/libstaticfilter.a

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.cc $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/libstaticfilter.a: $(LIB_OBJS)
	ar rcs $@ $^

$(BUILD)/static_filters_test: tests/static_filters_test.cc tests/utils.h $(BUILD)/libstaticfilter.a
	$(CXX) $(CXXFLAGS) -Itests $< $(BUILD)/libstaticfilter.a -o $@

$(BUILD)/demo: demo/demo.cc $(BUILD)/libstaticfilter.a third_party/nlohmann/json.hpp
	$(CXX) $(CXXFLAGS) $< $(BUILD)/libstaticfilter.a -o $@

test: $(BUILD)/static_filters_test
	./$(BUILD)/static_filters_test

demo: $(BUILD)/demo

clean:
	rm -rf $(BUILD)

.PHONY: all test demo clean
