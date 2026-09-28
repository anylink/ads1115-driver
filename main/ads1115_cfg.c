/**
 * @file ads1115_cfg.c
 * @brief 配置模型实现：默认值 + NVS 存储 + 请求队列
 */

#include "ads1115_cfg.h"

#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#define CFG_TAG       "ads1115_cfg"
#define CFG_NS        "adscfg"
#define CFG_KEY_BLOB  "chcfg"
#define CFG_KEY_VER   "ver"
#define CFG_VER       1u
#define CFG_REQ_QUEUE_LEN 8u

/** 出厂默认：与产品实际接线一致（CH1 = AIN1 电池分压，使能） */
static const app_ch_cfg_t s_defaults[PROTO_CHANNEL_COUNT] = {
    {0u, 4u, 4u, false},   /* CH0: AIN0-AIN1 差分 ±0.512V 128SPS，禁用 */
    {5u, 2u, 0u, true},    /* CH1: AIN1-GND  ±2.048V  8SPS，  使能（电池） */
    {6u, 1u, 1u, false},   /* CH2: AIN2-GND  ±4.096V 16SPS， 禁用 */
    {5u, 2u, 2u, false},   /* CH3: AIN1-GND  ±2.048V 32SPS， 禁用 */
};

static app_ch_cfg_t  s_cfg[PROTO_CHANNEL_COUNT];
static QueueHandle_t s_req_q;

/* ------------------------------------------------------------------ */
/* NVS                                                                 */
/* ------------------------------------------------------------------ */

static bool cfg_store(void)
{
    nvs_handle_t h;
    bool ok = false;

    if (nvs_open(CFG_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }

    uint8_t blob[PROTO_CHANNEL_COUNT * 4u];

    for (uint8_t i = 0u; i < PROTO_CHANNEL_COUNT; i++) {
        blob[i * 4u + 0u] = s_cfg[i].mux;
        blob[i * 4u + 1u] = s_cfg[i].pga;
        blob[i * 4u + 2u] = s_cfg[i].dr;
        blob[i * 4u + 3u] = s_cfg[i].enabled ? 1u : 0u;
    }

    if ((nvs_set_u8(h, CFG_KEY_VER, CFG_VER) == ESP_OK) &&
        (nvs_set_blob(h, CFG_KEY_BLOB, blob, sizeof(blob)) == ESP_OK) &&
        (nvs_commit(h) == ESP_OK)) {
        ok = true;
    }
    nvs_close(h);

    if (!ok) {
        ESP_LOGW(CFG_TAG, "配置写入 NVS 失败");
    }
    return ok;
}

static bool cfg_load(void)
{
    nvs_handle_t h;
    uint8_t blob[PROTO_CHANNEL_COUNT * 4u];
    uint8_t ver = 0u;
    size_t  len = sizeof(blob);

    if (nvs_open(CFG_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    bool ok = (nvs_get_u8(h, CFG_KEY_VER, &ver) == ESP_OK) &&
              (ver == CFG_VER) &&
              (nvs_get_blob(h, CFG_KEY_BLOB, blob, &len) == ESP_OK) &&
              (len == sizeof(blob));
    nvs_close(h);

    if (!ok) {
        return false;
    }

    for (uint8_t i = 0u; i < PROTO_CHANNEL_COUNT; i++) {
        if (blob[i * 4u + 0u] > 7u || blob[i * 4u + 1u] > 5u ||
            blob[i * 4u + 2u] > 7u || blob[i * 4u + 3u] > 1u) {
            return false;   /* 数据损坏，回退默认 */
        }
        s_cfg[i].mux     = blob[i * 4u + 0u];
        s_cfg[i].pga     = blob[i * 4u + 1u];
        s_cfg[i].dr      = blob[i * 4u + 2u];
        s_cfg[i].enabled = (blob[i * 4u + 3u] != 0u);
    }
    ESP_LOGI(CFG_TAG, "已从 NVS 加载通道配置");
    return true;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

void cfg_init(bool factory)
{
    s_req_q = xQueueCreate(CFG_REQ_QUEUE_LEN, sizeof(proto_cmd_msg_t));
    configASSERT(s_req_q != NULL);

    bool loaded = (!factory) && cfg_load();

    if (!loaded) {
        memcpy(s_cfg, s_defaults, sizeof(s_cfg));
        (void)cfg_store();
        ESP_LOGI(CFG_TAG, "使用出厂默认通道配置");
    }
}

bool cfg_get_channel(uint8_t idx, app_ch_cfg_t *out)
{
    if ((idx >= PROTO_CHANNEL_COUNT) || (out == NULL)) {
        return false;
    }
    *out = s_cfg[idx];
    return true;
}

void cfg_set_channel(const proto_ch_cfg_t *in)
{
    if (in == NULL || in->ch >= PROTO_CHANNEL_COUNT) {
        return;
    }
    s_cfg[in->ch].mux     = in->mux;
    s_cfg[in->ch].pga     = in->pga;
    s_cfg[in->ch].dr      = in->dr;
    s_cfg[in->ch].enabled = in->enabled;
    (void)cfg_store();
}

bool cfg_reset_defaults(void)
{
    memcpy(s_cfg, s_defaults, sizeof(s_cfg));
    return cfg_store();
}

bool cfg_post_request(const proto_cmd_msg_t *msg)
{
    if (xQueueSend(s_req_q, msg, 0) != pdTRUE) {
        ESP_LOGW(CFG_TAG, "请求队列满，丢弃命令 %s", proto_cmd_name(msg->cmd));
        return false;
    }
    return true;
}

QueueHandle_t cfg_request_queue(void)
{
    return s_req_q;
}
