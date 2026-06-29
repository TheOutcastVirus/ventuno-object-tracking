cmake_minimum_required(VERSION 3.22)
project(oak_camera)

# Require C++17 — depthai-core v3 needs it
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── ROS 2 dependencies ────────────────────────────────────────────────────────
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(image_transport REQUIRED)
find_package(camera_info_manager REQUIRED)
find_package(cv_bridge REQUIRED)

# ── DepthAI v3 ────────────────────────────────────────────────────────────────
# depthai-core must be installed on the target (Ventuno / dev machine).
# Install via:  sudo apt install libdepthai-dev   (if packaged)
# or build from source and set depthai_DIR to the cmake install prefix.
find_package(depthai REQUIRED)

# ── Node executable ───────────────────────────────────────────────────────────
add_executable(oak_camera_node
  src/main.cpp
  src/oak_camera_node.cpp
)

target_include_directories(oak_camera_node
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(oak_camera_node
  depthai::core
)

ament_target_dependencies(oak_camera_node
  rclcpp
  sensor_msgs
  image_transport
  camera_info_manager
  cv_bridge
)

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS oak_camera_node
  DESTINATION lib/${PROJECT_NAME}
)

install(DIRECTORY include/
  DESTINATION include
)

install(DIRECTORY config launch
  DESTINATION share/${PROJECT_NAME}
)

ament_package()
