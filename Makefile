CXX = g++
CXXFLAGS = -std=c++11 -Wall -Wextra -pedantic
AR = ar
ARFLAGS = rcs
RM = rm -f

LIB = libuthreads.a
OBJS = uthreads.o
HDRS = uthreads.h
TARGET = $(LIB)

all: $(TARGET)

$(LIB): $(OBJS)
	$(AR) $(ARFLAGS) $@ $^

uthreads.o: uthreads.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -c uthreads.cpp

.PHONY: clean tar

clean:
	$(RM) $(OBJS) $(LIB) ex2.tar

tar: clean
	tar -cvf ex2.tar uthreads.cpp uthreads.h Makefile README
