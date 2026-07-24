# A TurtleBot 4 That Follows You, Powered by an Arduino Ventuno Q

> **Subheading:** We pulled the Raspberry Pi out of a TurtleBot 4 and replaced it with an
> Arduino Ventuno Q. YOLOX-Tiny now runs on the board's Hexagon NPU, and the robot follows
> you around the room, with perception, tracking, and motor control all on-device: no cloud,
> no network, no laptop.

![The robot locking onto a person and following them at a fixed distance](images/tracking_demo.gif)

---

## Metadata for the Project Hub form

| Field | Value |
|---|---|
| Type | Showcase |
| License | MIT |
| Categories | Robotics · Machine Learning / AI · Embedded · Computer Vision |
| Difficulty | Intermediate–Advanced |

### Components and supplies

| Qty | Component |
|---|---|
| 1 | Arduino Ventuno Q |
| 1 | Clearpath TurtleBot 4 Lite, iRobot Create 3 base |

### Apps and platforms

Ubuntu 24.04 (Qualcomm image) · ROS 2 Jazzy · PyTorch ExecuTorch · Qualcomm QNN / QAIRT
SDK 2.48 · DepthAI · OpenCV · C++ / Python 3

---

## Intro

A TurtleBot 4 ships with a Raspberry Pi as its brain. It's a fine little computer, but it
means anything resembling real computer vision either crawls, or gets shipped off to a
laptop over Wi-Fi, or goes to the cloud. All three options are a compromise, and the third
one stops working the moment the network does.

So we took the Pi out and put an **Arduino Ventuno Q** in its place.

The Ventuno Q is a heterogeneous board: it has ARM cores for general compute, plus a Qualcomm
**Hexagon NPU** for neural network inference. That combination is exactly what a small
robot wants: the NPU runs the object detector, the CPU cores run ROS 2 and the control
loop, and the two talk to each other over shared memory instead of a network link.

The result is a robot that sees a person, locks onto them, and follows them at a fixed
distance. Everything runs on the robot. You can unplug the router and it keeps
working.

