#!/usr/bin/env python3
"""Follow the yolox_detector locked target with a TurtleBot 4.

Subscribes to ``/tracked_object`` (vision_msgs/Detection2D, the single target the
detector has locked onto) and publishes ``geometry_msgs/Twist`` velocity commands:

* **Angular** — proportional to the target's horizontal offset from image center,
  so the robot rotates to keep the target centered.
* **Linear** — proportional to the error between the target's measured stereo
  depth and a desired following distance. Invalid, stale, or low-coverage depth
  disables linear motion; the robot only centers the target until depth recovers.

When the target is lost (no message within ``lost_timeout``), the robot stops and,
after a short grace period, rotates in place toward the side the target was last
seen to re-acquire it (search-when-lost).

Safety: velocities are clamped to configurable caps, small errors are ignored via
deadbands, ``publish_cmd_vel:=false`` runs a no-motion dry run, and a zero Twist is
published on shutdown.
"""

import math

import numpy as np
import rclpy
from geometry_msgs.msg import Twist
from rclpy.node import Node
from sensor_msgs.msg import Image
from vision_msgs.msg import Detection2D


def clamp(value, limit):
    """Clamp ``value`` to the symmetric range [-limit, limit]."""
    return max(-limit, min(limit, value))


class RobotTracker(Node):
    def __init__(self):
        super().__init__('robot_tracker')

        # ── Parameters ───────────────────────────────────────────────────────
        # Defaults match the OAK RGB stream configured in camera.yaml.
        self.declare_parameter('image_width', 640)
        self.declare_parameter('image_height', 360)
        self.declare_parameter('angular_gain', 2.0)
        self.declare_parameter('linear_gain', 0.7)
        self.declare_parameter('target_distance_m', 1.2)
        self.declare_parameter('depth_topic', '/oak/depth/image_raw')
        self.declare_parameter('depth_max_age', 0.20)
        self.declare_parameter('depth_min_m', 0.4)
        self.declare_parameter('depth_max_m', 3.0)
        self.declare_parameter('depth_min_valid_fraction', 0.15)
        self.declare_parameter('depth_roi_side_trim', 0.20)
        self.declare_parameter('depth_roi_top_trim', 0.15)
        self.declare_parameter('depth_roi_bottom_trim', 0.20)
        self.declare_parameter('depth_smoothing', 0.4)
        self.declare_parameter('log_target_distance', False)
        self.declare_parameter('max_linear_speed', 0.25)
        self.declare_parameter('max_angular_speed', 1.5)
        self.declare_parameter('angular_deadband', 0.02)
        self.declare_parameter('distance_deadband', 0.03)
        self.declare_parameter('lost_timeout', 0.6)
        self.declare_parameter('search_grace', 0.5)
        self.declare_parameter('search_angular_speed', 0.4)
        self.declare_parameter('control_rate', 20.0)
        self.declare_parameter('cmd_vel_topic', '/cmd_vel')
        self.declare_parameter('publish_cmd_vel', True)

        g = self.get_parameter
        self.image_width = g('image_width').value
        self.image_height = g('image_height').value
        self.angular_gain = g('angular_gain').value
        self.linear_gain = g('linear_gain').value
        self.target_distance_m = g('target_distance_m').value
        self.depth_topic = g('depth_topic').value
        self.depth_max_age = g('depth_max_age').value
        self.depth_min_m = g('depth_min_m').value
        self.depth_max_m = g('depth_max_m').value
        self.depth_min_valid_fraction = g('depth_min_valid_fraction').value
        self.depth_roi_side_trim = g('depth_roi_side_trim').value
        self.depth_roi_top_trim = g('depth_roi_top_trim').value
        self.depth_roi_bottom_trim = g('depth_roi_bottom_trim').value
        self.depth_smoothing = g('depth_smoothing').value
        self.log_target_distance = g('log_target_distance').value
        self.max_linear_speed = g('max_linear_speed').value
        self.max_angular_speed = g('max_angular_speed').value
        self.angular_deadband = g('angular_deadband').value
        self.distance_deadband = g('distance_deadband').value
        self.lost_timeout = g('lost_timeout').value
        self.search_grace = g('search_grace').value
        self.search_angular_speed = g('search_angular_speed').value
        control_rate = g('control_rate').value
        cmd_vel_topic = g('cmd_vel_topic').value
        self.publish_cmd_vel = g('publish_cmd_vel').value

        # ── State ────────────────────────────────────────────────────────────
        self.last_target = None          # latest Detection2D
        self.last_target_time = None     # ROS time it arrived
        self.last_seen_sign = 1.0        # +1 target was on the right, -1 left
        self.last_depth = None           # latest RGB-aligned 16UC1 depth image
        self.last_depth_stamp_ns = None
        self.filtered_distance_m = None

        # ── ROS interfaces ───────────────────────────────────────────────────
        self.cmd_pub = self.create_publisher(Twist, cmd_vel_topic, 10)
        self.sub = self.create_subscription(
            Detection2D, '/tracked_object', self.on_target, 10)
        self.depth_sub = self.create_subscription(
            Image, self.depth_topic, self.on_depth, 1)
        self.timer = self.create_timer(1.0 / control_rate, self.control_step)

        self.get_logger().info(
            f"robot_tracker up: cmd_vel='{cmd_vel_topic}' "
            f"publish={self.publish_cmd_vel} "
            f"max_lin={self.max_linear_speed} max_ang={self.max_angular_speed} "
            f"target_distance={self.target_distance_m:.2f}m depth='{self.depth_topic}'")
        if not self.publish_cmd_vel:
            self.get_logger().warn(
                'publish_cmd_vel is FALSE: computing commands but not moving.')

    def on_target(self, msg):
        self.last_target = msg
        self.last_target_time = self.get_clock().now()

    def on_depth(self, msg):
        """Cache RGB-aligned 16-bit millimetre depth for the current target."""
        if msg.encoding not in ('16UC1', 'mono16'):
            self.get_logger().error(
                f"Expected 16UC1 depth on '{self.depth_topic}', got '{msg.encoding}'",
                throttle_duration_sec=2.0)
            return
        if msg.step < msg.width * 2 or len(msg.data) < msg.step * msg.height:
            self.get_logger().error('Received malformed depth image', throttle_duration_sec=2.0)
            return

        dtype = '>u2' if msg.is_bigendian else '<u2'
        raw = np.frombuffer(msg.data, dtype=dtype)
        # `step` may contain row padding, so first reshape by stride then crop.
        row_words = msg.step // 2
        self.last_depth = raw.reshape(msg.height, row_words)[:, :msg.width]
        self.last_depth_stamp_ns = (
            msg.header.stamp.sec * 1_000_000_000 + msg.header.stamp.nanosec)

    def control_step(self):
        twist = Twist()

        age = None
        if self.last_target_time is not None:
            age = (self.get_clock().now() - self.last_target_time).nanoseconds * 1e-9

        if self.last_target is not None and age is not None and age < self.lost_timeout:
            self._follow(self.last_target, twist)
        elif age is not None and age >= self.search_grace:
            # Lost for a while: rotate in place toward where it was last seen.
            twist.angular.z = self.search_angular_speed * self.last_seen_sign
        # else: never seen, or within grace period -> stay stopped.

        self._publish(twist)

    def _follow(self, target, twist):
        cx = target.bbox.center.position.x
        # Angular: normalized horizontal offset from image center, [-1, 1].
        ex = (cx - self.image_width / 2.0) / (self.image_width / 2.0)
        if abs(ex) > 1e-6:
            self.last_seen_sign = -1.0 if ex > 0 else 1.0  # search back toward it
        if abs(ex) < self.angular_deadband:
            ex = 0.0
        # +angular.z is CCW (left); target on the right (ex > 0) -> turn right.
        twist.angular.z = clamp(-self.angular_gain * ex, self.max_angular_speed)

        distance_m = self._estimate_target_distance(target)
        if distance_m is None:
            # Safety contract: when range is unreliable, retain centering only.
            # This is intentionally not a bbox-size fallback because that could
            # command forward motion from a bad depth observation.
            twist.linear.x = 0.0
            return

        if self.log_target_distance:
            self.get_logger().info(
                f'tracked person distance={distance_m:.2f} m',
                throttle_duration_sec=0.5)

        ed = distance_m - self.target_distance_m
        if abs(ed) < self.distance_deadband:
            ed = 0.0
        twist.linear.x = clamp(self.linear_gain * ed, self.max_linear_speed)

    def _estimate_target_distance(self, target):
        """Return a robust metric range for the locked target, or ``None``.

        Depth is RGB-aligned and measured in millimetres. We only sample a
        contracted inner box so background around a YOLOX box, limbs, and box
        edges do not dominate the person measurement. The median rejects the
        remaining isolated stereo outliers.
        """
        if self.last_depth is None or self.last_depth_stamp_ns is None:
            return None

        target_stamp_ns = (
            target.header.stamp.sec * 1_000_000_000 + target.header.stamp.nanosec)
        age = abs(target_stamp_ns - self.last_depth_stamp_ns) * 1e-9
        if age > self.depth_max_age:
            self.filtered_distance_m = None
            return None

        image_h, image_w = self.last_depth.shape
        cx = target.bbox.center.position.x
        cy = target.bbox.center.position.y
        box_w = target.bbox.size_x
        box_h = target.bbox.size_y
        x1 = max(0, math.floor(cx - box_w * (0.5 - self.depth_roi_side_trim)))
        x2 = min(image_w, math.ceil(cx + box_w * (0.5 - self.depth_roi_side_trim)))
        y1 = max(0, math.floor(cy - box_h * (0.5 - self.depth_roi_top_trim)))
        y2 = min(image_h, math.ceil(cy + box_h * (0.5 - self.depth_roi_bottom_trim)))
        if x2 <= x1 or y2 <= y1:
            return None

        roi_mm = self.last_depth[y1:y2, x1:x2]
        values_m = roi_mm.astype(np.float32).ravel() * 0.001
        valid = values_m[(values_m >= self.depth_min_m) & (values_m <= self.depth_max_m)]
        if valid.size < roi_mm.size * self.depth_min_valid_fraction:
            self.filtered_distance_m = None
            return None

        measured_m = float(np.median(valid))
        if self.filtered_distance_m is None:
            self.filtered_distance_m = measured_m
        else:
            alpha = self.depth_smoothing
            self.filtered_distance_m = (
                alpha * measured_m + (1.0 - alpha) * self.filtered_distance_m)
        return self.filtered_distance_m

    def _publish(self, twist):
        if self.publish_cmd_vel:
            self.cmd_pub.publish(twist)
        else:
            self.get_logger().info(
                f"[dry-run] lin={twist.linear.x:+.3f} ang={twist.angular.z:+.3f}",
                throttle_duration_sec=0.5)

    def stop(self):
        """Publish a single zero Twist so the robot halts on shutdown."""
        try:
            self.cmd_pub.publish(Twist())
        except Exception:
            pass


def main(args=None):
    rclpy.init(args=args)
    node = RobotTracker()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        node.stop()
        node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()


if __name__ == '__main__':
    main()
