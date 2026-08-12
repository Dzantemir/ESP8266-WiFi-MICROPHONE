cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

#message(STATUS "MXR_DEBUG: _link_flags = [-T${CMAKE_CURRENT_SOURCE_DIR}/ld/esp8266.project.ld.in]")
#get_target_property(_link_flags ${COMPONENT_LIB} INTERFACE_LINK_LIBRARIES)


#if(_link_flags)
#    string(REPLACE
#        "-T${IDF_PATH}/components/esp8266/ld/esp8266.project.ld.in"
#        "-T${CMAKE_CURRENT_SOURCE_DIR}/ld/esp8266.project.ld.in"
#        _link_flags "${_link_flags}"
#    )
#    set_target_properties(${COMPONENT_LIB} PROPERTIES
#        INTERFACE_LINK_LIBRARIES "${_link_flags}"
#    )
#endif()

project(esp-adpcm-streamer)

get_target_property(_esp8266_srcs __idf_esp8266 SOURCES)

if(_esp8266_srcs)
    set(_new_srcs "")
    foreach(_src ${_esp8266_srcs})
        get_filename_component(_fname ${_src} NAME)
        get_filename_component(_fdir  ${_src} DIRECTORY)
        get_filename_component(_dname ${_fdir} NAME)
        if(_fname STREQUAL "i2s.c" AND _dname STREQUAL "driver")
            message(STATUS "i2s_custom: excluding ${_src} from esp8266")
        else()
            list(APPEND _new_srcs "${_src}")
        endif()
    endforeach()
    set_target_properties(__idf_esp8266 PROPERTIES SOURCES "${_new_srcs}")
else()
    message(WARNING "i2s_custom: не удалось прочитать SOURCES __idf_esp8266")
endif()