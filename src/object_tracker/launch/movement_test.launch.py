"""Open-loop drive test for the Create 3 base.

Publishes a short, safe geometry_msgs/Twist sequence straight to /cmd_vel. Our
Create 3 runs Iron firmware at the ROOT namespace, so its motion_control node
subscribes to /cmd_vel directly -- no create3_republisher and no /_do_not_use
bridge is needed. The base is reached over the USB-ethernet gadget link (usb0,
192.168.186.3 <-> Create 3 192.168.186.2); see scripts/create3_usb_gadget.sh and
docs/create3_connection.md.

Requires the DDS environment to match the base (set in ~/.bashrc):
  ROS_DOMAIN_ID=0, RMW_IMPLEMENTATION=rmw_fastrtps_cpp,
  FASTRTPS_DEFAULT_PROFILES_FILE=scripts/fastdds_usb0.xml

    ros2 launch object_tracker movement_test.launch.py
    ros2 launch object_tracker movement_test.launch.py linear_speed:=0.1 repeat:=true

Make sure the robot has clearance (open floor or up on blocks) and is OFF the
dock -- a docked Create 3 ignores forward /cmd_vel. Ctrl-C stops it.
"""

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node
from launch_ros.parameter_descriptions import ParameterValue


def generate_launch_description():
    cmd_vel_topic_arg = DeclareLaunchArgument(
        "cmd_vel_topic", default_value="/cmd_vel",
        description="Velocity topic the Create 3 subscribes to (geometry_msgs/Twist)")

    linear_speed_arg = DeclareLaunchArgument(
        "linear_speed", default_value="0.15",
        description="Forward/back speed during the test (m/s)")

    angular_speed_arg = DeclareLaunchArgument(
        "angular_speed", default_value="0.6",
        description="Rotation speed during the test (rad/s)")

    repeat_arg = DeclareLaunchArgument(
        "repeat", default_value="false",
        description="Loop the sequence forever instead of running once")

    movement_test_node = Node(
        package="object_tracker",
        executable="movement_test",
        name="movement_test",
        parameters=[{
            "cmd_vel_topic": LaunchConfiguration("cmd_vel_topic"),
            "linear_speed": ParameterValue(
                LaunchConfiguration("linear_speed"), value_type=float),
            "angular_speed": ParameterValue(
                LaunchConfiguration("angular_speed"), value_type=float),
            "repeat": ParameterValue(
                LaunchConfiguration("repeat"), value_type=bool),
        }],
        output="screen",
    )

    return LaunchDescription([
        cmd_vel_topic_arg,
        linear_speed_arg,
        angular_speed_arg,
        repeat_arg,
        movement_test_node,
    ])
