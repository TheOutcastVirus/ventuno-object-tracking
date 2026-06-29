from launch import LaunchDescription
from launch_ros.actions import Node
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.substitutions import FindPackageShare


def generate_launch_description():
    config = PathJoinSubstitution([
        FindPackageShare("oak_camera"), "config", "camera.yaml"
    ])

    return LaunchDescription([
        DeclareLaunchArgument(
            "params_file",
            default_value=config,
            description="Path to the camera YAML parameter file",
        ),
        Node(
            package="oak_camera",
            executable="oak_camera_node",
            name="oak_camera_node",
            output="screen",
            parameters=[LaunchConfiguration("params_file")],
        ),
    ])
