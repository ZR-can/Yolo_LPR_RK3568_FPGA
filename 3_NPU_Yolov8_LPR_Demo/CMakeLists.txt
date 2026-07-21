cmake_minimum_required(VERSION 3.10)

project(yolov8_lpr_demo)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(ENABLE_QT_UI "Build the Qt PCIe LPR UI target" OFF)

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
    src/rga_overlay_renderer.cc
)

set(TARGET_PICTURE yolov8_lpr_picture_demo)
set(TARGET_VIDEO yolov8_lpr_video_demo)
set(TARGET_PCIE yolov8_lpr_pcie_demo)
set(PCIE_DRIVER_MODULE "${CMAKE_CURRENT_SOURCE_DIR}/pango_pci_driver.ko")
set(CHINESE_FONT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/model/simhei.ttf")

if (NOT EXISTS "${PCIE_DRIVER_MODULE}")
    message(FATAL_ERROR "PCIe driver module is missing: ${PCIE_DRIVER_MODULE}")
endif()

add_executable(${TARGET_PICTURE} src/main_picture.cc ${DEMO_COMMON_SRCS})
add_executable(${TARGET_VIDEO} src/main_video.cc ${DEMO_COMMON_SRCS})
add_executable(${TARGET_PCIE} src/main_pcie.cc src/pcie_frame_source.cc ${DEMO_COMMON_SRCS})

# 获取通用的模型文件
file(GLOB RKNN_FILES "${CMAKE_CURRENT_SOURCE_DIR}/model/*.rknn")
file(GLOB TXT_FILES "${CMAKE_CURRENT_SOURCE_DIR}/model/*.txt")
foreach(TARGET_NAME ${TARGET_PICTURE} ${TARGET_VIDEO} ${TARGET_PCIE})
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
        ${LIBRKNNRT}
        ${OpenCV_LIBS}
        dl
        Threads::Threads
    )

    if (NOT "${TARGET_NAME}" STREQUAL "${TARGET_PCIE}")
        target_link_libraries(${TARGET_NAME} mpputils)
    endif()

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
        install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/vpu_preflight.sh DESTINATION ${TARGET_NAME})
    elseif (${TARGET_NAME} STREQUAL ${TARGET_PCIE})
        install(FILES ${PCIE_DRIVER_MODULE} DESTINATION ${TARGET_NAME})
    endif()
endforeach()

if (ENABLE_QT_UI)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTOUIC ON)
    set(CMAKE_AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/src")

    find_package(Qt5 REQUIRED COMPONENTS Widgets)

    set(TARGET_PCIE_QT yolov8_lpr_pcie_qt_ui)
    add_executable(${TARGET_PCIE_QT}
        src/main_pcie.cc
        src/main_pcie_qt.cc
        src/pcie_qt_ui_helpers.cc
        src/pcie_qt_ui_helpers.h
        src/mainwindow.ui
        src/pcie_frame_source.cc
        ${DEMO_COMMON_SRCS}
    )

    target_compile_definitions(${TARGET_PCIE_QT} PRIVATE ZERO_COPY PCIE_QT_UI_BUILD)

    target_include_directories(${TARGET_PCIE_QT} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/utils
        ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/allocator/drm
        ${LIBRKNNRT_INCLUDES}
    )

    target_link_libraries(${TARGET_PCIE_QT}
        Qt5::Widgets
        imageutils
        fileutils
        imagedrawing
        ${LIBRKNNRT}
        ${OpenCV_LIBS}
        m
        dl
        Threads::Threads
    )

    install(TARGETS ${TARGET_PCIE_QT} DESTINATION ${TARGET_PCIE_QT})
    install(FILES ${RKNN_FILES} DESTINATION ${TARGET_PCIE_QT}/model)
    install(FILES ${TXT_FILES} DESTINATION ${TARGET_PCIE_QT}/model)
    if (EXISTS "${CHINESE_FONT_FILE}")
        install(FILES ${CHINESE_FONT_FILE} DESTINATION ${TARGET_PCIE_QT}/model)
    endif()
    install(FILES ${PCIE_DRIVER_MODULE} DESTINATION ${TARGET_PCIE_QT})
endif()
