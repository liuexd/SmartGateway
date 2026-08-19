#ifndef FRAME_H
#define FRAME_H

#include <stddef.h>
#include <stdint.h>

/* 协议规定的最大帧长度，包含结尾的 '\0' */
#define FRAME_MAX_LEN 256

/* frame_build_data() 的错误返回值 */
typedef enum
{
    FRAME_ERR_PARAM = -1,
    FRAME_ERR_TOO_LONG = -2,
    FRAME_ERR_NODE_ID = -3
} frame_error_t;

/*
 * =====================================================================
 * 通用键值对字段（DATA 帧 data 段的基础单元）
 * =====================================================================
 *
 * 一帧 DATA 的数据区由若干条 KEY=VALUE 组成，例如：
 *
 *   T=253,H=601,P=1013,LED=1
 *
 * 具体有哪些 key、每个 key 的含义，由协议字典约定（见 frame.h 顶部
 * 或协议文档），协议层只负责原样搬运，不做任何业务解释。
 * =====================================================================
 */

/* 单个 key 的最大长度（不含结尾 '\0'） */
#define FRAME_KV_KEY_MAX 16

/* 单个 value 的最大长度（不含结尾 '\0'） */
#define FRAME_KV_VALUE_MAX 32

/* 一帧 DATA 最多携带的字段数 */
#define FRAME_DATA_MAX_FIELDS 16

/* 一条键值对：key/value 统一按字符串存储 */
typedef struct
{
    char key[FRAME_KV_KEY_MAX];
    char value[FRAME_KV_VALUE_MAX];
} frame_kv_t;

/*
 * =====================================================================
 * 设备类型字典（DEV 字段）
 * =====================================================================
 *
 * DEV 是 DATA / CMD 帧中的约定字段（协议字典的一部分），
 * 用于标识节点/命令对应的设备类型，例如：
 *
 *   DATA: 1,NODE01,000001,DATA,DEV=1,T=253,H=601
 *   CMD : 1,GW01,000001,CMD,DEV=2,LED=1
 *
 * 取值与含义由本字典约定。未知取值一律视为 FRAME_DEVICE_UNKNOWN，
 * 上层按 UNKNOWN 处理（打印未知设备，不下发针对性命令）。
 * =====================================================================
 */

/* DEV 字段的键名 */
#define FRAME_KV_DEV "DEV"

/* 设备类型：数值即 DEV 字段的约定取值 */
typedef enum
{
    FRAME_DEVICE_UNKNOWN = 0, /* 未识别/未上报 */
    FRAME_DEVICE_THSENSOR = 1, /* 温湿度传感器 */
    FRAME_DEVICE_RELAY = 2,    /* 继电器开关 */
    FRAME_DEVICE_MOTOR = 3     /* 电机 */
} frame_device_type_t;

/*
 * 把 DEV 字段的文本值解析为设备类型。
 *
 * 例如：
 *   frame_device_type_from_text("1") -> FRAME_DEVICE_THSENSOR
 *   frame_device_type_from_text("2") -> FRAME_DEVICE_RELAY
 *   frame_device_type_from_text("x") -> FRAME_DEVICE_UNKNOWN
 *
 * @param text DEV 字段值（数字字符串，可为NULL/空串）
 *
 * @return 对应的设备类型；无法识别时返回 FRAME_DEVICE_UNKNOWN
 */
frame_device_type_t frame_device_type_from_text(
    const char *text
);

/*
 * 把设备类型转换为可读名称。
 *
 * 例如：
 *   FRAME_DEVICE_THSENSOR -> "THSENSOR"
 *   FRAME_DEVICE_UNKNOWN  -> "UNKNOWN"
 *
 * @param type 设备类型
 *
 * @return 名称字符串（'\0'结尾，永不返回NULL）
 */
const char *frame_device_type_name(
    frame_device_type_t type
);

