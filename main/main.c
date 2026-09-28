/**
 * @file main.c
 * @brief ADS1115 四路分时复用示例 + WiFi 遥测（v2）
 *
 * 组装层（composition root）：拥有 ads1115 驱动实例与配置模型，
 *   1. 启动时从 NVS 加载通道配置（或出厂默认）并同步到驱动；
 *   2. 主循环推进采样状态机，把新采样以 v2 协议（raw+volt）推给网络层；
 *   3. 消费配置请求队列：主机下发的通道命令在这里应用（驱动的唯一写者）；
 *   4. 上电按住 BOOT 键（GPIO9）可清除 NVS 恢复出厂。
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "ads1115.h"
#include "ads1115_esp32.h"
#include "ads1115_proto.h"
#include "ads1115_cfg.h"
#include "ads1115_net.h"

static const char *TAG = "ads1115_demo";

/*
 * I2C 引脚：按芯片型号给一份默认值，改这里即可。
 */
#if CONFIG_IDF_TARGET_ESP32
#  define APP_I2C_SDA_GPIO 21
#  define APP_I2C_SCL_GPIO 22
#elif CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32C3
#  define APP_I2C_SDA_GPIO 8
#  define APP_I2C_SCL_GPIO 9
#elif CONFIG_IDF_TARGET_ESP32C6 || CONFIG_IDF_TARGET_ESP32H2
#  define APP_I2C_SDA_GPIO 6
#  define APP_I2C_SCL_GPIO 7
#else
#  define APP_I2C_SDA_GPIO 8
#  define APP_I2C_SCL_GPIO 9
#endif

/* 恢复出厂：上电时按住开发板 BOOT 键（ESP32-C6 = GPIO9） */
#define APP_FACTORY_GPIO 9

#define APP_I2C_PORT        I2C_NUM_0
#define APP_I2C_SCL_HZ      400000u
#define APP_I2C_TIMEOUT_MS  100u
#define APP_POLL_PERIOD_MS  5u
#define APP_DUMP_PERIOD_MS  500u

/* ------------------------------------------------------------------ */
/* 配置 → 驱动 同步（换算恒为单位映射，工程值在主机侧计算）              */
/* ------------------------------------------------------------------ */

static void apply_channel_to_driver(ads1115_t *adc, uint8_t idx)
{
    app_ch_cfg_t c;
    ads1115_chan_cfg_t d;

    if ((adc == NULL) || !cfg_get_channel(idx, &c)) {
        return;
    }
    d.mux     = (ads1115_mux_t)c.mux;
    d.pga     = (ads1115_pga_t)c.pga;
    d.dr      = (ads1115_dr_t)c.dr;
    d.enabled = c.enabled;
    d.scale   = 1.0f;
    d.offset  = 0.0f;
    (void)ads1115_set_channel(adc, idx, &d);
}

static void sync_all_channels(ads1115_t *adc)
{
    for (uint8_t i = 0u; i < PROTO_CHANNEL_COUNT; i++) {
        apply_channel_to_driver(adc, i);
    }
}

/* ------------------------------------------------------------------ */
/* 主机命令应用（在采样主循环上下文执行——驱动的唯一写者）                */
/* ------------------------------------------------------------------ */

static void send_ack(proto_cmd_t cmd, bool ok, const char *detail)
{
    char *ack = proto_build_ack(proto_cmd_name(cmd), ok, detail);

    if (ack != NULL) {
        (void)net_send_line(ack);
        free(ack);
    }
}

