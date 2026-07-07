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
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace yolox_detector {

static const char * kCocoLabels[] = {
  "person","bicycle","car","motorcycle","airplane","bus","train","truck","boat",
  "traffic light","fire hydrant","stop sign","parking meter","bench","bird","cat",
  "dog","horse","sheep","cow","elephant","bear","zebra","giraffe","backpack",
  "umbrella","handbag","tie","suitcase","frisbee","skis","snowboard","sports ball",
  "kite","baseball bat","baseball glove","skateboard","surfboard","tennis racket",
  "bottle","wine glass","cup","fork","knife","spoon","bowl","banana","apple",
  "sandwich","orange","broccoli","carrot","hot dog","pizza","donut","cake","chair",
  "couch","potted plant","bed","dining table","toilet","tv","laptop","mouse",
  "remote","keyboard","cell phone","microwave","oven","toaster","sink",
  "refrigerator","book","clock","vase","scissors","teddy bear","hair drier",
  "toothbrush"
};
static constexpr int kNumCocoLabels = 80;

// Human-readable COCO label for a class id, or "unknown" when out of range.
static const char * label_for(int id)
{
  return (id >= 0 && id < kNumCocoLabels) ? kCocoLabels[id] : "unknown";
}

// Resolve a COCO label name to its class id, or -1 if the name is not a class.
static int id_for_label(const std::string & name)
{
  for (int i = 0; i < kNumCocoLabels; ++i) {
    if (name == kCocoLabels[i]) return i;
  }
  return -1;
}

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

  // ── Tracking parameters ──────────────────────────────────────────────────────
  this->declare_parameter("target_class", std::string("person"));
  this->declare_parameter("tracked_topic", std::string("/tracked_object"));
  this->declare_parameter("track_smoothing", 0.4);
  this->declare_parameter("track_gate", 0.15);
  this->declare_parameter("track_reset_frames", 10);

  auto target_class   = this->get_parameter("target_class").as_string();
  target_class_id_    = id_for_label(target_class);
  track_smoothing_    = static_cast<float>(this->get_parameter("track_smoothing").as_double());
  track_gate_         = static_cast<float>(this->get_parameter("track_gate").as_double());
  track_reset_frames_ = this->get_parameter("track_reset_frames").as_int();

  if (target_class_id_ < 0) {
    RCLCPP_WARN(this->get_logger(),
                "target_class '%s' is not a COCO class; tracker will stay idle "
                "until a valid class is set.", target_class.c_str());
  } else {
    RCLCPP_INFO(this->get_logger(), "Tracking target class '%s' (id %d)",
                target_class.c_str(), target_class_id_);
  }

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

  auto tracked_topic = this->get_parameter("tracked_topic").as_string();

  det_pub_     = this->create_publisher<vision_msgs::msg::Detection2DArray>(det_topic, 10);
  tracked_pub_ = this->create_publisher<vision_msgs::msg::Detection2D>(tracked_topic, 10);
  debug_pub_   = image_transport::create_publisher(this, debug_topic);

  // ── Subscriber ──────────────────────────────────────────────────────────────
  auto img_topic = this->get_parameter("image_topic").as_string();
  image_sub_ = image_transport::create_subscription(
    this, img_topic,
    std::bind(&YoloxDetectorNode::image_callback, this, std::placeholders::_1),
    "raw");

  RCLCPP_INFO(this->get_logger(), "Subscribing to '%s'", img_topic.c_str());

  // Allow switching the tracked class (and retuning) live via `ros2 param set`.
  param_cb_handle_ = this->add_on_set_parameters_callback(
    std::bind(&YoloxDetectorNode::on_set_parameters, this, std::placeholders::_1));
}

rcl_interfaces::msg::SetParametersResult YoloxDetectorNode::on_set_parameters(
  const std::vector<rclcpp::Parameter> & params)
{
  rcl_interfaces::msg::SetParametersResult result;
  result.successful = true;

  for (const auto & p : params) {
    if (p.get_name() == "target_class") {
      int new_id = id_for_label(p.as_string());
      if (new_id < 0) {
        result.successful = false;
        result.reason = "target_class '" + p.as_string() + "' is not a COCO class";
        return result;
      }
      target_class_id_ = new_id;
      have_track_ = false;           // drop the current lock; re-acquire next frame
      frames_since_seen_ = 0;
      RCLCPP_INFO(this->get_logger(), "Tracking target changed to '%s' (id %d)",
                  p.as_string().c_str(), new_id);
    } else if (p.get_name() == "track_smoothing") {
      track_smoothing_ = static_cast<float>(p.as_double());
    } else if (p.get_name() == "track_gate") {
      track_gate_ = static_cast<float>(p.as_double());
    } else if (p.get_name() == "track_reset_frames") {
      track_reset_frames_ = p.as_int();
    } else if (p.get_name() == "score_threshold") {
      score_threshold_ = static_cast<float>(p.as_double());
    }
  }
  return result;
}

