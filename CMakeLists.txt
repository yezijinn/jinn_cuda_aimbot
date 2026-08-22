cmake_minimum_required(VERSION 3.24)

project(mybot LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "mybot currently supports Windows builds only.")
endif()

if(NOT CMAKE_GENERATOR MATCHES "^Ninja" AND NOT CMAKE_GENERATOR MATCHES "Visual Studio")
    message(FATAL_ERROR "Only Ninja and Visual Studio generators are supported by the automated build. Current generator: ${CMAKE_GENERATOR}")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AIMBOT_USE_CUDA "Enable CUDA backend (USE_CUDA)" ON)
option(AIMBOT_COPY_RUNTIME_DLLS "Copy required runtime DLLs next to ai.exe after build" ON)

if(NOT AIMBOT_USE_CUDA)
    message(FATAL_ERROR "This project only supports the CUDA/TensorRT backend.")
endif()

# ============================================================
# 自动检测 CUDA + TensorRT 工具链
# CUDA/TensorRT is the only supported backend.
# ============================================================
find_package(CUDAToolkit QUIET)
if(DEFINED AIMBOT_DISABLE_CUDA_DETECTION AND AIMBOT_DISABLE_CUDA_DETECTION)
    set(CUDAToolkit_FOUND FALSE)
endif()
if(CUDAToolkit_FOUND)
    set(AIMBOT_DETECTED_TENSORRT_ROOT "")
    
    set(_trt_search_roots
        "${CMAKE_SOURCE_DIR}/CUDA.TensorRT"
        "${CMAKE_SOURCE_DIR}/modules"
    )
    if(DEFINED ENV{CUDA_PATH})
        get_filename_component(_cuda_parent "$ENV{CUDA_PATH}" DIRECTORY)
        list(APPEND _trt_search_roots "${_cuda_parent}")
    endif()
    list(APPEND _trt_search_roots "C:/Program Files/NVIDIA GPU Computing Toolkit")
    
    foreach(_root IN LISTS _trt_search_roots)
        # 直接检查 root 本身是不是 TensorRT SDK
        if(EXISTS "${_root}/include/NvInfer.h" AND
           EXISTS "${_root}/lib/nvinfer_10.lib")
            set(_candidate "${_root}")
        else()
            # 否则找 TensorRT-* 子目录
            file(GLOB _trt_candidates LIST_DIRECTORIES true "${_root}/TensorRT-*")
            list(SORT _trt_candidates COMPARE NATURAL ORDER DESCENDING)
            foreach(_candidate IN LISTS _trt_candidates)
                if(EXISTS "${_candidate}/include/NvInfer.h" AND
                   EXISTS "${_candidate}/lib/nvinfer_10.lib")
                    break()
                endif()
            endforeach()
        endif()
        
        if(_candidate AND
           EXISTS "${_candidate}/include/NvInfer.h" AND
           EXISTS "${_candidate}/lib/nvinfer_10.lib")
            set(AIMBOT_DETECTED_TENSORRT_ROOT "${_candidate}")
            break()
        endif()
    endforeach()

    if(AIMBOT_DETECTED_TENSORRT_ROOT)
        message(STATUS "")
        message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
        message(STATUS "  [自动检测] ✓ 发现 CUDA ${CUDAToolkit_VERSION}")
        message(STATUS "  [自动检测] ✓ 发现 TensorRT")
        message(STATUS "  [自动检测] 将使用 CUDA + TensorRT 后端构建")
        message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
        message(STATUS "")
        set(AIMBOT_USE_CUDA ON CACHE BOOL "Enable CUDA backend (USE_CUDA)" FORCE)
    else()
        message(STATUS "")
        message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
        message(STATUS "  [自动检测] ✓ 发现 CUDA ${CUDAToolkit_VERSION}")
        message(STATUS "  [自动检测] ✗ 未找到 TensorRT SDK")
        message(FATAL_ERROR "CUDA was found, but the TensorRT SDK was not found. CUDA/TensorRT is required.")
    endif()
else()
    message(FATAL_ERROR "CUDA Toolkit was not found. This project only supports NVIDIA CUDA/TensorRT.")
endif()

