#pragma once

#include <memory>
#include <string>

#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <sensor_msgs/msg/camera_info.hpp>
#include <image_transport/image_transport.hpp>
#include <camera_info_manager/camera_info_manager.hpp>

#include <depthai/depthai.hpp>

namespace oak_camera
{

class OakCameraNode : public rclcpp::Node
{
public:
  explicit OakCameraNode(const rclcpp::NodeOptions & options);
  ~OakCameraNode();

private:
  void declareParameters();
  void buildPipeline();
  void startDevice();
  void publishLoop();

  // DepthAI objects
  dai::Pipeline pipeline_;
  std::shared_ptr<dai::Device> device_;

  // Output queues (one per enabled stream)
  std::shared_ptr<dai::DataOutputQueue> rgb_queue_;
  std::shared_ptr<dai::DataOutputQueue> left_queue_;
  std::shared_ptr<dai::DataOutputQueue> right_queue_;

  // ROS publishers
  image_transport::CameraPublisher rgb_pub_;
  image_transport::CameraPublisher left_pub_;
  image_transport::CameraPublisher right_pub_;

  std::shared_ptr<camera_info_manager::CameraInfoManager> rgb_info_mgr_;
  std::shared_ptr<camera_info_manager::CameraInfoManager> left_info_mgr_;
  std::shared_ptr<camera_info_manager::CameraInfoManager> right_info_mgr_;

  // Publish timer
  rclcpp::TimerBase::SharedPtr timer_;

  // Params
  bool enable_rgb_;
  bool enable_left_;
  bool enable_right_;

  double rgb_fps_;
  int rgb_width_;
  int rgb_height_;
  std::string rgb_output_;

  double mono_fps_;
  int mono_width_;
  int mono_height_;

  std::string rgb_topic_;
  std::string left_topic_;
  std::string right_topic_;

  std::string rgb_camera_info_url_;
  std::string left_camera_info_url_;
  std::string right_camera_info_url_;

  std::string rgb_frame_id_;
  std::string left_frame_id_;
  std::string right_frame_id_;
};

}  // namespace oak_camera
