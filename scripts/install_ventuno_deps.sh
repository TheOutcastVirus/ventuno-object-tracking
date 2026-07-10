#!/usr/bin/env bash
# Install system and project dependencies for ventuno-object-tracking on Ubuntu
# 24.04 / ROS 2 Jazzy. Run as the target login user, not with sudo:
#
#   bash scripts/install_ventuno_deps.sh
#
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"

ROS_DISTRO="${ROS_DISTRO:-jazzy}"
EXECUTORCH_ROOT="/opt/executorch"
EXECUTORCH_REPO="${EXECUTORCH_REPO:-https://github.com/pytorch/executorch.git}"
EXECUTORCH_REF="${EXECUTORCH_REF:-}"
PROJECT_VENV="${PROJECT_VENV:-$HOME/.venv/ventuno-object-tracking}"
EXECUTORCH_VENV="${EXECUTORCH_VENV:-$HOME/.venv/executorch}"
ROS_DOMAIN_ID_VALUE="${ROS_DOMAIN_ID_VALUE:-0}"
QNN_SDK_VERSION="${QNN_SDK_VERSION:-2.48.0.260626}"
QNN_SDK_URL="${QNN_SDK_URL:-https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/${QNN_SDK_VERSION}/v${QNN_SDK_VERSION}.zip}"
QNN_SDK_ZIP="${QNN_SDK_ZIP:-}"
QNN_SDK_ROOT_EXPLICIT=0

INSTALL_EXPORT_PYTHON_DEPS=0
DOWNLOAD_YOLOX_WEIGHTS=0
SKIP_SAMPLE_IMAGES=0
SKIP_ROSDEP=0
SKIP_CREATE3_SERVICE=0
SKIP_WORKSPACE_BUILD=0
SKIP_DDS_TUNING=0
ALLOW_UNSUPPORTED_OS=0
UPGRADE_SYSTEM=0

log() { printf '\n[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
warn() { printf '\nWARN: %s\n' "$*" >&2; }
die() { printf '\nERROR: %s\n' "$*" >&2; exit 1; }

usage() {
  cat <<'EOF'
Usage: bash scripts/install_ventuno_deps.sh [options]

Installs Ubuntu/ROS 2 Jazzy dependencies, project Python dependencies, board
environment, DDS tuning, Create 3 USB gadget service, sample images, clones
ExecuTorch into /opt/executorch, builds it, and then builds the ROS workspace.

Options:
  --executorch-ref REF        Optional branch/tag/commit to checkout after clone.
  --qnn-sdk-root PATH         QAIRT/QNN SDK root. Defaults to /opt/qcom/aistack/qairt/2.48.0.260626.
  --qnn-sdk-url URL           QAIRT/QNN SDK zip URL to download if the SDK is missing.
  --qnn-sdk-zip PATH          Local QAIRT/QNN SDK zip to install if the SDK is missing.
  --with-export-python-deps   Install requirements-export.txt into the project venv.
  --download-yolox-weights    Download YOLOX .pth weights. Off by default; pre-exported .pte is expected.
  --skip-sample-images        Do not download sample COCO images for dataset launches.
  --skip-models               Compatibility alias for --skip-sample-images.
  --skip-rosdep               Do not run rosdep install.
  --skip-create3-service      Do not install/enable the Create 3 USB gadget service.
  --skip-workspace-build      Do not run colcon build.
  --skip-dds-tuning           Do not install ROS 2 DDS socket buffer sysctl tuning.
  --upgrade-system            Run apt-get upgrade after adding the ROS repository.
  --allow-unsupported-os      Continue even if the OS is not Ubuntu 24.04.
  -h, --help                  Show this help.

Environment overrides:
  ROS_DISTRO, EXECUTORCH_REPO, EXECUTORCH_REF, PROJECT_VENV,
  EXECUTORCH_VENV, QNN_SDK_VERSION, QNN_SDK_URL, QNN_SDK_ZIP,
  ROS_DOMAIN_ID_VALUE
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --clone-executorch|--build-executorch|--skip-executorch)
      warn "$1 is ignored; ExecuTorch is always cloned to /opt/executorch and built"
      ;;
    --executorch-root)
      warn "--executorch-root is ignored; ExecuTorch is always installed at /opt/executorch"
      shift
      ;;
    --executorch-ref) EXECUTORCH_REF="${2:?missing ref}"; shift ;;
    --qnn-sdk-root) QNN_SDK_ROOT="${2:?missing path}"; QNN_SDK_ROOT_EXPLICIT=1; shift ;;
    --qnn-sdk-url) QNN_SDK_URL="${2:?missing URL}"; shift ;;
    --qnn-sdk-zip) QNN_SDK_ZIP="${2:?missing path}"; shift ;;
    --with-export-python-deps) INSTALL_EXPORT_PYTHON_DEPS=1 ;;
    --download-yolox-weights) DOWNLOAD_YOLOX_WEIGHTS=1 ;;
    --skip-sample-images|--skip-models) SKIP_SAMPLE_IMAGES=1 ;;
    --skip-rosdep) SKIP_ROSDEP=1 ;;
    --skip-create3-service) SKIP_CREATE3_SERVICE=1 ;;
    --skip-workspace-build) SKIP_WORKSPACE_BUILD=1 ;;
    --skip-dds-tuning) SKIP_DDS_TUNING=1 ;;
    --upgrade-system) UPGRADE_SYSTEM=1 ;;
    --allow-unsupported-os) ALLOW_UNSUPPORTED_OS=1 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
  shift
