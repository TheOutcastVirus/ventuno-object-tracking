# Ventuno Q YOLOX ExecuTorch QNN Setup Runbook

This document records the setup used to run YOLOX Tiny through ExecuTorch with the Qualcomm QNN HTP backend on an Arduino Ventuno Q, then expose the result through ROS 2 Jazzy topics.

The current working target is:

- Board: Arduino Ventuno Q
- OS: Ubuntu 24.04 Qualcomm image
- SoC as reported by Linux: `QCS8275`
- SoC id as reported by Linux: `675`
- QNN/QAIRT install used: `/opt/qcom/aistack/qairt/2.47.0.260601`
- HTP skel libraries used: `hexagon-v75/unsigned`
- ROS: Jazzy
- Model: YOLOX Tiny, exported as an ExecuTorch `.pte` with QNN HTP delegation
- Runtime input for this phase: sample COCO images published as a ROS image topic

The final verified launch published non-empty detections at about 5 Hz on the sample image dataset, with `/detections/image` available for annotated images.

## High Level Result

The working path is:

1. Export YOLOX Tiny to an ExecuTorch `.pte` using the Qualcomm backend with `backend="htp"`.
2. Build the ROS package with `-DBUILD_QNN_BACKEND=ON` so `QnnBackend` is compiled into the detector node.
3. Launch the detector with `backend:=npu` and the QNN-exported model.
4. The ROS node selects `QnnBackend`, preloads QNN HTP libraries, loads the `.pte`, and runs `module_->execute("forward", ...)` through ExecuTorch.

The important code locations are:

- `src/yolox_detector/launch/dataset_detector.launch.py`: declares `backend`, defaulting to `npu`, and passes it to the node.
- `src/yolox_detector/src/yolox_detector_node.cpp`: maps `backend == "npu"` to `QnnBackend`.
- `src/yolox_detector/src/qnn_backend.cpp`: preloads `libQnnSystem.so`, `libQnnHtp.so`, and `libQnnHtpPrepare.so`, then loads and executes the ExecuTorch model.
- `tools/export_yolox_qnn.py`: exports the model using QNN HTP (`backend="htp"`, `QnnExecuTorchBackendType.kHtpBackend`).

## Persistent Board Environment

Repeated runtime paths were moved into `~/.ventuno_object_tracking_env` and sourced near the top of `~/.bashrc`, before the normal non-interactive shell return. This matters because most commands are run through `ssh ubuntu@ventuno 'bash -lc ...'`.

The environment file used on the working board is:

```bash
# Ventuno object tracking / Qualcomm AI runtime environment.

_vot_prepend_path() {
  var_name="$1"
  entry="$2"
  eval current="\${$var_name:-}"
  case ":$current:" in
    *":$entry:"*) ;;
    *)
      if [ -n "$current" ]; then
        export "$var_name=$entry:$current"
      else
        export "$var_name=$entry"
      fi
      ;;
  esac
}

export VENTUNO_OBJECT_TRACKING_ROOT="$HOME/Documents/ventuno-object-tracking"
export EXECUTORCH_ROOT="$HOME/Documents/executorch"
export QNN_SDK_ROOT=/opt/qcom/aistack/qairt/2.47.0.260601
export QAIRT_LIB="$QNN_SDK_ROOT/lib"

_vot_prepend_path PATH "$HOME/.local/bin"
_vot_prepend_path LD_LIBRARY_PATH "$EXECUTORCH_ROOT/build-x86/backends/qualcomm"
_vot_prepend_path LD_LIBRARY_PATH "$EXECUTORCH_ROOT/build-x86/lib/executorch/backends/qualcomm"
_vot_prepend_path LD_LIBRARY_PATH "$QAIRT_LIB/aarch64-oe-linux-gcc11.2"
_vot_prepend_path PYTHONPATH "$QAIRT_LIB/python"
_vot_prepend_path PYTHONPATH "$HOME/Documents"

# FastRPC expects semicolon-separated DSP search paths.
export ADSP_LIBRARY_PATH="$QAIRT_LIB/hexagon-v75/unsigned;/usr/lib/dsp/cdsp;/usr/lib/rfsa/adsp;/dsp/cdsp;/dsp"

unset -f _vot_prepend_path
```

