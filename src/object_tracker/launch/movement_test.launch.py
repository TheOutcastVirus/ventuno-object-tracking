"""Drive test: create3_republisher (base bridge) + movement_test.

Self-contained check that the robot moves. Brings up the Create 3 bridge (so
/cmd_vel reaches the base) and runs the open-loop movement_test sequence. Requires
the DDS environment to match the base (ROS_DOMAIN_ID, RMW_IMPLEMENTATION=rmw_fastrtps_cpp).

    ros2 launch object_tracker movement_test.launch.py
    ros2 launch object_tracker movement_test.launch.py linear_speed:=0.1 repeat:=true
    ros2 launch object_tracker movement_test.launch.py enable_republisher:=false  # bridge already up

Make sure the robot has clearance (open floor or up on blocks). Ctrl-C stops it.
"""

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, IncludeLaunchDescription
from launch.conditions import IfCondition
from launch.launch_description_sources import PythonLaunchDescriptionSource
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.parameter_descriptions import ParameterValue
from launch_ros.substitutions import FindPackageShare


def generate_launch_description():
    cmd_vel_topic_arg = DeclareLaunchArgument(
        "cmd_vel_topic", default_value="/cmd_vel",
        description="Velocity topic for the TurtleBot 4 base (geometry_msgs/Twist)")

    linear_speed_arg = DeclareLaunchArgument(
        "linear_speed", default_value="0.15",
        description="Forward/back speed during the test (m/s)")

    angular_speed_arg = DeclareLaunchArgument(
        "angular_speed", default_value="0.6",
        description="Rotation speed during the test (rad/s)")

    repeat_arg = DeclareLaunchArgument(
        "repeat", default_value="false",
        description="Loop the sequence forever instead of running once")

    enable_republisher_arg = DeclareLaunchArgument(
        "enable_republisher", default_value="true",
        description="Run create3_republisher to bridge the Create 3 base. Set false "
                    "if the base is bridged elsewhere or configured without _do_not_use")

    robot_ns_arg = DeclareLaunchArgument(
        "robot_ns", default_value="/_do_not_use",
        description="Namespace the Create 3 publishes its raw topics under "
                    "(stock TurtleBot 4 on Jazzy uses /_do_not_use)")

    republisher_ns_arg = DeclareLaunchArgument(
        "republisher_ns", default_value="/",
        description="Namespace the bridged clean topics (/cmd_vel, /odom, ...) appear "
                    "under. Must differ from robot_ns")

    create3_republisher = IncludeLaunchDescription(
        PythonLaunchDescriptionSource(PathJoinSubstitution(
            [FindPackageShare("create3_republisher"), "bringup",
             "create3_republisher_launch.py"])),
        launch_arguments={
            "robot_ns": LaunchConfiguration("robot_ns"),
            "republisher_ns": LaunchConfiguration("republisher_ns"),
        }.items(),
        condition=IfCondition(LaunchConfiguration("enable_republisher")),
    )

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
        enable_republisher_arg,
        robot_ns_arg,
        republisher_ns_arg,
        create3_republisher,
        movement_test_node,
    ])