done

if [ "$QNN_SDK_ROOT_EXPLICIT" -eq 0 ]; then
  unset QNN_SDK_ROOT
fi

if [ "${EUID}" -eq 0 ]; then
  TARGET_USER="${SUDO_USER:-root}"
else
  TARGET_USER="$(id -un)"
fi
TARGET_HOME="$(getent passwd "$TARGET_USER" | cut -d: -f6)"
TARGET_GROUP="$(id -gn "$TARGET_USER")"


case "$PROJECT_VENV" in
  "~"/*) PROJECT_VENV="$TARGET_HOME/${PROJECT_VENV#"~/"}" ;;
esac
case "$EXECUTORCH_VENV" in
  "~"/*) EXECUTORCH_VENV="$TARGET_HOME/${EXECUTORCH_VENV#"~/"}" ;;
esac

sudo_run() {
  if [ "${EUID}" -eq 0 ]; then
    "$@"
  else
    sudo "$@"
  fi
}

as_target_user() {
  if [ "$(id -un)" = "$TARGET_USER" ]; then
    "$@"
  else
    sudo -H -u "$TARGET_USER" "$@"
  fi
}

q() { printf '%q' "$1"; }

apt_install() {
  sudo_run apt-get install -y --no-install-recommends "$@"
}

apt_has() {
  apt-cache show "$1" >/dev/null 2>&1
}

install_available_packages() {
  local pkg available=() missing=()
  for pkg in "$@"; do
    if apt_has "$pkg"; then
      available+=("$pkg")
    else
      missing+=("$pkg")
    fi
  done

  if [ "${#available[@]}" -gt 0 ]; then
    apt_install "${available[@]}"
  fi

  if [ "${#missing[@]}" -gt 0 ]; then
    warn "apt packages not available on this image: ${missing[*]}"
  fi
}

read_os_release() {
  # shellcheck disable=SC1091
  . /etc/os-release
  OS_ID="${ID:-}"
  OS_VERSION_ID="${VERSION_ID:-}"
  OS_CODENAME="${VERSION_CODENAME:-${UBUNTU_CODENAME:-}}"
}

check_os() {
  read_os_release
  if [ "$OS_ID" != "ubuntu" ] || [ "$OS_VERSION_ID" != "24.04" ]; then
    if [ "$ALLOW_UNSUPPORTED_OS" -eq 1 ]; then
      warn "continuing on unsupported OS: ${PRETTY_NAME:-unknown}"
    else
      die "ROS 2 Jazzy apt packages target Ubuntu 24.04. Re-run with --allow-unsupported-os to continue."
    fi
  fi
}

configure_locale() {
  log "Configuring locale"
  apt_install locales software-properties-common curl ca-certificates gnupg lsb-release
  sudo_run locale-gen en_US en_US.UTF-8
  sudo_run update-locale LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8
  sudo_run add-apt-repository -y universe
}

configure_ros_apt_fallback() {
  log "Configuring ROS apt repository with keyring fallback"
  local keyring="/usr/share/keyrings/ros-archive-keyring.gpg"
  sudo_run curl -fsSL https://raw.githubusercontent.com/ros/rosdistro/master/ros.key -o "$keyring"
  printf 'deb [arch=%s signed-by=%s] http://packages.ros.org/ros2/ubuntu %s main\n' \
    "$(dpkg --print-architecture)" "$keyring" "$OS_CODENAME" |
    sudo_run tee /etc/apt/sources.list.d/ros2.list >/dev/null
}

configure_ros_apt() {
  if dpkg -s ros2-apt-source >/dev/null 2>&1; then
    log "ROS apt source package is already installed"
    return
  fi

  log "Configuring ROS apt repository"
  local latest tmp deb_url
  latest="$(curl -fsSL https://api.github.com/repos/ros-infrastructure/ros-apt-source/releases/latest |
    sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' |
    head -n1 || true)"

  if [ -n "$latest" ]; then
    tmp="$(mktemp)"
    deb_url="https://github.com/ros-infrastructure/ros-apt-source/releases/download/${latest}/ros2-apt-source_${latest}.${OS_CODENAME}_all.deb"
    if curl -fL "$deb_url" -o "$tmp"; then
      sudo_run dpkg -i "$tmp" || configure_ros_apt_fallback
    else
      warn "failed to fetch ros2-apt-source package; using keyring fallback"
      configure_ros_apt_fallback
    fi
    rm -f "$tmp"
  else
    warn "failed to discover latest ros2-apt-source release; using keyring fallback"
    configure_ros_apt_fallback
  fi
}

install_apt_dependencies() {
  log "Installing apt dependencies"
  sudo_run apt-get update
  if [ "$UPGRADE_SYSTEM" -eq 1 ]; then
    sudo_run apt-get upgrade -y
  fi

  apt_install \
    build-essential \
    clang \
    cmake \
    curl \
    git \
    libclang-dev \
    libffi-dev \
    libflatbuffers-dev \
    libopencv-dev \
    libssl-dev \
    ninja-build \
    patchelf \
    pkg-config \
    python3-argcomplete \
    python3-colcon-common-extensions \
    python3-pip \
    python3-rosdep \
    python3-venv \
    unzip \
    wget \
    zip \
    flatbuffers-compiler

  apt_install \
    "ros-${ROS_DISTRO}-ros-base" \
    "ros-${ROS_DISTRO}-ament-cmake" \
    "ros-${ROS_DISTRO}-camera-info-manager" \
    "ros-${ROS_DISTRO}-cv-bridge" \
    "ros-${ROS_DISTRO}-geometry-msgs" \
    "ros-${ROS_DISTRO}-image-transport" \
    "ros-${ROS_DISTRO}-rclcpp" \
    "ros-${ROS_DISTRO}-rclpy" \
    "ros-${ROS_DISTRO}-rmw-fastrtps-cpp" \
    "ros-${ROS_DISTRO}-rosidl-default-generators" \
    "ros-${ROS_DISTRO}-rqt" \
    "ros-${ROS_DISTRO}-rqt-common-plugins" \
    "ros-${ROS_DISTRO}-rqt-graph" \
    "ros-${ROS_DISTRO}-rqt-image-view" \
    "ros-${ROS_DISTRO}-rqt-topic" \
    "ros-${ROS_DISTRO}-sensor-msgs" \
    "ros-${ROS_DISTRO}-vision-msgs"

  install_available_packages \
    "ros-${ROS_DISTRO}-create3-republisher" \
    "ros-${ROS_DISTRO}-depthai-ros-v3" \
    "ros-${ROS_DISTRO}-depthai-v3" \
    "ros-${ROS_DISTRO}-image-view" \
    qcom-fastrpc1 \
    qcom-fastrpc-dev
}

detect_qnn_sdk() {
  if [ "$QNN_SDK_ROOT_EXPLICIT" -eq 1 ]; then
    [ -n "${QNN_SDK_ROOT:-}" ] || die "--qnn-sdk-root was set but QNN_SDK_ROOT is empty"
    return
  fi

  QNN_SDK_ROOT="/opt/qcom/aistack/qairt/$QNN_SDK_VERSION"
}

qnn_sdk_looks_installed() {
  [ -f "$QNN_SDK_ROOT/lib/aarch64-oe-linux-gcc11.2/libQnnSystem.so" ] &&
    [ -d "$QNN_SDK_ROOT/lib/hexagon-v75/unsigned" ]
}

ensure_qnn_sdk() {
  detect_qnn_sdk

  if qnn_sdk_looks_installed; then
    log "QAIRT/QNN SDK already installed at $QNN_SDK_ROOT"
    return
  fi

  if [ -e "$QNN_SDK_ROOT" ] && [ -n "$(find "$QNN_SDK_ROOT" -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null)" ]; then
    die "$QNN_SDK_ROOT exists but does not look like the QAIRT/QNN SDK. Set --qnn-sdk-root to the correct path or clean the partial install."
  fi

  log "Installing QAIRT/QNN SDK into $QNN_SDK_ROOT"
  local tmp_dir zip_path extract_dir qnn_lib sdk_root
  tmp_dir="$(mktemp -d)"
  zip_path="$tmp_dir/qairt.zip"
  extract_dir="$tmp_dir/extract"
  mkdir -p "$extract_dir"

  if [ -n "$QNN_SDK_ZIP" ]; then
    [ -f "$QNN_SDK_ZIP" ] || die "QNN SDK zip not found: $QNN_SDK_ZIP"
    cp "$QNN_SDK_ZIP" "$zip_path"
  else
    log "Downloading QAIRT/QNN SDK from $QNN_SDK_URL"
    curl -fL --retry 3 "$QNN_SDK_URL" -o "$zip_path" ||
      die "failed to download QAIRT/QNN SDK. If the Qualcomm URL requires login, download it manually and rerun with --qnn-sdk-zip PATH."
  fi

  unzip -q "$zip_path" -d "$extract_dir" || die "failed to extract QAIRT/QNN SDK zip"
  qnn_lib="$(find "$extract_dir" -type f -path '*/lib/aarch64-oe-linux-gcc11.2/libQnnSystem.so' | head -n1 || true)"
  [ -n "$qnn_lib" ] || die "extracted archive does not contain lib/aarch64-oe-linux-gcc11.2/libQnnSystem.so"
  sdk_root="$(dirname "$(dirname "$(dirname "$qnn_lib")")")"

  sudo_run install -d -m 0755 "$(dirname "$QNN_SDK_ROOT")"
  sudo_run install -d -m 0755 "$QNN_SDK_ROOT"
  sudo_run cp -a "$sdk_root/." "$QNN_SDK_ROOT/"

  rm -rf "$tmp_dir"
  qnn_sdk_looks_installed || die "QAIRT/QNN SDK install did not produce the expected runtime libraries at $QNN_SDK_ROOT"
}