unset(AIMBOT_PACKAGES_DIR CACHE)
unset(AIMBOT_OPENCV_CUDA_ROOT CACHE)
unset(AIMBOT_TENSORRT_ROOT CACHE)
unset(AIMBOT_CUDNN_ROOT CACHE)
set(AIMBOT_OPENCV_CUDA_ROOT
    "${CMAKE_SOURCE_DIR}/modules/opencv/build/cuda/install"
    CACHE PATH "Path to CUDA OpenCV install root")
set(AIMBOT_TENSORRT_ROOT "" CACHE PATH "Path to TensorRT root directory")
set(AIMBOT_ONNXRUNTIME_ROOT
    "${CMAKE_SOURCE_DIR}/../onnxruntime-win-x64-1.28.0"
    CACHE PATH "Path to ONNX Runtime C++ package")
set(AIMBOT_CUDNN_ROOT "$ENV{ProgramW6432}/NVIDIA/CUDNN/v9.17" CACHE PATH "Path to cuDNN root directory (optional)")
set(AIMBOT_OPENCV_INCLUDE_DIR "" CACHE PATH "Path to OpenCV include directory")
set(AIMBOT_OPENCV_LIBRARY "" CACHE FILEPATH "Path to opencv_world*.lib")
set(AIMBOT_OPENCV_DLL "" CACHE FILEPATH "Path to opencv_world*.dll")
set(AIMBOT_CPPWINRT_INCLUDE_DIR "" CACHE PATH "Path to Windows SDK cppwinrt include directory")

function(aimbot_pick_latest_dir out_var glob_pattern)
    file(GLOB _candidates LIST_DIRECTORIES true "${glob_pattern}")
    set(_dirs "")
    foreach(_candidate IN LISTS _candidates)
        if(IS_DIRECTORY "${_candidate}")
            list(APPEND _dirs "${_candidate}")
        endif()
    endforeach()
    if(_dirs)
        list(SORT _dirs COMPARE NATURAL ORDER DESCENDING)
        list(GET _dirs 0 _latest)
    else()
        set(_latest "")
    endif()
    set(${out_var} "${_latest}" PARENT_SCOPE)
endfunction()

function(aimbot_find_opencv out_include out_library out_dll)
    set(_found_include "")
    set(_found_library "")
    set(_found_dll "")

    foreach(_root IN LISTS ARGN)
        if(_found_library)
            break()
        endif()
        if(NOT EXISTS "${_root}/include/opencv2/opencv.hpp")
            continue()
        endif()

        file(GLOB _vc_dirs LIST_DIRECTORIES true "${_root}/x64/vc*")
        if(NOT _vc_dirs)
            continue()
        endif()
        list(SORT _vc_dirs COMPARE NATURAL ORDER DESCENDING)

        foreach(_vc_dir IN LISTS _vc_dirs)
            file(GLOB _opencv_world_libs "${_vc_dir}/lib/opencv_world*.lib")
            if(NOT _opencv_world_libs)
                continue()
            endif()
            list(SORT _opencv_world_libs COMPARE NATURAL ORDER DESCENDING)
            foreach(_lib IN LISTS _opencv_world_libs)
                get_filename_component(_stem "${_lib}" NAME_WE)
                if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT CMAKE_CONFIGURATION_TYPES)
                    if(_stem MATCHES "d$")
                        continue()
                    endif()
                endif()
                if(CMAKE_CONFIGURATION_TYPES)
                    if(_stem MATCHES "d$")
                        continue()
                    endif()
                endif()
                set(_dll "${_vc_dir}/bin/${_stem}.dll")
                if(EXISTS "${_dll}")
                    set(_found_include "${_root}/include")
                    set(_found_library "${_lib}")
                    set(_found_dll "${_dll}")
                    break()
                endif()
            endforeach()
            if(_found_library)
                break()
            endif()
        endforeach()
    endforeach()

    set(${out_include} "${_found_include}" PARENT_SCOPE)
    set(${out_library} "${_found_library}" PARENT_SCOPE)
    set(${out_dll} "${_found_dll}" PARENT_SCOPE)
endfunction()

function(aimbot_require_file path description)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "${description} not found: ${path}")
    endif()
endfunction()

function(aimbot_copy_if_exists target path)
    if(EXISTS "${path}")
        add_custom_command(
            TARGET ${target}
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${path}" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM
        )
    endif()
