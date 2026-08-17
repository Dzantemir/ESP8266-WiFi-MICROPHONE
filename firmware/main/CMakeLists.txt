idf_component_register(
    SRCS "main.c" "pipeline.c" "supervisor.c" "stream_control.c" "wifi_sta.c" "svc_port.c" "at_cmd.c" "at_handlers.c" "config_mgr.c" "i2s_capture.c" "agc.c" "adpcm_encoder.c" "tpdf_dither.c" "udp_stream.c" "tcp_stream.c" "rawtx_stream.c" "stream_mode.c" "socket_util.c" "battery.c"
    INCLUDE_DIRS include
)

# ===================================================================
#  Kconfig cross-validation (build-time checks)
# ===================================================================

# ---- WiFi password: empty (open AP) or 8-63 chars (WPA2) ----
string(LENGTH "${CONFIG_STREAMER_WIFI_PASSWORD}" _wifi_pwd_len)
if(_wifi_pwd_len GREATER 0 AND (_wifi_pwd_len LESS 8 OR _wifi_pwd_len GREATER 63))
    message(WARNING
        "STREAMER_WIFI_PASSWORD length ${_wifi_pwd_len} is invalid. "
        "Must be 0 (open network) or 8-63 characters (WPA2 requirement).")
endif()

# ---- WiFi SSID: 1-32 chars (802.11 limit) ----
string(LENGTH "${CONFIG_STREAMER_WIFI_SSID}" _wifi_ssid_len)
if(_wifi_ssid_len LESS 1 OR _wifi_ssid_len GREATER 32)
    message(WARNING
        "STREAMER_WIFI_SSID length ${_wifi_ssid_len} is invalid. "
        "Must be 1-32 characters (802.11 SSID limit).")
endif()

# ---- WiFi hostname: 1-23 chars (24-byte field with NUL) ----
string(LENGTH "${CONFIG_STREAMER_WIFI_HOSTNAME}" _hostname_len)
if(_hostname_len LESS 1 OR _hostname_len GREATER 23)
    message(WARNING
        "STREAMER_WIFI_HOSTNAME length ${_hostname_len} is invalid. "
        "Must be 1-23 characters (24-byte field w/ NUL).")
endif()

# ---- Reconnect backoff: MIN <= MAX ----
if(CONFIG_STREAMER_WIFI_RECONNECT_BACKOFF_MIN_MS GREATER
   CONFIG_STREAMER_WIFI_RECONNECT_BACKOFF_MAX_MS)
    message(WARNING
        "STREAMER_WIFI_RECONNECT_BACKOFF_MIN_MS "
        "(${CONFIG_STREAMER_WIFI_RECONNECT_BACKOFF_MIN_MS}) must be <= "
        "STREAMER_WIFI_RECONNECT_BACKOFF_MAX_MS "
        "(${CONFIG_STREAMER_WIFI_RECONNECT_BACKOFF_MAX_MS})")
endif()

# ---- Announce interval: MIN <= MAX ----
if(CONFIG_STREAMER_SVC_ANNOUNCE_ENABLED)
    if(CONFIG_STREAMER_SVC_ANNOUNCE_MIN_MS GREATER
       CONFIG_STREAMER_SVC_ANNOUNCE_MAX_MS)
        message(WARNING
            "STREAMER_SVC_ANNOUNCE_MIN_MS "
            "(${CONFIG_STREAMER_SVC_ANNOUNCE_MIN_MS}) must be <= "
            "STREAMER_SVC_ANNOUNCE_MAX_MS "
            "(${CONFIG_STREAMER_SVC_ANNOUNCE_MAX_MS})")
    endif()
endif()

# ---- Battery: CRITICAL < START ----
if(CONFIG_STREAMER_BATTERY_ENABLED)
    if(CONFIG_STREAMER_BATT_CRITICAL_MV GREATER_EQUAL
       CONFIG_STREAMER_BATT_START_MV)
        message(WARNING
            "STREAMER_BATT_CRITICAL_MV (${CONFIG_STREAMER_BATT_CRITICAL_MV}) "
            "must be < STREAMER_BATT_START_MV "
            "(${CONFIG_STREAMER_BATT_START_MV})")
    endif()
    if(CONFIG_STREAMER_BATT_CRITICAL_MV GREATER_EQUAL 4200)
        message(WARNING
            "STREAMER_BATT_CRITICAL_MV (${CONFIG_STREAMER_BATT_CRITICAL_MV}) "
            "must be < 4200 (battery_get_percent would underflow)")
    endif()