write_board_environment() {
  detect_qnn_sdk
  log "Writing board environment for $TARGET_USER"

  local env_file="$TARGET_HOME/.ventuno_object_tracking_env"
  local bashrc="$TARGET_HOME/.bashrc"
  local tmp env_line
  tmp="$(mktemp)"

  cat > "$tmp" <<EOF
# Ventuno object tracking environment. Generated by scripts/install_ventuno_deps.sh.

_vot_prepend_path() {
  var_name="\$1"
  entry="\$2"
  eval current="\\\${\$var_name:-}"
  case ":\$current:" in
    *":\$entry:"*) ;;
    *)
      if [ -n "\$current" ]; then
        export "\$var_name=\$entry:\$current"
      else
        export "\$var_name=\$entry"
      fi
      ;;
  esac
}

export VENTUNO_OBJECT_TRACKING_ROOT="$PROJECT_ROOT"
export EXECUTORCH_ROOT="$EXECUTORCH_ROOT"
export QNN_SDK_ROOT="$QNN_SDK_ROOT"
export QAIRT_LIB="\$QNN_SDK_ROOT/lib"
export ROS_DOMAIN_ID="$ROS_DOMAIN_ID_VALUE"
export RMW_IMPLEMENTATION="rmw_fastrtps_cpp"
export FASTRTPS_DEFAULT_PROFILES_FILE="\$VENTUNO_OBJECT_TRACKING_ROOT/scripts/fastdds_usb0.xml"

