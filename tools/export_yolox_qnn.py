#!/usr/bin/env python3
"""Export YOLOX-Tiny to ExecuTorch .pte with the Qualcomm QNN HTP backend.

This script is intended to run on the Ventuno Q with the local ExecuTorch
checkout available on PYTHONPATH and QAIRT/QNN environment sourced.
"""
import argparse
import importlib.util
import os
import pathlib
import sys

import torch


def _load_yolox_tiny_class(repo_root: pathlib.Path):
    export_script = repo_root / "tools" / "export_yolox_onnx.py"
    spec = importlib.util.spec_from_file_location("export_yolox_onnx", export_script)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.YOLOXTiny


def _calibration_tensors(num_batches: int, input_size: int):
    for _ in range(num_batches):
        yield (torch.rand(1, 3, input_size, input_size) * 255.0,)


def main() -> None:
    parser = argparse.ArgumentParser(description="Export YOLOX-Tiny to QNN .pte")
    parser.add_argument("--weights", default="models/yolox_tiny.pth")
    parser.add_argument("--output", default="models/yolox_tiny_qnn.pte")
    parser.add_argument("--input-size", type=int, default=416)
    parser.add_argument("--num-classes", type=int, default=80)
    parser.add_argument("--soc-model", default="SM8550")
    parser.add_argument("--build-folder", default="/home/ubuntu/Documents/executorch/build-x86")
    parser.add_argument("--calibration-batches", type=int, default=8)
    args = parser.parse_args()

    if "QNN_SDK_ROOT" not in os.environ:
        raise EnvironmentError("QNN_SDK_ROOT is not set; source QAIRT envsetup.sh first")

    repo_root = pathlib.Path(__file__).resolve().parents[1]
    output_path = pathlib.Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    from executorch.backends.qualcomm.export_utils import (
        QnnConfig,
        build_executorch_binary,
        make_quantizer,
    )
    from executorch.backends.qualcomm.quantizer.quantizer import QuantDtype
    from executorch.backends.qualcomm.serialization.qc_schema import (
        QnnExecuTorchBackendType,
    )

    YOLOXTiny = _load_yolox_tiny_class(repo_root)
    model = YOLOXTiny(num_classes=args.num_classes)
    state = torch.load(args.weights, map_location="cpu")
    model.load_state_dict(state.get("model", state))
    model.eval()

    qnn_config = QnnConfig(
        soc_model=args.soc_model,
        build_folder=args.build_folder,
        backend="htp",
        target="aarch64-oe-linux",
        compile_only=False,
        enable_x86_64=True,
    )

    quantizer = make_quantizer(
        quant_dtype=QuantDtype.use_8a8w,
        backend=QnnExecuTorchBackendType.kHtpBackend,
        soc_model=args.soc_model,
    )
    calibration = list(_calibration_tensors(args.calibration_batches, args.input_size))

    # build_executorch_binary appends .pte to file_name.
    file_stem = str(output_path.with_suffix(""))
    build_executorch_binary(
        model=model,
        qnn_config=qnn_config,
        file_name=file_stem,
        dataset=calibration,
        custom_quantizer=quantizer,
    )
    print(f"Saved: {output_path}")


if __name__ == "__main__":
    sys.exit(main())
