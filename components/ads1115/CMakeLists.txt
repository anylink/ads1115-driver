# ADS1115 核心驱动组件
#
# 刻意不声明任何 REQUIRES / PRIV_REQUIRES：
# 核心层只用 C 标准库，连 esp_err_t 都不认识。
# 这样它也能被直接丢进 STM32 / Linux / Arduino 工程复用。
idf_component_register(
    SRCS "ads1115.c"
    INCLUDE_DIRS "include"
)
