# ============================================================
#  1. Select source files based on integration mode
# ============================================================
set(MXR_SRCS "mxr_malloc.c")

if(CONFIG_MXR_INTEGRATION_WRAP)
    list(APPEND MXR_SRCS "mxr_heap_wrap.c")
elseif(CONFIG_MXR_INTEGRATION_COMPAT)
    list(APPEND MXR_SRCS "mxr_heap_compat.c")
elseif(CONFIG_MXR_INTEGRATION_PORT)
    list(APPEND MXR_SRCS "mxr_heap_compat.c" "mxr_heap_port.c")
endif()

idf_component_register(
    SRCS ${MXR_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES
        log
        newlib
        freertos
        esp8266
)

# ============================================================
#  2. Compatibility warning
# ============================================================
if(CONFIG_MXR_WARN_HEAP_TRACING AND CONFIG_HEAP_TRACING AND CONFIG_MXR_INTEGRATION_WRAP)
    message(WARNING
        "MxR-malloc: CONFIG_HEAP_TRACING is enabled. "
        "Original heap tracing is not compatible with MxR wrap mode. "
        "Disable CONFIG_HEAP_TRACING or disable this warning."
    )
endif()

# ============================================================
#  3. Validate MXR_REGION_CONFIG at configure time (DRAM)
# ============================================================
set(_MXR_CFG "${CONFIG_MXR_REGION_CONFIG}")
if(_MXR_CFG)
    string(REPLACE "," ";" _MXR_ENTRIES "${_MXR_CFG}")
    list(LENGTH _MXR_ENTRIES _MXR_DRAM_COUNT)
    foreach(_entry ${_MXR_ENTRIES})
        string(STRIP "${_entry}" _entry)
        string(REGEX MATCH "^[0-9]+-[0-9]+%$" _VALID "${_entry}")
        if(NOT _VALID)
            message(FATAL_ERROR
                "MxR-malloc: Invalid DRAM region entry '${_entry}' in "
                "CONFIG_MXR_REGION_CONFIG='${CONFIG_MXR_REGION_CONFIG}'. "
                "Expected format: <bytes>-<percent>%")
        endif()
    endforeach()
    if(_MXR_DRAM_COUNT GREATER 32)
        message(FATAL_ERROR
            "MxR-malloc: Too many DRAM regions (${_MXR_DRAM_COUNT}). "
            "Maximum is 32.")
    endif()
    # FIX(bug#4): первая граница должна быть <= 4 (MXR_ALIGN_SIZE),
    # иначе блоки размером 4..(min-1) не попадут ни в один регион.
    list(GET _MXR_ENTRIES 0 _MXR_FIRST_ENTRY)
    string(STRIP "${_MXR_FIRST_ENTRY}" _MXR_FIRST_ENTRY)
    if(_MXR_FIRST_ENTRY MATCHES "^([0-9]+)-")
        if(CMAKE_MATCH_1 GREATER 4)
            message(FATAL_ERROR
                "MxR-malloc: first DRAM region boundary is ${CMAKE_MATCH_1}, "
                "must be <= 4. Blocks smaller than ${CMAKE_MATCH_1} bytes "
                "would be unallocatable.")
        endif()
    endif()
    set(_MXR_PCT_SUM 0)
    foreach(_entry ${_MXR_ENTRIES})
        string(REGEX MATCH "^[0-9]+-([0-9]+)%$" _MATCH "${_entry}")
        if(_MATCH)
            math(EXPR _MXR_PCT_SUM "${_MXR_PCT_SUM} + ${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(_MXR_PCT_SUM GREATER 100)
        message(FATAL_ERROR
            "MxR-malloc: DRAM region percent sum is ${_MXR_PCT_SUM}%, must be <= 100%")
    endif()
    message(STATUS
        "MxR-malloc: DRAM region percent sum = ${_MXR_PCT_SUM}%")
    message(STATUS
        "MxR-malloc: ${_MXR_DRAM_COUNT} DRAM region(s) from '${CONFIG_MXR_REGION_CONFIG}'")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        "MXR_PARSED_REGION_COUNT=${_MXR_DRAM_COUNT}"
    )
else()
    message(STATUS
        "MxR-malloc: DRAM region config empty, using single flat region")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        "MXR_PARSED_REGION_COUNT=1"
    )
endif()

