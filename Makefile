CC := g++
CFLAGS := -g -Wall -std=gnu++17
UNAME := $(shell uname -s)
LDFLAGS := -g

C_SOURCES := $(wildcard src/*.cc)
BUILD_DIR := build
C_OBJS := $(patsubst src/%.cc,$(BUILD_DIR)/%.o,$(C_SOURCES))
C_DEPS := $(patsubst src/%.cc,$(BUILD_DIR)/%.d,$(C_SOURCES))

TARGET := $(BUILD_DIR)/mvvmm

all: $(TARGET)

$(TARGET): $(C_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(C_OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: src/%.cc
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

test: $(TARGET)
	./$(TARGET) -k ./vmlinuz -i ./initrd -m 4g -d disk.img -t vm0 2>$(BUILD_DIR)/mvvmm.err

bear:
	make clean
	bear -- make all

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean test bear

-include $(C_DEPS)
