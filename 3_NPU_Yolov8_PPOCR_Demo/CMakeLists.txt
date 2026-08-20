cmake_minimum_required(VERSION 3.10)

project(yolov8_ppocr_demo)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    add_compile_options(/utf-8)
endif()

include(CTest)

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
elseif (CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(OpenCV_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/opencv/opencv-linux-aarch64/share/OpenCV)
else()
    set(OpenCV_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/opencv/opencv-linux-armhf/share/OpenCV)
endif()
find_package(OpenCV REQUIRED)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

set(PCIE_DISPLAY_SRCS
    src/postprocess.cc
    src/yolov8_zero_copy.cc
    src/simple_tracker.cc
    src/plate_rule.cc
    src/drm_display.cc
    src/rga_overlay_renderer.cc
)
set(LPR_PIPELINE_SRCS
    src/lprnet.cc
    src/yolo_lpr_pipeline.cc
)
set(PPOCR_PIPELINE_SRCS
    src/ppocr_rec.cc
    src/ppocr_retry_policy.cc
    src/static_image_change_detector.cc
    src/obb_postprocess.cc
    src/yolo_ppocr_pipeline.cc
)

set(TARGET_PCIE_LPR yolov8_lpr_pcie_demo)
set(TARGET_PCIE_PPOCR yolov8_ppocr_pcie_demo)
set(TARGET_PPOCR_EVAL ppocr_rec_eval_demo)

set(PCIE_DRIVER_MODULE "${CMAKE_CURRENT_SOURCE_DIR}/pango_pci_driver.ko")
set(YOLO_MODEL_FILE "${CMAKE_CURRENT_SOURCE_DIR}/model/yolov8.rknn")
set(LABEL_FILE "${CMAKE_CURRENT_SOURCE_DIR}/model/labels_list.txt")
file(GLOB LPR_MODEL_FILES "${CMAKE_CURRENT_SOURCE_DIR}/model/lprnet*.rknn")
set(PPOCR_MODEL_FILE "${CMAKE_CURRENT_SOURCE_DIR}/model/ppocrv4_rec14_fold_affine_1x1_rk3568_hybrid_mmse_h2_add27_hsw4.rknn")
set(OBB_MODEL_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/../2_Model_Conversion_PC_Simulation/yolov8_obb/model/yolov8_obb_i8.rknn")
set(PPOCR_DICT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/model/cblprd_plate_dict.txt")
set(PPOCR_TEST_FILE "${CMAKE_CURRENT_SOURCE_DIR}/model/test_ppocr.jpg")

foreach(REQUIRED_FILE
        ${PCIE_DRIVER_MODULE}
        ${YOLO_MODEL_FILE}
        ${LABEL_FILE}
        ${PPOCR_MODEL_FILE}
        ${OBB_MODEL_FILE}
        ${PPOCR_DICT_FILE})
    if (NOT EXISTS "${REQUIRED_FILE}")
        message(FATAL_ERROR "Required deployment file is missing: ${REQUIRED_FILE}")
    endif()
endforeach()
if (NOT LPR_MODEL_FILES)
    message(FATAL_ERROR "No LPR reference model found under model/")
endif()

add_executable(${TARGET_PCIE_LPR}
    src/main_lpr.cc
    src/pcie_frame_source.cc
    ${PCIE_DISPLAY_SRCS}
    ${LPR_PIPELINE_SRCS}
)
add_executable(${TARGET_PCIE_PPOCR}
    src/main_ppocr.cc
    src/pcie_frame_source.cc
    ${PCIE_DISPLAY_SRCS}
    ${PPOCR_PIPELINE_SRCS}
)
add_executable(${TARGET_PPOCR_EVAL}
    src/main_ppocr_eval.cc
    src/ppocr_rec.cc
)

function(configure_pcie_target TARGET_NAME)
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
    if (CMAKE_SYSTEM_NAME STREQUAL "Android")
        target_link_libraries(${TARGET_NAME} log)
    endif()
endfunction()

configure_pcie_target(${TARGET_PCIE_LPR})
configure_pcie_target(${TARGET_PCIE_PPOCR})

target_include_directories(${TARGET_PPOCR_EVAL} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
    ${LIBRKNNRT_INCLUDES}
)
target_link_libraries(${TARGET_PPOCR_EVAL}
    imageutils
    fileutils
    ${LIBRKNNRT}
    ${OpenCV_LIBS}
)
if (CMAKE_SYSTEM_NAME STREQUAL "Android")
    target_link_libraries(${TARGET_PPOCR_EVAL} log)
endif()

install(TARGETS ${TARGET_PCIE_LPR} DESTINATION ${TARGET_PCIE_LPR})
install(FILES
    ${YOLO_MODEL_FILE}
    ${LABEL_FILE}
    ${LPR_MODEL_FILES}
    DESTINATION ${TARGET_PCIE_LPR}/model
)
install(FILES ${PCIE_DRIVER_MODULE} DESTINATION ${TARGET_PCIE_LPR})

install(TARGETS ${TARGET_PCIE_PPOCR} DESTINATION ${TARGET_PCIE_PPOCR})
install(FILES
    ${YOLO_MODEL_FILE}
    ${LABEL_FILE}
    ${PPOCR_MODEL_FILE}
    ${PPOCR_DICT_FILE}
    DESTINATION ${TARGET_PCIE_PPOCR}/model
)
install(FILES ${OBB_MODEL_FILE}
        DESTINATION ${TARGET_PCIE_PPOCR}/model
        RENAME yolov8_obb.rknn)
install(FILES ${PCIE_DRIVER_MODULE} DESTINATION ${TARGET_PCIE_PPOCR})

install(TARGETS ${TARGET_PPOCR_EVAL} DESTINATION ${TARGET_PPOCR_EVAL})
install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/eval_ppocr_rknn_subsets.sh
        DESTINATION ${TARGET_PPOCR_EVAL})
install(FILES ${PPOCR_MODEL_FILE} ${PPOCR_DICT_FILE}
        DESTINATION ${TARGET_PPOCR_EVAL}/model)
if (EXISTS "${PPOCR_TEST_FILE}")
    install(FILES ${PPOCR_TEST_FILE} DESTINATION ${TARGET_PPOCR_EVAL}/test)
endif()

if (BUILD_TESTING)
    add_executable(plate_rule_test
        tests/plate_rule_test.cc
        src/plate_rule.cc
        src/simple_tracker.cc
    )
    target_include_directories(plate_rule_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME plate_rule_test COMMAND plate_rule_test)

    add_executable(simple_tracker_motion_test
        tests/simple_tracker_motion_test.cc
        src/plate_rule.cc
        src/simple_tracker.cc
    )
    target_include_directories(simple_tracker_motion_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME simple_tracker_motion_test
             COMMAND simple_tracker_motion_test)

    add_executable(ppocr_retry_policy_test
        tests/ppocr_retry_policy_test.cc
        src/ppocr_retry_policy.cc
        src/plate_rule.cc
    )
    target_include_directories(ppocr_retry_policy_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/utils
    )
    add_test(NAME ppocr_retry_policy_test COMMAND ppocr_retry_policy_test)

    add_executable(static_image_change_detector_test
        tests/static_image_change_detector_test.cc
        src/static_image_change_detector.cc
    )
    target_include_directories(static_image_change_detector_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME static_image_change_detector_test
             COMMAND static_image_change_detector_test)

    add_executable(obb_postprocess_test
        validation/obb_postprocess_test.cc
        src/obb_postprocess.cc
    )
    target_include_directories(obb_postprocess_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/utils
        ${LIBRKNNRT_INCLUDES}
    )
    add_test(NAME obb_postprocess_test COMMAND obb_postprocess_test)

    add_executable(simple_tracker_static_image_test
        validation/simple_tracker_static_image_test.cc
        src/plate_rule.cc
        src/simple_tracker.cc
    )
    target_include_directories(simple_tracker_static_image_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME simple_tracker_static_image_test
             COMMAND simple_tracker_static_image_test)
endif()
