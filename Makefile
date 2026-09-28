CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Iinclude -O2

SRCS = src/main.cpp
TARGET = sm83
TEST_TARGET = sm83_tests

all: $(TARGET) $(TEST_TARGET)

$(TARGET): $(SRCS) include/*.hpp
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(TARGET)

$(TEST_TARGET): tests.cpp include/*.hpp
	$(CXX) $(CXXFLAGS) tests.cpp -o $(TEST_TARGET)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

clean:
	rm -f $(TARGET) $(TEST_TARGET)

.PHONY: all test clean