The `.bashrc` line is:

```bash
[ -f "$HOME/.ventuno_object_tracking_env" ] && . "$HOME/.ventuno_object_tracking_env"
```

Put this before any line like `case $- in *i*) ;; *) return;; esac`, otherwise non-interactive SSH commands will not see the environment.

## Board Identification and QNN Target Choice

On the working board:

```bash
cat /sys/devices/soc0/machine
# QCS8275

cat /sys/devices/soc0/soc_id
# 675
```

The Qualcomm stack did not work by treating this as an HTP V81 target. Runtime validation showed the board should use HTP V75 libraries:

```bash
$QNN_SDK_ROOT/bin/aarch64-oe-linux-gcc11.2/qnn-platform-validator \
  --backend dsp \
  --coreVersion \
  --testBackend \
  --debug \
  --targetPath /tmp/qnn_validator
```

The working DSP library path uses:

```bash
/opt/qcom/aistack/qairt/2.47.0.260601/lib/hexagon-v75/unsigned
```

The working ARM QNN libraries use:

```bash
/opt/qcom/aistack/qairt/2.47.0.260601/lib/aarch64-oe-linux-gcc11.2
```

## System Packages

Install the FastRPC packages. Without these, QNN failed at runtime because DSP/FastRPC libraries or device permissions were missing.

```bash
sudo apt-get update
sudo apt-get install -y qcom-fastrpc1 qcom-fastrpc-dev
```

The concrete failure fixed by this was missing FastRPC support such as `libcdsprpc.so` and `/dev/fastrpc-*` access.

## ExecuTorch Checkout and Qualcomm Backend Patches

ExecuTorch did not know this Ventuno target out of the box. The workaround was to add a `QCS8300` entry and map it to HTP V75 with 8 MB VTCM.

The ExecuTorch checkout on the board is:

```bash
/home/ubuntu/Documents/executorch
```

Patch these files in that checkout:

```text
backends/qualcomm/serialization/qc_schema.py
backends/qualcomm/serialization/qc_compiler_spec.fbs
backends/qualcomm/utils/utils.py
```

From the ExecuTorch checkout root, apply this exact patch:

```bash
cd "$EXECUTORCH_ROOT"

git apply <<'PATCH'
diff --git a/backends/qualcomm/serialization/qc_compiler_spec.fbs b/backends/qualcomm/serialization/qc_compiler_spec.fbs
index 57708c959e..4bab8fd34b 100644
--- a/backends/qualcomm/serialization/qc_compiler_spec.fbs
+++ b/backends/qualcomm/serialization/qc_compiler_spec.fbs
@@ -54,6 +54,7 @@ enum QcomChipset: int {
   SM8650 = 57,
   SM8750 = 69,
   SM8850 = 87,
+  QCS8300 = 82,
   SSG2115P = 46,
   SSG2125P = 58,
   SXR1230P = 45,
diff --git a/backends/qualcomm/serialization/qc_schema.py b/backends/qualcomm/serialization/qc_schema.py
index aeffbc069b..eda7a1286e 100644
--- a/backends/qualcomm/serialization/qc_schema.py
+++ b/backends/qualcomm/serialization/qc_schema.py
@@ -61,6 +61,7 @@ class QcomChipset(IntEnum):
     SM8650 = 57  # v75
     SM8750 = 69  # v79
     SM8850 = 87  # v81
+    QCS8300 = 82  # v75
     SSG2115P = 46  # v73
     SSG2125P = 58  # v73
     SXR1230P = 45  # v73
@@ -94,6 +95,7 @@ _soc_info_table = {
     QcomChipset.SM8850: SocInfo(
         QcomChipset.SM8850, HtpInfo(HtpArch.V81, 8), LpaiInfo(LpaiHardwareVersion.V6)
     ),
+    QcomChipset.QCS8300: SocInfo(QcomChipset.QCS8300, HtpInfo(HtpArch.V75, 8)),
     QcomChipset.SSG2115P: SocInfo(QcomChipset.SSG2115P, HtpInfo(HtpArch.V73, 2)),
     QcomChipset.SSG2125P: SocInfo(QcomChipset.SSG2125P, HtpInfo(HtpArch.V73, 2)),
     QcomChipset.SXR1230P: SocInfo(QcomChipset.SXR1230P, HtpInfo(HtpArch.V73, 2)),
diff --git a/backends/qualcomm/utils/utils.py b/backends/qualcomm/utils/utils.py
index 16a071f8cf..eaed368195 100644
--- a/backends/qualcomm/utils/utils.py
+++ b/backends/qualcomm/utils/utils.py
@@ -1303,6 +1303,7 @@ def get_soc_to_htp_arch_map():
         "SM8650": HtpArch.V75,
         "SM8750": HtpArch.V79,
         "SM8850": HtpArch.V81,
+        "QCS8300": HtpArch.V75,
         "SSG2115P": HtpArch.V73,
         "SSG2125P": HtpArch.V73,
         "SXR1230P": HtpArch.V73,
@@ -1336,6 +1337,7 @@ def get_soc_to_chipset_map():
         "SM8650": QcomChipset.SM8650,
         "SM8750": QcomChipset.SM8750,
         "SM8850": QcomChipset.SM8850,
+        "QCS8300": QcomChipset.QCS8300,
         "SSG2115P": QcomChipset.SSG2115P,
         "SSG2125P": QcomChipset.SSG2125P,
         "SXR1230P": QcomChipset.SXR1230P,
PATCH
```

