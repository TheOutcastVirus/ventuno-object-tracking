#include "oak_camera/oak_camera_node.hpp"

// depthai included ONLY in this translation unit — never in the header.
// This prevents DepthAI's bundled headers from conflicting with rclcpp.
#include <depthai/depthai.hpp>

#include <chrono>
#include <functional>
#include <stdexcept>

#include <cv_bridge/cv_bridge.hpp>
#include <opencv2/core.hpp>
#include <sensor_msgs/image_encodings.hpp>

namespace oak_camera
{

// Full struct definition — DAI types are completely contained here
struct OakCameraNode::DaiResources
{
  dai::Pipeline pipeline;
  std::shared_ptr<dai::Device> device;

  // Derive the queue type from what getOutputQueue actually returns —
  // avoids hard-coding DataOutputQueue/DataQueue which changed between versions.
  using QueuePtr = decltype(std::declval<dai::Device>().getOutputQueue("", 4, false));
  QueuePtr rgb_queue;
  QueuePtr left_queue;
  QueuePtr right_queue;
};

OakCameraNode::OakCameraNode(const rclcpp::NodeOptions & options)
: Node("oak_camera_node", options)
{
  dai_ = std::make_unique<DaiResources>();

  declareParameters();
  buildPipeline();
  startDevice();

  // Null-deleter shared_ptr so ImageTransport can hold a shared_ptr<Node>
  // without taking ownership. Safe because ImageTransport is stack-local here.
  auto node_shared = std::shared_ptr<rclcpp::Node>(
    static_cast<rclcpp::Node *>(this), [](rclcpp::Node *) {});
  image_transport::ImageTransport it(node_shared);

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

  using namespace std::chrono_literals;
  timer_ = create_wall_timer(10ms, std::bind(&OakCameraNode::publishLoop, this));

  RCLCPP_INFO(get_logger(), "OAK camera node started. rgb=%d left=%d right=%d",
    enable_rgb_, enable_left_, enable_right_);
}

OakCameraNode::~OakCameraNode()
{
  if (timer_) {
    timer_->cancel();
  }
  if (dai_ && dai_->device) {
    dai_->device->close();
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
    auto cam_rgb  = dai_->pipeline.create<dai::node::ColorCamera>();
    auto xout_rgb = dai_->pipeline.create<dai::node::XLinkOut>();

    xout_rgb->setStreamName("rgb");

    cam_rgb->setBoardSocket(dai::CameraBoardSocket::CAM_A);
    cam_rgb->setResolution(dai::ColorCameraProperties::SensorResolution::THE_1080_P);
    cam_rgb->setFps(static_cast<float>(rgb_fps_));
    cam_rgb->setColorOrder(dai::ColorCameraProperties::ColorOrder::BGR);

    if (rgb_output_ == "preview") {
      cam_rgb->setPreviewSize(rgb_width_, rgb_height_);
      cam_rgb->preview.link(xout_rgb->input);
    } else if (rgb_output_ == "isp") {
      cam_rgb->isp.link(xout_rgb->input);
    } else {
      cam_rgb->video.link(xout_rgb->input);
    }
  }

  if (enable_left_) {
    auto cam_left  = dai_->pipeline.create<dai::node::MonoCamera>();
    auto xout_left = dai_->pipeline.create<dai::node::XLinkOut>();

    xout_left->setStreamName("left");

    cam_left->setBoardSocket(dai::CameraBoardSocket::CAM_B);
    cam_left->setResolution(dai::MonoCameraProperties::SensorResolution::THE_400_P);
    cam_left->setFps(static_cast<float>(mono_fps_));
    cam_left->out.link(xout_left->input);
  }

  if (enable_right_) {
    auto cam_right  = dai_->pipeline.create<dai::node::MonoCamera>();
    auto xout_right = dai_->pipeline.create<dai::node::XLinkOut>();

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
    dai_->device = std::make_shared<dai::Device>(dai_->pipeline);
  } catch (const std::exception & e) {
    throw std::runtime_error(std::string("Failed to open OAK device: ") + e.what());
  }

  if (enable_rgb_) {
    dai_->rgb_queue = dai_->device->getOutputQueue("rgb", 4, false);
  }
  if (enable_left_) {
    dai_->left_queue = dai_->device->getOutputQueue("left", 4, false);
  }
  if (enable_right_) {
    dai_->right_queue = dai_->device->getOutputQueue("right", 4, false);
  }
}

static sensor_msgs::msg::Image::SharedPtr toImageMsg(
  const std::shared_ptr<dai::ImgFrame> & frame,
  const std::string & frame_id,
  const std::string & encoding)
{
  auto msg = std::make_shared<sensor_msgs::msg::Image>();

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
  if (enable_rgb_ && dai_->rgb_queue) {
    auto frame = dai_->rgb_queue->tryGet<dai::ImgFrame>();
    if (frame) {
      auto img = toImageMsg(frame, rgb_frame_id_, sensor_msgs::image_encodings::BGR8);
      auto info = rgb_info_mgr_->getCameraInfo();
      info.header = img->header;
      rgb_pub_.publish(*img, info);
    }
  }

  if (enable_left_ && dai_->left_queue) {
    auto frame = dai_->left_queue->tryGet<dai::ImgFrame>();
    if (frame) {
      auto img = toImageMsg(frame, left_frame_id_, sensor_msgs::image_encodings::MONO8);
      auto info = left_info_mgr_->getCameraInfo();
      info.header = img->header;
      left_pub_.publish(*img, info);
    }
  }

  if (enable_right_ && dai_->right_queue) {
    auto frame = dai_->right_queue->tryGet<dai::ImgFrame>();
    if (frame) {
      auto img = toImageMsg(frame, right_frame_id_, sensor_msgs::image_encodings::MONO8);
      auto info = right_info_mgr_->getCameraInfo();
      info.header = img->header;
      right_pub_.publish(*img, info);
    }
  }
}

}  // namespace oak_camera
