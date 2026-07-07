#!/usr/bin/env python3
"""Minimal open-loop drive test for the TurtleBot 4 base.

Publishes a short, safe, fixed sequence of geometry_msgs/Twist commands to /cmd_vel
so you can confirm the whole motion path works —
    this node -> /cmd_vel -> create3_republisher -> Create 3 base
— completely independent of the camera, detector, or tracker.

Sequence (all at low speed): forward, stop, back, stop, rotate left, stop,
rotate right, stop. A zero Twist is sent at the end and on Ctrl-C.

Safety:
* Put the robot on an open floor with clearance, or up on blocks so the wheels
  spin free.
* Speeds and durations are small and configurable; Ctrl-C stops immediately.
* The Create 3 stops on its own if commands stop arriving (cmd_vel_timeout ~0.5 s),
  so this node republishes the current command at `rate` Hz to keep it moving.

Run:
    ros2 run object_tracker movement_test
    ros2 run object_tracker movement_test --ros-args -p linear_speed:=0.1 -p repeat:=true
Or, to also bring up the Create 3 bridge:
    ros2 launch object_tracker movement_test.launch.py
"""

import rclpy
from geometry_msgs.msg import Twist
from rclpy.duration import Duration
from rclpy.node import Node


class MovementTest(Node):
    def __init__(self):
        super().__init__('movement_test')

        self.declare_parameter('cmd_vel_topic', '/cmd_vel')
        self.declare_parameter('linear_speed', 0.15)    # m/s
        self.declare_parameter('angular_speed', 0.6)    # rad/s
        self.declare_parameter('segment_duration', 1.5)  # s per motion phase
        self.declare_parameter('stop_duration', 0.7)    # s of pause between phases
        self.declare_parameter('rate', 20.0)            # Hz command republish
        self.declare_parameter('repeat', False)         # loop forever vs run once

        cmd_vel_topic = self.get_parameter('cmd_vel_topic').value
        lin = self.get_parameter('linear_speed').value
        ang = self.get_parameter('angular_speed').value
        move_t = self.get_parameter('segment_duration').value
        stop_t = self.get_parameter('stop_duration').value
        rate = self.get_parameter('rate').value
        self.repeat = self.get_parameter('repeat').value

        # Each phase: (label, linear.x, angular.z, duration_seconds)
        self.phases = [
            ('forward',      lin,  0.0, move_t),
            ('stop',         0.0,  0.0, stop_t),
            ('backward',    -lin,  0.0, move_t),
            ('stop',         0.0,  0.0, stop_t),
            ('rotate left',  0.0,  ang, move_t),
            ('stop',         0.0,  0.0, stop_t),
            ('rotate right', 0.0, -ang, move_t),
            ('stop',         0.0,  0.0, stop_t),
        ]

        self.cmd_pub = self.create_publisher(Twist, cmd_vel_topic, 10)
        self.phase_index = -1
        self.phase_end = self.get_clock().now()
        self.finished = False
        self.timer = self.create_timer(1.0 / rate, self.tick)

        self.get_logger().info(
            f"movement_test: publishing to '{cmd_vel_topic}' "
            f"lin={lin} ang={ang} seg={move_t}s repeat={self.repeat}. "
            f"Ensure the robot has clearance!")
        self._advance_phase()

    def _advance_phase(self):
        self.phase_index += 1
        if self.phase_index >= len(self.phases):
            if self.repeat:
                self.phase_index = 0
            else:
                self.finished = True
                return
        label, lx, az, dur = self.phases[self.phase_index]
        self.current = Twist()
        self.current.linear.x = float(lx)
        self.current.angular.z = float(az)
        self.phase_end = self.get_clock().now() + Duration(seconds=dur)
        self.get_logger().info(
            f"[{self.phase_index + 1}/{len(self.phases)}] {label} "
            f"(lin={lx:+.2f}, ang={az:+.2f})")

    def tick(self):
        if self.finished:
            self.cmd_pub.publish(Twist())  # hold stop
            self.get_logger().info('movement_test complete — robot stopped.')
            self.timer.cancel()
            rclpy.shutdown()
            return
        if self.get_clock().now() >= self.phase_end:
            self._advance_phase()
            if self.finished:
                return
        self.cmd_pub.publish(self.current)

    def stop(self):
        """Best-effort halt on shutdown."""
        try:
            self.cmd_pub.publish(Twist())
        except Exception:
            pass


def main(args=None):
    rclpy.init(args=args)
    node = MovementTest()
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
