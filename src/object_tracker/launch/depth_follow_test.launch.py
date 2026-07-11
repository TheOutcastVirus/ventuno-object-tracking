"""Dry-run person-distance test: OAK RGB+stereo depth -> YOLOX -> tracker.

This launch intentionally forces ``publish_cmd_vel`` to false. It cannot command
motion; the tracker prints a valid RGB-aligned stereo distance for its locked
person target approximately twice per second.

Example:
  ros2 launch object_tracker depth_follow_test.launch.py \
    backend:=npu model_path:=models/yolox_tiny_qnn.pte
"""

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import EnvironmentVariable, LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.parameter_descriptions import ParameterValue
from launch_ros.substitutions import FindPackageShare


def generate_launch_description():
    backend_arg = DeclareLaunchArgument(
        'backend', default_value='npu',
        description="YOLOX backend: 'cpu' (XNNPACK) or 'npu' (QNN HTP)")
    model_path_arg = DeclareLaunchArgument(
        'model_path', default_value='',
        description='Path to the YOLOX .pte model; empty selects the backend default')
    qnn_lib_dir_arg = DeclareLaunchArgument(
        'qnn_lib_dir',
        default_value=PathJoinSubstitution([
            EnvironmentVariable('QAIRT_LIB'), 'aarch64-oe-linux-gcc11.2']),
        description='Directory containing QNN runtime libraries')
    score_threshold_arg = DeclareLaunchArgument(
        'score_threshold', default_value='0.45',
        description='YOLOX minimum detection confidence')

    camera_params = PathJoinSubstitution(
        [FindPackageShare('oak_camera'), 'config', 'camera.yaml'])
    detector_params = PathJoinSubstitution(
        [FindPackageShare('yolox_detector'), 'config', 'detector.yaml'])
    tracker_params = PathJoinSubstitution(
        [FindPackageShare('object_tracker'), 'config', 'tracker.yaml'])

    camera = Node(
        package='oak_camera',
        executable='oak_camera_node',
        name='oak_camera_node',
        parameters=[camera_params],
        output='screen')

    detector = Node(
        package='yolox_detector',
        executable='yolox_detector_node',
        name='yolox_detector',
        parameters=[
            detector_params,
            {
                'backend': LaunchConfiguration('backend'),
                'model_path': LaunchConfiguration('model_path'),
                'qnn_lib_dir': LaunchConfiguration('qnn_lib_dir'),
                'image_topic': '/oak/rgb/image_raw',
                'score_threshold': LaunchConfiguration('score_threshold'),
            },
        ],
        output='screen')

    # This override is deliberate and non-configurable in this test launch.
    # The node computes commands for observability but never publishes cmd_vel.
    tracker = Node(
        package='object_tracker',
        executable='robot_tracker',
        name='robot_tracker',
        parameters=[
            tracker_params,
            {
                'publish_cmd_vel': False,
                'log_target_distance': True,
            },
        ],
        output='screen')

    return LaunchDescription([
        backend_arg,
        model_path_arg,
        qnn_lib_dir_arg,
        score_threshold_arg,
        camera,
        detector,
        tracker,
    ])
