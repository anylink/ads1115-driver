/**
 * @file ads1115_esp32.c
 * @brief ADS1115 的 ESP-IDF 适配层实现
 *
 * 与芯片型号无关：只使用 ESP-IDF 通用 i2c_master API 与 esp_timer，
 * 同一份代码在 ESP32 / S2 / S3 / C2 / C3 / C6 / H2 / P4 上均可编译运行。
 */

#include "ads1115_esp32.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#define ADS1115_ESP32_DEFAULT_SCL_HZ     400000u
#define ADS1115_ESP32_DEFAULT_TIMEOUT_MS 100u

/* ------------------------------------------------------------------ */
/* Port 回调实现                                                       */
/* ------------------------------------------------------------------ */

/**
 * 实现「写寄存器地址 + 读数据」的重复起始事务。
 * rx 为空时退化为纯写事务。
 */
static ads1115_err_t esp32_transfer(void *ctx,
                                    uint8_t dev_addr,
                                    const uint8_t *tx, size_t tx_len,
                                    uint8_t *rx, size_t rx_len)
{
    ads1115_esp32_ctx_t *io = (ads1115_esp32_ctx_t *)ctx;
    esp_err_t err;
    int timeout_ms;

    (void)dev_addr; /* 从机地址已绑定在 i2c_master_dev_handle_t 内，此处无需再用 */

    if ((io == NULL) || (io->dev == NULL)) {
        return ADS1115_ERR_PARAM;
    }

    timeout_ms = (int)io->timeout_ms;

    if ((rx != NULL) && (rx_len > 0u)) {
        err = i2c_master_transmit_receive(io->dev, tx, tx_len, rx, rx_len, timeout_ms);
    } else {
        err = i2c_master_transmit(io->dev, tx, tx_len, timeout_ms);
    }

    return (err == ESP_OK) ? ADS1115_OK : ADS1115_ERR_IO;
}

/** 毫秒延时：至少让出 1 个 tick，避免 tick=100Hz 时 1ms 变成不延时 */
static void esp32_delay_ms(void *ctx, uint32_t ms)
{
    TickType_t ticks;

    (void)ctx;

    ticks = pdMS_TO_TICKS(ms);
    if (ticks == 0u) {
        ticks = 1u;
    }
    vTaskDelay(ticks);
}

/** 单调毫秒时基，基于 esp_timer 高精度计数器；uint32 回绕由差值比较天然处理 */
static uint32_t esp32_now_ms(void *ctx)
{
    (void)ctx;

    return (uint32_t)(esp_timer_get_time() / 1000LL);
}

/* ------------------------------------------------------------------ */
/* 生命周期                                                            */
/* ------------------------------------------------------------------ */

ads1115_err_t ads1115_esp32_attach(ads1115_t *dev,
                                   i2c_master_bus_handle_t bus,
                                   ads1115_esp32_ctx_t *ctx,
                                   uint8_t i2c_addr)
{
    i2c_device_config_t  dev_cfg;
    ads1115_port_t       port;
    ads1115_err_t        ret;
    esp_err_t            err;

    if ((dev == NULL) || (bus == NULL) || (ctx == NULL)) {
        return ADS1115_ERR_PARAM;
    }

    if (ctx->scl_speed_hz == 0u) {
        ctx->scl_speed_hz = ADS1115_ESP32_DEFAULT_SCL_HZ;
    }
    if (ctx->timeout_ms == 0u) {
        ctx->timeout_ms = ADS1115_ESP32_DEFAULT_TIMEOUT_MS;
    }

    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = i2c_addr;
    dev_cfg.scl_speed_hz    = ctx->scl_speed_hz;

    err = i2c_master_bus_add_device(bus, &dev_cfg, &ctx->dev);
    if (err != ESP_OK) {
        ctx->dev = NULL;
        return ADS1115_ERR_IO;
    }
    ctx->bus = bus;

    port.transfer = esp32_transfer;
    port.delay_ms = esp32_delay_ms;
    port.now_ms   = esp32_now_ms;
    port.ctx      = ctx;

    ret = ads1115_init(dev, &port, i2c_addr);
    if (ret != ADS1115_OK) {
        (void)i2c_master_bus_rm_device(ctx->dev);
        ctx->dev = NULL;
    }

    return ret;
}

ads1115_err_t ads1115_esp32_detach(ads1115_t *dev, ads1115_esp32_ctx_t *ctx)
{
    esp_err_t err;

    if ((dev == NULL) || (ctx == NULL) || (ctx->dev == NULL)) {
        return ADS1115_ERR_PARAM;
    }

    err = i2c_master_bus_rm_device(ctx->dev);
    ctx->dev   = NULL;
    dev->inited = false;

    return (err == ESP_OK) ? ADS1115_OK : ADS1115_ERR_IO;
}
