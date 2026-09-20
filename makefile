# 编译器
CC := gcc

# 编译选项
CFLAGS := -Wall -Wextra -Werror -std=c11 -pthread
LDFLAGS := -pthread

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
	gateway/bluetooth_worker.c \
	gateway/wifi_server.c \
	gateway/wifi_client.c \
	gateway/serial_port.c \
	gateway/tcp_client.c \
	gateway/message_queue.c \
	gateway/server_link.c

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
	$(CC) $^ $(LDFLAGS) -o $@


# ============================================================
# 生成服务器程序
# ============================================================

server: $(BUILD_DIR)/gateway_server

$(BUILD_DIR)/gateway_server: $(COMMON_OBJS) $(SERVER_OBJS)
	$(CC) $^ $(LDFLAGS) -o $@


# ============================================================
# 生成模拟节点程序
# ============================================================

mock_node: $(BUILD_DIR)/mock_node

$(BUILD_DIR)/mock_node: $(COMMON_OBJS) $(TOOLS_OBJS)
	$(CC) $^ $(LDFLAGS) -o $@


# ============================================================
# 生成WIFI模拟节点程序
# ============================================================

mock_wifi_node: $(BUILD_DIR)/mock_wifi_node

$(BUILD_DIR)/mock_wifi_node: $(COMMON_OBJS) $(MOCK_WIFI_OBJS)
	$(CC) $^ $(LDFLAGS) -o $@


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


# ============================================================
# 单元测试
# ============================================================
#
# tests/ 下的每个 .c 都自带 main，各自编译成一个独立可执行文件。
#
# 为了保持 makefile 简单，所有测试统一链接同一组源文件；
# 多余的目标文件不会被链入（链接器只取用到的符号）。
#
# 其中 test_tcp_client / test_tcp_loop 需要真实的 TCP 服务端，
# 属于集成测试，单独放在 test-tcp 目标里，不参与 make test。

TEST_DIR := $(BUILD_DIR)/tests

# 纯单测（不依赖外部进程）
UNIT_TEST_NAMES := \
	test_crc16 \
	test_frame \
	test_frame_parser \
	test_line_parser \
	test_message_json \
	test_message_queue \
	test_node_store

# 集成测试（需要先启动 build/gateway_server）
#
# 注意：test_tcp_loop 是无限循环的持续发送程序（while(1)+sleep(1)），
# 没有"成功"这一终态，无法自动判定成败，因此列为手动联调工具，
# 不参与自动运行，只由 tests-bin 负责构建。
TCP_TEST_NAMES := \
	test_tcp_client

MANUAL_TEST_NAMES := \
	test_tcp_loop

TEST_LIBS := \
	common/crc16.c \
	common/frame.c \
	common/frame_parser.c \
	common/line_parser.c \
	common/message_json.c \
	server/node_store.c \
	gateway/message_queue.c \
	gateway/tcp_client.c

UNIT_TEST_BINS := $(addprefix $(TEST_DIR)/,$(UNIT_TEST_NAMES))
TCP_TEST_BINS  := $(addprefix $(TEST_DIR)/,$(TCP_TEST_NAMES))
MANUAL_TEST_BINS := $(addprefix $(TEST_DIR)/,$(MANUAL_TEST_NAMES))

# 测试程序在编译期需要 -Icommon -Igateway -Iserver，
# 这里提供直接的 .c -> 可执行文件规则。
#
# 必须同时依赖 TEST_LIBS：否则修改被测源码（如 gateway/message_queue.c）
# 后测试不会重建，跑的还是旧二进制。
$(TEST_DIR)/%: tests/%.c $(TEST_LIBS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(TEST_LIBS) -o $@ $(LDFLAGS)

# 所有测试的二进制
tests-bin: $(UNIT_TEST_BINS) $(TCP_TEST_BINS) $(MANUAL_TEST_BINS)

# 运行纯单测
#
# 用 timeout 兜底：若线程死锁（例如 shutdown 没能唤醒消费者），
# 测试会挂住而不是退出，超时即判定失败。
test: $(UNIT_TEST_BINS)
	@fail=0; \
	for t in $(UNIT_TEST_NAMES); do \
		printf '%-22s ' "$$t"; \
		if timeout 10 $(TEST_DIR)/$$t >/dev/null 2>&1; then \
			echo "PASS"; \
		else \
			case $$? in \
				124) echo "TIMEOUT (可能死锁)";; \
				*)   echo "FAIL";; \
			esac; \
			fail=1; \
		fi; \
	done; \
	if [ $$fail -ne 0 ]; then \
		echo "存在失败用例"; \
		exit 1; \
	fi; \
	echo "全部单元测试通过"