static void process_cfg_requests(ads1115_t *adc)
{
    proto_cmd_msg_t msg;

    while (xQueueReceive(cfg_request_queue(), &msg, 0) == pdTRUE) {
        switch (msg.cmd) {
        case PROTO_CMD_SET_CH:
            cfg_set_channel(&msg.ch);
            apply_channel_to_driver(adc, msg.ch.ch);
            ESP_LOGI(TAG, "应用通道配置 CH%u: mux=%u pga=%u dr=%u en=%d",
                     msg.ch.ch, msg.ch.mux, msg.ch.pga, msg.ch.dr, msg.ch.enabled);
            send_ack(PROTO_CMD_SET_CH, true, NULL);
            break;

        case PROTO_CMD_RESET_CFG:
            if (cfg_reset_defaults()) {
                sync_all_channels(adc);
                send_ack(PROTO_CMD_RESET_CFG, true, NULL);
            } else {
                send_ack(PROTO_CMD_RESET_CFG, false, "nvs write failed");
            }
            break;

        case PROTO_CMD_GET_CFG: {
            proto_ch_cfg_t view[PROTO_CHANNEL_COUNT];

            for (uint8_t i = 0u; i < PROTO_CHANNEL_COUNT; i++) {
                app_ch_cfg_t c;

                (void)cfg_get_channel(i, &c);
                view[i].ch      = i;
                view[i].mux     = c.mux;
                view[i].pga     = c.pga;
                view[i].dr      = c.dr;
                view[i].enabled = c.enabled;
            }
            char *hello = proto_build_hello(view, PROTO_CHANNEL_COUNT);
            if (hello != NULL) {
                (void)net_send_line(hello);
                free(hello);
            }
            break;
        }

        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 采样推送：按 seq 去重，只推新结果                                    */
/* ------------------------------------------------------------------ */

static void push_new_samples(ads1115_t *adc)
{
    static uint32_t last_seq[PROTO_CHANNEL_COUNT];

    for (uint8_t i = 0u; i < PROTO_CHANNEL_COUNT; i++) {
        const ads1115_result_t *r = ads1115_get_result(adc, i);

        if ((r == NULL) || !r->valid || (r->seq == last_seq[i])) {
            continue;
        }
        last_seq[i] = r->seq;
        net_push_sample(esp_timer_get_time() / 1000LL, i,
                        r->raw, r->volt, r->seq);
    }
}

/* ------------------------------------------------------------------ */
/* 串口调试输出                                                        */
/* ------------------------------------------------------------------ */

static void dump_results(const ads1115_t *adc)
{
    for (uint8_t i = 0u; i < ADS1115_CHANNEL_COUNT; i++) {
        const ads1115_result_t *r = ads1115_get_result(adc, i);

        if ((r == NULL) || !r->valid) {
            continue;
        }
        ESP_LOGI(TAG, "CH%u  raw=%6d  volt=%9.4f mV  seq=%lu",
                 (unsigned)i, (int)r->raw,
                 (double)(r->volt * 1000.0f), (unsigned long)r->seq);
    }
}

/* ------------------------------------------------------------------ */
/* 恢复出厂：上电按住 BOOT 键清除全部 NVS                               */
/* ------------------------------------------------------------------ */

static bool factory_button_held(void)
{
    const gpio_num_t pin = (gpio_num_t)APP_FACTORY_GPIO;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    (void)gpio_config(&io);

    for (int i = 0; i < 5; i++) {
        if (gpio_get_level(pin) != 0u) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

/* ------------------------------------------------------------------ */

static bool i2c_bus_init(i2c_master_bus_handle_t *out_bus)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = APP_I2C_PORT,
        .sda_io_num        = (gpio_num_t)APP_I2C_SDA_GPIO,
        .scl_io_num        = (gpio_num_t)APP_I2C_SCL_GPIO,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    return (i2c_new_master_bus(&bus_cfg, out_bus) == ESP_OK);
}

void app_main(void)
{
    i2c_master_bus_handle_t bus = NULL;
    static ads1115_esp32_ctx_t io_ctx;   /* static：生命周期覆盖整个运行期 */
    static ads1115_t adc;
    uint32_t last_dump_ms = 0u;

    /* NVS 初始化 + 恢复出厂（按住 BOOT 上电） */
    esp_err_t nvs_err = nvs_flash_init();
    if ((nvs_err == ESP_ERR_NVS_NO_FREE_PAGES) ||
        (nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    bool factory = factory_button_held();
    if (factory) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
        ESP_LOGW(TAG, "检测到 BOOT 键按下：已恢复出厂设置");
    }

    if (!i2c_bus_init(&bus)) {
        ESP_LOGE(TAG, "I2C bus init failed");
        return;
    }

    io_ctx.scl_speed_hz = APP_I2C_SCL_HZ;
    io_ctx.timeout_ms   = APP_I2C_TIMEOUT_MS;

    if (ads1115_esp32_attach(&adc, bus, &io_ctx, ADS1115_I2C_ADDR_MIN) != ADS1115_OK) {
        ESP_LOGE(TAG, "ads1115 attach failed, check wiring / ADDR pin");
        return;
    }

    cfg_init(factory);
    sync_all_channels(&adc);

    net_start();
    ESP_LOGI(TAG, "init ok, enabled_mask = 0x%02X (bit n = channel n)", adc.enabled_mask);

    while (true) {
        ads1115_err_t poll_err = ads1115_poll(&adc);

        if ((poll_err != ADS1115_OK) && (poll_err != ADS1115_ERR_NO_ENABLED)) {
            ESP_LOGW(TAG, "poll err = %d", (int)poll_err);
        }

        push_new_samples(&adc);
        process_cfg_requests(&adc);

        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if ((now_ms - last_dump_ms) >= APP_DUMP_PERIOD_MS) {
            last_dump_ms = now_ms;
            dump_results(&adc);
        }

        vTaskDelay(pdMS_TO_TICKS(APP_POLL_PERIOD_MS));
    }
}
