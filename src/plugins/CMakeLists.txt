add_library(gstdeepgramsink SHARED
    gstdeepgramsink.c
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0>=1.18)
pkg_check_modules(GST_BASE REQUIRED gstreamer-base-1.0>=1.18)
pkg_check_modules(SOUP REQUIRED libsoup-3.0)
pkg_check_modules(JSON_GLIB REQUIRED json-glib-1.0)

target_include_directories(gstdeepgramsink PUBLIC
    ${GST_INCLUDE_DIRS}
    ${GST_BASE_INCLUDE_DIRS}
    ${SOUP_INCLUDE_DIRS}
    ${JSON_GLIB_INCLUDE_DIRS}
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