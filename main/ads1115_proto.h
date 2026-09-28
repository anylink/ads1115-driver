/**
 * @file ads1115_proto.h
 * @brief 上位机↔设备 JSON 行协议（v2）：帧解析与构建，纯逻辑无 OS 依赖
 *
 * 帧都是一行 JSON（'\n' 结尾由传输层处理）。方向：
 *
 *   设备→主机：
 *     {"type":"hello","fw":"2.0","cfg":[{"ch":0,"mux":0,"pga":2,"dr":4,"enabled":false},...]}
 *     {"dev_ms":123,"ch":1,"raw":8304,"volt":0.519,"seq":74}
 *     {"type":"ack","cmd":"set_ch","ok":true}
 *     {"type":"event","name":"net_rolled_back"}
 *
 *   主机→设备：
 *     {"cmd":"ping"}
 *     {"cmd":"set_ch","ch":1,"mux":5,"pga":2,"dr":0,"enabled":true}
 *     {"cmd":"set_net","ssid":"x","pass":"y"}
 *     {"cmd":"reset_cfg"}
 *     {"cmd":"get_cfg"}
 *
 * 数值含义（与驱动位域一致）：
 *   mux 0..7（0..3 差分 AIN0-AIN1/0-3/1-3/2-3，4..7 单端 AIN0..AIN3 对 GND）
 *   pga 0..5（±6.144/4.096/2.048/1.024/0.512/0.256 V）
 *   dr  0..7（8/16/32/64/128/250/475/860 SPS）
 */

#ifndef ADS1115_PROTO_H
#define ADS1115_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_FW_VERSION     "2.0"
#define PROTO_CHANNEL_COUNT  4u
#define PROTO_SSID_MAX       32u
#define PROTO_PASS_MAX       64u

typedef enum {
    PROTO_CMD_NONE = 0,
    PROTO_CMD_PING,
    PROTO_CMD_SET_CH,
    PROTO_CMD_SET_NET,
    PROTO_CMD_RESET_CFG,
    PROTO_CMD_GET_CFG,
} proto_cmd_t;

/** 单通道设备侧配置（主机可配置的完整集合；换算在主机侧） */
typedef struct {
    uint8_t ch;
    uint8_t mux;
    uint8_t pga;
    uint8_t dr;
    bool    enabled;
} proto_ch_cfg_t;

/** 解析出的一条主机命令 */
typedef struct {
    proto_cmd_t    cmd;
    proto_ch_cfg_t ch;        /* cmd == SET_CH 时有效 */
    char           ssid[PROTO_SSID_MAX + 1u];  /* cmd == SET_NET 时有效 */
    char           pass[PROTO_PASS_MAX + 1u];
} proto_cmd_msg_t;

/**
 * @brief 解析一行主机命令，失败（坏帧/未知命令/参数越界）返回 false
 */
bool proto_parse_command(const char *line, size_t len, proto_cmd_msg_t *out);

/**
 * @brief 构建设备侧帧，返回 malloc 字符串（调用方 free()），失败返回 NULL
 */
char *proto_build_hello(const proto_ch_cfg_t *cfgs, uint8_t count);
char *proto_build_ack(const char *cmd_name, bool ok, const char *detail);
char *proto_build_event(const char *name, const char *detail);

/** 构建采样帧（高频路径，写进调用方缓冲），返回写入字节数，<0 表示缓冲不足 */
int proto_build_sample(char *buf, size_t size,
                       int64_t dev_ms, uint8_t ch,
                       int16_t raw, float volt, uint32_t seq);

const char *proto_cmd_name(proto_cmd_t cmd);

#ifdef __cplusplus
}
#endif

#endif /* ADS1115_PROTO_H */
