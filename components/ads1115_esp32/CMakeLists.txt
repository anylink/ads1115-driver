# ADS1115 的 ESP-IDF 适配层组件
#
# 依赖只在【这一层】出现：核心组件 ads1115 依旧零依赖。
# REQUIRES 里带上 driver 是因为公开头文件 ads1115_esp32.h 暴露了 i2c_master 类型。
idf_component_register(
    SRCS "ads1115_esp32.c"
    INCLUDE_DIRS "include"
    REQUIRES "ads1115" "driver" "esp_timer"
    PRIV_REQUIRES "freertos"
)
