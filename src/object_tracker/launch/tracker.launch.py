from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.parameter_descriptions import ParameterValue
from launch_ros.substitutions import FindPackageShare


def generate_launch_description():
    publish_cmd_vel_arg = DeclareLaunchArgument(
        "publish_cmd_vel", default_value="true",
        description="false = dry run: compute and log Twist without moving the robot")

    cmd_vel_topic_arg = DeclareLaunchArgument(
        "cmd_vel_topic", default_value="/cmd_vel",
        description="Velocity topic for the TurtleBot 4 base (geometry_msgs/Twist)")

    params_file = PathJoinSubstitution(
        [FindPackageShare("object_tracker"), "config", "tracker.yaml"])

    tracker_node = Node(
        package="object_tracker",
        executable="robot_tracker",
        name="robot_tracker",
        parameters=[
            params_file,
            {
                "publish_cmd_vel": ParameterValue(
                    LaunchConfiguration("publish_cmd_vel"), value_type=bool),
                "cmd_vel_topic": LaunchConfiguration("cmd_vel_topic"),
            },
        ],
        output="screen",
    )

    return LaunchDescription([
        publish_cmd_vel_arg,
        cmd_vel_topic_arg,
        tracker_node,
    ])
