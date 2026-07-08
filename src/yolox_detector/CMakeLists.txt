cmake_minimum_required(VERSION 3.22)
project(yolox_detector)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── ROS 2 ─────────────────────────────────────────────────────────────────────
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(vision_msgs REQUIRED)
find_package(image_transport REQUIRED)
find_package(cv_bridge REQUIRED)
find_package(OpenCV REQUIRED)

# ── ExecuTorch ────────────────────────────────────────────────────────────────
# Build ExecuTorch from source and point CMAKE_PREFIX_PATH at the install dir,
# or set EXECUTORCH_INSTALL_DIR explicitly:
#   cmake -DEXECUTORCH_INSTALL_DIR=/opt/executorch ..
if(DEFINED EXECUTORCH_INSTALL_DIR)
  list(APPEND CMAKE_PREFIX_PATH ${EXECUTORCH_INSTALL_DIR})
endif()

# The native Qualcomm ExecuTorch build currently exports optional LLM/tokenizer
# targets even when this package only links the runtime module API. Predefine the
# tokenizer targets so ExecuTorch's package config can load without requiring
# the full tokenizer dependency graph.
foreach(_tokenizer_target
  tokenizers::regex_lookahead
  tokenizers::pcre2-8-static
  tokenizers::tokenizers
  tokenizers::re2
  tokenizers::sentencepiece-static
)
  if(NOT TARGET ${_tokenizer_target})
    add_library(${_tokenizer_target} INTERFACE IMPORTED)
  endif()
endforeach()

find_package(executorch REQUIRED)

# ── Backend selection ─────────────────────────────────────────────────────────
option(BUILD_QNN_BACKEND "Build the QTI QNN NPU backend" OFF)
option(BUILD_XNNPACK_BACKEND "Build the ExecuTorch XNNPACK CPU backend" ON)

if(BUILD_XNNPACK_BACKEND)
  foreach(_xnnpack_target xnnpack_backend XNNPACK xnnpack-microkernels-prod)
    if(NOT TARGET ${_xnnpack_target})
      message(FATAL_ERROR
        "BUILD_XNNPACK_BACKEND=ON requires an ExecuTorch build with "
        "EXECUTORCH_BUILD_XNNPACK=ON so models/yolox_tiny_xnnpack.pte can run on CPU. "
        "Missing target: ${_xnnpack_target}")
    endif()
  endforeach()

  if(COMMAND executorch_target_link_options_shared_lib)
    executorch_target_link_options_shared_lib(xnnpack_backend)
  endif()
endif()

set(SOURCES
  src/main.cpp
  src/yolox_detector_node.cpp
  src/cpu_backend.cpp
  src/postprocess.cpp
)

if(BUILD_QNN_BACKEND)
  list(APPEND SOURCES src/qnn_backend.cpp)
endif()

# ── Executable ────────────────────────────────────────────────────────────────
add_executable(yolox_detector_node ${SOURCES})
add_executable(dataset_image_publisher_node src/dataset_image_publisher_node.cpp)

target_include_directories(yolox_detector_node
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${OpenCV_INCLUDE_DIRS}
    ${EXECUTORCH_INCLUDE_DIRS}
)

target_compile_definitions(yolox_detector_node PRIVATE
  $<$<BOOL:${BUILD_QNN_BACKEND}>:HAVE_QNN_BACKEND>
)

target_link_libraries(yolox_detector_node
  executorch
  executorch_core
  extension_module_static
  extension_tensor
  portable_ops_lib
  ${OpenCV_LIBS}
  $<$<BOOL:${BUILD_XNNPACK_BACKEND}>:xnnpack_backend>
  $<$<BOOL:${BUILD_XNNPACK_BACKEND}>:XNNPACK>
  $<$<BOOL:${BUILD_XNNPACK_BACKEND}>:xnnpack-microkernels-prod>
  $<$<BOOL:${BUILD_QNN_BACKEND}>:qnn_executorch_backend>
)

ament_target_dependencies(yolox_detector_node
  rclcpp
  sensor_msgs
  vision_msgs
  image_transport
  cv_bridge
)

target_include_directories(dataset_image_publisher_node
  PRIVATE
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(dataset_image_publisher_node
  ${OpenCV_LIBS}
)

ament_target_dependencies(dataset_image_publisher_node
  rclcpp
  sensor_msgs
  cv_bridge
)

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS
  yolox_detector_node
  dataset_image_publisher_node
  DESTINATION lib/${PROJECT_NAME}
)

install(DIRECTORY include/
  DESTINATION include
)

install(DIRECTORY config launch
  DESTINATION share/${PROJECT_NAME}
)

ament_package()