If the patch context does not match because the ExecuTorch revision changed, make the same four changes manually:

- Add `QCS8300 = 82` to the `QcomChipset` enum in both `qc_schema.py` and `qc_compiler_spec.fbs`.
- Add `QcomChipset.QCS8300: SocInfo(QcomChipset.QCS8300, HtpInfo(HtpArch.V75, 8))` to `_soc_info_table`.
- Add `"QCS8300": HtpArch.V75` to `get_soc_to_htp_arch_map()`.
- Add `"QCS8300": QcomChipset.QCS8300` to `get_soc_to_chipset_map()`.

The exact working checkout has these observed mappings:

```text
qc_schema.py: QCS8300 = 82
qc_schema.py: QcomChipset.QCS8300 -> HtpArch.V75, vtcm 8
utils.py: "QCS8300" -> HtpArch.V75
utils.py: "QCS8300" -> QcomChipset.QCS8300
```

There was also an ExecuTorch Python import issue where source-tree serialization resources were missing from the import location. The working board copied:

```text
/home/ubuntu/Documents/executorch/schema/program.fbs
/home/ubuntu/Documents/executorch/schema/scalar_type.fbs
```

into:

```text
/home/ubuntu/Documents/executorch/exir/_serialize/
```

If export fails with missing schema resources, repeat those copies.

## Build ExecuTorch

Build ExecuTorch natively on the Ventuno with the Qualcomm backend enabled. The ROS build points CMake at:

```bash
$EXECUTORCH_ROOT/build-x86/lib/cmake/ExecuTorch
```

The name `build-x86` is just the existing build folder name from the ExecuTorch Qualcomm scripts. On the Ventuno it contains native ARM artifacts because CMake is run directly on the board.

Install build prerequisites:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential \
  cmake \
  ninja-build \
  clang \
  libclang-dev \
  git \
  wget \
  curl \
  libssl-dev \
  libffi-dev \
  pkg-config \
  patchelf \
  flatbuffers-compiler \
  libflatbuffers-dev \
  python3-venv \
  python3-pip
```

Set up the Python environment used by export and by the Qualcomm build tooling:

```bash
cd "$EXECUTORCH_ROOT"
python3 -m venv "$HOME/.venv/executorch"
source "$HOME/.venv/executorch/bin/activate"
python -m pip install --upgrade pip setuptools wheel
python -m pip install -r requirements.txt
python -m pip install -r backends/qualcomm/requirements.txt
```

If your ExecuTorch checkout uses submodules, initialize them before configuring CMake:

```bash
cd "$EXECUTORCH_ROOT"
git submodule update --init --recursive
```

Then configure, build, and install ExecuTorch into `$EXECUTORCH_ROOT/build-x86`:

```bash
cd "$EXECUTORCH_ROOT"
source "$HOME/.venv/executorch/bin/activate"

