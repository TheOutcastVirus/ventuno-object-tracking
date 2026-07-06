from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare

from launch import LaunchDescription


def generate_launch_description():
    backend_arg = DeclareLaunchArgument(
        "backend",
        default_value="npu",
        description="Inference backend: 'cpu' (XNNPACK) or 'npu' (QNN HTP)",
    )

    model_path_arg = DeclareLaunchArgument(
        "model_path",
        default_value="models/yolox_tiny_qnn.pte",
        description="Path to .pte model file",
    )

    image_topic_arg = DeclareLaunchArgument(
        "image_topic",
        default_value="/oak/rgb/image_raw",
        description="Image topic shared by dataset publisher and detector",
    )

    qnn_lib_dir_arg = DeclareLaunchArgument(
        "qnn_lib_dir",
        default_value="/opt/qcom/aistack/qairt/2.47.0.260601/lib/aarch64-oe-linux-gcc11.2",
        description="Directory containing QNN runtime libraries",
    )

    score_threshold_arg = DeclareLaunchArgument(
        "score_threshold",
        default_value="0.45",
        description="Minimum confidence score for detections (0.0–1.0)",
    )

    params_file = PathJoinSubstitution(
        [FindPackageShare("yolox_detector"), "config", "detector.yaml"]
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
                # "qnn_lib_dir": LaunchConfiguration("qnn_lib_dir"),
                "image_topic": LaunchConfiguration("image_topic"),
                "score_threshold": LaunchConfiguration("score_threshold"),
            },
        ],
        output="screen",
    )

    return LaunchDescription(
        [
            backend_arg,
            model_path_arg,
            image_topic_arg,
            qnn_lib_dir_arg,
            score_threshold_arg,
            detector_node,
        ]
    )
