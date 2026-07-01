#include <memory>
#include <rclcpp/rclcpp.hpp>
#include "oak_camera/oak_camera_node.hpp"

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  auto node = std::make_shared<oak_camera::OakCameraNode>(rclcpp::NodeOptions());
  rclcpp::spin(node->get_node_base_interface());
  rclcpp::shutdown();
  return 0;
}
