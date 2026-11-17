# AirDrop receive boot window for esp32-photoframe.
#
# ESP32Drop's receive path is the AWDL port plus the AirDrop port; its codecs are
# header-only. The sender/BLE files are intentionally not compiled (they are a
# different build of the library, and this firmware never sends).
set(ESP32DROP_DIR "${CMAKE_CURRENT_LIST_DIR}/../../third_party/ESP32Drop/ESP32Drop")

idf_component_register(
    SRCS
        "airdrop_bridge.cpp"
        "${ESP32DROP_DIR}/src/awdl/port/awdl_port_esp32.cpp"
        "${ESP32DROP_DIR}/src/airdrop/port/ad_port_esp32.cpp"
    INCLUDE_DIRS
        "include"
        "${ESP32DROP_DIR}/src"
        "${ESP32DROP_DIR}/src/awdl"
        "${ESP32DROP_DIR}/src/awdl/core"
        "${ESP32DROP_DIR}/src/awdl/port"
        "${ESP32DROP_DIR}/src/airdrop"
        "${ESP32DROP_DIR}/src/airdrop/core"
        "${ESP32DROP_DIR}/src/airdrop/port"
    REQUIRES
        airdrop_compat
        board_hal
        esp_driver_gpio
        esp_event
        esp_netif
        esp_timer
        esp_wifi
        freertos
        lwip
        mbedtls
        nvs_flash
)

# ESP32Drop is written for the Arduino ESP32 core but uses ESP-IDF APIs; the
# shim supplies the Arduino names its ports include.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-parameter)
