cmake_minimum_required(VERSION 3.10)
project(gst-deepgram-plugin C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0>=1.18)
pkg_check_modules(GST_BASE REQUIRED gstreamer-base-1.0>=1.18)
pkg_check_modules(SOUP REQUIRED libsoup-3.0)
pkg_check_modules(JSON_GLIB REQUIRED json-glib-1.0)

include_directories(
    ${GST_INCLUDE_DIRS}
    ${GST_BASE_INCLUDE_DIRS}
    ${SOUP_INCLUDE_DIRS}
    ${JSON_GLIB_INCLUDE_DIRS}
)

link_directories(
    ${GST_LIBRARY_DIRS}
    ${GST_BASE_LIBRARY_DIRS}
    ${SOUP_LIBRARY_DIRS}
    ${JSON_GLIB_LIBRARY_DIRS}
)

add_library(gstdeepgramsink SHARED
    src/gstdeepgramsink.c
)
target_link_libraries(gstdeepgramsink
    ${GST_LIBRARIES}
    ${GST_BASE_LIBRARIES}
    ${SOUP_LIBRARIES}
    ${JSON_GLIB_LIBRARIES}
)

set(GST_PLUGIN_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/lib/gstreamer-1.0" CACHE STRING "GStreamer plugin install dir")
install(TARGETS gstdeepgramsink
    LIBRARY DESTINATION ${GST_PLUGIN_INSTALL_DIR}
)

add_executable(deepgram_test_app src/main.c)
target_link_libraries(deepgram_test_app
    gstdeepgramsink
    ${GST_LIBRARIES}
    ${GST_BASE_LIBRARIES}
)
install(TARGETS deepgram_test_app RUNTIME DESTINATION bin)
