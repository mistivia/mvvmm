CC := gcc
CFLAGS := -g -Wall -std=gnu99
UNAME := $(shell uname -s)
LDFLAGS := -g

C_SOURCES := $(shell find . -maxdepth 1 -name '*.c')
C_OBJS := $(C_SOURCES:.c=.o)
C_DEPS := $(C_SOURCES:.c=.d)

TARGET := mvvmm

all: $(TARGET)

$(TARGET): $(C_OBJS)
	$(CC) $(C_OBJS) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

test: mvvmm
	./mvvmm -k ./vmlinuz -i ./initrd -m 4g -d disk.img -t vm0 2>mvvmm.err

clean:
	rm -f $(C_OBJS) $(C_DEPS) $(TARGET)

.PHONY: all clean test

-include $(C_DEPS)
