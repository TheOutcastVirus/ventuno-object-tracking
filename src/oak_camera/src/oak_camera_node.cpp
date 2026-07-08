#include "oak_camera/oak_camera_node.hpp"

// depthai included ONLY in this translation unit — never in the header.
// This prevents DepthAI's bundled headers from conflicting with rclcpp.
#include <depthai/depthai.hpp>

#include <chrono>
#include <functional>
#include <stdexcept>

#include <cv_bridge/cv_bridge/cv_bridge.hpp>
#include <opencv2/core.hpp>
#include <sensor_msgs/sensor_msgs/image_encodings.hpp>

namespace oak_camera
{

// Full struct definition — DAI types are completely contained here.
// In depthai v3, Pipeline owns the implicit Device and all node graph state.
// Queues are created directly from Node::Output, not via XLinkOut or device->getOutputQueue.
struct OakCameraNode::DaiResources
{
  dai::Pipeline pipeline;

  std::shared_ptr<dai::MessageQueue> rgb_queue;
  std::shared_ptr<dai::MessageQueue> left_queue;
  std::shared_ptr<dai::MessageQueue> right_queue;
};

OakCameraNode::OakCameraNode(const rclcpp::NodeOptions & options)
: Node("oak_camera_node", options)
{
  declareParameters();

  try {
    // Pipeline() with default createImplicitDevice=true connects to the OAK device.
    dai_ = std::make_unique<DaiResources>();
    buildPipeline();
    startDevice();
  } catch (const std::exception & e) {
    throw std::runtime_error(std::string("Failed to initialize OAK device: ") + e.what());
  }

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
  timer_ = create_wall_timer(5ms, std::bind(&OakCameraNode::publishLoop, this));

  RCLCPP_INFO(get_logger(), "OAK camera node started. rgb=%d left=%d right=%d",
    enable_rgb_, enable_left_, enable_right_);
}

OakCameraNode::~OakCameraNode()
{
  if (timer_) {
    timer_->cancel();
  }
  // dai_ RAII: Pipeline destructor closes the device.
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
  // Use the v3 Camera node for all sensors. ColorCamera/MonoCamera are deprecated
  // and broken: they mis-negotiate ISP scaling, causing the firmware to crash with
  // "post-proc expected WxH, received NxM". Camera::requestOutput() declares the
  // desired output size before build() locks in ISP configuration, avoiding the
  // mismatch. rgb_output_ (video/isp/preview) is no longer meaningful with the
  // unified Camera API and is intentionally ignored.

  if (enable_rgb_) {
    auto cam = dai_->pipeline.create<dai::node::Camera>();
    cam->build(dai::CameraBoardSocket::CAM_A);
    auto * out = cam->requestOutput(
      {static_cast<uint32_t>(rgb_width_), static_cast<uint32_t>(rgb_height_)},
      std::nullopt,
      dai::ImgResizeMode::CROP,
      static_cast<float>(rgb_fps_));
    // depth=1 non-blocking: always hold only the newest frame; older frames are
    // dropped immediately rather than queuing up and adding display lag.
    dai_->rgb_queue = out->createOutputQueue(1, false);
    RCLCPP_INFO(get_logger(), "RGB stream: %dx%d @ %.1f fps -> '%s'",
      rgb_width_, rgb_height_, rgb_fps_, rgb_topic_.c_str());
  }

  if (enable_left_) {
    auto cam = dai_->pipeline.create<dai::node::Camera>();
    cam->build(dai::CameraBoardSocket::CAM_B);
    auto * out = cam->requestOutput(
      {static_cast<uint32_t>(mono_width_), static_cast<uint32_t>(mono_height_)},
      std::nullopt,
      dai::ImgResizeMode::CROP,
      static_cast<float>(mono_fps_));
    dai_->left_queue = out->createOutputQueue(1, false);
    RCLCPP_INFO(get_logger(), "Left stream: %dx%d @ %.1f fps -> '%s'",
      mono_width_, mono_height_, mono_fps_, left_topic_.c_str());
  }

  if (enable_right_) {
    auto cam = dai_->pipeline.create<dai::node::Camera>();
    cam->build(dai::CameraBoardSocket::CAM_C);
    auto * out = cam->requestOutput(
      {static_cast<uint32_t>(mono_width_), static_cast<uint32_t>(mono_height_)},
      std::nullopt,
      dai::ImgResizeMode::CROP,
      static_cast<float>(mono_fps_));
    dai_->right_queue = out->createOutputQueue(1, false);
    RCLCPP_INFO(get_logger(), "Right stream: %dx%d @ %.1f fps -> '%s'",
      mono_width_, mono_height_, mono_fps_, right_topic_.c_str());
  }
}

void OakCameraNode::startDevice()
{
  // In v3, pipeline.start() triggers firmware upload and begins streaming.
  dai_->pipeline.start();
}

static sensor_msgs::msg::Image::SharedPtr toImageMsg(
  const std::shared_ptr<dai::ImgFrame> & frame,
  const std::string & frame_id,
  const std::string & encoding)
{
  auto msg = std::make_shared<sensor_msgs::msg::Image>();

  // Use the frame's hardware capture timestamp rather than the receive time.
  // This reflects when the sensor actually captured the image; steady_clock::now()
  // would add USB transfer + scheduling jitter to every stamp.
  auto tp = std::chrono::time_point_cast<std::chrono::nanoseconds>(frame->getTimestamp());
  auto ns = tp.time_since_epoch().count();
  msg->header.stamp.sec     = static_cast<int32_t>(ns / 1'000'000'000LL);
  msg->header.stamp.nanosec = static_cast<uint32_t>(ns % 1'000'000'000LL);
  msg->header.frame_id = frame_id;

  // getCvFrame() handles all pixel format conversions (NV12, YUV, BGR888i, RAW8, …)
  // and returns a properly-typed cv::Mat (BGR for color, GRAY for mono).
  cv::Mat cv_frame = frame->getCvFrame();

  msg->height = static_cast<uint32_t>(cv_frame.rows);
  msg->width  = static_cast<uint32_t>(cv_frame.cols);
  msg->encoding = encoding;
  msg->step = static_cast<uint32_t>(cv_frame.step[0]);
  msg->is_bigendian = false;

  const auto * data_ptr = cv_frame.ptr<uint8_t>();
  msg->data.assign(data_ptr, data_ptr + cv_frame.total() * cv_frame.elemSize());

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
