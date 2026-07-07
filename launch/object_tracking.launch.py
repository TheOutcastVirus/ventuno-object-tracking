"""Full object-tracking demo: OAK camera -> YOLOX detector -> TurtleBot 4 follower.

Starts three nodes:
  * oak_camera_node   — publishes RGB on /oak/rgb/image_raw
  * yolox_detector    — detects, locks a single `target_class`, publishes
                        /detections, /detections/image and /tracked_object
  * robot_tracker     — drives the base (/cmd_vel) to follow /tracked_object

Defaults to the NPU (QNN) backend, since only models/yolox_tiny_qnn.pte exists on
disk (the CPU .pte is not present). Switch the followed object at runtime with:
    ros2 param set /yolox_detector target_class bottle

Examples:
    ros2 launch object_tracking.launch.py
    ros2 launch object_tracking.launch.py target_class:=bottle
    ros2 launch object_tracking.launch.py publish_cmd_vel:=false   # dry run
    ros2 launch object_tracking.launch.py backend:=cpu model_path:=models/yolox_tiny_xnnpack.pte
"""

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.parameter_descriptions import ParameterValue
from launch_ros.substitutions import FindPackageShare


def generate_launch_description():
    backend_arg = DeclareLaunchArgument(
        "backend", default_value="npu",
        description="Inference backend: 'cpu' (XNNPACK) or 'npu' (QNN HTP)")

    model_path_arg = DeclareLaunchArgument(
        "model_path", default_value="models/yolox_tiny_qnn.pte",
        description="Path to the .pte model file")

    image_topic_arg = DeclareLaunchArgument(
        "image_topic", default_value="/oak/rgb/image_raw",
        description="RGB image topic shared by the camera and detector")

    target_class_arg = DeclareLaunchArgument(
        "target_class", default_value="person",
        description="COCO class to lock onto and follow (e.g. person, bottle, chair)")

    qnn_lib_dir_arg = DeclareLaunchArgument(
        "qnn_lib_dir",
        default_value="/opt/qcom/aistack/qairt/2.47.0.260601/lib/aarch64-oe-linux-gcc11.2",
        description="Directory containing QNN runtime libraries (npu backend)")

    publish_cmd_vel_arg = DeclareLaunchArgument(
        "publish_cmd_vel", default_value="true",
        description="false = dry run: log Twist commands without moving the robot")

    cmd_vel_topic_arg = DeclareLaunchArgument(
        "cmd_vel_topic", default_value="/cmd_vel",
        description="Velocity topic for the TurtleBot 4 base (geometry_msgs/Twist)")

    camera_params = PathJoinSubstitution(
        [FindPackageShare("oak_camera"), "config", "camera.yaml"])
    detector_params = PathJoinSubstitution(
        [FindPackageShare("yolox_detector"), "config", "detector.yaml"])
    tracker_params = PathJoinSubstitution(
        [FindPackageShare("object_tracker"), "config", "tracker.yaml"])

    camera_node = Node(
        package="oak_camera",
        executable="oak_camera_node",
        name="oak_camera",
        parameters=[camera_params],
        output="screen",
    )

    detector_node = Node(
        package="yolox_detector",
        executable="yolox_detector_node",
        name="yolox_detector",
        parameters=[
            detector_params,
            {
                "backend": LaunchConfiguration("backend"),
                "model_path": LaunchConfiguration("model_path"),
                "qnn_lib_dir": LaunchConfiguration("qnn_lib_dir"),
                "image_topic": LaunchConfiguration("image_topic"),
                "target_class": LaunchConfiguration("target_class"),
            },
        ],
        output="screen",
    )

    tracker_node = Node(
        package="object_tracker",
        executable="robot_tracker",
        name="robot_tracker",
        parameters=[
            tracker_params,
            {
                "publish_cmd_vel": ParameterValue(
                    LaunchConfiguration("publish_cmd_vel"), value_type=bool),
                "cmd_vel_topic": LaunchConfiguration("cmd_vel_topic"),
            },
        ],
        output="screen",
    )

    return LaunchDescription([
        backend_arg,
        model_path_arg,
        image_topic_arg,
        target_class_arg,
        qnn_lib_dir_arg,
        publish_cmd_vel_arg,
        cmd_vel_topic_arg,
        camera_node,
        detector_node,
        tracker_node,
    ])
