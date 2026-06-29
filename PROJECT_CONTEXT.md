# Ventuno Object Tracking — Project Context

## Goal

Build a simple object tracking demo running on a TurtleBot 4 robot, replacing the stock Raspberry Pi with an Arduino Ventuno Q board.

## Hardware

### Arduino Ventuno Q
- Hybrid Linux SoC + microcontroller board
- Running Ubuntu 24.04
- Replaces the stock Raspberry Pi on the TurtleBot 4

### Clearpath TurtleBot 4
- Mobile robot platform by Clearpath Robotics
- Stock setup: Raspberry Pi running Ubuntu 22.04
  - Raspberry Pi communicates with robot drive hardware over USB
  - Clearpath provides drivers that interface with robot control over USB
- The Ventuno Q will replace the Raspberry Pi, taking over that USB-based driver role

### OAK-D Lite
- DepthAI stereo camera by Luxonis
- Already mounted on the robot
- Provides RGB + depth output
- Will be the primary sensor for object detection and tracking

## Software Stack (planned)

- Ubuntu 24.04 on the Ventuno Q
- ROS 2 (likely Jazzy, which targets Ubuntu 24.04)
- DepthAI / depthai-python SDK for OAK-D Lite
- Clearpath TurtleBot 4 drivers ported/adapted from the 22.04 Raspberry Pi setup

## Project Phases

1. **Context & planning** ← current phase
   - Document hardware setup, constraints, and goals
   - Identify compatibility gaps (Ubuntu 24.04 vs 22.04, Ventuno vs RPi)

2. **Bring up the Ventuno Q**
   - Confirm Ubuntu 24.04 boots and USB devices enumerate correctly
   - Install ROS 2 Jazzy
   - Validate Clearpath TurtleBot 4 USB drivers work on the Ventuno

3. **Camera integration**
   - Install DepthAI SDK on Ubuntu 24.04 / Ventuno
   - Confirm OAK-D Lite streams RGB and depth data

4. **Object detection**
   - Run a lightweight detection model (e.g., MobileNet-SSD or YOLOv8n) on the OAK-D Lite's onboard MyriadX VPU
   - Validate detection outputs

5. **Object tracking**
   - Integrate a tracker (e.g., OAK-D built-in object tracker or a CPU-side tracker)
   - Output tracked bounding boxes + IDs per frame

6. **Robot control loop**
   - Publish detection/tracking results to ROS 2 topics
   - Implement a simple controller that steers the robot to keep the tracked object centered
   - Test closed-loop behavior on the TurtleBot 4

## Key Constraints & Open Questions

- **Ubuntu 24.04 compatibility**: Clearpath's TurtleBot 4 drivers officially target 22.04 — need to verify or patch for 24.04.
- **Ventuno Q specifics**: Architecture (ARM? x86?), GPIO/USB capabilities, and any board-specific quirks need to be confirmed.
- **ROS 2 distro**: Jazzy is the 24.04-native distro; check that all required Clearpath and DepthAI ROS 2 packages are available for Jazzy.
- **Compute budget**: Object tracking should be offloaded to the OAK-D Lite's VPU where possible to keep the main CPU free for control.
