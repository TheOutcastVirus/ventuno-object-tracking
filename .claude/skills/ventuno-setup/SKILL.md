---
name: ventuno-setup
description: Set up, debug, and verify the Ventuno Q board for this repo (ROS 2 Jazzy, ExecuTorch/QNN NPU inference, OAK-D camera, Create 3 base). Use whenever the user is bringing up a new/wiped Ventuno board, hitting a build/runtime failure on the board, asking why NPU inference or the Create 3 link isn't working, or wants to replicate this setup on another board.
---

# Ventuno Q setup and debugging

This board runs `yolox_detector` (YOLOX-Tiny via ExecuTorch, on-device NPU inference),
`oak_camera`, and `object_tracker` on a Ventuno Q replacing a TurtleBot 4's Raspberry Pi.
`scripts/install_ventuno_deps.sh` automates the whole bring-up; this skill is for when
that script hasn't been run yet, partially failed, or the board is already set up but
something is misbehaving.

## Orientation

- **Board**: Arduino Ventuno Q, reports as `QCS8275` / soc_id `675` (`cat /sys/devices/soc0/machine`,
  `cat /sys/devices/soc0/soc_id`). ExecuTorch's Qualcomm backend has no native entry for this chipset —
  the project patches in a `QCS8300` compatibility target mapped to `HtpArch.V75`. Don't assume a newer
  HTP version (e.g. V81); this board validates as **V75**.
- **Persistent env**: `~/.ventuno_object_tracking_env`, sourced from `~/.bashrc` (must be sourced before
  the non-interactive-shell early-return line, since most commands run over `ssh ubuntu@ventuno 'bash -lc ...'`).
  Sets `VENTUNO_OBJECT_TRACKING_ROOT`, `EXECUTORCH_ROOT`, `QNN_SDK_ROOT`, `QAIRT_LIB`, `LD_LIBRARY_PATH`,
  `PYTHONPATH`, and the semicolon-separated `ADSP_LIBRARY_PATH` FastRPC needs. Also see `ROS_DOMAIN_ID`,
  `RMW_IMPLEMENTATION`, `FASTRTPS_DEFAULT_PROFILES_FILE` for the Create 3 DDS link.
- **ExecuTorch** lives at `/opt/executorch`, built natively on-device into `build-x86` (name is inherited
  from the Qualcomm build scripts; on the Ventuno it holds native ARM artifacts, not x86 — don't let that
  name mislead you).
- **QNN/QAIRT SDK** at `/opt/qcom/aistack/qairt/<version>` (currently `2.48.0.260626`).

## First step for any setup/debug request

Run (or re-run) the installer — it's idempotent and safe to re-run after a partial failure:

```bash
bash scripts/install_ventuno_deps.sh
```

Useful flags when isolating a failure: `--skip-workspace-build`, `--skip-rosdep`,
`--skip-create3-service`, `--skip-sample-images`, `--qnn-sdk-zip PATH` (if the Qualcomm
SDK URL needs auth and manual download), `--allow-unsupported-os`. Run `--help` for the full list.

If the installer fails, read the stage it died in (`log()` lines show progress) and jump to the
matching reference below rather than re-running the whole thing blind.

## Troubleshooting index

| Symptom | Reference |
|---|---|
| ExecuTorch build/export fails, QNN backend errors, empty/garbage detections, FastRPC/DSP errors, `find_package(executorch)` CMake errors | [references/executorch-qnn.md](references/executorch-qnn.md) |
| Create 3 doesn't respond to `/cmd_vel`, `ros2 node list` doesn't show the base, robot won't drive, link works then dies after a reboot | [references/create3-connection.md](references/create3-connection.md) |
| A raw ROS image topic (e.g. `/oak/rgb/image_raw`) publishes far below its configured rate / is jittery, DDS discovery seems to miss a peer on a specific NIC | [references/ros-networking.md](references/ros-networking.md) |

## General debugging approach

1. Identify which stage is broken: package install, QNN SDK, ExecuTorch build, ROS workspace build,
   model export, or runtime (launch/inference/networking).
2. Check the persistent env is actually loaded in the shell you're debugging in — a lot of "it worked
   over SSH but not interactively" (or vice versa) bugs are `~/.ventuno_object_tracking_env` not being
   sourced. `echo $QNN_SDK_ROOT` is a quick sanity check.
3. For anything QNN/HTP/FastRPC related, confirm the board identity first (`cat /sys/devices/soc0/machine`)
   — don't assume it matches the `QCS8275` board this was developed against if setting up a new unit.
4. Prefer reproducing the exact verified commands in the reference docs over improvising new ones; the
   references capture what specifically was tried and failed, not just the final answer.

## Verifying a working setup end-to-end

```bash
source /opt/ros/jazzy/setup.bash
source install/setup.bash
ros2 launch yolox_detector dataset_detector.launch.py backend:=npu
```

In another shell: `ros2 topic hz /detections` should show ~5 Hz with non-empty
`vision_msgs/msg/Detection2DArray` messages. `ros2 node list` should show
`/dataset_image_publisher` and `/yolox_detector`.