cmake -S . -B build-x86 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$EXECUTORCH_ROOT/build-x86" \
  -DQNN_SDK_ROOT="$QNN_SDK_ROOT" \
  -DEXECUTORCH_BUILD_QNN=ON \
  -DEXECUTORCH_BUILD_DEVTOOLS=ON \
  -DEXECUTORCH_BUILD_EXECUTOR_RUNNER=ON \
  -DEXECUTORCH_BUILD_EXTENSION_MODULE=ON \
  -DEXECUTORCH_BUILD_EXTENSION_DATA_LOADER=ON \
  -DEXECUTORCH_BUILD_EXTENSION_FLAT_TENSOR=ON \
  -DEXECUTORCH_BUILD_EXTENSION_NAMED_DATA_MAP=ON \
  -DEXECUTORCH_BUILD_EXTENSION_TENSOR=ON \
  -DEXECUTORCH_BUILD_EXTENSION_LLM=ON \
  -DEXECUTORCH_BUILD_EXTENSION_LLM_RUNNER=ON \
  -DEXECUTORCH_BUILD_KERNELS_QUANTIZED=ON \
  -DEXECUTORCH_BUILD_KERNELS_QUANTIZED_AOT=ON \
  -DEXECUTORCH_BUILD_PORTABLE_OPS=ON \
  -DEXECUTORCH_BUILD_TESTS=OFF \
  -DEXECUTORCH_BUILD_XNNPACK=OFF \
  -DEXECUTORCH_ENABLE_EVENT_TRACER=ON \
  -DEXECUTORCH_ENABLE_LOGGING=ON \
  -DEXECUTORCH_USE_DL=ON \
  -DPYTHON_EXECUTABLE="$HOME/.venv/executorch/bin/python"

cmake --build build-x86 --target install -j"$(nproc)"
```

The Qualcomm helper script can also produce this tree. This is equivalent for the native build and uses the same `build-x86` output directory:

```bash
cd "$EXECUTORCH_ROOT"
source "$HOME/.venv/executorch/bin/activate"
./backends/qualcomm/scripts/build.sh \
  --skip_linux_android \
  --release \
  --job_number "$(nproc)"
```

After the build, copy the Qualcomm Python extension artifacts and schema resources into the source tree locations used by the export path:

```bash
cd "$EXECUTORCH_ROOT"
mkdir -p backends/qualcomm/python exir/_serialize
cp -fv build-x86/backends/qualcomm/Py* backends/qualcomm/python/
cp -fv schema/program.fbs exir/_serialize/program.fbs
cp -fv schema/scalar_type.fbs exir/_serialize/scalar_type.fbs

if [ -f build-x86/kernels/quantized/libquantized_ops_aot_lib.so ]; then
  cp -fv build-x86/kernels/quantized/libquantized_ops_aot_lib.so kernels/quantized/
fi
```

Verify the installed CMake package and QNN backend library exist:

```bash
test -f "$EXECUTORCH_ROOT/build-x86/lib/cmake/ExecuTorch/executorch-config.cmake"
test -f "$EXECUTORCH_ROOT/build-x86/lib/cmake/ExecuTorch/ExecuTorchTargets.cmake"
test -f "$EXECUTORCH_ROOT/build-x86/lib/executorch/backends/qualcomm/libqnn_executorch_backend.so"
```

The ROS package links these ExecuTorch targets:

```text
executorch
executorch_core
extension_module_static
extension_tensor
portable_ops_lib
qnn_executorch_backend
```

The package had to work around optional tokenizer targets exported by ExecuTorch CMake. `src/yolox_detector/CMakeLists.txt` predefines those tokenizer targets as imported interface libraries so `find_package(executorch REQUIRED)` succeeds without pulling in unrelated tokenizer dependencies.

## Python Environment for Export

The working board uses a Python venv at:

```bash
$HOME/.venv/executorch
```

Activate it for model export:

```bash
source "$HOME/.venv/executorch/bin/activate"
```

The export needs PyTorch and the local ExecuTorch checkout importable. The persistent `PYTHONPATH` includes:

```bash
$QAIRT_LIB/python
$HOME/Documents
```

That lets Python import both QAIRT/QNN Python pieces and the local `executorch` source checkout.

## Model and Dataset

Create the model and dataset directories:

```bash
cd "$VENTUNO_OBJECT_TRACKING_ROOT"
mkdir -p models datasets/sample_images
```

Download YOLOX Tiny weights:

```bash
curl -L \
  -o models/yolox_tiny.pth \
  https://github.com/Megvii-BaseDetection/YOLOX/releases/download/0.1.1/yolox_tiny.pth