/**
 * @brief 生成一帧 DATA（数据）帧，数据区为任意键值对列表
 *
 * 生成格式：
 * @1,NODE01,000001,DATA,K1=V1,K2=V2,...*CCCC\r\n
 *
 * CRC 计算范围：
 * 1,NODE01,000001,DATA,K1=V1,K2=V2,...
 *
 * @param out        输出缓冲区
 * @param out_size   输出缓冲区大小
 * @param node_id    节点编号，例如 "NODE01"
 * @param sequence   帧序号，范围 0~999999
 * @param fields     键值对数组
 * @param field_count 键值对数量（0 表示只上报节点身份，不带数据）
 *
 * @return 成功时返回帧的实际长度，不包含字符串结尾 '\0'
 *         失败时返回负数错误码
 */
int frame_build_data_kv(
    char *out,
    size_t out_size,
    const char *node_id,
    uint32_t sequence,
    const frame_kv_t *fields,
    size_t field_count
);

/**
 * @brief 生成一帧温湿度数据（兼容旧接口的便捷封装）
 *
 * 内部等价于调用 frame_build_data_kv() 生成 T/H 两条键值对。
 * 新代码建议直接使用 frame_build_data_kv()，
 * 以支持任意字段（T/H/P/LED...）和多设备类型。
 *
 * 生成格式：
 * @1,NODE01,000001,DATA,T=253,H=601*CCCC\r\n
 *
 * @param out             输出缓冲区
 * @param out_size        输出缓冲区大小
 * @param node_id         节点编号，例如 "NODE01"
 * @param sequence        帧序号，范围 0~999999
 * @param temperature_x10 温度放大 10 倍后的整数，例如 253 表示 25.3℃
 * @param humidity_x10    湿度放大 10 倍后的整数，例如 601 表示 60.1%
 *
 * @return 成功时返回帧的实际长度，不包含字符串结尾 '\0'
 *         失败时返回负数错误码
 */
int frame_build_data(
    char *out,
    size_t out_size,
    const char *node_id,
    uint32_t sequence,
    int temperature_x10,
    int humidity_x10
);

/*
 * =====================================================================
 * 命令/应答（ACK/NACK）协议
 * =====================================================================
 *
 * 网关 -> 节点：CMD 帧（frame_command_t），用于下发控制指令
 * 节点 -> 网关：ACK 帧（frame_ack_t），表示命令执行成功
 * 节点 -> 网关：NACK 帧（frame_nack_t），表示命令执行失败
 *
 * 三者通过相同的 sequence 序号相互配对，
 * 网关收到应答后即可确定对应的是哪一条命令。
 * =====================================================================
 */

/*
 * 网关下发的控制命令数据。
 *
 * sender     ：命令发送方标识（网关编号），例如 "GW01"
 * sequence   ：命令序号，用于和节点的 ACK/NACK 应答配对
 * fields     ：命令参数键值对数组，例如 LED=1、MOTOR=50、DEV=2...
 * field_count：实际参数数量
 *
 * 参数含义由协议字典约定（如 LED 0=灭 1=亮），
 * 协议层不解释任何 key，节点侧按需取用。
 */
typedef struct 
{
    char sender[32];
    uint32_t sequence;

    frame_kv_t fields[FRAME_DATA_MAX_FIELDS];
    size_t field_count;
} frame_command_t;

/*
 * 节点对命令的成功应答（Acknowledgment）。
 *
 * node_id     ：应答的节点编号，例如 "NODE01"
 * sequence    ：所应答命令的序号，与 frame_command_t 中的序号一致
 * fields      ：执行后设备状态的键值对回显（如 LED=1、MOTOR=50...）
 * field_count ：实际回显数量
 */
typedef struct 
{
    char node_id[32];
    uint32_t sequence;

    frame_kv_t fields[FRAME_DATA_MAX_FIELDS];
    size_t field_count;
} frame_ack_t;

/*
 * 节点对命令的失败应答（Negative Acknowledgment）。
 *
 * node_id：应答的节点编号，例如 "NODE01"
 * sequence：所应答命令的序号，与 frame_command_t 中的序号一致
 * error：失败原因描述字符串，例如 "bad_crc"、"unsupported_cmd"
 */
typedef struct 
{
    char node_id[32];
    uint32_t sequence;
    char error[32];
} frame_nack_t;

