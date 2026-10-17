idf_component_register(SRCS "clipboard_main.c" "board.c" "ui.c" "link.c"
                            "hub_store.c" "hub.c" "hub_ble.c" "hub_assets.c"
                       INCLUDE_DIRS "."
                       REQUIRES bt esp_timer nvs_flash fatfs json mbedtls
                       EMBED_FILES "ui_assets.bin")
