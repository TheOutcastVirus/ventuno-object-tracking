# Ventuno Object Tracking — Project Context

## Goal

Build a simple object tracking demo running on a TurtleBot 4 robot, replacing the stock Raspberry Pi with an Arduino Ventuno Q board. The main goal of this project is to get AI models running on the edge on the Ventuno Q board, showing developers the possibilities of running AI on Qualcomm hardware. Specifically, we want to run the models on the Ventuno Q's NPU, through the Executorch library using Qualcomm's runtime. 

## Hardware

### Arduino Ventuno Q
- Hybrid Linux SoC + microcontroller board 
- ARM-based architecture
- IQ-8275 Chipset, with a Hexagon NPU and an Adreno GPU
- 16 GB RAM, 64 GB eMMC storage
- Running a version of Ubuntu 24.04 by Qualcomm
- Will replace the stock Raspberry Pi 4 on the TurtleBot 4

### Clearpath TurtleBot 4 Lite
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
- ROS 2 Jazzy
- DepthAI / depthai-python SDK for OAK-D Lite
- Clearpath TurtleBot 4 drivers ported/adapted from the 22.04 Raspberry Pi setup
- Pytorch's Executorch (https://github.com/pytorch/executorch) using Qualcomm's runtime

## Key Constraints & Open Questions

- **Ventuno Q support for Executorch**: the Ventuno Q is an unreleased board. Use the IQ8 chipset whose NPU, and the NPU is the same as other boards that Qualcomm has support for with the Executorch. But there isn't specifically support for the Ventuno Q in Executorch's chipset enumeration. So we will have to hack together our own compatibility for the chipset.
  - Even though the chipset is similar to the ones that are supported, there might be some differences, such as memory layout, that might cause issues for us.
- **Cross Compilation**: Although the official documentation for Executorch and the Qualcomm backend tells us to cross compile, we should be able to do everything natively on the Ventuno Q.
- **Missing Python packages**: Some Python packages might not be supported on the certain architecture, and we will need to find alternatives or workarounds.