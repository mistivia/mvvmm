CC := gcc
CFLAGS := -g -Wall -std=gnu99
UNAME := $(shell uname -s)
LDFLAGS := -g

C_SOURCES := $(shell find . -maxdepth 1 -name '*.c')
C_OBJS := $(C_SOURCES:.c=.o)

TARGET := mvvmm

all: $(TARGET)

$(TARGET): $(C_OBJS)
	$(CC) $(C_OBJS) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

test: mvvmm
	./mvvmm -k ./vmlinuz -i ./initrd -m 1g 2>>mvvmm.err
# 	./mvvmm -k ./vmlinuz -m 1g 2>>mvvmm.err

clean:
	rm -f $(C_OBJS) $(TARGET)

.PHONY: all clean test
