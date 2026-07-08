"""Follower node for the Create 3 base.

Runs robot_tracker, which publishes geometry_msgs/Twist to /cmd_vel to follow
/tracked_object. Our Create 3 runs Iron firmware at the ROOT namespace, so its
motion_control node subscribes to /cmd_vel directly -- no create3_republisher and
no /_do_not_use bridge is needed. The base is reached over the USB-ethernet gadget
link (usb0); see scripts/create3_usb_gadget.sh and docs/create3_connection.md.

Requires the DDS environment to match the base (set in ~/.bashrc):
  ROS_DOMAIN_ID=0, RMW_IMPLEMENTATION=rmw_fastrtps_cpp,
  FASTRTPS_DEFAULT_PROFILES_FILE=scripts/fastdds_usb0.xml

Set publish_cmd_vel:=false for a dry run (compute and log Twist without moving).
"""

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
        description="Velocity topic the Create 3 subscribes to (geometry_msgs/Twist)")

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