```

Download a small COCO image sample:

```bash
cd "$VENTUNO_OBJECT_TRACKING_ROOT/datasets/sample_images"
for id in 000000000139 000000000285 000000000632 000000000724 000000000785; do
  curl -L -O "http://images.cocodataset.org/val2017/${id}.jpg"
done
```

The working board has:

```text
datasets/sample_images/000000000139.jpg
datasets/sample_images/000000000285.jpg
datasets/sample_images/000000000632.jpg
datasets/sample_images/000000000724.jpg
datasets/sample_images/000000000785.jpg
```

## Export YOLOX Tiny to QNN HTP `.pte`

Run export on the Ventuno:

```bash
cd "$VENTUNO_OBJECT_TRACKING_ROOT"
source "$HOME/.venv/executorch/bin/activate"

python tools/export_yolox_qnn.py \
  --weights models/yolox_tiny.pth \
  --output models/yolox_tiny_qnn.pte \
  --soc-model QCS8300 \
  --calibration-batches 4

deactivate
```

Expected successful signs during export:

```text
[Qnn ExecuTorch]: Creating new backend bundle.
Starting stage: Graph Preparation Initializing
...
====== DDR bandwidth summary ======
...
Saved: models/yolox_tiny_qnn.pte
```

The current working output file is about 5.3 MB:

```bash
ls -lh models/yolox_tiny_qnn.pte
```

## Build the ROS Package

Build only the detector package with QNN enabled:

```bash
cd "$VENTUNO_OBJECT_TRACKING_ROOT"
source /opt/ros/jazzy/setup.bash

colcon build \
  --packages-select yolox_detector \
  --cmake-args \
    -Dexecutorch_DIR=$EXECUTORCH_ROOT/build-x86/lib/cmake/ExecuTorch \
    -DBUILD_QNN_BACKEND=ON
```

Expected result:

```text
Finished <<< yolox_detector
Summary: 1 package finished
```

## Launch Dataset Inference on the NPU

Run this on the Ventuno:

```bash
cd "$VENTUNO_OBJECT_TRACKING_ROOT"
source /opt/ros/jazzy/setup.bash
source install/setup.bash

ros2 launch yolox_detector dataset_detector.launch.py \
  backend:=npu \
  model_path:=models/yolox_tiny_qnn.pte \
  dataset_path:=datasets/sample_images \
  publish_rate:=5.0 \
  qnn_lib_dir:=$QAIRT_LIB/aarch64-oe-linux-gcc11.2
```

Or from a development machine:

```bash
ssh ubuntu@ventuno 'bash -lc '\''
cd "$VENTUNO_OBJECT_TRACKING_ROOT"
source /opt/ros/jazzy/setup.bash
source install/setup.bash

ros2 launch yolox_detector dataset_detector.launch.py \
  backend:=npu \
  model_path:=models/yolox_tiny_qnn.pte \
  dataset_path:=datasets/sample_images \
  publish_rate:=5.0 \
  qnn_lib_dir:=$QAIRT_LIB/aarch64-oe-linux-gcc11.2
'\'''
```

The launch should start two nodes:

```text
/dataset_image_publisher
/yolox_detector
```

And publish these main topics:

```text
/camera/rgb/image_raw
/detections
/detections/image
```

## Runtime Verification

In another shell on the Ventuno:

```bash
cd "$VENTUNO_OBJECT_TRACKING_ROOT"
source /opt/ros/jazzy/setup.bash
source install/setup.bash

