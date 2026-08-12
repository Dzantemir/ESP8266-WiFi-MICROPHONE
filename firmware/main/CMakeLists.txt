idf_component_register(
    SRCS "main.c" "pipeline.c" "supervisor.c" "stream_control.c" "wifi_sta.c" "svc_port.c" "at_cmd.c" "at_handlers.c" "config_mgr.c" "i2s_capture.c" "agc.c" "adpcm_encoder.c" "tpdf_dither.c" "udp_stream.c" "tcp_stream.c" "rawtx_stream.c" "stream_mode.c" "socket_util.c" "battery.c"
    INCLUDE_DIRS include
)

# Кастомный include/driver/i2s.h ДОЛЖЕН быть раньше SDK-овского
target_include_directories(${COMPONENT_LIB} BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/i2s_custom/include")

