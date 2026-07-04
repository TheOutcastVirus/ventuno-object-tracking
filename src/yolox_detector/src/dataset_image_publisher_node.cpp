#include <cv_bridge/cv_bridge.hpp>
#include <opencv2/imgcodecs.hpp>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/image.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

bool has_image_extension(const fs::path & path)
{
  std::string ext = path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return ext == ".jpg" || ext == ".jpeg" || ext == ".png" ||
         ext == ".bmp" || ext == ".webp";
}

std::vector<fs::path> collect_images(const std::string & dataset_path)
{
  std::vector<fs::path> images;
  fs::path root(dataset_path);
  if (!fs::exists(root)) {
    return images;
  }

  if (fs::is_regular_file(root) && has_image_extension(root)) {
    images.push_back(root);
    return images;
  }

  for (const auto & entry : fs::recursive_directory_iterator(root)) {
    if (entry.is_regular_file() && has_image_extension(entry.path())) {
      images.push_back(entry.path());
    }
  }
  std::sort(images.begin(), images.end());
  return images;
}

}  // namespace

class DatasetImagePublisherNode : public rclcpp::Node {
public:
  DatasetImagePublisherNode()
  : Node("dataset_image_publisher")
  {
    declare_parameter("dataset_path", std::string("datasets/sample_images"));
    declare_parameter("image_topic", std::string("/camera/rgb/image_raw"));
    declare_parameter("frame_id", std::string("dataset_camera"));
    declare_parameter("publish_rate", 15.0);
    declare_parameter("loop", true);

    dataset_path_ = get_parameter("dataset_path").as_string();
    image_topic_ = get_parameter("image_topic").as_string();
    frame_id_ = get_parameter("frame_id").as_string();
    loop_ = get_parameter("loop").as_bool();
    double publish_rate = get_parameter("publish_rate").as_double();
    if (publish_rate <= 0.0) {
      throw std::invalid_argument("publish_rate must be greater than zero");
    }

    images_ = collect_images(dataset_path_);
    if (images_.empty()) {
      throw std::runtime_error("No images found in dataset_path: " + dataset_path_);
    }

    publisher_ = create_publisher<sensor_msgs::msg::Image>(image_topic_, 10);
    auto period = std::chrono::duration<double>(1.0 / publish_rate);
    timer_ = create_wall_timer(
      std::chrono::duration_cast<std::chrono::nanoseconds>(period),
      std::bind(&DatasetImagePublisherNode::publish_next, this));

    RCLCPP_INFO(
      get_logger(), "Publishing %zu dataset images from '%s' to '%s' at %.2f Hz",
      images_.size(), dataset_path_.c_str(), image_topic_.c_str(), publish_rate);
  }

private:
  void publish_next()
  {
    if (images_.empty()) {
      return;
    }
    if (next_index_ >= images_.size()) {
      if (!loop_) {
        timer_->cancel();
        RCLCPP_INFO(get_logger(), "Dataset playback complete");
        return;
      }
      next_index_ = 0;
    }

    const fs::path & path = images_[next_index_++];
    cv::Mat image = cv::imread(path.string(), cv::IMREAD_COLOR);
    if (image.empty()) {
      RCLCPP_WARN(get_logger(), "Skipping unreadable image: %s", path.c_str());
      return;
    }

    std_msgs::msg::Header header;
    header.stamp = now();
    header.frame_id = frame_id_;
    auto msg = cv_bridge::CvImage(header, "bgr8", image).toImageMsg();
    publisher_->publish(*msg);
  }

  std::string dataset_path_;
  std::string image_topic_;
  std::string frame_id_;
  bool loop_{true};
  std::vector<fs::path> images_;
  std::size_t next_index_{0};
  rclcpp::Publisher<sensor_msgs::msg::Image>::SharedPtr publisher_;
  rclcpp::TimerBase::SharedPtr timer_;
};

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<DatasetImagePublisherNode>());
  rclcpp::shutdown();
  return 0;
}
