idf_component_register(
    SRCS
        "src/homepi_identity.c"
        "src/nvs_manager.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES nvs_flash esp_http_client
    PRIV_REQUIRES mbedtls json esp_app_format
)
