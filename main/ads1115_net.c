/**
 * @file ads1115_net.c
 * @brief 网络层实现：WiFi STA + mDNS + TCP + 配置切换回滚 + 帧收发
 *
 * 结构（单管理者任务模型）：
 *   manager_task:
 *     等 WiFi 就绪 → [有换网请求则先执行 应用/回滚 流程] → mDNS 解析上位机
 *     → TCP 连接 → 收发循环（select 100ms 轮询：收→解析分发，发→清空队列）
 *     → 断开后延时 3s 重来
 *
 * 线程模型：
 *   - 本模块所有状态只被 manager_task 触碰；对外接口经队列通信，天然安全。
 *   - WiFi 换网流程中 s_applying 置位，事件回调不再自动重连，由流程自己控制。
 *   - WiFi 凭据：NVS("adsnet") 优先，缺省用 Kconfig；换网时旧凭据备份于
 *     NVS 的 b_ 前缀键，60s 内拿不到 IP 即回滚。
 */

#include "ads1115_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "mdns.h"
#include "nvs.h"
#include "lwip/sockets.h"

#include "ads1115_proto.h"
#include "ads1115_cfg.h"

#define NET_TAG "ads1115_net"

#define NET_LINE_MAX        256u    /* 单帧缓冲 */
#define NET_TX_QUEUE_LEN    32u
#define NET_RX_BUF          512u
#define NET_CMD_MAX         512u    /* 协议解析上限（与 proto 一致） */
#define NET_TX_STACK        6144u
#define NET_RESOLVE_TIMEOUT 5000u   /* mDNS 单次解析超时 (ms) */
#define NET_RETRY_MS        3000u   /* 断线重连间隔 */
#define NET_SEND_TIMEOUT_S  1
#define NET_SELECT_MS       100     /* 收发循环轮询周期 */
#define NET_APPLY_TIMEOUT_MS 60000u /* 换网后等待 IP 的回滚窗口 */
#define NET_ROLLBACK_WAIT_MS 10000u /* 回滚后等待恢复连接的时间 */

#define WIFI_NS        "adsnet"
#define WIFI_KEY_SSID  "ssid"
#define WIFI_KEY_PASS  "pass"
#define WIFI_KEY_BSSID "b_ssid"
#define WIFI_KEY_BPASS "b_pass"

#define WIFI_UP_BIT BIT0

typedef struct {
    uint16_t len;
    char     buf[NET_LINE_MAX];
} net_line_t;

typedef enum {
    SERVE_ERR = 0,      /* 连接异常，走重连 */
    SERVE_CLOSE,        /* 主动关闭（如换网请求） */
} serve_result_t;

static QueueHandle_t      s_tx_q;
static EventGroupHandle_t s_ev;

/* WiFi 凭据：活动 + 备份（均在 manager 任务内访问） */
static char s_wifi_ssid[PROTO_SSID_MAX + 1u];
static char s_wifi_pass[PROTO_PASS_MAX + 1u];
static char s_bak_ssid[PROTO_SSID_MAX + 1u];
static char s_bak_pass[PROTO_PASS_MAX + 1u];

static volatile bool s_applying;        /* 换网流程中，事件回调不自动重连 */
static volatile bool s_pending_apply;   /* 有待应用的换网请求 */
static volatile bool s_close_requested; /* 收发循环主动退出标志 */
static volatile bool s_host_alive;      /* 本连接内收到过主机命令（hello 已送达） */
static char s_pending_ssid[PROTO_SSID_MAX + 1u];
static char s_pending_pass[PROTO_PASS_MAX + 1u];
static const char *s_event_on_connect;  /* 重连成功后要补发的事件名 */

/* ------------------------------------------------------------------ */
/* WiFi 事件                                                           */
/* ------------------------------------------------------------------ */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;

    if ((base == WIFI_EVENT) && (id == WIFI_EVENT_STA_START)) {
        if (!s_applying) {
            esp_wifi_connect();
        }
    } else if ((base == WIFI_EVENT) && (id == WIFI_EVENT_STA_DISCONNECTED)) {
        xEventGroupClearBits(s_ev, WIFI_UP_BIT);
        if (!s_applying) {
            ESP_LOGW(NET_TAG, "WiFi 断开，正在重连…");
            esp_wifi_connect();
        }
    } else if ((base == IP_EVENT) && (id == IP_EVENT_STA_GOT_IP)) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(NET_TAG, "WiFi 已连接，本机 IP=" IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_ev, WIFI_UP_BIT);
    }
}

