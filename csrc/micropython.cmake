# webdisplay/csrc/micropython.cmake

# Create an INTERFACE library for your module
add_library(usermod_webdisplay INTERFACE)

# Add your source files here
target_sources(usermod_webdisplay INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/webdisplay.c
    ${CMAKE_CURRENT_LIST_DIR}/webdisplay_html.c
)

# Include directory (so headers are found)
target_include_directories(usermod_webdisplay INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

# Link into MicroPython usermod system
target_link_libraries(usermod INTERFACE usermod_webdisplay)