# ============================================================
#  3b. Validate MXR_IRAM_FALLBACK_REGION_CONFIG (IRAM fallback)
# ============================================================
if(CONFIG_MXR_USE_IRAM)
    set(_MXR_IRAM_CFG "${CONFIG_MXR_IRAM_FALLBACK_REGION_CONFIG}")
    if(_MXR_IRAM_CFG)
        string(REPLACE "," ";" _MXR_IRAM_ENTRIES "${_MXR_IRAM_CFG}")
        list(LENGTH _MXR_IRAM_ENTRIES _MXR_IRAM_COUNT)
        foreach(_entry ${_MXR_IRAM_ENTRIES})
            string(STRIP "${_entry}" _entry)
            string(REGEX MATCH "^[0-9]+-[0-9]+%$" _VALID "${_entry}")
            if(NOT _VALID)
                message(FATAL_ERROR
                    "MxR-malloc: Invalid IRAM fallback region entry '${_entry}' in "
                    "CONFIG_MXR_IRAM_FALLBACK_REGION_CONFIG='${CONFIG_MXR_IRAM_FALLBACK_REGION_CONFIG}'. "
                    "Expected format: <bytes>-<percent>%")
            endif()
        endforeach()
        if(_MXR_IRAM_COUNT GREATER 32)
            message(FATAL_ERROR
                "MxR-malloc: Too many IRAM fallback regions (${_MXR_IRAM_COUNT}). "
                "Maximum is 32.")
        endif()
        list(GET _MXR_IRAM_ENTRIES 0 _MXR_IRAM_FIRST_ENTRY)
        string(STRIP "${_MXR_IRAM_FIRST_ENTRY}" _MXR_IRAM_FIRST_ENTRY)
        if(_MXR_IRAM_FIRST_ENTRY MATCHES "^([0-9]+)-")
            if(CMAKE_MATCH_1 GREATER 4)
                message(FATAL_ERROR
                    "MxR-malloc: first IRAM fallback region boundary is "
                    "${CMAKE_MATCH_1}, must be <= 4.")
            endif()
        endif()
    set(_MXR_PCT_SUM 0)
    foreach(_entry ${_MXR_IRAM_ENTRIES})
        string(REGEX MATCH "^[0-9]+-([0-9]+)%$" _MATCH "${_entry}")
        if(_MATCH)
            math(EXPR _MXR_PCT_SUM "${_MXR_PCT_SUM} + ${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(_MXR_PCT_SUM GREATER 100)
        message(FATAL_ERROR
            "MxR-malloc: IRAM region percent sum is ${_MXR_PCT_SUM}%, must be <= 100%")
    endif()
    message(STATUS
        "MxR-malloc: IRAM region percent sum = ${_MXR_PCT_SUM}%")
        message(STATUS
            "MxR-malloc: ${_MXR_IRAM_COUNT} IRAM fallback region(s) from "
            "'${CONFIG_MXR_IRAM_FALLBACK_REGION_CONFIG}'")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE
            "MXR_IRAM_FB_PARSED_REGION_COUNT=${_MXR_IRAM_COUNT}"
        )
    else()
        message(STATUS
            "MxR-malloc: IRAM fallback region config empty, using single flat region")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE
            "MXR_IRAM_FB_PARSED_REGION_COUNT=1"
        )
    endif()
endif()

# ============================================================
#  4. Linker wraps (ONLY applied in Wrap mode!)
# ============================================================
if(CONFIG_MXR_INTEGRATION_WRAP)
    set(MXR_WRAP_FLAGS
        "-Wl,--wrap=heap_caps_init"
        "-Wl,--wrap=_heap_caps_malloc"
        "-Wl,--wrap=_heap_caps_free"
        "-Wl,--wrap=_heap_caps_realloc"
        "-Wl,--wrap=_heap_caps_calloc"
        "-Wl,--wrap=_heap_caps_zalloc"
    )
    if(CONFIG_MXR_WRAP_HEAP_QUERY)
        list(APPEND MXR_WRAP_FLAGS
            "-Wl,--wrap=heap_caps_get_free_size"
            "-Wl,--wrap=heap_caps_get_minimum_free_size"
            "-Wl,--wrap=heap_caps_get_dram_free_size"
        )
    endif()
    if(CONFIG_MXR_WRAP_DEFAULT_POOL)
        list(APPEND MXR_WRAP_FLAGS
            "-Wl,--wrap=heap_caps_malloc_default"
            "-Wl,--wrap=heap_caps_realloc_default"
        )
    endif()
    if(CONFIG_MXR_WRAP_ESP_SYSTEM)
        list(APPEND MXR_WRAP_FLAGS
            "-Wl,--wrap=esp_get_free_heap_size"
            "-Wl,--wrap=esp_get_minimum_free_heap_size"
            "-Wl,--wrap=esp_get_free_internal_heap_size"
        )
    endif()
    if(CONFIG_MXR_WRAP_LIBC)
        list(APPEND MXR_WRAP_FLAGS
            "-Wl,--wrap=malloc"
            "-Wl,--wrap=free"
            "-Wl,--wrap=calloc"
            "-Wl,--wrap=realloc"
            "-Wl,--wrap=zalloc"
        )
    endif()
    target_link_libraries(${COMPONENT_LIB} INTERFACE ${MXR_WRAP_FLAGS})
endif()
if(CONFIG_MXR_INTEGRATION_PORT)
    message(WARNING
        "MxR-malloc: PORT mode defines malloc/free/calloc/realloc/zalloc "
        "directly. Ensure original newlib/libc implementations are excluded "
        "from the build, otherwise duplicate symbol errors will occur."
    )
endif()