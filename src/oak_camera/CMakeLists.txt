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
# Installed via ros-jazzy-depthai-v3. The cmake config in the lib directory
# exports depthai_v3::core with headers and linked libraries.
find_package(depthai_v3 REQUIRED CONFIG
  PATHS /opt/ros/jazzy/lib/aarch64-linux-gnu/cmake/depthai_v3
  NO_DEFAULT_PATH
)

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
  depthai_v3::core
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