endfunction()

function(aimbot_remove_runtime_files target)
    set(_script "${CMAKE_CURRENT_BINARY_DIR}/${target}_remove_stale_runtime.cmake")
    file(WRITE "${_script}" "if(NOT DEFINED TARGET_DIR)\n  message(FATAL_ERROR \"TARGET_DIR is not set\")\nendif()\nset(_patterns\n")
    foreach(_pattern IN LISTS ARGN)
        file(APPEND "${_script}" "  \"${_pattern}\"\n")
    endforeach()
    file(APPEND "${_script}" [=[
)
foreach(_pattern IN LISTS _patterns)
  file(GLOB _matches "${TARGET_DIR}/${_pattern}")
  if(_matches)
    file(REMOVE ${_matches})
  endif()
endforeach()
]=])
    add_custom_command(
        TARGET ${target}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} "-DTARGET_DIR=$<TARGET_FILE_DIR:${target}>" -P "${_script}"
        VERBATIM
    )
endfunction()

if(AIMBOT_USE_CUDA)
    if(AIMBOT_DETECTED_TENSORRT_ROOT)
        set(AIMBOT_TENSORRT_ROOT "${AIMBOT_DETECTED_TENSORRT_ROOT}" CACHE PATH "Path to TensorRT root directory" FORCE)
    else()
        aimbot_pick_latest_dir(_tensorrt_default "${CMAKE_SOURCE_DIR}/modules/TensorRT-*")
        if(AIMBOT_TENSORRT_ROOT STREQUAL "" OR
           NOT EXISTS "${AIMBOT_TENSORRT_ROOT}/include/NvInfer.h" OR
           NOT EXISTS "${AIMBOT_TENSORRT_ROOT}/lib/nvinfer_10.lib")
            set(AIMBOT_TENSORRT_ROOT "${_tensorrt_default}" CACHE PATH "Path to TensorRT root directory" FORCE)
        endif()
    endif()
endif()

set(_opencv_candidate_roots
    "${AIMBOT_OPENCV_CUDA_ROOT}"
    "${CMAKE_SOURCE_DIR}/modules/opencv/build/install")

if(AIMBOT_OPENCV_INCLUDE_DIR STREQUAL "" OR
   AIMBOT_OPENCV_LIBRARY STREQUAL "" OR
   AIMBOT_OPENCV_DLL STREQUAL "" OR
   NOT EXISTS "${AIMBOT_OPENCV_INCLUDE_DIR}/opencv2/opencv.hpp" OR
   NOT EXISTS "${AIMBOT_OPENCV_LIBRARY}" OR
   NOT EXISTS "${AIMBOT_OPENCV_DLL}")
    aimbot_find_opencv(_opencv_include_detected _opencv_library_detected _opencv_dll_detected ${_opencv_candidate_roots})
    if(NOT _opencv_library_detected STREQUAL "")
        set(AIMBOT_OPENCV_INCLUDE_DIR "${_opencv_include_detected}" CACHE PATH "Path to OpenCV include directory" FORCE)
        set(AIMBOT_OPENCV_LIBRARY "${_opencv_library_detected}" CACHE FILEPATH "Path to opencv_world*.lib" FORCE)
        set(AIMBOT_OPENCV_DLL "${_opencv_dll_detected}" CACHE FILEPATH "Path to opencv_world*.dll" FORCE)
    endif()
endif()

if(AIMBOT_CPPWINRT_INCLUDE_DIR STREQUAL "")
    if(DEFINED CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION AND DEFINED ENV{WindowsSdkDir})
        file(TO_CMAKE_PATH "$ENV{WindowsSdkDir}" _winsdk_dir)
        set(AIMBOT_CPPWINRT_INCLUDE_DIR "${_winsdk_dir}/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/cppwinrt")
    endif()
endif()

