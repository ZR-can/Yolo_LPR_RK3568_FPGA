cmake_minimum_required(VERSION 3.15)

project(yolov8_traffic_demo LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

option(DISABLE_RGA "Use CPU image conversion instead of RGA" OFF)
set(PCIE_DEMO_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../3_NPU_Yolov8_PPOCR_Demo"
    CACHE PATH "Path to the reused PCIe/UI bridge backend")
set(TRAFFIC_3RDPARTY_ROOT
    "${PCIE_DEMO_ROOT}/3rdparty"
    CACHE PATH "Path to the reused RKNN/RGA/JPEG third-party libraries")

if(NOT TARGET_SOC)
    set(TARGET_SOC rk356x)
endif()
if(NOT EXISTS "${TRAFFIC_3RDPARTY_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "Third-party directory not found: ${TRAFFIC_3RDPARTY_ROOT}")
endif()
if(NOT EXISTS "${PCIE_DEMO_ROOT}/include/pcie_demo_bridge.h")
    message(FATAL_ERROR "PCIe UI bridge header not found: ${PCIE_DEMO_ROOT}")
endif()

add_subdirectory("${TRAFFIC_3RDPARTY_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/thirdparty")
add_subdirectory(utils)

set(YOLOV8_TRAFFIC_COMMON_SOURCES
    src/postprocess.cc
    src/yolov8_zero_copy.cc
)

add_executable(yolov8_traffic_benchmark
    src/main.cc
    ${YOLOV8_TRAFFIC_COMMON_SOURCES}
)

set(PCIE_DRIVER_MODULE "${CMAKE_CURRENT_SOURCE_DIR}/pango_pci_driver.ko")
if(NOT EXISTS "${PCIE_DRIVER_MODULE}")
    message(FATAL_ERROR "PCIe driver module is missing: ${PCIE_DRIVER_MODULE}")
endif()

add_executable(yolov8_traffic_pcie_demo
    src/main_pcie_traffic.cc
    src/traffic_violation.cc
    src/traffic_temporal_tracker.cc
    src/traffic_overlay_renderer.cc
    src/pcie_frame_source.cc
    src/drm_display.cc
    ${YOLOV8_TRAFFIC_COMMON_SOURCES}
)

foreach(TRAFFIC_TARGET yolov8_traffic_benchmark yolov8_traffic_pcie_demo)
    target_compile_definitions(${TRAFFIC_TARGET} PRIVATE ZERO_COPY)
    target_include_directories(${TRAFFIC_TARGET} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/utils
        ${PCIE_DEMO_ROOT}/include
        ${TRAFFIC_3RDPARTY_ROOT}/allocator/drm
        ${LIBRKNNRT_INCLUDES}
        ${LIBRGA_INCLUDES}
    )
    target_link_libraries(${TRAFFIC_TARGET} PRIVATE
        imageutils
        fileutils
        imagedrawing
        ${LIBRKNNRT}
        dl
        m
        Threads::Threads
    )
    install(TARGETS ${TRAFFIC_TARGET} DESTINATION ${TRAFFIC_TARGET})
    install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/model/yolov8_traffic_i8.rknn
        ${CMAKE_CURRENT_SOURCE_DIR}/model/labels_list.txt
        ${CMAKE_CURRENT_SOURCE_DIR}/model/traffic_roi.conf
        DESTINATION ${TRAFFIC_TARGET}/model
    )
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/README.md DESTINATION ${TRAFFIC_TARGET})
endforeach()

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/test/
    DESTINATION yolov8_traffic_benchmark/test
    FILES_MATCHING PATTERN "*.jpg" PATTERN "*.jpeg" PATTERN "*.png" PATTERN "*.bmp"
)
install(FILES ${PCIE_DRIVER_MODULE} DESTINATION yolov8_traffic_pcie_demo)
