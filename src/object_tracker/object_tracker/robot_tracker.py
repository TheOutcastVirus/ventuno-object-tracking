#!/usr/bin/env python3
"""Follow the yolox_detector locked target with a TurtleBot 4.

Subscribes to ``/tracked_object`` (vision_msgs/Detection2D, the single target the
detector has locked onto) and publishes ``geometry_msgs/Twist`` velocity commands:

* **Angular** — proportional to the target's horizontal offset from image center,
  so the robot rotates to keep the target centered.
* **Linear** — proportional to the error between the target's bounding-box height
  (as a fraction of the image height) and a desired "fill". Since a closer object
  appears larger, this holds a roughly constant following distance without any
  depth sensor. Too far -> drive forward; too close -> back up.

When the target is lost (no message within ``lost_timeout``), the robot stops and,
after a short grace period, rotates in place toward the side the target was last
seen to re-acquire it (search-when-lost).

Safety: velocities are clamped to configurable caps, small errors are ignored via
deadbands, ``publish_cmd_vel:=false`` runs a no-motion dry run, and a zero Twist is
published on shutdown.
"""

import math

import rclpy
from geometry_msgs.msg import Twist
from rclpy.node import Node
from vision_msgs.msg import Detection2D


def clamp(value, limit):
    """Clamp ``value`` to the symmetric range [-limit, limit]."""
    return max(-limit, min(limit, value))


class RobotTracker(Node):
    def __init__(self):
        super().__init__('robot_tracker')

        # ── Parameters ───────────────────────────────────────────────────────
        self.declare_parameter('image_width', 1920)
        self.declare_parameter('image_height', 1080)
        self.declare_parameter('angular_gain', 1.2)
        self.declare_parameter('linear_gain', 0.8)
        self.declare_parameter('target_bbox_fill', 0.5)
        self.declare_parameter('max_linear_speed', 0.20)
        self.declare_parameter('max_angular_speed', 1.0)
        self.declare_parameter('angular_deadband', 0.05)
        self.declare_parameter('distance_deadband', 0.05)
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
        self.target_bbox_fill = g('target_bbox_fill').value
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

        # ── ROS interfaces ───────────────────────────────────────────────────
        self.cmd_pub = self.create_publisher(Twist, cmd_vel_topic, 10)
        self.sub = self.create_subscription(
            Detection2D, '/tracked_object', self.on_target, 10)
        self.timer = self.create_timer(1.0 / control_rate, self.control_step)

        self.get_logger().info(
            f"robot_tracker up: cmd_vel='{cmd_vel_topic}' "
            f"publish={self.publish_cmd_vel} "
            f"max_lin={self.max_linear_speed} max_ang={self.max_angular_speed} "
            f"target_fill={self.target_bbox_fill}")
        if not self.publish_cmd_vel:
            self.get_logger().warn(
                'publish_cmd_vel is FALSE: computing commands but not moving.')

    def on_target(self, msg):
        self.last_target = msg
        self.last_target_time = self.get_clock().now()

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

        # Linear: bbox height as a fraction of the image = distance proxy.
        fill = target.bbox.size_y / float(self.image_height)
        ed = self.target_bbox_fill - fill  # fill < target -> too far -> forward
        if abs(ed) < self.distance_deadband:
            ed = 0.0
        twist.linear.x = clamp(self.linear_gain * ed, self.max_linear_speed)

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