_vot_prepend_path PATH "\$HOME/.local/bin"
_vot_prepend_path LD_LIBRARY_PATH "\$EXECUTORCH_ROOT/build-x86/backends/qualcomm"
_vot_prepend_path LD_LIBRARY_PATH "\$EXECUTORCH_ROOT/build-x86/lib/executorch/backends/qualcomm"
_vot_prepend_path LD_LIBRARY_PATH "\$QAIRT_LIB/aarch64-oe-linux-gcc11.2"
_vot_prepend_path PYTHONPATH "\$QAIRT_LIB/python"
_vot_prepend_path PYTHONPATH "\$HOME/Documents"

# FastRPC expects semicolon-separated DSP search paths.
export ADSP_LIBRARY_PATH="\$QAIRT_LIB/hexagon-v75/unsigned;/usr/lib/dsp/cdsp;/usr/lib/rfsa/adsp;/dsp/cdsp;/dsp"

unset -f _vot_prepend_path
EOF

  sudo_run install -o "$TARGET_USER" -g "$TARGET_GROUP" -m 0644 "$tmp" "$env_file"
  rm -f "$tmp"

  sudo_run touch "$bashrc"
  sudo_run chown "$TARGET_USER:$TARGET_GROUP" "$bashrc"

  ros_line="[ -f /opt/ros/$ROS_DISTRO/setup.bash ] && . /opt/ros/$ROS_DISTRO/setup.bash"
  env_line='[ -f "$HOME/.ventuno_object_tracking_env" ] && . "$HOME/.ventuno_object_tracking_env"'
  if ! grep -Fq "$ros_line" "$bashrc" || ! grep -Fq "$env_line" "$bashrc"; then
    tmp="$(mktemp)"
    if ! grep -Fq "$ros_line" "$bashrc"; then
      printf '%s\n' "$ros_line" > "$tmp"
    fi
    if ! grep -Fq "$env_line" "$bashrc"; then
      printf '%s\n' "$env_line" >> "$tmp"
    fi
    cat "$bashrc" >> "$tmp"
    sudo_run install -o "$TARGET_USER" -g "$TARGET_GROUP" -m 0644 "$tmp" "$bashrc"
    rm -f "$tmp"
  fi
}

