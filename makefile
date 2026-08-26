# 编译器
CC := gcc

# 编译选项
CFLAGS := -Wall -Wextra -Werror -std=c11

# 头文件搜索目录
CPPFLAGS := -Icommon -Igateway -Iserver

# 最终生成文件目录
BUILD_DIR := build


# ============================================================
# common 模块
# ============================================================

COMMON_SRCS := \
	common/crc16.c \
	common/frame.c \
	common/frame_parser.c \
	common/line_parser.c \
	common/message_json.c

COMMON_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(COMMON_SRCS))


# ============================================================
# gateway 模块
# ============================================================

GATEWAY_SRCS := \
	gateway/main.c \
	gateway/gateway_app.c \
	gateway/gateway_loop.c \
	gateway/wifi_server.c \
	gateway/serial_port.c \
	gateway/tcp_client.c

GATEWAY_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(GATEWAY_SRCS))


# ============================================================
# server 模块
# ============================================================

SERVER_SRCS := \
	server/main.c \
	server/tcp_server.c \
	server/node_store.c \
	server/command_manager.c

SERVER_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SERVER_SRCS))


# ============================================================
# tools 模块
# ============================================================

TOOLS_SRCS := \
	tools/mock_node.c \
	tools/mock_node_app.c \
	gateway/serial_port.c

TOOLS_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(TOOLS_SRCS))


# ============================================================
# MOCK WIFI NODE
# ============================================================

MOCK_WIFI_SRCS := \
	tools/mock_wifi_node.c

MOCK_WIFI_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(MOCK_WIFI_SRCS))


# ============================================================
# 默认目标
# ============================================================

.PHONY: all gateway server mock_node mock_wifi_node clean

all: gateway server mock_node mock_wifi_node


# ============================================================
# 生成网关程序
# ============================================================

gateway: $(BUILD_DIR)/smart_gateway

$(BUILD_DIR)/smart_gateway: $(COMMON_OBJS) $(GATEWAY_OBJS)
	$(CC) $^ -o $@


# ============================================================
# 生成服务器程序
# ============================================================

server: $(BUILD_DIR)/gateway_server

$(BUILD_DIR)/gateway_server: $(COMMON_OBJS) $(SERVER_OBJS)
	$(CC) $^ -o $@


# ============================================================
# 生成模拟节点程序
# ============================================================

mock_node: $(BUILD_DIR)/mock_node

$(BUILD_DIR)/mock_node: $(COMMON_OBJS) $(TOOLS_OBJS)
	$(CC) $^ -o $@


# ============================================================
# 生成WIFI模拟节点程序
# ============================================================

mock_wifi_node: $(BUILD_DIR)/mock_wifi_node

$(BUILD_DIR)/mock_wifi_node: $(COMMON_OBJS) $(MOCK_WIFI_OBJS)
	$(CC) $^ -o $@


# ============================================================
# 将任意 .c 编译成对应的 .o
# ============================================================

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@


# ============================================================
# 清除编译结果
# ============================================================

clean:
	rm -rf $(BUILD_DIR)