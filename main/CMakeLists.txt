idf_component_register(
    SRCS "TTS.c" "I2S.c" "main.c" "WiFi.c" "BaiduTTS.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        nvs_flash
        LED
        esp-sr
        esp_timer
        esp_http_client
        esp_wifi
        esp_netif
        mbedtls
        cjson
)
