#pragma once
#include "yolox_detector/inference_backend.hpp"
#include "yolox_detector/postprocess.hpp"
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <vision_msgs/msg/detection2_d.hpp>
#include <vision_msgs/msg/detection2_d_array.hpp>
#include <image_transport/image_transport.hpp>
#include <memory>
#include <string>
#include <vector>

namespace yolox_detector {

class YoloxDetectorNode : public rclcpp::Node {
public:
  explicit YoloxDetectorNode(const rclcpp::NodeOptions & options = rclcpp::NodeOptions());

private:
  void image_callback(const sensor_msgs::msg::Image::ConstSharedPtr & msg);

  // Select + smooth the single tracked target from this frame's detections.
  // `img_width` scales the association gate. Returns true and fills `out` when a
  // locked target is available to publish.
  bool update_track(const std::vector<Detection> & dets, int img_width,
                    vision_msgs::msg::Detection2D & out);

  rcl_interfaces::msg::SetParametersResult on_set_parameters(
    const std::vector<rclcpp::Parameter> & params);

  std::unique_ptr<InferenceBackend> backend_;

  float score_threshold_{0.45f};
  float nms_threshold_{0.45f};
  int num_classes_{80};

  // ── Tracking state ────────────────────────────────────────────────────────
  int target_class_id_{0};       // resolved COCO id for target_class (-1 = unknown)
  float track_smoothing_{0.4f};  // EMA alpha applied to the locked bbox
  float track_gate_{0.15f};      // max center dist (fraction of width) to associate
  int track_reset_frames_{10};   // frames unseen before the lock is dropped

  bool have_track_{false};
  float track_cx_{0.f}, track_cy_{0.f}, track_w_{0.f}, track_h_{0.f};
  float track_score_{0.f};
  int frames_since_seen_{0};

  image_transport::Subscriber image_sub_;
  rclcpp::Publisher<vision_msgs::msg::Detection2DArray>::SharedPtr det_pub_;
  rclcpp::Publisher<vision_msgs::msg::Detection2D>::SharedPtr tracked_pub_;
  image_transport::Publisher debug_pub_;
  OnSetParametersCallbackHandle::SharedPtr param_cb_handle_;
};

}  // namespace yolox_detector