if(AIMBOT_CPPWINRT_INCLUDE_DIR STREQUAL "")
    if(DEFINED CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
        set(_vs_sdk_candidate "C:/Program Files (x86)/Windows Kits/10/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/cppwinrt")
        if(EXISTS "${_vs_sdk_candidate}/winrt/base.h")
            set(AIMBOT_CPPWINRT_INCLUDE_DIR "${_vs_sdk_candidate}")
        endif()
    endif()
endif()

if(AIMBOT_CPPWINRT_INCLUDE_DIR STREQUAL "")
    file(GLOB _winsdk_cppwinrt_candidates LIST_DIRECTORIES true "C:/Program Files (x86)/Windows Kits/10/Include/*/cppwinrt")
    if(_winsdk_cppwinrt_candidates)
        list(SORT _winsdk_cppwinrt_candidates COMPARE NATURAL ORDER DESCENDING)
        list(GET _winsdk_cppwinrt_candidates 0 AIMBOT_CPPWINRT_INCLUDE_DIR)
    endif()
endif()

set(SRC_DIR "${CMAKE_SOURCE_DIR}")

set(AIMBOT_SOURCES
    "${SRC_DIR}/benchmarks/provider_benchmark.cpp"
    "${SRC_DIR}/capture/capture.cpp"
    "${SRC_DIR}/capture/duplication_api_capture.cpp"
    "${SRC_DIR}/capture/udp_capture.cpp"
    "${SRC_DIR}/capture/virtual_camera.cpp"
    "${SRC_DIR}/capture/winrt_capture.cpp"
    "${SRC_DIR}/config/config.cpp"
    "${SRC_DIR}/detector/detection_buffer.cpp"
    "${SRC_DIR}/detector/postProcess.cpp"
    "${SRC_DIR}/imgui/imgui.cpp"
    "${SRC_DIR}/imgui/imgui_draw.cpp"
    "${SRC_DIR}/imgui/imgui_impl_dx11.cpp"
    "${SRC_DIR}/imgui/imgui_impl_win32.cpp"
    "${SRC_DIR}/imgui/imgui_tables.cpp"
    "${SRC_DIR}/imgui/imgui_widgets.cpp"
    "${SRC_DIR}/modules/imgui-1.92.8/misc/cpp/imgui_stdlib.cpp"
    "${SRC_DIR}/keyboard/keyboard_listener.cpp"
    "${SRC_DIR}/keyboard/keycodes.cpp"
    "${SRC_DIR}/mem/cpu_affinity_manager.cpp"
    "${SRC_DIR}/modules/makcu/src/makcu.cpp"
    "${SRC_DIR}/modules/makcu/src/serialport.cpp"
    "${SRC_DIR}/mouse/hid.c"
    "${SRC_DIR}/mouse/KmboxAConnection.cpp"
    "${SRC_DIR}/mouse/kmboxA.cpp"
    "${SRC_DIR}/mouse/kmboxNetConnection.cpp"
    "${SRC_DIR}/mouse/kmbox_net/kmboxNet.cpp"
    "${SRC_DIR}/mouse/Makcu.cpp"
    "${SRC_DIR}/mouse/MouseInput.cpp"
    "${SRC_DIR}/mouse/mouse.cpp"
    "${SRC_DIR}/mouse/AimbotTarget.cpp"
    # MouseController 移植模块 (Kalman + MinimumJerk + PID + Humanizer)
    "${SRC_DIR}/mouse/algo/KalmanTracker.cpp"
    "${SRC_DIR}/mouse/algo/MinimumJerkPlanner.cpp"
    "${SRC_DIR}/mouse/algo/PIDController.cpp"
    "${SRC_DIR}/mouse/algo/AxisController.cpp"
    "${SRC_DIR}/mouse/algo/Humanizer.cpp"
    "${SRC_DIR}/mouse/algo/TargetPublisher.cpp"
    "${SRC_DIR}/mouse/algo/MouseController.cpp"
    "${SRC_DIR}/overlay/draw_ai.cpp"
    "${SRC_DIR}/overlay/draw_buttons.cpp"
    "${SRC_DIR}/overlay/draw_capture.cpp"
    "${SRC_DIR}/overlay/onnx_inspector.cpp"
    "${SRC_DIR}/overlay/onnx_donor/OnnxRuntimeReader.cpp"
    "${SRC_DIR}/overlay/onnx_donor/MetadataReader.cpp"
    "${SRC_DIR}/overlay/onnx_donor/OnnxProtoParser.cpp"
    "${SRC_DIR}/overlay/onnx_donor/YoloAnalyzer.cpp"
    "${SRC_DIR}/overlay/onnx_donor/JsonWriter.cpp"
    "${SRC_DIR}/overlay/config_dirty.cpp"
    "${SRC_DIR}/overlay/draw_debug.cpp"
    
    "${SRC_DIR}/overlay/draw_mouse.cpp"
    "${SRC_DIR}/overlay/draw_overlay.cpp"
    "${SRC_DIR}/overlay/draw_stats.cpp"
    "${SRC_DIR}/overlay/draw_target.cpp"
    
    "${SRC_DIR}/overlay/overlay.cpp"
    "${SRC_DIR}/runtime/thread_loops_shared.cpp"
    "${SRC_DIR}/runtime/mouse_thread_loop.cpp"
    "${SRC_DIR}/runtime/trigger_system.cpp"
    "${SRC_DIR}/scr/data_collector.cpp"
    "${SRC_DIR}/scr/other_tools.cpp"
    "${SRC_DIR}/mybot.cpp"
)

if(NOT CMAKE_CUDA_FLAGS MATCHES "(^| )--allow-unsupported-compiler($| )")
    set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} --allow-unsupported-compiler")
