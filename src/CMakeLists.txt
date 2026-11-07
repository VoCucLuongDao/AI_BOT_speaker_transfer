FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(
    SRCS ${app_sources}
    REQUIRES spiffs driver bt nvs_flash esp-libhelix-mp3 esp_wifi esp_netif esp_event esp_timer
)
