idf_component_register(
    SRCS "i2s.c"
    INCLUDE_DIRS include
    REQUIRES mxr_malloc esp8266 freertos log
)
