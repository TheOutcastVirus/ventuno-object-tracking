#include "yolox_detector/yolox_detector_node.hpp"
#include "yolox_detector/cpu_backend.hpp"
#ifdef HAVE_QNN_BACKEND
#include "yolox_detector/qnn_backend.hpp"
#endif
#include <cv_bridge/cv_bridge.hpp>
#include <opencv2/imgproc.hpp>
#include <vision_msgs/msg/detection2_d.hpp>
#include <vision_msgs/msg/object_hypothesis_with_pose.hpp>
#include <rclcpp/rclcpp.hpp>
#include <stdexcept>

namespace yolox_detector {

YoloxDetectorNode::YoloxDetectorNode(const rclcpp::NodeOptions & options)
: Node("yolox_detector", options)
{
  // ── Parameters ──────────────────────────────────────────────────────────────
  this->declare_parameter("backend", std::string("cpu"));
  this->declare_parameter("model_path", std::string("models/yolox_tiny_xnnpack.pte"));
  this->declare_parameter("qnn_lib_dir", std::string(""));
  this->declare_parameter("input_width", 416);
  this->declare_parameter("input_height", 416);
  this->declare_parameter("score_threshold", 0.45);
  this->declare_parameter("nms_threshold", 0.45);
  this->declare_parameter("num_classes", 80);
  this->declare_parameter("image_topic", std::string("/camera/rgb/image_raw"));
  this->declare_parameter("detections_topic", std::string("/detections"));
  this->declare_parameter("debug_image_topic", std::string("/detections/image"));

  auto backend_name = this->get_parameter("backend").as_string();
  auto model_path   = this->get_parameter("model_path").as_string();
  auto qnn_lib_dir  = this->get_parameter("qnn_lib_dir").as_string();
  int in_w  = this->get_parameter("input_width").as_int();
  int in_h  = this->get_parameter("input_height").as_int();
  score_threshold_ = static_cast<float>(this->get_parameter("score_threshold").as_double());
  nms_threshold_   = static_cast<float>(this->get_parameter("nms_threshold").as_double());
  num_classes_     = this->get_parameter("num_classes").as_int();

  // ── Backend ──────────────────────────────────────────────────────────────────
  if (backend_name == "cpu") {
    backend_ = std::make_unique<CpuBackend>(in_w, in_h);
  } else if (backend_name == "npu") {
#ifdef HAVE_QNN_BACKEND
    backend_ = std::make_unique<QnnBackend>(in_w, in_h, qnn_lib_dir);
#else
    throw std::runtime_error(
      "yolox_detector was compiled without QNN backend support. "
      "Rebuild with -DBUILD_QNN_BACKEND=ON.");
#endif
  } else {
    throw std::invalid_argument("Unknown backend '" + backend_name +
                                "'. Choose 'cpu' or 'npu'.");
  }

  if (!backend_->load(model_path)) {
    throw std::runtime_error("Failed to load model from: " + model_path);
  }
  RCLCPP_INFO(this->get_logger(), "Loaded model '%s' on '%s' backend",
              model_path.c_str(), backend_name.c_str());

  // ── Publishers ──────────────────────────────────────────────────────────────
  auto det_topic   = this->get_parameter("detections_topic").as_string();
  auto debug_topic = this->get_parameter("debug_image_topic").as_string();

  det_pub_   = this->create_publisher<vision_msgs::msg::Detection2DArray>(det_topic, 10);
  debug_pub_ = image_transport::create_publisher(this, debug_topic);

  // ── Subscriber ──────────────────────────────────────────────────────────────
  auto img_topic = this->get_parameter("image_topic").as_string();
  image_sub_ = image_transport::create_subscription(
    this, img_topic,
    std::bind(&YoloxDetectorNode::image_callback, this, std::placeholders::_1),
    "raw");

  RCLCPP_INFO(this->get_logger(), "Subscribing to '%s'", img_topic.c_str());
}

void YoloxDetectorNode::image_callback(
  const sensor_msgs::msg::Image::ConstSharedPtr & msg)
{
  // Convert ROS image → BGR float32
  cv_bridge::CvImageConstPtr cv_img;
  try {
    cv_img = cv_bridge::toCvShare(msg, sensor_msgs::image_encodings::BGR8);
  } catch (const cv_bridge::Exception & e) {
    RCLCPP_ERROR(this->get_logger(), "cv_bridge: %s", e.what());
    return;
  }

  int in_w = backend_->input_width();
  int in_h = backend_->input_height();

  float scale, pad_left, pad_top;
  cv::Mat lb = letterbox(cv_img->image, in_w, in_h, scale, pad_left, pad_top);

  // Inference
  std::vector<float> raw;
  try {
    raw = backend_->infer(lb);
  } catch (const std::exception & e) {
    RCLCPP_ERROR(this->get_logger(), "Inference error: %s", e.what());
    return;
  }

  int num_anchors = static_cast<int>(raw.size()) / (5 + num_classes_);
  auto dets = decode_and_nms(raw, num_anchors, num_classes_,
                             in_w, in_h, scale, pad_left, pad_top,
                             score_threshold_, nms_threshold_);

  // ── Publish detections ──────────────────────────────────────────────────────
  vision_msgs::msg::Detection2DArray det_arr;
  det_arr.header = msg->header;

  for (const auto & d : dets) {
    vision_msgs::msg::Detection2D det;
    det.header = msg->header;

    float cx = (d.x1 + d.x2) * 0.5f;
    float cy = (d.y1 + d.y2) * 0.5f;
    float w  = d.x2 - d.x1;
    float h  = d.y2 - d.y1;

    det.bbox.center.position.x = cx;
    det.bbox.center.position.y = cy;
    det.bbox.size_x = w;
    det.bbox.size_y = h;

    vision_msgs::msg::ObjectHypothesisWithPose hyp;
    hyp.hypothesis.class_id = std::to_string(d.class_id);
    hyp.hypothesis.score    = d.score;
    det.results.push_back(hyp);

    det_arr.detections.push_back(det);
  }
  det_pub_->publish(det_arr);

  // ── Publish debug image ─────────────────────────────────────────────────────
  if (debug_pub_.getNumSubscribers() > 0) {
    cv::Mat dbg = cv_img->image.clone();
    for (const auto & d : dets) {
      cv::rectangle(dbg,
                    cv::Point(static_cast<int>(d.x1), static_cast<int>(d.y1)),
                    cv::Point(static_cast<int>(d.x2), static_cast<int>(d.y2)),
                    cv::Scalar(0, 255, 0), 2);
    }
    auto dbg_msg = cv_bridge::CvImage(msg->header, "bgr8", dbg).toImageMsg();
    debug_pub_.publish(*dbg_msg);
  }
}

}  // namespace yolox_detector
