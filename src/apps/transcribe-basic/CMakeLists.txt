add_executable(transcribe_basic transcribe_basic.c)
target_link_libraries(transcribe_basic
    gstdeepgramsink
    ${GST_LIBRARIES}
    ${GST_BASE_LIBRARIES}
)

install(TARGETS transcribe_basic RUNTIME DESTINATION bin)