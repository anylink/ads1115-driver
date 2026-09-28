/**
 * @file ads1115_esp32.h
 * @brief ADS1115 的 ESP-IDF 适配层（可选件）
 *
 * 这一层是核心驱动与 ESP-IDF 之间唯一的接触面。
 * 它做三件事：
 *   1. 把 i2c_master_bus 句柄包装成一个 i2c_master_dev 设备
 *   2. 用 IDF 的 i2c_master_transmit() / i2c_master_transmit_receive()
 *      实现 ads1115_i2c_transfer_fn 回调
 *   3. 用 esp_timer 与 vTaskDelay 提供时基
 *
 * 芯片无关性说明：
 *   i2c_master 驱动是 ESP-IDF 的通用外设 API，在 ESP32 / ESP32-S2 / S3 /
 *   C2 / C3 / C6 / H2 / P4 上接口完全一致，因此本文件无需任何
 *   CONFIG_IDF_TARGET_* 条件编译即可同时支持全部 ESP32 系列。
 *   唯一与芯片相关的是 GPIO 引脚号，那属于应用层（见 main/main.c），不属于驱动。
 */

#ifndef ADS1115_ESP32_H
#define ADS1115_ESP32_H

#include "driver/i2c_master.h"
#include "ads1115.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief ESP-IDF 侧的平台上下文
 *
 * 生命周期必须覆盖 ads1115_t 的整个使用期（建议 static 或放在长生命周期结构体里）。
 */
typedef struct {
    i2c_master_bus_handle_t bus;        /**< 由调用方创建的总线句柄 */
    i2c_master_dev_handle_t dev;        /**< 由本模块 attach 时创建 */
    uint32_t                scl_speed_hz; /**< 入参：SCL 频率，0 表示默认 400 kHz */
    uint32_t                timeout_ms;   /**< 入参：单次 I2C 传输超时，0 表示默认 100 ms */
} ads1115_esp32_ctx_t;

/**
 * @brief 在给定 I2C 总线上创建 ADS1115 设备并完成驱动初始化
 *
 * 等价于「i2c_master_bus_add_device + ads1115_init」两步，只是把平台细节封装在这里。
 * 调用完毕后 dev 即可直接用于 ads1115_set_channels / ads1115_poll。
 *
 * @param dev       待初始化的驱动实例（调用方分配，可 static）
 * @param bus       已由 i2c_new_master_bus() 创建好的总线
 * @param ctx       平台上下文（调用方分配，可 static），只需预置 scl_speed_hz / timeout_ms
 * @param i2c_addr  7 位从机地址（0x48-0x4B）
 * @return ADS1115_OK 或错误码
 */
ads1115_err_t ads1115_esp32_attach(ads1115_t *dev,
                                   i2c_master_bus_handle_t bus,
                                   ads1115_esp32_ctx_t *ctx,
                                   uint8_t i2c_addr);

/**
 * @brief 释放设备：从总线摘除设备（不销毁总线，总线由调用方负责）
 */
ads1115_err_t ads1115_esp32_detach(ads1115_t *dev, ads1115_esp32_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ADS1115_ESP32_H */
