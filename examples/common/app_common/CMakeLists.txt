idf_component_register(
    SRC_DIRS "src"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_wifi esp_driver_touch_sens esp_lcd console esp_driver_i2c
    EMBED_FILES "${CMAKE_CURRENT_LIST_DIR}/../assets/audio/wakeup.mp3"
    EMBED_FILES "${CMAKE_CURRENT_LIST_DIR}/../assets/audio/finish_reminder.mp3"
    EMBED_FILES "${CMAKE_CURRENT_LIST_DIR}/../assets/audio/wakeup_end.mp3"
    EMBED_FILES "${CMAKE_CURRENT_LIST_DIR}/../assets/audio/quota_exceeded.mp3"
    EMBED_FILES "${CMAKE_CURRENT_LIST_DIR}/../assets/audio/server_disconnect.mp3"
    EMBED_FILES "${CMAKE_CURRENT_LIST_DIR}/../assets/audio/agent_not_configured.mp3"
    EMBED_FILES "${CMAKE_CURRENT_LIST_DIR}/../assets/audio/wifi_connect_success.mp3"
    EMBED_FILES "${CMAKE_CURRENT_LIST_DIR}/../assets/audio/wifi_connect_error.mp3"
    EMBED_FILES "${CMAKE_CURRENT_LIST_DIR}/../assets/audio/wifi_disconnect.mp3"
    EMBED_FILES "${CMAKE_CURRENT_LIST_DIR}/../assets/audio/server_connecting.mp3"
    EMBED_FILES "${CMAKE_CURRENT_LIST_DIR}/../assets/audio/server_connected.mp3"
    EMBED_FILES "${CMAKE_CURRENT_LIST_DIR}/../assets/audio/low_power.mp3"
    EMBED_FILES "${CMAKE_CURRENT_LIST_DIR}/../assets/audio/scan_qrcode.mp3"
)
