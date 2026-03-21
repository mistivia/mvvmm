CXX := g++
CFLAGS := -g -Wall -std=gnu++17
UNAME := $(shell uname -s)
LDFLAGS := -g

CXX_SOURCES := $(wildcard src/*.cc)
BUILD_DIR := build
CXX_OBJS := $(patsubst src/%.cc,$(BUILD_DIR)/%.o,$(CXX_SOURCES))
CXX_DEPS := $(patsubst src/%.cc,$(BUILD_DIR)/%.d,$(CXX_SOURCES))

TARGET := $(BUILD_DIR)/mvvmm

all: $(TARGET)

$(TARGET): $(CXX_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXX_OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: src/%.cc
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CFLAGS) -MMD -MP -c $< -o $@

test: $(TARGET)
	sudo ./$(TARGET) -k ./vmlinuz -i ./initrd -m 4g -d disk.img -t vm0 2>$(BUILD_DIR)/mvvmm.err

bear:
	make clean
	bear -- make all

fmt:
	clang-format -i $(CXX_SOURCES)

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean test bear

-include $(CXX_DEPS)