# 集成测试：自动拉起 server（端口 9000）再运行测试程序
#
# 注意测试里写死了 127.0.0.1:9000，所以这里必须用 9000。
test-tcp: $(TCP_TEST_BINS) $(BUILD_DIR)/gateway_server
	@port=9000; \
	if nc -z 127.0.0.1 $$port 2>/dev/null; then \
		echo "端口 $$port 已被占用，请先停止占用进程"; \
		exit 1; \
	fi; \
	(sleep 20 | ./$(BUILD_DIR)/gateway_server $$port >$(BUILD_DIR)/test_server.log 2>&1 &) ; \
	pid=$$!; \
	ready=0; \
	for i in $$(seq 1 50); do \
		if nc -z 127.0.0.1 $$port 2>/dev/null; then ready=1; break; fi; \
		sleep 0.1; \
	done; \
	if [ $$ready -ne 1 ]; then \
		echo "server 启动失败"; \
		pkill -f "gateway_server $$port"; \
		exit 1; \
	fi; \
	echo "server 已就绪 (端口 $$port)"; \
	fail=0; \
	for t in $(TCP_TEST_NAMES); do \
		printf '%-22s ' "$$t"; \
		if timeout 10 $(TEST_DIR)/$$t >/dev/null 2>&1; then \
			echo "PASS"; \
		else \
			echo "FAIL"; \
			fail=1; \
		fi; \
	done; \
	pkill -f "gateway_server $$port" 2>/dev/null; \
	if [ $$fail -ne 0 ]; then exit 1; fi; \
	echo "全部集成测试通过"

.PHONY: tests-bin test test-tcp test-asan test-tsan test-sanitizers

# ============================================================
# Sanitizer 验证
# ============================================================
#
# 并发代码光"跑通一次"是不够的：数据竞争和内存错误往往是偶发的。
# 这两个目标用编译期插桩把它们变成确定性可复现的问题。
#
#   make test-tsan  ThreadSanitizer：数据竞争、锁误用、死锁
#   make test-asan  AddressSanitizer + UBSan：越界、用后释放、未定义行为
#
# 两者互斥，不能同时启用（TSan 与 ASan 不能共存）。
# 注意：TSan 会显著放慢执行，且对时序敏感，故不加 timeout 之外的额外约束。

# 公共部分：与 TEST_LIBS 相同，但用 sanitizer 重新编译一份，
# 不污染正常构建产物。
SAN_DIR := $(BUILD_DIR)/sanitizers

SAN_CFLAGS_COMMON := -Wall -Wextra -Werror -std=c11 -pthread -g -O1

# 用变量拼接，避免 sanitizer 开关写死在配方里
TSAN_FLAGS := -fsanitize=thread
ASAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

# 运行单个测试的公共 shell 片段（逐个跑并汇总结果）
# 用法：$(call run_san_tests,<目录>)
define run_san_tests
	@fail=0; \
	for t in $(UNIT_TEST_NAMES); do \
		printf '%-22s ' "$$t"; \
		if timeout 60 $(1)/$$t >/dev/null 2>&1; then \
			echo "PASS"; \
		else \
			case $$? in \
				124) echo "TIMEOUT (可能死锁)";; \
				*)   echo "FAIL";; \
			esac; \
			fail=1; \
		fi; \
	done; \
	if [ $$fail -ne 0 ]; then exit 1; fi; \
	echo "全部单元测试通过 ($(2))"
endef

# 逐个构建 sanitizer 版本的测试二进制
$(SAN_DIR)/tsan/%: tests/%.c $(TEST_LIBS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(SAN_CFLAGS_COMMON) $(TSAN_FLAGS) $< $(TEST_LIBS) -o $@ $(LDFLAGS)

$(SAN_DIR)/asan/%: tests/%.c $(TEST_LIBS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(SAN_CFLAGS_COMMON) $(ASAN_FLAGS) $< $(TEST_LIBS) -o $@ $(LDFLAGS)

TSAN_TEST_BINS := $(addprefix $(SAN_DIR)/tsan/,$(UNIT_TEST_NAMES))
ASAN_TEST_BINS := $(addprefix $(SAN_DIR)/asan/,$(UNIT_TEST_NAMES))

san-tests-bin: $(TSAN_TEST_BINS) $(ASAN_TEST_BINS)

test-tsan: $(TSAN_TEST_BINS)
	$(call run_san_tests,$(SAN_DIR)/tsan,ThreadSanitizer)

test-asan: $(ASAN_TEST_BINS)
	$(call run_san_tests,$(SAN_DIR)/asan,ASan+UBSan)

# 依次跑两种 sanitizer
test-sanitizers: test-tsan test-asan
	@echo "Sanitizer 检查全部通过"