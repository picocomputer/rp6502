add_library(_tinyusb STATIC EXCLUDE_FROM_ALL)

include_directories( ${CMAKE_CURRENT_LIST_DIR} )

file(GLOB_RECURSE TINY_USB_SRC_FILES ${CMAKE_CURRENT_LIST_DIR}/*.c)

target_link_libraries(_tinyusb PUBLIC
    pico_stdlib
)

target_sources(_tinyusb PRIVATE
    ${TINY_USB_SRC_FILES}
)