install_dds_tuning() {
  if [ "$SKIP_DDS_TUNING" -eq 1 ]; then
    return
  fi

  log "Installing DDS socket buffer tuning"
  local tmp
  tmp="$(mktemp)"
  cat > "$tmp" <<'EOF'
net.core.rmem_max=16777216
net.core.wmem_max=16777216
net.core.rmem_default=16777216
net.core.wmem_default=16777216
EOF
  sudo_run install -m 0644 "$tmp" /etc/sysctl.d/60-ros2-dds.conf
  rm -f "$tmp"
  sudo_run sysctl --system
}

install_create3_service() {
  if [ "$SKIP_CREATE3_SERVICE" -eq 1 ]; then
    return
  fi

  if ! command -v systemctl >/dev/null 2>&1; then
    warn "systemctl not found; skipping Create 3 USB gadget service"
    return
  fi

  log "Installing Create 3 USB gadget systemd service"
  local tmp
  tmp="$(mktemp)"
  cat > "$tmp" <<EOF
[Unit]
Description=Create 3 USB-ethernet gadget (usb0 -> 192.168.186.3, base 192.168.186.2)
Documentation=file://$PROJECT_ROOT/docs/create3_connection.md
DefaultDependencies=no
After=sys-kernel-config.mount
Wants=sys-kernel-config.mount
Before=network-pre.target
Wants=network-pre.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=$PROJECT_ROOT/scripts/create3_usb_gadget.sh
ExecStop=$PROJECT_ROOT/scripts/create3_usb_gadget.sh down

[Install]
WantedBy=multi-user.target
EOF
  sudo_run install -m 0644 "$tmp" /etc/systemd/system/create3-usb-gadget.service
  rm -f "$tmp"
  sudo_run systemctl daemon-reload

  if [ -d /sys/class/udc ] && [ -n "$(find /sys/class/udc -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null)" ]; then
    sudo_run systemctl enable --now create3-usb-gadget.service
  else
    warn "no USB device controller found; service installed but not enabled"
  fi
}

setup_rosdep() {
  if [ "$SKIP_ROSDEP" -eq 1 ]; then
    return
  fi

  log "Running rosdep"
  if [ ! -f /etc/ros/rosdep/sources.list.d/20-default.list ]; then
    sudo_run rosdep init || true
  fi
  as_target_user rosdep update || warn "rosdep update failed"

  if [ -d "$PROJECT_ROOT/src" ]; then
    as_target_user bash -lc "source $(q "/opt/ros/$ROS_DISTRO/setup.bash") && cd $(q "$PROJECT_ROOT") && rosdep install --from-paths src --ignore-src -r -y --rosdistro $(q "$ROS_DISTRO")" ||
      warn "rosdep install reported unresolved dependencies"
  fi
}