endif()

# ---- Stream stop timeout > UDP send timeout ----
if(CONFIG_STREAMER_UDP_SEND_TIMEOUT_MS_ENABLED)
    if(CONFIG_STREAMER_STREAM_STOP_TIMEOUT_UDP_MS LESS_EQUAL
       CONFIG_STREAMER_UDP_SEND_TIMEOUT_MS)
        message(WARNING
            "STREAMER_STREAM_STOP_TIMEOUT_UDP_MS "
            "(${CONFIG_STREAMER_STREAM_STOP_TIMEOUT_UDP_MS}) must be > "
            "STREAMER_UDP_SEND_TIMEOUT_MS "
            "(${CONFIG_STREAMER_UDP_SEND_TIMEOUT_MS}). "
            "Otherwise force-deleting a task blocked in sendto() "
            "corrupts lwIP state.")
    endif()
endif()

# ---- Stream stop timeout > TCP worst-case send time ----
# TCP_EAGAIN_MAX_RETRIES = 0 (hardcoded in board_network.h)
# Worst-case send hold = SEND_TIMEOUT * (RETRIES + 1) = SEND_TIMEOUT * 1
if(CONFIG_STREAMER_TCP_SEND_TIMEOUT_MS_ENABLED)
    set(_tcp_worst_send ${CONFIG_STREAMER_TCP_SEND_TIMEOUT_MS})
    if(CONFIG_STREAMER_STREAM_STOP_TIMEOUT_TCP_MS LESS_EQUAL ${_tcp_worst_send})
        message(WARNING
            "STREAMER_STREAM_STOP_TIMEOUT_TCP_MS "
            "(${CONFIG_STREAMER_STREAM_STOP_TIMEOUT_TCP_MS}) must be > "
            "TCP_SEND_TIMEOUT_MS * (TCP_EAGAIN_MAX_RETRIES + 1) = "
            "${_tcp_worst_send}. Otherwise stop_streaming() force-deletes "
            "the TX task while it still holds s_client_mutex.")
    endif()
endif()

# ---- Task priority sanity (warnings, not errors) ----
if(NOT CONFIG_STREAMER_TASK_PRIO_I2S GREATER CONFIG_STREAMER_TASK_PRIO_ADPCM)
    message(WARNING
        "STREAMER_TASK_PRIO_I2S (${CONFIG_STREAMER_TASK_PRIO_I2S}) should be > "
        "STREAMER_TASK_PRIO_ADPCM (${CONFIG_STREAMER_TASK_PRIO_ADPCM}). "
        "I2S must be highest priority or DMA buffers overflow.")
endif()
if(NOT CONFIG_STREAMER_TASK_PRIO_ADPCM GREATER CONFIG_STREAMER_TASK_PRIO_UDP)
    message(WARNING
        "STREAMER_TASK_PRIO_ADPCM (${CONFIG_STREAMER_TASK_PRIO_ADPCM}) should be > "
        "STREAMER_TASK_PRIO_UDP (${CONFIG_STREAMER_TASK_PRIO_UDP}).")
endif()

# ---- SVC watchdog > INFO interval (sanity) ----
if(CONFIG_STREAMER_SVC_WATCHDOG_TIMEOUT_MS LESS_EQUAL
   CONFIG_STREAMER_SVC_INFO_INTERVAL_MS)
    message(WARNING
        "STREAMER_SVC_WATCHDOG_TIMEOUT_MS "
        "(${CONFIG_STREAMER_SVC_WATCHDOG_TIMEOUT_MS}) should be > "
        "STREAMER_SVC_INFO_INTERVAL_MS "
        "(${CONFIG_STREAMER_SVC_INFO_INTERVAL_MS}). "
        "Watchdog shorter than INFO interval makes no sense.")
endif()

# ---- TCP keepalive: idle > interval ----
if(CONFIG_STREAMER_TCP_KEEPALIVE_ENABLED)
    if(CONFIG_STREAMER_TCP_KEEPIDLE LESS_EQUAL CONFIG_STREAMER_TCP_KEEPINTVL)
        message(WARNING
            "STREAMER_TCP_KEEPIDLE (${CONFIG_STREAMER_TCP_KEEPIDLE}) should be > "
            "STREAMER_TCP_KEEPINTVL (${CONFIG_STREAMER_TCP_KEEPINTVL}).")
    endif()
endif()


# Кастомный include/driver/i2s.h ДОЛЖЕН быть раньше SDK-овского
target_include_directories(${COMPONENT_LIB} BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/i2s_custom/include")

