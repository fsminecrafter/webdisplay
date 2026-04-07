add_library(usermod_webdisplay INTERFACE)

target_sources(usermod_webdisplay INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/webdisplay.c
)

target_include_directories(usermod_webdisplay INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(usermod INTERFACE usermod_webdisplay)
