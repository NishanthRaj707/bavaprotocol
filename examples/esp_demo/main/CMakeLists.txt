# ESP-IDF Component Registration for esp_demo main component
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES bava freertos driver log
)