setup_project_python() {
  log "Installing project Python dependencies into $PROJECT_VENV"
  as_target_user python3 -m venv "$PROJECT_VENV"
  as_target_user bash -lc "source $(q "$PROJECT_VENV/bin/activate") && python -m pip install --upgrade pip setuptools wheel"
  if [ "$INSTALL_EXPORT_PYTHON_DEPS" -eq 1 ]; then
    as_target_user bash -lc "source $(q "$PROJECT_VENV/bin/activate") && cd $(q "$PROJECT_ROOT") && python -m pip install -r requirements-export.txt"
  else
    as_target_user bash -lc "source $(q "$PROJECT_VENV/bin/activate") && cd $(q "$PROJECT_ROOT") && python -m pip install -r requirements.txt"
  fi
}

setup_assets() {
  log "Preparing model and dataset directories"
  as_target_user mkdir -p "$PROJECT_ROOT/models" "$PROJECT_ROOT/datasets/sample_images"

  if [ "$DOWNLOAD_YOLOX_WEIGHTS" -eq 1 ]; then
    log "Downloading YOLOX weights"
    as_target_user bash -lc "cd $(q "$PROJECT_ROOT") && source $(q "$PROJECT_VENV/bin/activate") && python scripts/download_models.py --skip-export"
  else
    log "Skipping YOLOX weight download; expecting pre-exported models in $PROJECT_ROOT/models"
  fi

  if [ "$SKIP_SAMPLE_IMAGES" -eq 1 ]; then
    return
  fi

  log "Downloading sample COCO images"
  local id dest
  for id in 000000000139 000000000285 000000000632 000000000724 000000000785; do
    dest="$PROJECT_ROOT/datasets/sample_images/${id}.jpg"
    if [ ! -f "$dest" ]; then
      as_target_user curl -fL "http://images.cocodataset.org/val2017/${id}.jpg" -o "$dest"
    fi
  done
}

ensure_executorch_checkout() {
  if [ -d "$EXECUTORCH_ROOT/.git" ]; then
    log "ExecuTorch checkout already exists at $EXECUTORCH_ROOT"
    return
  fi

  if [ -e "$EXECUTORCH_ROOT" ] && [ -n "$(find "$EXECUTORCH_ROOT" -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null)" ]; then
    die "$EXECUTORCH_ROOT exists but is not an ExecuTorch git checkout"
  fi

  log "Cloning ExecuTorch into $EXECUTORCH_ROOT"
  sudo_run install -d -o "$TARGET_USER" -g "$TARGET_GROUP" -m 0755 "$EXECUTORCH_ROOT"
  as_target_user git clone --recursive "$EXECUTORCH_REPO" "$EXECUTORCH_ROOT"
  if [ -n "$EXECUTORCH_REF" ]; then
    as_target_user bash -lc "cd $(q "$EXECUTORCH_ROOT") && git checkout $(q "$EXECUTORCH_REF") && git submodule update --init --recursive"
  fi
}

patch_executorch_for_ventuno() {
  if [ ! -d "$EXECUTORCH_ROOT/.git" ]; then
    warn "ExecuTorch checkout not found at $EXECUTORCH_ROOT"
    return 1
  fi

  if grep -q "QCS8300" "$EXECUTORCH_ROOT/backends/qualcomm/serialization/qc_schema.py" 2>/dev/null &&
     grep -q '"QCS8300"' "$EXECUTORCH_ROOT/backends/qualcomm/utils/utils.py" 2>/dev/null; then
    log "ExecuTorch Ventuno/QCS8300 patch already appears to be applied"
    return 0
  fi

  log "Applying ExecuTorch QCS8300 compatibility patch"
  as_target_user bash -lc "cd $(q "$EXECUTORCH_ROOT") && git apply" <<'PATCH'
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
}


