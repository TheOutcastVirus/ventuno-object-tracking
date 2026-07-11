import numpy as np
import pytest
import rclpy
from geometry_msgs.msg import Twist
from vision_msgs.msg import Detection2D

from object_tracker.robot_tracker import RobotTracker


@pytest.fixture
def tracker():
    rclpy.init()
    node = RobotTracker()
    try:
        yield node
    finally:
        node.destroy_node()
        rclpy.shutdown()


def target_at(stamp):
    target = Detection2D()
    target.header.stamp = stamp.to_msg()
    target.bbox.center.position.x = 320.0
    target.bbox.center.position.y = 180.0
    target.bbox.size_x = 160.0
    target.bbox.size_y = 240.0
    return target


def test_inner_roi_depth_drives_metric_following(tracker):
    stamp = tracker.get_clock().now()
    tracker.last_depth_stamp_ns = stamp.nanoseconds
    # Background depth exists outside the contracted person ROI. The estimator
    # must use the target's inner ROI rather than the full RGB frame.
    tracker.last_depth = np.full((360, 640), 3000, dtype=np.uint16)
    tracker.last_depth[100:260, 200:440] = 2000

    target = target_at(stamp)
    assert tracker._estimate_target_distance(target) == pytest.approx(2.0)

    command = Twist()
    tracker._follow(target, command)
    assert command.linear.x > 0.0


def test_stale_or_invalid_depth_disables_linear_motion(tracker):
    stamp = tracker.get_clock().now()
    target = target_at(stamp)
    tracker.last_depth = np.full((360, 640), 2000, dtype=np.uint16)
    tracker.last_depth_stamp_ns = stamp.nanoseconds - 1_000_000_000

    command = Twist()
    tracker._follow(target, command)
    assert command.linear.x == 0.0

    tracker.last_depth_stamp_ns = stamp.nanoseconds
    tracker.last_depth.fill(0)
    tracker.filtered_distance_m = None
    command = Twist()
    tracker._follow(target, command)
    assert command.linear.x == 0.0
