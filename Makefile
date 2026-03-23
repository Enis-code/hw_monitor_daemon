CC = gcc
# Optimization: -O2 for perf (loops, inlining, no busy-wait overhead) + -g for debug symbols
# Keeps strict warnings (-Wall etc.) as base; production-ready without sacrificing safety
CFLAGS = -Wall -Wextra -Werror -std=c11 -pthread -Iinclude -O2 -g
LDFLAGS = -pthread

SRC_DIR = src
INC_DIR = include
BUILD_DIR = build

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

TARGET = $(BUILD_DIR)/hw_monitor_daemon

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)/*

.PHONY: all clean
