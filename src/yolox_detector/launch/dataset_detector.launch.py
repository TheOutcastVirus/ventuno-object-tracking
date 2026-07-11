from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import EnvironmentVariable, LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare


def generate_launch_description():
    backend_arg = DeclareLaunchArgument(
        "backend", default_value="npu",
        description="Inference backend: 'cpu' (XNNPACK) or 'npu' (QNN HTP)")

    model_path_arg = DeclareLaunchArgument(
        "model_path", default_value="",
        description="Path to .pte model file; empty selects the default for the backend")

    dataset_path_arg = DeclareLaunchArgument(
        "dataset_path", default_value="datasets/sample_images",
        description="Directory or image file to publish as detector input")

    image_topic_arg = DeclareLaunchArgument(
        "image_topic", default_value="/camera/rgb/image_raw",
        description="Image topic shared by dataset publisher and detector")

    publish_rate_arg = DeclareLaunchArgument(
        "publish_rate", default_value="15.0",
        description="Dataset playback rate in Hz")

    qnn_lib_dir_arg = DeclareLaunchArgument(
        "qnn_lib_dir",
        default_value=PathJoinSubstitution([
            EnvironmentVariable("QAIRT_LIB"), "aarch64-oe-linux-gcc11.2"]),
        description="Directory containing QNN runtime libraries; defaults to "
                    "$QAIRT_LIB/aarch64-oe-linux-gcc11.2")

    params_file = PathJoinSubstitution(
        [FindPackageShare("yolox_detector"), "config", "detector.yaml"])

    dataset_node = Node(
        package="yolox_detector",
        executable="dataset_image_publisher_node",
        name="dataset_image_publisher",
        parameters=[
            params_file,
            {
                "dataset_path": LaunchConfiguration("dataset_path"),
                "image_topic": LaunchConfiguration("image_topic"),
                "publish_rate": LaunchConfiguration("publish_rate"),
            },
        ],
        output="screen",
    )

    detector_node = Node(
        package="yolox_detector",
        executable="yolox_detector_node",
        name="yolox_detector",
        parameters=[
            params_file,
            {
                "backend": LaunchConfiguration("backend"),
                "model_path": LaunchConfiguration("model_path"),
                "qnn_lib_dir": LaunchConfiguration("qnn_lib_dir"),
                "image_topic": LaunchConfiguration("image_topic"),
            },
        ],
        output="screen",
    )

    return LaunchDescription([
        backend_arg,
        model_path_arg,
        dataset_path_arg,
        image_topic_arg,
        publish_rate_arg,
        qnn_lib_dir_arg,
        dataset_node,
        detector_node,
    ])
