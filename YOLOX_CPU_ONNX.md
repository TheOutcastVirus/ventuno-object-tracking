# CPU-Only YOLOX Setup (ONNX Runtime — No ExecuTorch)

This guide lets you run YOLOX-Tiny object detection on CPU without building or installing ExecuTorch.
It replaces the C++ `yolox_detector` node with a Python ROS 2 node backed by ONNX Runtime.

---

## How it differs from the default setup

| | Default (C++ + ExecuTorch) | This guide (Python + ONNX Runtime) |
|---|---|---|
| Inference library | ExecuTorch (must build from source) | `onnxruntime` (`pip install`) |
| Model format | `.pte` (ExecuTorch flatbuffer) | `.onnx` (standard) |
| ROS 2 node | C++ (`yolox_detector_node`) | Python script |
| Build required | `colcon build` with ExecuTorch CMake | None |
| Platform | ARM64 / x86_64 | Any Python platform |

---

## Prerequisites

### System packages

```bash
sudo apt install python3-pip python3-cv-bridge ros-jazzy-vision-msgs ros-jazzy-image-transport
```

### Python packages

**Runtime only** (inference — no model export):

```bash
pip install -r requirements.txt
```

**Full setup** (runtime + one-time model export):

```bash
pip install -r requirements-export.txt
```

> `onnxruntime` pulls in its own CPU-optimized kernels — no CUDA or special hardware needed.

---

## Step 1: Download and export the model

Run the download script. It fetches the official YOLOX-Tiny weights (~19 MB) from the
[YOLOX GitHub releases](https://github.com/Megvii-BaseDetection/YOLOX/releases/tag/0.1.1rc0)
and exports them to ONNX format automatically:

```bash
cd /path/to/ventuno-object-tracking
pip install -r requirements-export.txt
python3 scripts/download_models.py
```

This creates:
- `models/yolox_tiny.pth` — original PyTorch weights (checksum-verified, ~19 MB)
- `models/yolox_tiny.onnx` — ONNX model ready for inference (~25 MB)

The `models/` directory is git-ignored; these files live on the machine only.

> **Auto-download on first run:** If you skip this step, the detector node will run
> `scripts/download_models.py` automatically when it starts and the model file is missing.
> This requires the export dependencies (`requirements-export.txt`) to be installed.

### Options

```bash
# Download .pth only (skip ONNX export — e.g. you'll export manually later)
python3 scripts/download_models.py --skip-export

# Use a different input resolution (must match node parameters)
python3 scripts/download_models.py --input-size 640
```

---

## Step 2: (Optional) Manual ONNX export

If you have a custom `.pth` checkpoint or need to re-export at a different resolution,
use `tools/export_yolox_onnx.py` directly:

```bash
python3 tools/export_yolox_onnx.py \
    --weights yolox_tiny.pth \
    --output models/yolox_tiny.onnx \
    --input-size 416
```

---

## Step 3: Run the detector

Make the node executable (once):

```bash
chmod +x scripts/yolox_detector_cpu.py
```

### Source ROS 2

```bash
source /opt/ros/jazzy/setup.bash
```

### Run the detector node alone

```bash
python3 scripts/yolox_detector_cpu.py --ros-args \
    -p model_path:=$(pwd)/models/yolox_tiny.onnx \
    -p image_topic:=/camera/rgb/image_raw \
    -p score_threshold:=0.45 \
    -p nms_threshold:=0.45
```

### Run with the OAK-D camera (two terminals)

Terminal 1 — camera node (unchanged from the main setup):

```bash
source /opt/ros/jazzy/setup.bash && source install/setup.bash
ros2 run oak_camera oak_camera_node
```

Terminal 2 — detector:

```bash
source /opt/ros/jazzy/setup.bash
python3 scripts/yolox_detector_cpu.py --ros-args \
    -p model_path:=$(pwd)/models/yolox_tiny.onnx
```

### Optional: launch file

```bash
ros2 launch launch/detector_cpu.launch.py
```

---

## Parameters

| Parameter | Default | Description |
|---|---|---|
| `model_path` | `<repo>/models/yolox_tiny.onnx` | Path to exported ONNX model |
| `input_width` | `416` | Model input width (must match export) |
| `input_height` | `416` | Model input height (must match export) |
| `score_threshold` | `0.45` | Minimum objectness × class score |
| `nms_threshold` | `0.45` | IoU threshold for NMS suppression |
| `num_classes` | `80` | Number of COCO classes |
| `image_topic` | `/camera/rgb/image_raw` | Input image topic |
| `detections_topic` | `/detections` | Output `Detection2DArray` topic |
| `debug_image_topic` | `/detections/image` | Annotated debug image topic |

---

## Published topics

| Topic | Type | Description |
|---|---|---|
| `/detections` | `vision_msgs/Detection2DArray` | Bounding boxes, class IDs, scores |
| `/detections/image` | `sensor_msgs/Image` | Debug frame with drawn boxes (lazy — only rendered when subscribed) |

---

## Performance notes

- Expected latency: **60–120 ms/frame** on a modern x86 CPU (YOLOX-Tiny, 416×416)
- ONNX Runtime's CPU provider uses multi-threaded execution automatically
- To limit threads (e.g., on embedded platforms): add `sess_options.intra_op_num_threads = 2` before `InferenceSession()`
- For the Ventuno Q (ARM64 Cortex-A), expect ~150–250 ms/frame — similar to ExecuTorch XNNPACK; use the NPU path for real-time performance

---

## Troubleshooting

**`ModuleNotFoundError: No module named 'onnxruntime'`**
```bash
pip install -r requirements.txt
```

**`cv_bridge` import error**
```bash
sudo apt install ros-jazzy-cv-bridge
```

**Auto-download fails / `No module named 'torch'`**
```bash
pip install -r requirements-export.txt
python3 scripts/download_models.py
```
The export dependencies are only needed once for the `.pth` → `.onnx` step; not at inference time.
The YOLOX architecture is inlined in `tools/export_yolox_onnx.py` — no `yolox` package or git installs required.

**Model loads but no detections**
- Verify the ONNX model was exported with `decode_in_inference=False` (the default in this guide)
- Lower `score_threshold` temporarily to `0.1` to verify the pipeline produces any output
- Check image topic: `ros2 topic echo /camera/rgb/image_raw --once`

**`InvalidGraph` or shape mismatch during ONNX export**
- Use opset 11 (`--opset 11`) — some YOLOX ops are not supported in older opsets
- Ensure PyTorch ≥ 1.12

**Wrong class IDs**
- Class IDs are COCO indices (0–79). Map them with the standard COCO label list for human-readable names.