endif()
if(NOT CUDA_NVCC_FLAGS MATCHES "(^| )--allow-unsupported-compiler($| )")
    set(CUDA_NVCC_FLAGS "${CUDA_NVCC_FLAGS} --allow-unsupported-compiler")
endif()
enable_language(CUDA)
list(APPEND AIMBOT_SOURCES
    "${SRC_DIR}/detector/cuda_preprocess.cu"
    "${SRC_DIR}/detector/cuda_opencv_stubs.cpp"
    "${SRC_DIR}/detector/trt_detector.cpp"
    "${SRC_DIR}/mem/gpu_resource_manager.cpp"
    "${SRC_DIR}/tensorrt/nvinf.cpp"
)

add_library(serial_embedded STATIC
    "${SRC_DIR}/modules/serial/src/serial.cc"
    "${SRC_DIR}/modules/serial/src/impl/win.cc"
    "${SRC_DIR}/modules/serial/src/impl/list_ports/list_ports_win.cc"
)

target_include_directories(serial_embedded PUBLIC "${SRC_DIR}/modules/serial/include")
target_link_libraries(serial_embedded PUBLIC setupapi)

if(MSVC)
    set_target_properties(serial_embedded PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

add_executable(ai ${AIMBOT_SOURCES})

include(CTest)
if(BUILD_TESTING)
    add_executable(trigger_system_smoke_test
        "${SRC_DIR}/runtime/trigger_system_smoke_test.cpp"
        "${SRC_DIR}/runtime/trigger_system.cpp"
    )
    target_include_directories(trigger_system_smoke_test PRIVATE "${SRC_DIR}")
    if(MSVC)
        target_compile_options(trigger_system_smoke_test PRIVATE /utf-8)
    endif()
    add_test(NAME trigger_system_smoke_test COMMAND trigger_system_smoke_test)

    add_executable(kmboxNet_protocol_test
        "${SRC_DIR}/mouse/kmboxNetProtocolTest.cpp"
        "${SRC_DIR}/mouse/KmboxNetConnection.cpp"
        "${SRC_DIR}/mouse/kmbox_net/kmboxNet.cpp"
    )
    target_include_directories(kmboxNet_protocol_test PRIVATE
        "${SRC_DIR}"
        "${SRC_DIR}/mouse"
    )
    if(MSVC)
        target_compile_options(kmboxNet_protocol_test PRIVATE /utf-8 /W3 /sdl)
    endif()
    target_link_libraries(kmboxNet_protocol_test PRIVATE ws2_32)
    add_test(NAME kmboxNet_protocol_test COMMAND kmboxNet_protocol_test)

    add_executable(camera_enumeration_cache_test
        "${SRC_DIR}/capture/camera_enumeration_cache_test.cpp"
    )
    target_include_directories(camera_enumeration_cache_test PRIVATE "${SRC_DIR}/capture")
    if(MSVC)
        target_compile_options(camera_enumeration_cache_test PRIVATE /utf-8)
    endif()
    add_test(NAME camera_enumeration_cache_test COMMAND camera_enumeration_cache_test)

    add_executable(onnx_inspector_regression_test
        "${SRC_DIR}/overlay/onnx_inspector_regression_test.cpp"
        "${SRC_DIR}/overlay/onnx_inspector.cpp"
        "${SRC_DIR}/overlay/onnx_donor/OnnxRuntimeReader.cpp"
        "${SRC_DIR}/overlay/onnx_donor/MetadataReader.cpp"
        "${SRC_DIR}/overlay/onnx_donor/OnnxProtoParser.cpp"
        "${SRC_DIR}/overlay/onnx_donor/YoloAnalyzer.cpp"
        "${SRC_DIR}/overlay/onnx_donor/JsonWriter.cpp"
    )
    target_include_directories(onnx_inspector_regression_test PRIVATE
        "${SRC_DIR}"
        "${SRC_DIR}/overlay/onnx_donor"
        "${AIMBOT_ONNXRUNTIME_ROOT}/include"
    )
    target_link_directories(onnx_inspector_regression_test PRIVATE "${AIMBOT_ONNXRUNTIME_ROOT}/lib")
    target_link_libraries(onnx_inspector_regression_test PRIVATE onnxruntime bcrypt ws2_32)
    target_compile_definitions(onnx_inspector_regression_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    aimbot_copy_if_exists(onnx_inspector_regression_test "${AIMBOT_ONNXRUNTIME_ROOT}/lib/onnxruntime.dll")
    aimbot_copy_if_exists(onnx_inspector_regression_test "${AIMBOT_ONNXRUNTIME_ROOT}/lib/onnxruntime_providers_shared.dll")
    if(MSVC)
        target_compile_options(onnx_inspector_regression_test PRIVATE /utf-8)
    endif()
    add_test(NAME onnx_inspector_regression_test
        COMMAND onnx_inspector_regression_test "${SRC_DIR}/depth_models/depth_anything_v2_vits_224.onnx")
endif()

set_target_properties(ai PROPERTIES
    OUTPUT_NAME "ai"
)

if(MSVC)
    set_target_properties(ai PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

if(AIMBOT_USE_CUDA)
    set_target_properties(ai PROPERTIES
        CUDA_STANDARD 17
        CUDA_STANDARD_REQUIRED ON
    )
endif()

target_include_directories(ai PRIVATE
    "${SRC_DIR}"
    "${SRC_DIR}/benchmarks"
    "${SRC_DIR}/capture"
    "${SRC_DIR}/config"
    "${SRC_DIR}/detector"
    "${SRC_DIR}/imgui"
    "${SRC_DIR}/keyboard"
    "${SRC_DIR}/include"
    "${SRC_DIR}/mem"
    "${SRC_DIR}/mouse"
    "${SRC_DIR}/mouse/algo"
    "${SRC_DIR}/overlay"
    "${SRC_DIR}/overlay/onnx_donor"
    "${SRC_DIR}/scr"
    "${SRC_DIR}/modules/makcu/include"
    "${SRC_DIR}/modules/serial/include"
    "${SRC_DIR}/modules/stb"
    "${SRC_DIR}/modules/imgui-1.92.8"
    "${SRC_DIR}/modules/imgui-1.92.8/backends"
)

target_compile_definitions(ai PRIVATE
    UNICODE
    _UNICODE
    _CONSOLE
    _CRT_SECURE_NO_WARNINGS
    # C++/WinRT still includes the experimental coroutine header in C++17 mode.
    _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
)

if(MSVC)
    target_compile_options(ai PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
        $<$<COMPILE_LANGUAGE:CXX>:/W3>
        $<$<COMPILE_LANGUAGE:CXX>:/sdl>
        $<$<COMPILE_LANGUAGE:CXX>:/permissive->
        $<$<COMPILE_LANGUAGE:CXX>:/EHsc>
    )
endif()

target_compile_definitions(ai PRIVATE USE_CUDA)
target_include_directories(ai PRIVATE "${SRC_DIR}/tensorrt")

aimbot_require_file("${AIMBOT_OPENCV_INCLUDE_DIR}/opencv2/opencv.hpp" "OpenCV headers")
aimbot_require_file("${AIMBOT_OPENCV_LIBRARY}" "OpenCV library")
aimbot_require_file("${AIMBOT_OPENCV_DLL}" "OpenCV runtime DLL")
if(AIMBOT_CPPWINRT_INCLUDE_DIR STREQUAL "")
    message(FATAL_ERROR "C++/WinRT include directory is not set. Set AIMBOT_CPPWINRT_INCLUDE_DIR manually.")
endif()
aimbot_require_file("${AIMBOT_CPPWINRT_INCLUDE_DIR}/winrt/base.h" "C++/WinRT headers")

target_include_directories(ai PRIVATE
    "${AIMBOT_OPENCV_INCLUDE_DIR}"
    "${AIMBOT_CPPWINRT_INCLUDE_DIR}"
)

target_link_libraries(ai PRIVATE
    serial_embedded
    "${AIMBOT_OPENCV_LIBRARY}"
    d3d11
    dxgi
    dwmapi
    d2d1
    setupapi
    comdlg32
    bcrypt
    WindowsApp
)

find_package(CUDAToolkit REQUIRED)

aimbot_require_file("${AIMBOT_ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h" "ONNX Runtime headers")
aimbot_require_file("${AIMBOT_ONNXRUNTIME_ROOT}/lib/onnxruntime.lib" "ONNX Runtime import library")
aimbot_require_file("${AIMBOT_ONNXRUNTIME_ROOT}/lib/onnxruntime.dll" "ONNX Runtime DLL")
target_include_directories(ai PRIVATE "${AIMBOT_ONNXRUNTIME_ROOT}/include")
target_link_directories(ai PRIVATE "${AIMBOT_ONNXRUNTIME_ROOT}/lib")
    target_link_libraries(ai PRIVATE onnxruntime shell32)

    aimbot_require_file("${AIMBOT_TENSORRT_ROOT}/include/NvInfer.h" "TensorRT headers")
    aimbot_require_file("${AIMBOT_TENSORRT_ROOT}/lib/nvinfer_10.lib" "TensorRT nvinfer_10.lib")
    aimbot_require_file("${AIMBOT_TENSORRT_ROOT}/lib/nvonnxparser_10.lib" "TensorRT nvonnxparser_10.lib")

    target_include_directories(ai PRIVATE "${AIMBOT_TENSORRT_ROOT}/include")
    target_link_directories(ai PRIVATE "${AIMBOT_TENSORRT_ROOT}/lib")
    target_link_libraries(ai PRIVATE
        CUDA::cuda_driver
        CUDA::cudart
        nvinfer_10
        nvonnxparser_10
    )

    if(EXISTS "${AIMBOT_CUDNN_ROOT}/include/13.1/cudnn.h")
        target_include_directories(ai PRIVATE "${AIMBOT_CUDNN_ROOT}/include/13.1")
    endif()

if(AIMBOT_COPY_RUNTIME_DLLS)
    aimbot_copy_if_exists(ai "${AIMBOT_OPENCV_DLL}")
    aimbot_copy_if_exists(ai "${AIMBOT_ONNXRUNTIME_ROOT}/lib/onnxruntime.dll")
    aimbot_copy_if_exists(ai "${AIMBOT_ONNXRUNTIME_ROOT}/lib/onnxruntime_providers_shared.dll")

    set(_aimbot_cuda_trt_dir "${CMAKE_SOURCE_DIR}/CUDA.TensorRT")
    if(EXISTS "${_aimbot_cuda_trt_dir}")
        file(GLOB _cuda_trt_dlls "${_aimbot_cuda_trt_dir}/*.dll")
        foreach(_dll IN LISTS _cuda_trt_dlls)
            get_filename_component(_dll_name "${_dll}" NAME)
            if(NOT _dll_name STREQUAL "opencv_world4130.dll" AND
               NOT _dll_name STREQUAL "onnxruntime.dll" AND
               NOT _dll_name STREQUAL "onnxruntime_providers_shared.dll")
                aimbot_copy_if_exists(ai "${_dll}")
            endif()
        endforeach()
    endif()
endif()

message(STATUS "Aimbot backend: CUDA/TensorRT")
message(STATUS "OpenCV include: ${AIMBOT_OPENCV_INCLUDE_DIR}")
message(STATUS "OpenCV lib: ${AIMBOT_OPENCV_LIBRARY}")
message(STATUS "C++/WinRT include: ${AIMBOT_CPPWINRT_INCLUDE_DIR}")
