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
