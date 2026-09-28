idf_component_register(
    SRCS "main.c" "ads1115_net.c" "ads1115_cfg.c" "ads1115_proto.c"
    INCLUDE_DIRS "."
    REQUIRES "ads1115" "ads1115_esp32" "driver"
    # WiFi/mDNS/TCP/NVS/JSON 只被网络与配置模块使用，属私有依赖
    PRIV_REQUIRES "nvs_flash" "esp_wifi" "esp_netif" "esp_event" "esp_timer"
                  "lwip" "freertos" "log" "mdns" "json"
)
