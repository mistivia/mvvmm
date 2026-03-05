CC := g++
CFLAGS := -g -Wall -std=gnu++17
UNAME := $(shell uname -s)
LDFLAGS := -g

C_SOURCES := $(shell find . -maxdepth 1 -name '*.cc')
C_OBJS := $(C_SOURCES:.cc=.o)
C_DEPS := $(C_SOURCES:.cc=.d)

TARGET := mvvmm

all: $(TARGET)

$(TARGET): $(C_OBJS)
	$(CC) $(C_OBJS) -o $@ $(LDFLAGS)

%.o: %.cc
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

test: mvvmm
	./mvvmm -k ./vmlinuz -i ./initrd -m 4g -d disk.img -t vm0 2>mvvmm.err

clean:
	rm -f $(C_OBJS) $(C_DEPS) $(TARGET)

.PHONY: all clean test

-include $(C_DEPS)