![YOLOX-Tiny detections running on the Ventuno Q's Hexagon NPU](images/detections_demo.gif)

## What it does

- **Sees in 3D.** An OAK-D Lite streams RGB plus stereo depth into ROS 2, with the depth
  map aligned to the RGB frame so detection boxes can be sampled directly for range.
- **Detects on the NPU.** YOLOX-Tiny runs through ExecuTorch on the Hexagon NPU via
  Qualcomm's QNN HTP backend, detecting all 80 COCO classes.
- **Locks onto one target.** A lightweight tracker picks a single object of the configured
  class and holds the lock across frames, surviving brief occlusion and missed detections.
- **Follows at a set distance.** A proportional controller drives the Create 3 base to keep
  the target centered in frame and 1.2 m away, backing up if you walk toward it.
- **Recovers when it loses you.** Lose the target and it stops, waits, then rotates in
  place toward wherever you were last seen to re-acquire.

## Why the Ventuno Q

Three reasons this board in particular:

**The NPU is the point.** YOLOX-Tiny on a CPU is the kind of workload that eats a small
ARM chip alive and leaves nothing for anything else. Offloading it to the Hexagon NPU frees
the CPU cores entirely for ROS 2, the DDS middleware, the camera driver, and the control
loop, all of which have their own real-time-ish demands.

**It drops into the Pi's slot.** The TurtleBot 4 has a compute bay sized and powered for a
single-board computer. The Ventuno Q takes that role directly, so this is a swap rather
than a rebuild.

## How it works

```
OAK-D Lite ──RGB + aligned depth──▶ oak_camera
                                        │
                              /oak/rgb/image_raw
                                        ▼
                                 yolox_detector  ──▶ /detections (all objects)
                                  (Hexagon NPU)  ──▶ /tracked_object (the one target)
                                        │
                                        ▼
                                  object_tracker ◀── /oak/depth/image_raw
                                        │
                                    /cmd_vel
                                        ▼
                             create3_republisher ──▶ Create 3 base
```

<!-- IMAGE: replace the ASCII diagram above with a proper block diagram before publishing -->

Four ROS 2 nodes, launched together by `launch/object_tracking.launch.py`.

### `oak_camera`: eyes

A DepthAI driver publishing RGB and RGB-aligned stereo depth as 16-bit millimetres.

The interesting decision here is the resolution: **640×360**, not 1080p. The OAK-D Lite on
this setup is on a USB2 link, and 1080p NV12 needs roughly 93 MB/s against the ~35–40 MB/s
the link can actually deliver. That bandwidth ceiling (not the camera, not the compute)
was the frame rate bottleneck. Dropping to 640×360 needs about 10 MB/s, keeps the native
16:9 aspect ratio so nothing gets cropped out of the field of view, and costs *nothing* in
detection quality because the detector resizes everything to 416×416 before inference
anyway.

### `yolox_detector`: brain

A C++ node wrapping the ExecuTorch runtime, with two interchangeable backends selected by a
`backend` parameter: `npu` (QNN HTP) or `cpu` (XNNPACK). The NPU path has one wrinkle worth
knowing about: the QNN shared libraries have to be pre-loaded by hand before ExecuTorch
can find them:

```cpp
// qnn_backend.cpp: RTLD_GLOBAL so symbols are visible to libs loaded after these
const char * libs[] = {"libQnnSystem.so", "libQnnHtp.so", "libQnnHtpPrepare.so", nullptr};
for (int i = 0; libs[i]; ++i) {
  std::string path = lib_dir.empty() ? libs[i] : (lib_dir + "/" + libs[i]);
  if (!dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL)) {
    throw std::runtime_error(std::string("Failed to load QNN lib: ") + dlerror());
  }
}
module_ = std::make_unique<Module>(model_path, Module::LoadMode::MmapUseMlock);
```

The same node also does the single-target tracking. Raw per-frame detections are too jumpy
to drive a robot with, so it maintains a lock: each frame it associates the nearest
same-class detection to the current lock, rejects any association that jumps more than 15%
of the image width (that's the `track_gate`), and smooths the surviving box with an
exponential moving average. If the target goes unseen for 10 consecutive frames the lock
drops and it re-acquires from scratch. Only that one smoothed box gets published to
`/tracked_object`.

### `object_tracker`: reflexes

A Python node that turns a bounding box into wheel velocities.

Getting range out of the depth map is the fiddly part. You cannot just take the depth at
the centre of the bounding box, and you cannot average the whole box either, because a YOLOX
box around a person contains a lot of background between the arms and legs and around the
shoulders, and that background is metres further away. Averaging it pulls the estimate
badly wrong. So the node contracts the box before sampling (20% off each side, 15% off the
top, 20% off the bottom) and takes the **median** of the valid pixels inside that inner
region, which throws out whatever stereo outliers remain:

```python
x1 = max(0, math.floor(cx - box_w * (0.5 - self.depth_roi_side_trim)))
x2 = min(image_w, math.ceil(cx + box_w * (0.5 - self.depth_roi_side_trim)))
y1 = max(0, math.floor(cy - box_h * (0.5 - self.depth_roi_top_trim)))
y2 = min(image_h, math.ceil(cy + box_h * (0.5 - self.depth_roi_bottom_trim)))

roi_mm = self.last_depth[y1:y2, x1:x2]
values_m = roi_mm.astype(np.float32).ravel() * 0.001
valid = values_m[(values_m >= self.depth_min_m) & (values_m <= self.depth_max_m)]
if valid.size < roi_mm.size * self.depth_min_valid_fraction:
    return None          # not enough real depth, so don't trust it
measured_m = float(np.median(valid))
```

That measurement is smoothed again with an EMA, and rejected outright if it's stale by more
than 200 ms relative to the detection it belongs to.

Control itself is deliberately simple: proportional on two axes, running at 20 Hz.
Horizontal error drives rotation; the difference between measured range and the 1.2 m
target drives forward speed. Both have deadbands so the robot isn't constantly twitching,
and both are clamped (0.25 m/s, 1.5 rad/s) to speeds that are sane indoors.

There's one safety decision in here worth calling out. When depth is unavailable or
untrustworthy, the node keeps *centering* the target but sets forward velocity to zero. The
tempting fallback, estimating range from how big the bounding box is, was deliberately not
implemented, because a bad box-size estimate can command the robot to drive forward into
something. Refusing to move is the better failure mode.

## Getting the model onto the NPU

The board reports as SoC `QCS8275` (soc_id `675`), with HTP architecture **V75** and
`hexagon-v75/unsigned` skel libraries. One detail that will cost you an afternoon if you
don't know it: ExecuTorch's Qualcomm backend has no native entry for this chipset. The
project patches in a `QCS8300` compatibility target mapped to `HtpArch.V75`, and models get
exported with `--soc-model QCS8300`. Don't assume a newer HTP version; this board
validates as V75, and a mismatch shows up as garbage detections rather than a clean error.

The pipeline is PyTorch YOLOX-Tiny → ExecuTorch `.pte`, once per backend, and the two
artifacts are very different objects:

| Model | Backend | Size |
|---|---|---|
| `yolox_tiny_qnn.pte` | QNN HTP (NPU), quantized | 5.3 MB |
| `yolox_tiny_xnnpack.pte` | XNNPACK (CPU), float | 20 MB |
| `yolox_tiny.onnx` | intermediate | 20 MB |

**Both exported models are committed to the repo**, so you don't need to run the export
yourself to reproduce this: clone, build, launch.

## Build it yourself

### 1. Hardware

Remove the Raspberry Pi from the TurtleBot 4's compute bay and mount the Ventuno Q in its
place. Connect the OAK-D Lite over USB, and wire the Create 3 USB-ethernet link.

### 2. Software

One script does the whole bring-up: ROS and system dependencies, the QAIRT/QNN SDK,
building ExecuTorch with the Qualcomm backend, the Create 3 USB-ethernet service, and the
ROS workspace build. It's idempotent, so it's safe to re-run after a partial failure:

```bash
git clone https://github.com/TheOutcastVirus/ventuno-object-tracking.git
cd ventuno-object-tracking
bash scripts/install_ventuno_deps.sh
```

### 3. Test the detector with no robot at all

This runs the full NPU inference path against bundled sample COCO images. No camera, no
base, and a good way to confirm the hard part works before you involve anything that moves:

```bash
source /opt/ros/jazzy/setup.bash
source install/setup.bash
ros2 launch yolox_detector dataset_detector.launch.py backend:=npu
```

In another shell, `ros2 topic hz /detections` should show ~5 Hz of non-empty
`vision_msgs/Detection2DArray`, and `/detections/image` carries annotated frames you can
view in `rqt_image_view`.

### 4. Run the robot

Do the dry run first:

```bash
ros2 launch launch/object_tracking.launch.py publish_cmd_vel:=false
```

Watch the logged velocity commands, walk around in front of the camera, confirm the signs
and magnitudes look sane. Then let it move:

```bash
ros2 launch launch/object_tracking.launch.py
ros2 launch launch/object_tracking.launch.py target_class:=bottle
ros2 launch launch/object_tracking.launch.py log_target_distance:=true
```

## Tuning

Everything lives in `src/object_tracker/config/`. The two that matter:

| Parameter | Default | What it does |
|---|---|---|
| `target_distance_m` | 1.2 | Following distance, metres |
| `angular_gain` | 2.0 | Turn effort per unit horizontal error |
| `linear_gain` | 0.7 | Drive effort per metre of range error |
| `max_linear_speed` | 0.25 | m/s cap, forward and reverse |
| `max_angular_speed` | 1.5 | rad/s cap |
| `lost_timeout` | 0.6 | Seconds without a target before following stops |
| `search_angular_speed` | 0.4 | rad/s in-place rotation to re-acquire |

Raise `angular_gain` and it snaps onto a moving target faster at the cost of some jitter
when standing still. Raise `linear_gain` and it closes distance more aggressively; this is
the one to be careful with, since it's the gain attached to forward motion.

## What was hard

**Standing up ExecuTorch + QNN on the board.** This was most of the project. Unrecognised
chipset, HTP version mismatches, FastRPC and DSP library paths, ExecuTorch needing to be
built natively on-device, and Python export failing on missing schema resources. All of it
is written down in `.claude/skills/ventuno-setup/references/executorch-qnn.md`, including
the things that *didn't* work, which is usually the more useful half.

**The Create 3 link.** DDS discovery, the `_do_not_use` namespace, USB-ethernet gadget
config that has to survive a reboot, and a robot that responds to nothing while every node
looks perfectly healthy.

**USB2 bandwidth on the camera.** Diagnosed as a frame rate problem, actually a link
saturation problem. Fixed by realising the detector downsamples to 416×416 regardless, so
the high-resolution stream was pure waste.

**Depth on box edges.** The first version averaged the whole bounding box and produced
range estimates that were confidently, uselessly wrong. ROI contraction plus a median fixed it.

## What's next

- Multi-target tracking, so it can choose who to follow rather than taking the nearest match
- Obstacle avoidance while following, since right now it will happily drive at a chair
- A larger detection model, since there should be NPU headroom
- Gesture or voice commands to start and stop following

## Links

- **Repo:** https://github.com/TheOutcastVirus/ventuno-object-tracking
- **Board setup and debugging notes:** `.claude/skills/ventuno-setup/`, covering full ExecuTorch/QNN
  bring-up, the Create 3 connection, and DDS tuning. These are written as an *agent skill*:
  plain markdown structured so a coding agent can read them and debug the board for you, but
  perfectly readable as normal documentation too. If you're bringing up a Qualcomm board with
  ExecuTorch, the QNN reference is probably the most useful file in the repo.
