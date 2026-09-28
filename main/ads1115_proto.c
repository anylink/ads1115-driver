/**
 * @file ads1115_proto.c
 * @brief JSON 行协议实现（基于 IDF 自带 cJSON）
 */

#include "ads1115_proto.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

/* ------------------------------------------------------------------ */
/* 命令解析                                                            */
/* ------------------------------------------------------------------ */

static bool get_u8(const cJSON *root, const char *key, uint8_t max, uint8_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);

    if (!cJSON_IsNumber(item) || (item->valuedouble < 0.0) ||
        (item->valuedouble > (double)max)) {
        return false;
    }
    *out = (uint8_t)item->valuedouble;
    return true;
}

static bool parse_set_ch(const cJSON *root, proto_cmd_msg_t *out)
{
    uint8_t enabled_flag;

    if (!get_u8(root, "ch", PROTO_CHANNEL_COUNT - 1u, &out->ch.ch) ||
        !get_u8(root, "mux", 7u, &out->ch.mux) ||
        !get_u8(root, "pga", 5u, &out->ch.pga) ||
        !get_u8(root, "dr", 7u, &out->ch.dr)) {
        return false;
    }

    /* enabled 允许 0/1 数字或 true/false 布尔 */
    const cJSON *en = cJSON_GetObjectItemCaseSensitive(root, "enabled");

    if (cJSON_IsBool(en)) {
        enabled_flag = cJSON_IsTrue(en) ? 1u : 0u;
    } else if (cJSON_IsNumber(en) &&
               ((en->valuedouble == 0.0) || (en->valuedouble == 1.0))) {
        enabled_flag = (uint8_t)en->valuedouble;
    } else {
        return false;
    }
    out->ch.enabled = (enabled_flag != 0u);
    return true;
}

static void copy_bounded(char *dst, size_t dst_size, const cJSON *item)
{
    const char *src = cJSON_IsString(item) ? item->valuestring : "";
    size_t n = strlen(src);

    if (n >= dst_size) {
        n = dst_size - 1u;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

bool proto_parse_command(const char *line, size_t len, proto_cmd_msg_t *out)
{
    cJSON *root;
    const cJSON *cmd;
    bool ok = false;

    if ((line == NULL) || (out == NULL) || (len == 0u) || (len > 512u)) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    root = cJSON_ParseWithLength(line, len);
    if (root == NULL) {
        return false;
    }

    cmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsString(cmd)) {
        cJSON_Delete(root);
        return false;
    }

    if (strcmp(cmd->valuestring, "ping") == 0) {
        out->cmd = PROTO_CMD_PING;   /* hello 回执：无需应答 */
        ok = true;
    } else if (strcmp(cmd->valuestring, "set_ch") == 0) {
        out->cmd = PROTO_CMD_SET_CH;
        ok = parse_set_ch(root, out);
    } else if (strcmp(cmd->valuestring, "set_net") == 0) {
        out->cmd = PROTO_CMD_SET_NET;
        copy_bounded(out->ssid, sizeof(out->ssid),
                     cJSON_GetObjectItemCaseSensitive(root, "ssid"));
        copy_bounded(out->pass, sizeof(out->pass),
                     cJSON_GetObjectItemCaseSensitive(root, "pass"));
        ok = (out->ssid[0] != '\0');
    } else if (strcmp(cmd->valuestring, "reset_cfg") == 0) {
        out->cmd = PROTO_CMD_RESET_CFG;
        ok = true;
    } else if (strcmp(cmd->valuestring, "get_cfg") == 0) {
        out->cmd = PROTO_CMD_GET_CFG;
        ok = true;
    }

    cJSON_Delete(root);
    return ok;
}

const char *proto_cmd_name(proto_cmd_t cmd)
{
    switch (cmd) {
    case PROTO_CMD_PING:       return "ping";
    case PROTO_CMD_SET_CH:     return "set_ch";
    case PROTO_CMD_SET_NET:    return "set_net";
    case PROTO_CMD_RESET_CFG:  return "reset_cfg";
    case PROTO_CMD_GET_CFG:    return "get_cfg";
    default:                   return "none";
    }
}

/* ------------------------------------------------------------------ */
/* 帧构建                                                              */
/* ------------------------------------------------------------------ */

static cJSON *build_base(const char *type)
{
    cJSON *root = cJSON_CreateObject();

    if (root != NULL) {
        cJSON_AddStringToObject(root, "type", type);
    }
    return root;
}

char *proto_build_hello(const proto_ch_cfg_t *cfgs, uint8_t count)
{
    cJSON *root = build_base("hello");
    cJSON *arr;
    char *out;

    if (root == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "fw", PROTO_FW_VERSION);
    arr = cJSON_AddArrayToObject(root, "cfg");

    for (uint8_t i = 0u; i < count; i++) {
        cJSON *item = cJSON_CreateObject();

        if (item == NULL) {
            break;
        }
        cJSON_AddNumberToObject(item, "ch", cfgs[i].ch);
        cJSON_AddNumberToObject(item, "mux", cfgs[i].mux);
        cJSON_AddNumberToObject(item, "pga", cfgs[i].pga);
        cJSON_AddNumberToObject(item, "dr", cfgs[i].dr);
        cJSON_AddBoolToObject(item, "enabled", cfgs[i].enabled);
        cJSON_AddItemToArray(arr, item);
    }

    out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

char *proto_build_ack(const char *cmd_name, bool ok, const char *detail)
{
    cJSON *root = build_base("ack");
    char *out;

    if (root == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "cmd", cmd_name);
    cJSON_AddBoolToObject(root, "ok", ok);
    if (detail != NULL) {
        cJSON_AddStringToObject(root, "detail", detail);
    }
    out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

char *proto_build_event(const char *name, const char *detail)
{
    cJSON *root = build_base("event");
    char *out;

    if (root == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "name", name);
    if (detail != NULL) {
        cJSON_AddStringToObject(root, "detail", detail);
    }
    out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

int proto_build_sample(char *buf, size_t size,
                       int64_t dev_ms, uint8_t ch,
                       int16_t raw, float volt, uint32_t seq)
{
    return snprintf(buf, size,
                    "{\"dev_ms\":%lld,\"ch\":%u,\"raw\":%d,\"volt\":%.6f,"
                    "\"seq\":%lu}",
                    (long long)dev_ms, (unsigned)ch, (int)raw,
                    (double)volt, (unsigned long)seq);
}