build_executorch() {
  detect_qnn_sdk
  [ -d "$EXECUTORCH_ROOT/.git" ] || die "ExecuTorch checkout missing at $EXECUTORCH_ROOT"
  qnn_sdk_looks_installed || die "QNN SDK missing or incomplete at $QNN_SDK_ROOT. Rerun with --qnn-sdk-zip PATH if the Qualcomm download requires login."

  patch_executorch_for_ventuno

  log "Building ExecuTorch with Qualcomm QNN backend"
  as_target_user bash -lc "cd $(q "$EXECUTORCH_ROOT") && git submodule update --init --recursive"
  as_target_user python3 -m venv "$EXECUTORCH_VENV"
  as_target_user bash -lc "cd $(q "$EXECUTORCH_ROOT") && source $(q "$EXECUTORCH_VENV/bin/activate") && python -m pip install --upgrade pip setuptools wheel && if [ -x ./install_requirements.sh ]; then ./install_requirements.sh; elif [ -f requirements.txt ]; then python -m pip install -r requirements.txt; fi && if [ -f backends/qualcomm/requirements.txt ]; then python -m pip install -r backends/qualcomm/requirements.txt; fi"

  as_target_user bash -lc "cd $(q "$EXECUTORCH_ROOT") && source $(q "$EXECUTORCH_VENV/bin/activate") && cmake -S . -B build-x86 \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=$(q "$EXECUTORCH_ROOT/build-x86") \
    -DQNN_SDK_ROOT=$(q "$QNN_SDK_ROOT") \
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
    -DPYTHON_EXECUTABLE=$(q "$EXECUTORCH_VENV/bin/python")"

  as_target_user bash -lc "cd $(q "$EXECUTORCH_ROOT") && source $(q "$EXECUTORCH_VENV/bin/activate") && cmake --build build-x86 --target install -j\"\$(nproc)\""
  as_target_user bash -lc "cd $(q "$EXECUTORCH_ROOT") && mkdir -p backends/qualcomm/python exir/_serialize kernels/quantized && if compgen -G 'build-x86/backends/qualcomm/Py*' >/dev/null; then cp -fv build-x86/backends/qualcomm/Py* backends/qualcomm/python/; else echo 'WARN: no ExecuTorch Qualcomm Python artifacts found under build-x86/backends/qualcomm/Py*; continuing because they are only needed for Python export' >&2; fi && cp -fv schema/program.fbs exir/_serialize/program.fbs && cp -fv schema/scalar_type.fbs exir/_serialize/scalar_type.fbs && if [ -f build-x86/kernels/quantized/libquantized_ops_aot_lib.so ]; then cp -fv build-x86/kernels/quantized/libquantized_ops_aot_lib.so kernels/quantized/; fi"

  test -f "$EXECUTORCH_ROOT/build-x86/lib/cmake/ExecuTorch/executorch-config.cmake"
  test -f "$EXECUTORCH_ROOT/build-x86/lib/cmake/ExecuTorch/ExecuTorchTargets.cmake"
  test -f "$EXECUTORCH_ROOT/build-x86/lib/executorch/backends/qualcomm/libqnn_executorch_backend.so"
}

build_workspace() {
  if [ "$SKIP_WORKSPACE_BUILD" -eq 1 ]; then
    return
  fi

  local et_cmake="$EXECUTORCH_ROOT/build-x86/lib/cmake/ExecuTorch"
  if [ ! -f "$et_cmake/executorch-config.cmake" ]; then
    warn "ExecuTorch CMake package not found at $et_cmake; skipping colcon build"
    return
  fi

  log "Building ROS workspace"
  as_target_user bash -lc "cd $(q "$PROJECT_ROOT") && source $(q "/opt/ros/$ROS_DISTRO/setup.bash") && colcon build --symlink-install --cmake-args -Dexecutorch_DIR=$(q "$et_cmake") -DBUILD_QNN_BACKEND=ON -DBUILD_XNNPACK_BACKEND=OFF"
}

main() {
  check_os
  configure_locale
  configure_ros_apt
  install_apt_dependencies
  ensure_qnn_sdk
  write_board_environment
  install_dds_tuning
  install_create3_service
  setup_rosdep
  setup_project_python
  setup_assets
  ensure_executorch_checkout
  build_executorch
  build_workspace

  log "Install script finished"
  cat <<EOF

Open a new shell, or run:
  source /opt/ros/$ROS_DISTRO/setup.bash
  source "$TARGET_HOME/.ventuno_object_tracking_env"

Project root:
  $PROJECT_ROOT

ExecuTorch root:
  $EXECUTORCH_ROOT
EOF
}

main "$@"
