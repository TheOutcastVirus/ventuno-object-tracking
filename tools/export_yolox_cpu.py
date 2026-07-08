"""Export YOLOX-tiny to ExecuTorch .pte format with XNNPACK backend (CPU).

Run this on a dev machine (not the robot):
  pip install torch torchvision yolox
  pip install "executorch[xnnpack]"  # from pytorch/executorch

Output: models/yolox_tiny_xnnpack.pte
"""

import pathlib
import torch
from yolox.models import YOLOX, YOLOPAFPN, YOLOXHead
from executorch.backends.xnnpack.partition.xnnpack_partitioner import XnnpackPartitioner
from executorch.exir import to_edge_transform_and_lower, EdgeCompileConfig


def build_model(num_classes: int = 80) -> torch.nn.Module:
    depth, width = 0.33, 0.375  # YOLOX-tiny
    backbone = YOLOPAFPN(depth, width, in_channels=[256, 512, 1024])
    head = YOLOXHead(num_classes, width, in_channels=[256, 512, 1024])
    model = YOLOX(backbone, head)
    model.eval()
    return model


def main() -> None:
    input_h, input_w = 416, 416
    num_classes = 80

    print("Building model ...")
    model = build_model(num_classes)

    ckpt = torch.load("models/yolox_tiny.pth", map_location="cpu")
    model.load_state_dict(ckpt["model"] if "model" in ckpt else ckpt)

    example_input = (torch.zeros(1, 3, input_h, input_w),)

    print("Exporting to ExecuTorch IR ...")
    ep = torch.export.export(model, example_input)

    print("Lowering with XNNPACK partitioner ...")
    et_program = to_edge_transform_and_lower(
        ep,
        compile_config=EdgeCompileConfig(_check_ir_validity=False),
        partitioner=[XnnpackPartitioner()],
    ).to_executorch()

    out_path = pathlib.Path("models/yolox_tiny_xnnpack.pte")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(et_program.buffer)

    print(f"Saved: {out_path}  ({out_path.stat().st_size / 1024:.1f} KB)")
    print(f"Model input: {input_w}x{input_h}, classes: {num_classes}")


if __name__ == "__main__":
    main()