/*
 * 生成一帧 CMD（命令）帧，命令参数为任意键值对列表。
 *
 * 帧格式示例：
 * @1,GW01,000001,CMD,DEV=2,LED=1,MOTOR=50*CCCC\r\n
 *
 * @param out        输出缓冲区
 * @param out_size   输出缓冲区大小
 * @param sender     发送方标识（网关编号），例如 "GW01"
 * @param sequence   命令序号，范围 0~999999
 * @param fields     命令参数键值对数组
 * @param field_count 参数数量（0 表示无参数命令）
 *
 * @return 成功时返回帧的实际长度（不含结尾 '\0'），失败返回负数错误码
 */
int frame_build_command_kv(
    char *out,
    size_t out_size,
    const char *sender,
    uint32_t sequence,
    const frame_kv_t *fields,
    size_t field_count
);

/*
 * 生成一帧 CMD（命令）帧（兼容旧接口的便捷封装）。
 *
 * 内部等价于调用 frame_build_command_kv() 生成 LED 一条键值对。
 * 新代码建议直接使用 frame_build_command_kv()，
 * 以支持多参数命令（DEV/LED/MOTOR...）。
 *
 * 帧格式示例：
 * @1,GW01,000001,CMD,LED=1*CCCC\r\n
 *
 * @param out      输出缓冲区
 * @param out_size 输出缓冲区大小
 * @param sender   发送方标识（网关编号），例如 "GW01"
 * @param sequence 命令序号，范围 0~999999
 * @param led_value LED 控制值（任意整数；协议层不校验合法性，
 *                  具体语义/合法范围由设备侧决定，0=灭，1=亮）
 *
 * @return 成功时返回帧的实际长度（不含结尾 '\0'），失败返回负数错误码
 */
int frame_build_command(
    char *out,
    size_t out_size,
    const char *sender,
    uint32_t sequence,
    int led_value
);

/*
 * 生成一帧 ACK（成功应答）帧，回显任意键值对列表。
 *
 * 帧格式示例：
 * @1,NODE01,000001,ACK,LED=1*CCCC\r\n
 *
 * @param out        输出缓冲区
 * @param out_size   输出缓冲区大小
 * @param node_id    节点编号，例如 "NODE01"
 * @param sequence   所应答命令的序号，必须与命令帧一致
 * @param fields     设备状态回显键值对数组（如 LED=1、MOTOR=50）
 * @param field_count 回显数量（0 表示只确认序号，不回显状态）
 *
 * @return 成功时返回帧的实际长度（不含结尾 '\0'），失败返回负数错误码
 */
int frame_build_ack_kv(
    char *out,
    size_t out_size,
    const char *node_id,
    uint32_t sequence,
    const frame_kv_t *fields,
    size_t field_count
);

/*
 * 生成一帧 ACK（成功应答）帧（兼容旧接口的便捷封装）。
 *
 * 内部等价于调用 frame_build_ack_kv() 回显 LED 一条键值对。
 * 新代码建议直接使用 frame_build_ack_kv()。
 *
 * 帧格式示例：
 * @1,NODE01,000001,ACK,LED=1*CCCC\r\n
 *
 * @param out       输出缓冲区
 * @param out_size  输出缓冲区大小
 * @param node_id   节点编号，例如 "NODE01"
 * @param sequence  所应答命令的序号，必须与命令帧一致
 * @param led_value 执行后 LED 的实际状态（协议层不校验合法性）
 *
 * @return 成功时返回帧的实际长度（不含结尾 '\0'），失败返回负数错误码
 */
int frame_build_ack(
    char *out,
    size_t out_size,
    const char *node_id,
    uint32_t sequence,
    int led_value
);

/*
 * 生成一帧 NACK（失败应答）帧。
 *
 * 帧格式示例：
 * @NODE01,000001,NACK,ERR=unsupported_cmd*CCCC\r\n
 *
 * @param out      输出缓冲区
 * @param out_size 输出缓冲区大小
 * @param node_id  节点编号，例如 "NODE01"
 * @param sequence 所应答命令的序号，必须与命令帧一致
 * @param error    失败原因描述字符串
 *
 * @return 成功时返回帧的实际长度（不含结尾 '\0'），失败返回负数错误码
 */
int frame_build_nack(
    char *out,
    size_t out_size,
    const char *node_id,
    uint32_t sequence,
    const char *error
);


#endif