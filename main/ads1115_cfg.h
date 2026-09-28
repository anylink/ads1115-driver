/**
 * @file ads1115_cfg.h
 * @brief 通道配置模型 + NVS 持久化 + 主机命令请求队列
 *
 * 职责边界（高内聚）：
 *   - 本模块持有"设备侧配置"的唯一模型（4 路的 mux/pga/dr/enabled），
 *     负责默认值、NVS 加载/保存。
 *   - 主机命令经由请求队列传递；队列的消费者是采样主循环
 *     （它同时拥有 ads1115 驱动实例，避免跨任务直接操作驱动的竞态）。
 *   - 换算（scale/offset/通道名/单位）属于主机侧，本模块一概不管。
 */

#ifndef ADS1115_CFG_H
#define ADS1115_CFG_H

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "ads1115_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t mux;
    uint8_t pga;
    uint8_t dr;
    bool    enabled;
} app_ch_cfg_t;

/**
 * @brief 初始化配置模块（创建请求队列 + 加载配置）
 *
 * @param factory true 表示跳过 NVS 直接用默认值（恢复出厂后调用）
 */
void cfg_init(bool factory);

/** 读一路配置视图（idx 越界时返回 false） */
bool cfg_get_channel(uint8_t idx, app_ch_cfg_t *out);

/** 覆盖一路配置并写入 NVS */
void cfg_set_channel(const proto_ch_cfg_t *in);

/** 恢复默认配置并写入 NVS（返回是否成功保存） */
bool cfg_reset_defaults(void);

/** 提交一条主机命令到请求队列（非阻塞；队列满则丢弃并返回 false） */
bool cfg_post_request(const proto_cmd_msg_t *msg);

/** 请求队列句柄（主循环用 xQueueReceive 轮询） */
QueueHandle_t cfg_request_queue(void);

#ifdef __cplusplus
}
#endif

#endif /* ADS1115_CFG_H */
