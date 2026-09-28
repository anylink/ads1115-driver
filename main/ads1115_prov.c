/**
 * @file ads1115_prov.c
 * @brief SoftAP 配网实现：热点 + 极简配网页（GET 表单，无需任何 App）
 *
 * 热点：SSID "ADS1115-Setup"，密码 "12345678"，网关 192.168.4.1
 * 页面：浏览器打开 http://192.168.4.1 → 表单提交 /save?ssid=..&pass=..
 *       → URL 解码 → 经 apply_fn 回调交回 net 层应用
 */

#include "ads1115_prov.h"

#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"

#define PROV_TAG          "ads1115_prov"
#define PROV_AP_SSID      "ADS1115-Setup"
#define PROV_AP_PASS      "12345678"
#define PROV_AP_CHANNEL   6
#define PROV_MAX_STA      4

static prov_apply_fn_t s_apply_fn;
static httpd_handle_t  s_httpd;
static esp_netif_t    *s_ap_netif;
static volatile bool   s_ap_up;
static volatile bool   s_applied;

/* ------------------------------------------------------------------ */
/* URL 解码（%XX 与 '+'）                                              */
/* ------------------------------------------------------------------ */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void url_decode(const char *src, char *dst, size_t dst_size)
{
    size_t o = 0;
    for (size_t i = 0; src[i] != '\0' && o + 1 < dst_size; i++) {
        if (src[i] == '%' && hex_val(src[i + 1]) >= 0 && hex_val(src[i + 2]) >= 0) {
            dst[o++] = (char)((hex_val(src[i + 1]) << 4) | hex_val(src[i + 2]));
            i += 2;
        } else if (src[i] == '+') {
            dst[o++] = ' ';
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* HTTP 处理                                                           */
/* ------------------------------------------------------------------ */

static const char PAGE[] =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ADS1115 WiFi 配网</title></head>"
    "<body style='font-family:sans-serif;max-width:420px;margin:24px auto'>"
    "<h3>ADS1115 WiFi 配网</h3>"
    "<p style='color:#888'>填写设备要连接的 WiFi（2.4G），保存后设备将自动"
    "连接并回到上位机。若 60 秒内未连上，热点会重新开启。</p>"
    "<form action='/save' method='get'>"
    "<label>WiFi 名称<br>"
    "<input name='ssid' maxlength='32' style='width:100%;padding:8px'>"
    "</label><br><br>"
    "<label>WiFi 密码<br>"
    "<input name='pass' type='password' maxlength='64' "
    "style='width:100%;padding:8px'></label><br><br>"
    "<button style='width:100%;padding:12px'>保存并连接</button>"
    "</form></body></html>";

static esp_err_t page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_handler(httpd_req_t *req)
{
    char query[256] = { 0 };
    char raw[80] = { 0 };
    char ssid[33] = { 0 };
    char pass[65] = { 0 };

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "ssid", raw, sizeof(raw)) == ESP_OK) {
            url_decode(raw, ssid, sizeof(ssid));
        }
        memset(raw, 0, sizeof(raw));
        if (httpd_query_key_value(query, "pass", raw, sizeof(raw)) == ESP_OK) {
            url_decode(raw, pass, sizeof(pass));
        }
    }

    if (ssid[0] == '\0') {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "<meta charset='utf-8'>WiFi 名称不能为空，请返回重填。");
        return ESP_OK;
    }

    ESP_LOGI(PROV_TAG, "配网页提交：ssid=\"%s\"", ssid);
    if (s_apply_fn != NULL) {
        s_apply_fn(ssid, pass);
        s_applied = true;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    const char *ok =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "</head><body style='font-family:sans-serif;max-width:420px;"
        "margin:24px auto'><h3>已保存 ✓</h3>"
        "<p>设备正在连接该 WiFi。成功后热点会关闭，设备将自动连接上位机；"
        "若 60 秒内未连上，本热点会重新开启，可返回重试。</p></body></html>";
    return httpd_resp_send(req, ok, HTTPD_RESP_USE_STRLEN);
}

static httpd_uri_t uri_page = { .uri = "/", .method = HTTP_GET,
                                .handler = page_handler };
static httpd_uri_t uri_save = { .uri = "/save", .method = HTTP_GET,
                                .handler = save_handler };

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

void prov_start(prov_apply_fn_t apply_fn)
{
    if (s_ap_up) {
        return;   /* 已在配网模式，幂等 */
    }
    s_apply_fn = apply_fn;
    s_applied = false;

    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    esp_wifi_set_mode(WIFI_MODE_AP);

    wifi_config_t ap_cfg = { 0 };
    strncpy((char *)ap_cfg.ap.ssid, PROV_AP_SSID, sizeof(ap_cfg.ap.ssid) - 1u);
    ap_cfg.ap.ssid_len = (uint8_t)strlen(PROV_AP_SSID);
    strncpy((char *)ap_cfg.ap.password, PROV_AP_PASS,
            sizeof(ap_cfg.ap.password) - 1u);
    ap_cfg.ap.channel = PROV_AP_CHANNEL;
    ap_cfg.ap.max_connection = PROV_MAX_STA;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    httpd_config_t httpd_cfg = HTTPD_DEFAULT_CONFIG();
    httpd_cfg.max_uri_handlers = 4;
    if (httpd_start(&s_httpd, &httpd_cfg) == ESP_OK) {
        httpd_register_uri_handler(s_httpd, &uri_page);
        httpd_register_uri_handler(s_httpd, &uri_save);
    } else {
        ESP_LOGE(PROV_TAG, "httpd 启动失败");
    }

    s_ap_up = true;
    ESP_LOGW(PROV_TAG, "配网热点已开启：SSID=\"%s\" 密码=\"%s\"，"
             "连接后浏览器打开 http://192.168.4.1",
             PROV_AP_SSID, PROV_AP_PASS);
}

void prov_stop(void)
{
    if (!s_ap_up) {
        return;
    }
    if (s_httpd != NULL) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    esp_wifi_stop();   /* 停 AP；随后 net 层会切回 STA 并连接 */
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    s_ap_up = false;
    ESP_LOGI(PROV_TAG, "配网热点已关闭");
}

bool prov_was_applied(void)
{
    return s_applied;
}

void prov_reset_applied(void)
{
    s_applied = false;
}