ros2 node list
ros2 topic list
ros2 topic hz /detections
ros2 topic echo --once /detections
ros2 topic echo --once /detections/image --field header
```

The verified run published `/detections` at about 5 Hz and produced non-empty `vision_msgs/msg/Detection2DArray` messages with scores above the configured `0.45` threshold.

To check that the process has loaded QNN/FastRPC libraries:

```bash
PID=$(pgrep -f yolox_detector_node)
grep -E 'libQnn|libcdsprpc|libxdsprpc|hexagon|fastrpc' /proc/$PID/maps | sort -u
```

## Saving Annotated Images

The node publishes annotated images on `/detections/image` only when there is a subscriber. If `image_view` is installed, save images with:

```bash
cd "$VENTUNO_OBJECT_TRACKING_ROOT"
source /opt/ros/jazzy/setup.bash
source install/setup.bash

mkdir -p artifacts/annotated
cd artifacts/annotated
ros2 run image_view image_saver --ros-args -r image:=/detections/image
```

If `image_view` is not installed, install it or write a small `rclpy` subscriber using `cv_bridge` to subscribe to `/detections/image` and write the frames with OpenCV.

## Issues Encountered and Fixes

### ExecuTorch Did Not Know the Ventuno SoC

Symptom: export or QNN backend setup did not accept the board target cleanly.

Cause: Ventuno reports as `QCS8275` / soc id `675`, but the ExecuTorch Qualcomm backend did not have a matching supported chipset enum for this board.

Fix: add a `QCS8300 = 82` compatibility target in ExecuTorch Qualcomm serialization and utility mappings, and map it to `HtpArch.V75` with 8 MB VTCM. Export with `--soc-model QCS8300`.

### Wrong HTP Assumption

Symptom: QNN setup did not work when assuming a newer HTP target.

Cause: this board validates as HTP V75, not V81.

Fix: use `hexagon-v75/unsigned` in `ADSP_LIBRARY_PATH` and map `QCS8300` to `HtpArch.V75`.

### FastRPC Libraries and Permissions Missing

Symptom: QNN validation/runtime failed around DSP/FastRPC loading, including missing `libcdsprpc.so` or missing `/dev/fastrpc-*` access.

Fix:

```bash
sudo apt-get install -y qcom-fastrpc1 qcom-fastrpc-dev
```

### ADSP Library Path Separator

Symptom: DSP skel libraries were not found reliably even when the paths looked correct.

Cause: FastRPC expects `ADSP_LIBRARY_PATH` entries separated by semicolons, not normal Linux colon separators.

Fix:

```bash
export ADSP_LIBRARY_PATH="$QAIRT_LIB/hexagon-v75/unsigned;/usr/lib/dsp/cdsp;/usr/lib/rfsa/adsp;/dsp/cdsp;/dsp"
```

### ExecuTorch Python Schema Resources Missing

Symptom: Python export failed importing serialization schema resources from the source checkout.

Fix: copy `schema/program.fbs` and `schema/scalar_type.fbs` into `exir/_serialize/` inside the ExecuTorch checkout.

### ROS `cv_bridge` Include on Jazzy

Symptom: ROS build failed because the include path/header style changed.

Fix: use the Jazzy-compatible header:

```cpp
#include <cv_bridge/cv_bridge.hpp>
```

### ExecuTorch Tensor Shape Type

Symptom: C++ build failed around `from_blob` shape construction.

Fix: use `executorch::aten::SizesType` for the shape vector in the backend implementations.

### ExecuTorch Module Loading API

Symptom: backend code did not match the current ExecuTorch `Module` API.

Fix: construct `Module(model_path, Module::LoadMode::MmapUseMlock)` and call `module_->load()` with no model path argument.

### ExecuTorch CMake Export Pulled Optional Tokenizer Targets

Symptom: `find_package(executorch REQUIRED)` failed because optional tokenizer targets were referenced by the package config even though this ROS package does not use them.

Fix: predefine those tokenizer targets as imported interface libraries before `find_package(executorch REQUIRED)` in `src/yolox_detector/CMakeLists.txt`.

### Empty Detections at First Runtime

Symptom: the node loaded and published `/detections`, but arrays were empty at `score_threshold: 0.45`.

Causes:

- Input preprocessing normalized images to `0..1`, but YOLOX export/runtime expected BGR float values in `0..255` with letterbox padding around `114`.
- Postprocess treated objectness and class logits as probabilities instead of applying sigmoid.

Fixes:

- Remove the `1/255` normalization before inference.
- Use float letterbox padding value `114`.
- Convert resized images to `CV_32FC3` after letterboxing.
- Apply sigmoid to objectness and class logits during decode.
- Update QNN calibration tensors to match runtime scale: `torch.rand(...) * 255.0`.

After these fixes, the same launch produced non-empty detections.

## Replicating on a Different Ventuno Q

Use this sequence on the new board.

1. Confirm the board identity:

```bash
cat /sys/devices/soc0/machine
cat /sys/devices/soc0/soc_id
```

2. Install runtime packages:

```bash
sudo apt-get update
sudo apt-get install -y qcom-fastrpc1 qcom-fastrpc-dev
```

3. Confirm QAIRT/QNN exists. If the path differs, update `QNN_SDK_ROOT` in `~/.ventuno_object_tracking_env`:

```bash
ls /opt/qcom/aistack/qairt/2.47.0.260601
ls /opt/qcom/aistack/qairt/2.47.0.260601/lib/aarch64-oe-linux-gcc11.2/libQnnHtp.so
ls /opt/qcom/aistack/qairt/2.47.0.260601/lib/hexagon-v75/unsigned
```

4. Create and source `~/.ventuno_object_tracking_env` from `~/.bashrc` as shown above.

5. Clone or copy the project to:

```bash
$HOME/Documents/ventuno-object-tracking
```

6. Clone or copy ExecuTorch to:

```bash
$HOME/Documents/executorch
```

7. Apply the exact `git apply` patch in `ExecuTorch Checkout and Qualcomm Backend Patches`. It adds `QCS8300 = 82`, maps that chipset to `HtpArch.V75` with 8 MB VTCM, and registers the `QCS8300` string in the Qualcomm utility maps.

8. Build ExecuTorch with the Qualcomm backend. The full command block is in `Build ExecuTorch`; the compact form is:

```bash
cd "$EXECUTORCH_ROOT"
python3 -m venv "$HOME/.venv/executorch"
source "$HOME/.venv/executorch/bin/activate"
python -m pip install --upgrade pip setuptools wheel
python -m pip install -r requirements.txt
python -m pip install -r backends/qualcomm/requirements.txt
git submodule update --init --recursive

