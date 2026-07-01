#include "oak_camera/oak_camera_node.hpp"

#include <chrono>
#include <functional>
#include <stdexcept>

#include <cv_bridge/cv_bridge.hpp>
#include <opencv2/core.hpp>
#include <sensor_msgs/image_encodings.hpp>

namespace oak_camera
{

OakCameraNode::OakCameraNode(const rclcpp::NodeOptions & options)
: Node("oak_camera_node", options)
{
  declareParameters();
  buildPipeline();
  startDevice();

  // image_transport needs a shared_ptr to this node
  auto node_base = std::shared_ptr<rclcpp::Node>(this, [](rclcpp::Node *) {});
  image_transport::ImageTransport it(node_base);

  if (enable_rgb_) {
    rgb_info_mgr_ = std::make_shared<camera_info_manager::CameraInfoManager>(
      this, "oak_rgb", rgb_camera_info_url_);
    rgb_pub_ = it.advertiseCamera(rgb_topic_, 1);
  }
  if (enable_left_) {
    left_info_mgr_ = std::make_shared<camera_info_manager::CameraInfoManager>(
      this, "oak_left", left_camera_info_url_);
    left_pub_ = it.advertiseCamera(left_topic_, 1);
  }
  if (enable_right_) {
    right_info_mgr_ = std::make_shared<camera_info_manager::CameraInfoManager>(
      this, "oak_right", right_camera_info_url_);
    right_pub_ = it.advertiseCamera(right_topic_, 1);
  }

  // Drive publish loop from a timer at ~100 Hz so we don't block the executor
  using namespace std::chrono_literals;
  timer_ = create_wall_timer(10ms, std::bind(&OakCameraNode::publishLoop, this));

  RCLCPP_INFO(get_logger(), "OAK camera node started. rgb=%d left=%d right=%d",
    enable_rgb_, enable_left_, enable_right_);
}

OakCameraNode::~OakCameraNode()
{
  timer_->cancel();
  if (device_) {
    device_->close();
  }
}

void OakCameraNode::declareParameters()
{
  enable_rgb_   = declare_parameter("enable_rgb",   true);
  enable_left_  = declare_parameter("enable_left",  false);
  enable_right_ = declare_parameter("enable_right", false);

  rgb_fps_    = declare_parameter("rgb_fps",    30.0);
  rgb_width_  = declare_parameter("rgb_width",  1920);
  rgb_height_ = declare_parameter("rgb_height", 1080);
  rgb_output_ = declare_parameter("rgb_output", std::string("video"));

  mono_fps_    = declare_parameter("mono_fps",    30.0);
  mono_width_  = declare_parameter("mono_width",  640);
  mono_height_ = declare_parameter("mono_height", 400);

  rgb_topic_   = declare_parameter("rgb_topic",   std::string("oak/rgb/image_raw"));
  left_topic_  = declare_parameter("left_topic",  std::string("oak/left/image_raw"));
  right_topic_ = declare_parameter("right_topic", std::string("oak/right/image_raw"));

  rgb_camera_info_url_   = declare_parameter("rgb_camera_info_url",   std::string(""));
  left_camera_info_url_  = declare_parameter("left_camera_info_url",  std::string(""));
  right_camera_info_url_ = declare_parameter("right_camera_info_url", std::string(""));

  rgb_frame_id_   = declare_parameter("rgb_frame_id",   std::string("oak_rgb_camera_optical_frame"));
  left_frame_id_  = declare_parameter("left_frame_id",  std::string("oak_left_camera_optical_frame"));
  right_frame_id_ = declare_parameter("right_frame_id", std::string("oak_right_camera_optical_frame"));
}

void OakCameraNode::buildPipeline()
{
  if (enable_rgb_) {
    auto cam_rgb = pipeline_.create<dai::node::ColorCamera>();
    auto xout_rgb = pipeline_.create<dai::node::XLinkOut>();

    xout_rgb->setStreamName("rgb");

    cam_rgb->setBoardSocket(dai::CameraBoardSocket::CAM_A);
    cam_rgb->setResolution(dai::ColorCameraProperties::SensorResolution::THE_1080_P);
    cam_rgb->setFps(static_cast<float>(rgb_fps_));
    cam_rgb->setColorOrder(dai::ColorCameraProperties::ColorOrder::BGR);

    // Choose which output to stream
    if (rgb_output_ == "preview") {
      cam_rgb->setPreviewSize(rgb_width_, rgb_height_);
      cam_rgb->preview.link(xout_rgb->input);
    } else if (rgb_output_ == "isp") {
      cam_rgb->isp.link(xout_rgb->input);
    } else {
      // "video" is the default — full-resolution encoded output
      cam_rgb->video.link(xout_rgb->input);
    }
  }

  if (enable_left_) {
    auto cam_left = pipeline_.create<dai::node::MonoCamera>();
    auto xout_left = pipeline_.create<dai::node::XLinkOut>();

    xout_left->setStreamName("left");

    cam_left->setBoardSocket(dai::CameraBoardSocket::CAM_B);
    cam_left->setResolution(dai::MonoCameraProperties::SensorResolution::THE_400_P);
    cam_left->setFps(static_cast<float>(mono_fps_));

    cam_left->out.link(xout_left->input);
  }

  if (enable_right_) {
    auto cam_right = pipeline_.create<dai::node::MonoCamera>();
    auto xout_right = pipeline_.create<dai::node::XLinkOut>();

    xout_right->setStreamName("right");

    cam_right->setBoardSocket(dai::CameraBoardSocket::CAM_C);
    cam_right->setResolution(dai::MonoCameraProperties::SensorResolution::THE_400_P);
    cam_right->setFps(static_cast<float>(mono_fps_));

    cam_right->out.link(xout_right->input);
  }
}

void OakCameraNode::startDevice()
{
  try {
    device_ = std::make_shared<dai::Device>(pipeline_);
  } catch (const std::exception & e) {
    throw std::runtime_error(std::string("Failed to open OAK device: ") + e.what());
  }

  if (enable_rgb_) {
    rgb_queue_ = device_->getOutputQueue("rgb", 4, false);
  }
  if (enable_left_) {
    left_queue_ = device_->getOutputQueue("left", 4, false);
  }
  if (enable_right_) {
    right_queue_ = device_->getOutputQueue("right", 4, false);
  }
}

// Helper: convert an ImgFrame to a ROS Image message
static sensor_msgs::msg::Image::SharedPtr toImageMsg(
  const std::shared_ptr<dai::ImgFrame> & frame,
  const std::string & frame_id,
  const std::string & encoding)
{
  auto msg = std::make_shared<sensor_msgs::msg::Image>();

  // Timestamp from device sequence number + host clock
  auto tp = std::chrono::time_point_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now());
  auto ns = tp.time_since_epoch().count();
  msg->header.stamp.sec     = static_cast<int32_t>(ns / 1'000'000'000LL);
  msg->header.stamp.nanosec = static_cast<uint32_t>(ns % 1'000'000'000LL);
  msg->header.frame_id = frame_id;

  msg->height   = frame->getHeight();
  msg->width    = frame->getWidth();
  msg->encoding = encoding;
  msg->step     = static_cast<uint32_t>(frame->getData().size() / frame->getHeight());
  msg->is_bigendian = false;

  const auto & data = frame->getData();
  msg->data.assign(data.begin(), data.end());

  return msg;
}

void OakCameraNode::publishLoop()
{
  if (enable_rgb_ && rgb_queue_) {
    auto frame = rgb_queue_->tryGet<dai::ImgFrame>();
    if (frame) {
      auto img = toImageMsg(frame, rgb_frame_id_, sensor_msgs::image_encodings::BGR8);
      auto info = rgb_info_mgr_->getCameraInfo();
      info.header = img->header;
      rgb_pub_.publish(*img, info);
    }
  }

  if (enable_left_ && left_queue_) {
    auto frame = left_queue_->tryGet<dai::ImgFrame>();
    if (frame) {
      auto img = toImageMsg(frame, left_frame_id_, sensor_msgs::image_encodings::MONO8);
      auto info = left_info_mgr_->getCameraInfo();
      info.header = img->header;
      left_pub_.publish(*img, info);
    }
  }

  if (enable_right_ && right_queue_) {
    auto frame = right_queue_->tryGet<dai::ImgFrame>();
    if (frame) {
      auto img = toImageMsg(frame, right_frame_id_, sensor_msgs::image_encodings::MONO8);
      auto info = right_info_mgr_->getCameraInfo();
      info.header = img->header;
      right_pub_.publish(*img, info);
    }
  }
}

}  // namespace oak_camera
