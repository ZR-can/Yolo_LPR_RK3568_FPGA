cmake_minimum_required(VERSION 3.10)

project(yolov8_lpr_pcie_qt_ui)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(DEMO_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOUIC_SEARCH_PATHS "${DEMO_ROOT}/src")

set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")

add_subdirectory("${DEMO_ROOT}/3rdparty" 3rdparty.out)
add_subdirectory("${DEMO_ROOT}/utils" utils.out)

if (CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(OpenCV_DIR "${DEMO_ROOT}/3rdparty/opencv/opencv-android-sdk-build/sdk/native/jni/abi-${CMAKE_ANDROID_ARCH_ABI}")
else()
    if (CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(OpenCV_DIR "${DEMO_ROOT}/3rdparty/opencv/opencv-linux-aarch64/share/OpenCV")
    else()
        set(OpenCV_DIR "${DEMO_ROOT}/3rdparty/opencv/opencv-linux-armhf/share/OpenCV")
    endif()
endif()

find_package(OpenCV REQUIRED)
find_package(Qt5 REQUIRED COMPONENTS Widgets)
find_package(Threads REQUIRED)

set(QT_PCIE_SRCS
    "${DEMO_ROOT}/src/main_pcie_qt.cc"
    "${DEMO_ROOT}/src/pcie_qt_ui_helpers.cc"
    "${DEMO_ROOT}/src/mainwindow.ui"
    "${DEMO_ROOT}/src/pcie_frame_source.cc"
    "${DEMO_ROOT}/src/postprocess.cc"
    "${DEMO_ROOT}/src/yolov8_zero_copy.cc"
    "${DEMO_ROOT}/src/lprnet.cc"
    "${DEMO_ROOT}/src/yolo_lpr_pipeline.cc"
    "${DEMO_ROOT}/src/simple_tracker.cc"
)

add_executable(yolov8_lpr_pcie_qt_ui ${QT_PCIE_SRCS})

target_compile_definitions(yolov8_lpr_pcie_qt_ui PRIVATE ZERO_COPY)

target_include_directories(yolov8_lpr_pcie_qt_ui PRIVATE
    "${DEMO_ROOT}"
    "${DEMO_ROOT}/include"
    "${DEMO_ROOT}/utils"
    "${DEMO_ROOT}/3rdparty/allocator/drm"
    ${LIBRKNNRT_INCLUDES}
)

target_link_libraries(yolov8_lpr_pcie_qt_ui
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

set(PCIE_DRIVER_MODULE "${DEMO_ROOT}/pango_pci_driver.ko")
set(CHINESE_FONT_FILE "${DEMO_ROOT}/model/simhei.ttf")
file(GLOB RKNN_FILES "${DEMO_ROOT}/model/*.rknn")
file(GLOB TXT_FILES "${DEMO_ROOT}/model/*.txt")

install(TARGETS yolov8_lpr_pcie_qt_ui DESTINATION yolov8_lpr_pcie_qt_ui)
install(FILES ${RKNN_FILES} DESTINATION yolov8_lpr_pcie_qt_ui/model)
install(FILES ${TXT_FILES} DESTINATION yolov8_lpr_pcie_qt_ui/model)
if (EXISTS "${CHINESE_FONT_FILE}")
    install(FILES "${CHINESE_FONT_FILE}" DESTINATION yolov8_lpr_pcie_qt_ui/model)
endif()
if (EXISTS "${PCIE_DRIVER_MODULE}")
    install(FILES "${PCIE_DRIVER_MODULE}" DESTINATION yolov8_lpr_pcie_qt_ui)
endif()
