#!/usr/bin/env python3
"""
Download YOLOX-Tiny weights from the official GitHub release and export to ONNX.

This is a one-time setup step. After this runs, the detector only needs onnxruntime.

Usage:
    python3 scripts/download_models.py               # downloads + exports to models/
    python3 scripts/download_models.py --skip-export # download .pth only, no ONNX export
    python3 scripts/download_models.py --input-size 640
"""
import argparse
import hashlib
import os
import urllib.request

# Official YOLOX GitHub release asset
YOLOX_TINY_URL = (
    "https://github.com/Megvii-BaseDetection/YOLOX/releases/download/"
    "0.1.1rc0/yolox_tiny.pth"
)
YOLOX_TINY_MD5 = "b6dca9f6c9e18fc65f8f16af94748c16"

MODELS_DIR = os.path.join(os.path.dirname(__file__), "..", "models")


def _md5(path: str) -> str:
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def _download(url: str, dest: str) -> None:
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    print(f"Downloading {os.path.basename(dest)} ...")

    def _progress(block_num, block_size, total_size):
        downloaded = block_num * block_size
        if total_size > 0:
            pct = min(100, downloaded * 100 // total_size)
            mb = downloaded / 1_048_576
            total_mb = total_size / 1_048_576
            print(f"\r  {pct:3d}%  {mb:.1f} / {total_mb:.1f} MB", end="", flush=True)

    urllib.request.urlretrieve(url, dest, reporthook=_progress)
    print()  # newline after progress


def download_pth(dest: str) -> None:
    if os.path.exists(dest):
        if _md5(dest) == YOLOX_TINY_MD5:
            print(f"Already exists (checksum OK): {dest}")
            return
        print(f"Checksum mismatch — re-downloading {dest}")
        os.remove(dest)
    _download(YOLOX_TINY_URL, dest)
    actual = _md5(dest)
    if actual != YOLOX_TINY_MD5:
        raise RuntimeError(
            f"Checksum mismatch after download.\n  expected: {YOLOX_TINY_MD5}\n  got:      {actual}"
        )
    print(f"Checksum OK: {dest}")


def export_onnx(pth_path: str, onnx_path: str, input_size: int, opset: int) -> None:
    try:
        import torch
    except ImportError:
        raise SystemExit(
            "Export requires torch:\n"
            "  pip install torch torchvision\n"
            "(onnxruntime is enough for inference — torch is only needed here)"
        )

    # Import the self-contained model definition from tools/export_yolox_onnx.py
    import importlib.util
    export_script = os.path.join(os.path.dirname(__file__), "..", "tools", "export_yolox_onnx.py")
    spec = importlib.util.spec_from_file_location("export_yolox_onnx", export_script)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    YOLOXTiny = mod.YOLOXTiny

    print(f"Exporting {pth_path} → {onnx_path} (input {input_size}×{input_size}) ...")

    model = YOLOXTiny()
    ckpt = torch.load(pth_path, map_location="cpu")
    model.load_state_dict(ckpt.get("model", ckpt))
    model.eval()

    dummy = torch.zeros(1, 3, input_size, input_size)
    os.makedirs(os.path.dirname(onnx_path) or ".", exist_ok=True)

    torch.onnx.export(
        model,
        dummy,
        onnx_path,
        input_names=["images"],
        output_names=["output"],
        opset_version=opset,
        dynamic_axes={"images": {0: "batch_size"}},
    )
    size_mb = os.path.getsize(onnx_path) / 1_048_576
    print(f"Saved: {onnx_path}  ({size_mb:.1f} MB)")


def main():
    parser = argparse.ArgumentParser(description="Download and export YOLOX-Tiny")
    parser.add_argument(
        "--models-dir", default=MODELS_DIR, help="Directory to save model files"
    )
    parser.add_argument(
        "--input-size", default=416, type=int,
        help="Input resolution for ONNX export (default: 416)"
    )
    parser.add_argument(
        "--opset", default=11, type=int, help="ONNX opset version (default: 11)"
    )
    parser.add_argument(
        "--skip-export", action="store_true",
        help="Only download the .pth; skip ONNX export"
    )
    args = parser.parse_args()

    models_dir = os.path.abspath(args.models_dir)
    pth_path = os.path.join(models_dir, "yolox_tiny.pth")
    onnx_path = os.path.join(models_dir, "yolox_tiny.onnx")

    download_pth(pth_path)

    if not args.skip_export:
        if os.path.exists(onnx_path):
            print(f"ONNX model already exists: {onnx_path}")
        else:
            export_onnx(pth_path, onnx_path, args.input_size, args.opset)

    print("\nDone.")
    print(f"  .pth : {pth_path}")
    if not args.skip_export:
        print(f"  .onnx: {onnx_path}")


if __name__ == "__main__":
    main()