cmake -S . -B build-x86 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$EXECUTORCH_ROOT/build-x86" \
  -DQNN_SDK_ROOT="$QNN_SDK_ROOT" \
  -DEXECUTORCH_BUILD_QNN=ON \
  -DEXECUTORCH_BUILD_DEVTOOLS=ON \
  -DEXECUTORCH_BUILD_EXECUTOR_RUNNER=ON \
  -DEXECUTORCH_BUILD_EXTENSION_MODULE=ON \
  -DEXECUTORCH_BUILD_EXTENSION_DATA_LOADER=ON \
  -DEXECUTORCH_BUILD_EXTENSION_FLAT_TENSOR=ON \
  -DEXECUTORCH_BUILD_EXTENSION_NAMED_DATA_MAP=ON \
  -DEXECUTORCH_BUILD_EXTENSION_TENSOR=ON \
  -DEXECUTORCH_BUILD_EXTENSION_LLM=ON \
  -DEXECUTORCH_BUILD_EXTENSION_LLM_RUNNER=ON \
  -DEXECUTORCH_BUILD_KERNELS_QUANTIZED=ON \
  -DEXECUTORCH_BUILD_KERNELS_QUANTIZED_AOT=ON \
  -DEXECUTORCH_BUILD_PORTABLE_OPS=ON \
  -DEXECUTORCH_BUILD_TESTS=OFF \
  -DEXECUTORCH_BUILD_XNNPACK=OFF \
  -DEXECUTORCH_ENABLE_EVENT_TRACER=ON \
  -DEXECUTORCH_ENABLE_LOGGING=ON \
  -DEXECUTORCH_USE_DL=ON \
  -DPYTHON_EXECUTABLE="$HOME/.venv/executorch/bin/python"

cmake --build build-x86 --target install -j"$(nproc)"

mkdir -p backends/qualcomm/python exir/_serialize
cp -fv build-x86/backends/qualcomm/Py* backends/qualcomm/python/
cp -fv schema/program.fbs exir/_serialize/program.fbs
cp -fv schema/scalar_type.fbs exir/_serialize/scalar_type.fbs

