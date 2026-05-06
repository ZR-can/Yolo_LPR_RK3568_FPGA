cmake_minimum_required(VERSION 3.10)

project(yolov8_lpr_demo)

#set(CMAKE_CXX_STANDARD 11)
#set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (ENABLE_ASAN)
    message(STATUS "BUILD WITH ADDRESS SANITIZER")
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
    set(CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")
endif()

set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")

if (TARGET_SOC STREQUAL "rv1106" OR TARGET_SOC STREQUAL "rv1103")
    add_definitions(-DRV1106_1103)
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/allocator/dma)
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/3rdparty 3rdparty.out)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/utils utils.out)

if (CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(OpenCV_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/opencv/opencv-android-sdk-build/sdk/native/jni/abi-${CMAKE_ANDROID_ARCH_ABI})
else()
    if (CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(OpenCV_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/opencv/opencv-linux-aarch64/share/OpenCV)
    else()
        set(OpenCV_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/opencv/opencv-linux-armhf/share/OpenCV)
    endif()
endif()
find_package(OpenCV REQUIRED)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

set(DEMO_COMMON_SRCS
    src/postprocess.cc
    src/yolov8_zero_copy.cc
    src/lprnet.cc
    src/yolo_lpr_pipeline.cc
    src/simple_tracker.cc
    src/drm_display.cc
)

set(TARGET_PICTURE yolov8_lpr_picture_demo)
set(TARGET_VIDEO yolov8_lpr_video_demo)

add_executable(${TARGET_PICTURE} src/main_picture.cc ${DEMO_COMMON_SRCS})
add_executable(${TARGET_VIDEO} src/main_video.cc ${DEMO_COMMON_SRCS})

# 获取通用的模型文件
file(GLOB RKNN_FILES "${CMAKE_CURRENT_SOURCE_DIR}/model/*.rknn")
file(GLOB TXT_FILES "${CMAKE_CURRENT_SOURCE_DIR}/model/*.txt")
foreach(TARGET_NAME ${TARGET_PICTURE} ${TARGET_VIDEO})
    target_compile_definitions(${TARGET_NAME} PRIVATE ZERO_COPY)

    target_include_directories(${TARGET_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/utils
        ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/allocator/drm
        ${LIBRKNNRT_INCLUDES}
        ${LIBRGA_INCLUDES}
    )

    target_link_libraries(${TARGET_NAME}
        imageutils
        fileutils
        imagedrawing
        mpputils
        ${LIBRKNNRT}
        ${OpenCV_LIBS}
        dl
        Threads::Threads
    )

    if (CMAKE_SYSTEM_NAME STREQUAL "Android")
        target_link_libraries(${TARGET_NAME} log)
    endif()

    # 1. 将可执行文件安装到以 target 命名的独立子目录中
    install(TARGETS ${TARGET_NAME} DESTINATION ${TARGET_NAME})

    # 2. 为各自的目录拷贝共用的模型文件
    install(FILES ${RKNN_FILES} DESTINATION ${TARGET_NAME}/model)
    install(FILES ${TXT_FILES} DESTINATION ${TARGET_NAME}/model)

    # 3. 根据 target 区分拷贝对应的测试资源
    if (${TARGET_NAME} STREQUAL ${TARGET_PICTURE})
        file(GLOB TEST_JPG_FILES "${CMAKE_CURRENT_SOURCE_DIR}/model/test*.jpg")
        install(FILES ${TEST_JPG_FILES} DESTINATION ${TARGET_NAME}/test)
    elseif (${TARGET_NAME} STREQUAL ${TARGET_VIDEO})
        file(GLOB TEST_VIDEO_FILES 
            "${CMAKE_CURRENT_SOURCE_DIR}/model/testvideo*.h264" 
            "${CMAKE_CURRENT_SOURCE_DIR}/model/testvideo*.h265"
        )
        install(FILES ${TEST_VIDEO_FILES} DESTINATION ${TARGET_NAME}/test)
    endif()
endforeach()