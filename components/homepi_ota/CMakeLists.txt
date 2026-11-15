idf_component_register(
    SRCS
        "src/homepi_ota.c"
        "src/ota_version.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_https_ota esp_http_client
    PRIV_REQUIRES mbedtls json app_update esp_app_format nvs_flash homepi_identity
    EMBED_TXTFILES "ota_signing_pub.pem"
)
