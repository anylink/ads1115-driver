/**
 * @file ads1115_prov.h
 * @brief SoftAP 配网模块：设备连不上 WiFi 时开启热点，内建网页收集新凭据
 *
 * 流程：net 层 30 s 内拿不到 IP → prov_start(apply_fn)；
 * 用户连接热点 "ADS1115-Setup"（密码 12345678），浏览器打开 192.168.4.1，
 * 提交新的 WiFi 名称/密码 → 经 apply_fn 回调交回 net 层应用；
 * 60 s 内未连上，net 层重新 prov_start（回到热点模式）。
 */

#ifndef ADS1115_PROV_H
#define ADS1115_PROV_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 页面提交新凭据后的回调（net 层实现：保存 + 切 STA 连接） */
typedef void (*prov_apply_fn_t)(const char *ssid, const char *pass);

/** 启动配网热点与内建网页（重复调用幂等） */
void prov_start(prov_apply_fn_t apply_fn);

/** 停止网页与热点 */
void prov_stop(void);

/** 页面是否已提交过新凭据（由 net 层在应用后查询） */
bool prov_was_applied(void);

/** 清除"已提交"标记（重新进入配网前调用） */
void prov_reset_applied(void);

#ifdef __cplusplus
}
#endif

#endif /* ADS1115_PROV_H */