static void wifi_apply_credentials(const char *ssid, const char *pass)
{
    wifi_config_t cfg = { 0 };

    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1u);
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1u);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
}

/* ------------------------------------------------------------------ */
/* 凭据存取（NVS）                                                      */
/* ------------------------------------------------------------------ */

static void creds_save_active(const char *ssid, const char *pass)
{
    nvs_handle_t h;

    if (nvs_open(WIFI_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, WIFI_KEY_SSID, ssid);
        nvs_set_str(h, WIFI_KEY_PASS, pass);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void creds_save_backup(void)
{
    nvs_handle_t h;

    if (nvs_open(WIFI_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, WIFI_KEY_BSSID, s_wifi_ssid);
        nvs_set_str(h, WIFI_KEY_BPASS, s_wifi_pass);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void creds_clear_backup(void)
{
    nvs_handle_t h;

    if (nvs_open(WIFI_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, WIFI_KEY_BSSID);
        nvs_erase_key(h, WIFI_KEY_BPASS);
        nvs_commit(h);
        nvs_close(h);
    }
    s_bak_ssid[0] = '\0';
    s_bak_pass[0] = '\0';
}

/** 启动时装载凭据：NVS 优先，否则用 Kconfig 出厂值 */
static void creds_load(void)
{
    nvs_handle_t h;
    char ssid[PROTO_SSID_MAX + 1u] = { 0 };
    char pass[PROTO_PASS_MAX + 1u] = { 0 };
    size_t len_s = sizeof(ssid);
    size_t len_p = sizeof(pass);
    bool loaded = false;

    if (nvs_open(WIFI_NS, NVS_READONLY, &h) == ESP_OK) {
        loaded = (nvs_get_str(h, WIFI_KEY_SSID, ssid, &len_s) == ESP_OK) &&
                 (nvs_get_str(h, WIFI_KEY_PASS, pass, &len_p) == ESP_OK) &&
                 (ssid[0] != '\0');
        nvs_close(h);
    }

    if (!loaded) {
        strncpy(ssid, CONFIG_ADS1115_WIFI_SSID, sizeof(ssid) - 1u);
        strncpy(pass, CONFIG_ADS1115_WIFI_PASS, sizeof(pass) - 1u);
        ESP_LOGI(NET_TAG, "使用编译期默认 WiFi：%s", ssid);
    }
    strncpy(s_wifi_ssid, ssid, sizeof(s_wifi_ssid) - 1u);
    strncpy(s_wifi_pass, pass, sizeof(s_wifi_pass) - 1u);
}

/* ------------------------------------------------------------------ */
/* WiFi 生命周期                                                       */
/* ------------------------------------------------------------------ */

static void wifi_sta_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    (void)esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));

    wifi_apply_credentials(s_wifi_ssid, s_wifi_pass);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ------------------------------------------------------------------ */
/* 换网：应用 / 回滚                                                    */
/* ------------------------------------------------------------------ */

static void apply_pending_net(void)
{
    bool ok = false;

    s_applying = true;
    creds_save_backup();

    ESP_LOGW(NET_TAG, "应用新 WiFi \"%s\"（%us 内失败将回滚）",
             s_pending_ssid, NET_APPLY_TIMEOUT_MS / 1000u);
    xEventGroupClearBits(s_ev, WIFI_UP_BIT);
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));

    strncpy(s_wifi_ssid, s_pending_ssid, sizeof(s_wifi_ssid) - 1u);
    strncpy(s_wifi_pass, s_pending_pass, sizeof(s_wifi_pass) - 1u);
    creds_save_active(s_wifi_ssid, s_wifi_pass);
    wifi_apply_credentials(s_wifi_ssid, s_wifi_pass);
    esp_wifi_connect();

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(NET_APPLY_TIMEOUT_MS);

    while (xTaskGetTickCount() < deadline) {
        if ((xEventGroupGetBits(s_ev) & WIFI_UP_BIT) != 0u) {
            ok = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (ok) {
        creds_clear_backup();               /* 新凭据转正 */
        s_event_on_connect = "net_applied";
        ESP_LOGI(NET_TAG, "新 WiFi 生效");
    } else {
        ESP_LOGW(NET_TAG, "新 WiFi 连接失败，回滚到 \"%s\"", s_bak_ssid);
        strncpy(s_wifi_ssid, s_bak_ssid, sizeof(s_wifi_ssid) - 1u);
        strncpy(s_wifi_pass, s_bak_pass, sizeof(s_wifi_pass) - 1u);
        creds_save_active(s_wifi_ssid, s_wifi_pass);
        creds_clear_backup();
        wifi_apply_credentials(s_wifi_ssid, s_wifi_pass);
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_wifi_connect();

        TickType_t back_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(NET_ROLLBACK_WAIT_MS);

        while (xTaskGetTickCount() < back_deadline) {
            if ((xEventGroupGetBits(s_ev) & WIFI_UP_BIT) != 0u) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        s_event_on_connect = "net_rolled_back";
    }

    s_pending_apply = false;
    s_applying = false;
}

/* ------------------------------------------------------------------ */
/* 上位机发现与 TCP                                                     */
/* ------------------------------------------------------------------ */

static int resolve_host(char *ip_str, size_t len)
{
    esp_ip4_addr_t addr;

    if (mdns_query_a(CONFIG_ADS1115_HOST_NAME, NET_RESOLVE_TIMEOUT, &addr) == ESP_OK) {
        snprintf(ip_str, len, IPSTR, IP2STR(&addr));
        return 0;
    }
    if (CONFIG_ADS1115_HOST_IP_FALLBACK[0] != '\0') {
        snprintf(ip_str, len, "%s", CONFIG_ADS1115_HOST_IP_FALLBACK);
        return 0;
    }
    return -1;
}

static int tcp_connect(const char *ip, int port)
{
    struct sockaddr_in dst;
    struct timeval tv;
    int fd;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &dst.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) != 0) {
        close(fd);
        return -1;
    }

    tv.tv_sec  = NET_SEND_TIMEOUT_S;
    tv.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

static int send_all(int fd, const char *buf, size_t len)
{
    size_t off = 0u;

    while (off < len) {
        int w = send(fd, buf + off, len - off, 0);
        if (w <= 0) {
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 下行命令分发                                                        */
/* ------------------------------------------------------------------ */

static void handle_line(const char *line, size_t len)
{
    proto_cmd_msg_t msg;

    if (!proto_parse_command(line, len, &msg)) {
        return;   /* 坏帧直接忽略 */
    }

    switch (msg.cmd) {
    case PROTO_CMD_PING:
        /* hello 回执：不动作（s_host_alive 已在收到行时置位） */
        break;

    case PROTO_CMD_SET_CH:
    case PROTO_CMD_RESET_CFG:
    case PROTO_CMD_GET_CFG:
        /* 通道类命令转交采样主循环（驱动的所有者）应用并回 ack */
        (void)cfg_post_request(&msg);
        break;

    case PROTO_CMD_SET_NET:
        strncpy(s_pending_ssid, msg.ssid, sizeof(s_pending_ssid) - 1u);
        strncpy(s_pending_pass, msg.pass, sizeof(s_pending_pass) - 1u);
        s_pending_apply = true;
        s_close_requested = true;
        {
            char *ack = proto_build_ack("set_net", true,
                                        "applying, rollback in 60s on failure");
            if (ack != NULL) {
                (void)net_send_line(ack);
                free(ack);
            }
        }
        ESP_LOGW(NET_TAG, "收到换网请求 \"%s\"", msg.ssid);
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* 连接内收发循环                                                       */
/* ------------------------------------------------------------------ */

static serve_result_t serve_connection(int fd)
{
    char    rx[NET_RX_BUF];
    size_t  rxlen = 0u;
    net_line_t item;
    uint32_t ticks = 0u;

    for (;;) {
        fd_set rs;
        struct timeval tv = { .tv_sec = 0, .tv_usec = NET_SELECT_MS * 1000 };

        FD_ZERO(&rs);
        FD_SET(fd, &rs);
        int r = select(fd + 1, &rs, NULL, NULL, &tv);

        if (r < 0) {
            return SERVE_ERR;
        }

        if (r > 0) {
            int n = recv(fd, rx + rxlen, sizeof(rx) - rxlen, 0);
            if (n <= 0) {
                return SERVE_ERR;
            }
            rxlen += (size_t)n;

            size_t start = 0u;
            for (size_t i = 0u; i < rxlen; i++) {
                if (rx[i] == '\n') {
                    s_host_alive = true;   /* 主机能发命令 = hello 已送达 */
                    handle_line(rx + start, i - start);
                    start = i + 1u;
                }
            }
            memmove(rx, rx + start, rxlen - start);
            rxlen -= start;
        }

        while (xQueueReceive(s_tx_q, &item, 0) == pdTRUE) {
            if (send_all(fd, item.buf, item.len) != 0) {
                return SERVE_ERR;
            }
        }

        /* hello 兜底：主机一直没吭声就每秒补投一次 get_cfg（上限 10 次），
         * 覆盖"连接初期 hello 单帧丢失"的竞态窗口 */
        if (!s_host_alive && (ticks < 10u * (1000u / NET_SELECT_MS))) {
            ticks++;
            if ((ticks % (1000u / NET_SELECT_MS)) == 0u) {
                proto_cmd_msg_t hello_req = { .cmd = PROTO_CMD_GET_CFG };
                (void)cfg_post_request(&hello_req);
            }
        }

        if (s_close_requested) {
            s_close_requested = false;
            return SERVE_CLOSE;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 管理者任务                                                           */
/* ------------------------------------------------------------------ */

static void manager_task(void *arg)
{
    (void)arg;

    for (;;) {
        (void)xEventGroupWaitBits(s_ev, WIFI_UP_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

        if (s_pending_apply) {
            apply_pending_net();
        }

        char ip[16];
        if (resolve_host(ip, sizeof(ip)) != 0) {
            ESP_LOGW(NET_TAG, "找不到上位机 %s.local，%us 后重试",
                     CONFIG_ADS1115_HOST_NAME, NET_RETRY_MS / 1000u);
            vTaskDelay(pdMS_TO_TICKS(NET_RETRY_MS));
            continue;
        }

        int fd = tcp_connect(ip, CONFIG_ADS1115_HOST_PORT);
        if (fd < 0) {
            ESP_LOGW(NET_TAG, "连接 %s:%d 失败，%us 后重试",
                     ip, CONFIG_ADS1115_HOST_PORT, NET_RETRY_MS / 1000u);
            vTaskDelay(pdMS_TO_TICKS(NET_RETRY_MS));
            continue;
        }
        ESP_LOGI(NET_TAG, "已连接上位机 %s:%d", ip, CONFIG_ADS1115_HOST_PORT);
        s_host_alive = false;

        /* 连接建立：请主循环推送 hello（配置模型归主循环所有） */
        proto_cmd_msg_t hello_req = { .cmd = PROTO_CMD_GET_CFG };
        (void)cfg_post_request(&hello_req);

        if (s_event_on_connect != NULL) {
            vTaskDelay(pdMS_TO_TICKS(200));   /* 让 hello 先入队 */
            char *ev = proto_build_event(s_event_on_connect, NULL);
            if (ev != NULL) {
                (void)net_send_line(ev);
                free(ev);
            }
            s_event_on_connect = NULL;
        }

        serve_result_t res = serve_connection(fd);
        if (res == SERVE_ERR) {
            /* hello 是单帧，连接初期竞态丢失后设备将永远沉默——
             * 断开前补投一次 get_cfg，重连后必有一份新 hello */
            (void)cfg_post_request(&hello_req);
        }
        close(fd);
        ESP_LOGW(NET_TAG, "与上位机断开，%us 后重连", NET_RETRY_MS / 1000u);
        vTaskDelay(pdMS_TO_TICKS(NET_RETRY_MS));
    }
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

void net_start(void)
{
    s_ev   = xEventGroupCreate();
    s_tx_q = xQueueCreate(NET_TX_QUEUE_LEN, sizeof(net_line_t));
    configASSERT((s_ev != NULL) && (s_tx_q != NULL));

    creds_load();
    wifi_sta_start();

    ESP_ERROR_CHECK(mdns_init());
    (void)mdns_hostname_set("ads1115-device");

    BaseType_t ok = xTaskCreate(manager_task, "ads1115_net", NET_TX_STACK,
                                NULL, 5u, NULL);
    configASSERT(ok == pdPASS);

    ESP_LOGI(NET_TAG, "网络层已启动（协议 v%s）", PROTO_FW_VERSION);
}

bool net_send_line(const char *line)
{
    if (line == NULL) {
        return false;
    }

    net_line_t item;
    size_t n = strlen(line);

    if (n >= NET_LINE_MAX) {
        ESP_LOGW(NET_TAG, "帧超长被丢弃（%u >= %u）: %.32s…",
                 (unsigned)n, NET_LINE_MAX, line);
        return false;
    }
    memcpy(item.buf, line, n);
    item.buf[n] = '\n';
    item.len = n + 1u;

    return xQueueSend(s_tx_q, &item, 0) == pdTRUE;
}

void net_push_sample(int64_t dev_ms, uint8_t ch,
                     int16_t raw, float volt, uint32_t seq)
{
    net_line_t item;

    int n = proto_build_sample(item.buf, sizeof(item.buf) - 1u,
                               dev_ms, ch, raw, volt, seq);
    if (n <= 0) {
        return;
    }
    item.buf[n] = '\n';
    item.len = (uint16_t)(n + 1);
    (void)xQueueSend(s_tx_q, &item, 0);
}
