# Register the module with MicroPython

add_library(usermod_webdisplay INTERFACE)

target_sources(usermod_webdisplay INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/webdisplay.c
)

target_include_directories(usermod_webdisplay INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_compile_definitions(usermod_webdisplay INTERFACE
    MODULE_WEBDISPLAY_ENABLED=1
)

target_link_libraries(usermod INTERFACE usermod_webdisplay)
