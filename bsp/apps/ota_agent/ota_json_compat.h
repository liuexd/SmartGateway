#ifndef OTA_JSON_COMPAT_H
#define OTA_JSON_COMPAT_H

/*
 * json-c 版本兼容层
 *
 * 背景：
 *   开发机（Ubuntu）的 json-c 为 0.18，提供 json_tokener_get_parse_end()。
 *   目标板 100ASK i.MX6ULL 的 Buildroot 2020.02 rootfs 携带 json-c 0.13.1，
 *   该版本没有 json_tokener_get_parse_end()（0.14 才引入）。
 *
 * 为什么不能"直接用宿主机头文件编译"：
 *   ARM 构建必须链接目标 sysroot 的头文件与库；一旦混入宿主机 /usr/include，
 *   编译可能通过，但目标板运行时缺少对应符号，属于典型的交叉编译陷阱。
 *
 * 兼容策略：
 *   0.13.1 的 struct json_tokener 是公开结构（非 opaque），其中 char_offset
 *   记录 json_tokener_parse_ex() 已经消费的字节数，语义与 0.14+ 的
 *   json_tokener_get_parse_end() 相同，且该字段在解析成功返回前不会被重置。
 *   因此旧版本直接用该字段，新版本调用官方 API。
 *
 * 调用方只使用 ota_json_parse_end() 这一个抽象，不需要感知 json-c 版本。
 */

#include <json-c/json.h>

#if defined(JSON_C_VERSION_NUM) && (JSON_C_VERSION_NUM >= 0x000e00)

/* json-c >= 0.14：官方接口 */
#define ota_json_parse_end(tok) json_tokener_get_parse_end(tok)

#else

/*
 * json-c < 0.14：char_offset 即已消费字节数。
 * 该结构为公开定义，因此这里可以直接读取字段。
 */
#define ota_json_parse_end(tok) ((size_t)((tok)->char_offset))

#endif

#endif /* OTA_JSON_COMPAT_H */
