idf_component_register(
    SRCS "main.c" "settings.c" "motion.c" "cmd.c" "io.c" "net.c" "web.c" "led.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer esp_rom nvs_flash esp_wifi esp_netif esp_event
             esp_http_server spiffs lwip esp_system
    EMBED_TXTFILES "index.html")