// Select the tracked target from this frame's detections and smooth the lock.
// Association: if a lock exists, prefer the matching-class detection whose center
// is nearest the smoothed lock within `track_gate` (× image width); otherwise
// (re)acquire the largest-area matching detection. Returns true and fills `out`
// while a lock is held; publishing is skipped when the target is absent so the
// downstream controller can time out and search.
bool YoloxDetectorNode::update_track(
  const std::vector<Detection> & dets, int img_width,
  vision_msgs::msg::Detection2D & out)
{
  if (target_class_id_ < 0) {
    have_track_ = false;
    return false;
  }

  const Detection * best = nullptr;
  const float gate_px = track_gate_ * static_cast<float>(img_width);

  if (have_track_) {
    float best_dist = std::numeric_limits<float>::max();
    for (const auto & d : dets) {
      if (d.class_id != target_class_id_) continue;
      float cx = (d.x1 + d.x2) * 0.5f;
      float cy = (d.y1 + d.y2) * 0.5f;
      float dist = std::hypot(cx - track_cx_, cy - track_cy_);
      if (dist < best_dist) { best_dist = dist; best = &d; }
    }
    // Reject an association that jumps too far from the current lock.
    if (best && gate_px > 0.f && best_dist > gate_px) best = nullptr;
  }

  if (!best) {
    // (Re)acquire: largest-area matching detection.
    float best_area = 0.f;
    for (const auto & d : dets) {
      if (d.class_id != target_class_id_) continue;
      float area = (d.x2 - d.x1) * (d.y2 - d.y1);
      if (area > best_area) { best_area = area; best = &d; }
    }
  }

  if (!best) {
    // Target not seen this frame.
    if (have_track_ && ++frames_since_seen_ > track_reset_frames_) {
      have_track_ = false;
    }
    return false;
  }

  float cx = (best->x1 + best->x2) * 0.5f;
  float cy = (best->y1 + best->y2) * 0.5f;
  float w  = best->x2 - best->x1;
  float h  = best->y2 - best->y1;

  if (!have_track_) {
    track_cx_ = cx; track_cy_ = cy; track_w_ = w; track_h_ = h;
  } else {
    float a = track_smoothing_;   // weight on the new measurement
    track_cx_ = (1 - a) * track_cx_ + a * cx;
    track_cy_ = (1 - a) * track_cy_ + a * cy;
    track_w_  = (1 - a) * track_w_  + a * w;
    track_h_  = (1 - a) * track_h_  + a * h;
  }
  track_score_ = best->score;
  have_track_ = true;
  frames_since_seen_ = 0;

  out.bbox.center.position.x = track_cx_;
  out.bbox.center.position.y = track_cy_;
  out.bbox.size_x = track_w_;
  out.bbox.size_y = track_h_;

  vision_msgs::msg::ObjectHypothesisWithPose hyp;
  hyp.hypothesis.class_id = label_for(target_class_id_);
  hyp.hypothesis.score    = track_score_;
  out.results.clear();
  out.results.push_back(hyp);
  return true;
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
    hyp.hypothesis.class_id = label_for(d.class_id);
    hyp.hypothesis.score    = d.score;
    det.results.push_back(hyp);

    det_arr.detections.push_back(det);
  }
  det_pub_->publish(det_arr);

  // ── Track + publish the single locked target ─────────────────────────────────
  vision_msgs::msg::Detection2D tracked;
  bool have_target = update_track(dets, cv_img->image.cols, tracked);
  if (have_target) {
    tracked.header = msg->header;
    tracked_pub_->publish(tracked);
  }

  // ── Publish debug image ─────────────────────────────────────────────────────
  if (debug_pub_.getNumSubscribers() > 0) {
    cv::Mat dbg = cv_img->image.clone();
    for (const auto & d : dets) {
      int x1 = static_cast<int>(d.x1);
      int y1 = static_cast<int>(d.y1);
      int x2 = static_cast<int>(d.x2);
      int y2 = static_cast<int>(d.y2);

      cv::rectangle(dbg, cv::Point(x1, y1), cv::Point(x2, y2),
                    cv::Scalar(0, 255, 0), 2);

      const char * name = (d.class_id >= 0 && d.class_id < kNumCocoLabels)
                          ? kCocoLabels[d.class_id] : "unknown";
      char label[64];
      std::snprintf(label, sizeof(label), "%s %.0f%%", name, d.score * 100.f);

      int baseline = 0;
      double font_scale = 0.55;
      int thickness = 1;
      cv::Size text_size = cv::getTextSize(
        label, cv::FONT_HERSHEY_SIMPLEX, font_scale, thickness, &baseline);

      int text_y = (y1 - 4 > text_size.height) ? y1 - 4 : y2 + text_size.height + 4;
      cv::rectangle(dbg,
                    cv::Point(x1, text_y - text_size.height - baseline),
                    cv::Point(x1 + text_size.width, text_y + baseline),
                    cv::Scalar(0, 255, 0), cv::FILLED);
      cv::putText(dbg, label, cv::Point(x1, text_y),
                  cv::FONT_HERSHEY_SIMPLEX, font_scale,
                  cv::Scalar(0, 0, 0), thickness, cv::LINE_AA);
    }

    // Overlay the locked track in red (thicker) on top of the green detections.
    if (have_target) {
      int tx1 = static_cast<int>(track_cx_ - track_w_ * 0.5f);
      int ty1 = static_cast<int>(track_cy_ - track_h_ * 0.5f);
      int tx2 = static_cast<int>(track_cx_ + track_w_ * 0.5f);
      int ty2 = static_cast<int>(track_cy_ + track_h_ * 0.5f);
      cv::rectangle(dbg, cv::Point(tx1, ty1), cv::Point(tx2, ty2),
                    cv::Scalar(0, 0, 255), 3);
      char tlabel[80];
      std::snprintf(tlabel, sizeof(tlabel), "TRACK: %s %.0f%%",
                    label_for(target_class_id_), track_score_ * 100.f);
      cv::putText(dbg, tlabel, cv::Point(tx1, std::max(ty1 - 8, 12)),
                  cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 0, 255), 2,
                  cv::LINE_AA);
      cv::drawMarker(dbg, cv::Point(static_cast<int>(track_cx_),
                                    static_cast<int>(track_cy_)),
                     cv::Scalar(0, 0, 255), cv::MARKER_CROSS, 16, 2);
    }

    auto dbg_msg = cv_bridge::CvImage(msg->header, "bgr8", dbg).toImageMsg();
    debug_pub_.publish(*dbg_msg);
  }
}

}  // namespace yolox_detector
