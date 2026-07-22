cmake_minimum_required(VERSION 3.15)

add_library(fileutils STATIC file_utils.c)
target_include_directories(fileutils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(imagedrawing STATIC image_drawing.c)
target_include_directories(imagedrawing PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(imageutils STATIC image_utils.c)
if(DISABLE_RGA)
    target_compile_definitions(imageutils PRIVATE DISABLE_RGA)
endif()
target_include_directories(imageutils PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${STB_INCLUDES}
    ${LIBJPEG_INCLUDES}
    ${LIBRGA_INCLUDES}
)
target_link_libraries(imageutils PUBLIC
    ${LIBJPEG}
    ${LIBRGA}
    Threads::Threads
)
