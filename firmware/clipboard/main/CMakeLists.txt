idf_component_register(SRCS "clipboard_main.c" "board.c" "ui.c" "link.c"
                            "hub_store.c" "hub.c" "hub_ble.c" "hub_assets.c" "screen.c" "pair_crypto.c"
                       INCLUDE_DIRS "."
                       REQUIRES bt esp_timer nvs_flash fatfs json mbedtls esp_driver_ledc
                       EMBED_FILES "ui_assets.bin")
