# Define compiler and flags
CXX = g++
CXXFLAGS = -Wall -std=c++11 -I.

# Source files
SRCS = uthreads.cpp
HEADERS = uthreads.h
OBJS = $(SRCS:.cpp=.o)

# Static library name
TARGET = libuthreads.a

# Test files
TEST_DIR = test
OUT_DIR = $(TEST_DIR)/out
TEST_SRCS = $(wildcard $(TEST_DIR)/*.cpp)
TEST_NAMES = $(notdir $(TEST_SRCS:.cpp=))
TEST_BINS = $(addprefix $(OUT_DIR)/, $(TEST_NAMES))

# Default target
all: $(TARGET) $(OUT_DIR) $(TEST_BINS)

# Create static library
$(TARGET): $(OBJS)
	ar rcs $@ $^

# Create object files
%.o: %.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Create output directory if it doesn't exist
$(OUT_DIR):
	mkdir -p $@

# Compile and link each test
$(OUT_DIR)/%: $(TEST_DIR)/%.cpp $(TARGET)
	$(CXX) $(CXXFLAGS) $< $(TARGET) -o $@

# Clean
clean:
	rm -f $(OBJS) $(TARGET) $(OUT_DIR)/*
