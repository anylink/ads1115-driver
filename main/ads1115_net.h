/**
 * @file ads1115_net.h
 * @brief WiFi 遥测 + 主机命令通道（网络层）
 *
 * 职责（高内聚）：
 *   - WiFi STA / mDNS 发现 / TCP 连接与自动重连 / WiFi 配置切换（带回滚）
 *   - 帧收发：下行帧入发送队列，上行帧解析成命令后分发
 *     （通道类命令转交 cfg 模块的请求队列，由采样主循环应用；网络类命令自己处理）
 *   - 不接触 ADS1115 驱动，也不懂通道配置语义——只搬运字节和帧
 *
 * 对外接口全部线程安全，可在任意任务调用。
 */

#ifndef ADS1115_NET_H
#define ADS1115_NET_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 启动 WiFi/mDNS/网络管理任务（非阻塞，立即返回） */
void net_start(void);

/**
 * @brief 发送一行 JSON 帧（自动补 '\n'）
 * @return false 表示发送队列满被丢弃（连接断开时的正常现象）
 */
bool net_send_line(const char *line);

/** 推送一个采样帧（v2：只含原始码值与引脚电压，换算在主机侧） */
void net_push_sample(int64_t dev_ms, uint8_t ch,
                     int16_t raw, float volt, uint32_t seq);

#ifdef __cplusplus
}
#endif

#endif /* ADS1115_NET_H */