test -f "$EXECUTORCH_ROOT/build-x86/lib/cmake/ExecuTorch/executorch-config.cmake"
test -f "$EXECUTORCH_ROOT/build-x86/lib/cmake/ExecuTorch/ExecuTorchTargets.cmake"
test -f "$EXECUTORCH_ROOT/build-x86/lib/executorch/backends/qualcomm/libqnn_executorch_backend.so"
```

9. Confirm the Python venv can import PyTorch and the local ExecuTorch checkout:

```bash
source "$HOME/.venv/executorch/bin/activate"
python -c 'import torch; import executorch; print(torch.__version__)'
deactivate
```

10. Download YOLOX Tiny and sample images.

11. Export the model:

```bash
cd "$VENTUNO_OBJECT_TRACKING_ROOT"
source "$HOME/.venv/executorch/bin/activate"
python tools/export_yolox_qnn.py \
  --weights models/yolox_tiny.pth \
  --output models/yolox_tiny_qnn.pte \
  --soc-model QCS8300 \
  --calibration-batches 4
deactivate
```

12. Build the ROS package:

```bash
cd "$VENTUNO_OBJECT_TRACKING_ROOT"
source /opt/ros/jazzy/setup.bash
colcon build \
  --packages-select yolox_detector \
  --cmake-args \
    -Dexecutorch_DIR=$EXECUTORCH_ROOT/build-x86/lib/cmake/ExecuTorch \
    -DBUILD_QNN_BACKEND=ON
```

13. Launch and verify:

```bash
cd "$VENTUNO_OBJECT_TRACKING_ROOT"
source /opt/ros/jazzy/setup.bash
source install/setup.bash
ros2 launch yolox_detector dataset_detector.launch.py \
  backend:=npu \
  model_path:=models/yolox_tiny_qnn.pte \
  dataset_path:=datasets/sample_images \
  publish_rate:=5.0 \
  qnn_lib_dir:=$QAIRT_LIB/aarch64-oe-linux-gcc11.2
```

In another terminal:

```bash
source /opt/ros/jazzy/setup.bash
cd "$VENTUNO_OBJECT_TRACKING_ROOT"
source install/setup.bash
ros2 topic hz /detections
ros2 topic echo --once /detections
```

## Known Non-Blocking Warning

The working board emitted this during ROS setup:

```text
not found: "/home/ubuntu/Documents/ventuno-object-tracking/install/oak_camera/share/oak_camera/local_setup.bash"
```

This did not block the dataset YOLOX path. It indicates the workspace install setup references an incomplete or missing `oak_camera` package. That should be cleaned up before the later OAK-D integration phase, but it is not required for sample-image inference.

## Current Verified Commands

The latest successful export/build sequence was:

```bash
ssh ubuntu@ventuno 'bash -lc '\''
set -e
cd "$VENTUNO_OBJECT_TRACKING_ROOT"
source "$HOME/.venv/executorch/bin/activate"
python tools/export_yolox_qnn.py \
  --weights models/yolox_tiny.pth \
  --output models/yolox_tiny_qnn.pte \
  --soc-model QCS8300 \
  --calibration-batches 4
deactivate
source /opt/ros/jazzy/setup.bash
colcon build \
  --packages-select yolox_detector \
  --cmake-args \
    -Dexecutorch_DIR=$EXECUTORCH_ROOT/build-x86/lib/cmake/ExecuTorch \
    -DBUILD_QNN_BACKEND=ON
'\'''
```

The latest successful runtime launch was:

```bash
ssh ubuntu@ventuno 'bash -lc '\''
cd "$VENTUNO_OBJECT_TRACKING_ROOT"
source /opt/ros/jazzy/setup.bash
source install/setup.bash
ros2 launch yolox_detector dataset_detector.launch.py \
  backend:=npu \
  model_path:=models/yolox_tiny_qnn.pte \
  dataset_path:=datasets/sample_images \
  publish_rate:=5.0 \
  qnn_lib_dir:=$QAIRT_LIB/aarch64-oe-linux-gcc11.2
'\'''
```
