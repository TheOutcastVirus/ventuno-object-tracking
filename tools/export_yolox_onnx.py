#!/usr/bin/env python3
"""Export YOLOX-Tiny to ONNX.

Self-contained — no yolox package, no git installs, no onnxsim.
Only requires: pip install torch torchvision

Usage:
    python3 tools/export_yolox_onnx.py --weights yolox_tiny.pth --output models/yolox_tiny.onnx
"""
import argparse
import os

import torch
import torch.nn as nn


# ---------------------------------------------------------------------------
# YOLOX-Tiny architecture (inlined from https://github.com/Megvii-BaseDetection/YOLOX,
# Apache-2.0 license — only the pieces needed for inference/export)
# ---------------------------------------------------------------------------

class SiLU(nn.Module):
    def forward(self, x):
        return x * torch.sigmoid(x)


class BaseConv(nn.Module):
    def __init__(self, in_c, out_c, k, s, groups=1, bias=False):
        super().__init__()
        pad = (k - 1) // 2
        self.conv = nn.Conv2d(in_c, out_c, k, s, pad, groups=groups, bias=bias)
        self.bn = nn.BatchNorm2d(out_c, eps=1e-3, momentum=0.03)
        self.act = SiLU()

    def forward(self, x):
        return self.act(self.bn(self.conv(x)))


class DWConv(nn.Module):
    def __init__(self, in_c, out_c, k, s=1):
        super().__init__()
        self.dconv = BaseConv(in_c, in_c, k, s, groups=in_c)
        self.pconv = BaseConv(in_c, out_c, 1, 1)

    def forward(self, x):
        return self.pconv(self.dconv(x))


class Bottleneck(nn.Module):
    def __init__(self, in_c, out_c, shortcut=True, expansion=0.5):
        super().__init__()
        hidden = int(out_c * expansion)
        self.conv1 = BaseConv(in_c, hidden, 1, 1)
        self.conv2 = BaseConv(hidden, out_c, 3, 1)
        self.use_add = shortcut and in_c == out_c

    def forward(self, x):
        y = self.conv2(self.conv1(x))
        return x + y if self.use_add else y


class CSPLayer(nn.Module):
    def __init__(self, in_c, out_c, n=1, shortcut=True, expansion=0.5):
        super().__init__()
        hidden = int(out_c * expansion)
        self.conv1 = BaseConv(in_c, hidden, 1, 1)
        self.conv2 = BaseConv(in_c, hidden, 1, 1)
        self.conv3 = BaseConv(2 * hidden, out_c, 1, 1)
        self.m = nn.Sequential(*[Bottleneck(hidden, hidden, shortcut, 1.0) for _ in range(n)])

    def forward(self, x):
        return self.conv3(torch.cat([self.m(self.conv1(x)), self.conv2(x)], dim=1))


class Focus(nn.Module):
    def __init__(self, in_c, out_c, k=1):
        super().__init__()
        self.conv = BaseConv(in_c * 4, out_c, k, 1)

    def forward(self, x):
        return self.conv(torch.cat([
            x[..., ::2, ::2], x[..., 1::2, ::2],
            x[..., ::2, 1::2], x[..., 1::2, 1::2],
        ], dim=1))


class SPPBottleneck(nn.Module):
    def __init__(self, in_c, out_c, kernel_sizes=(5, 9, 13)):
        super().__init__()
        hidden = in_c // 2
        self.conv1 = BaseConv(in_c, hidden, 1, 1)
        self.m = nn.ModuleList([nn.MaxPool2d(k, stride=1, padding=k // 2) for k in kernel_sizes])
        self.conv2 = BaseConv(hidden * (len(kernel_sizes) + 1), out_c, 1, 1)

    def forward(self, x):
        x = self.conv1(x)
        return self.conv2(torch.cat([x] + [m(x) for m in self.m], dim=1))


class CSPDarknet(nn.Module):
    def __init__(self, dep_mul, wid_mul):
        super().__init__()
        base_c = int(wid_mul * 64)
        base_d = max(round(dep_mul * 3), 1)
        self.stem = Focus(3, base_c, k=3)
        self.dark2 = nn.Sequential(
            BaseConv(base_c, base_c * 2, 3, 2),
            CSPLayer(base_c * 2, base_c * 2, n=base_d),
        )
        self.dark3 = nn.Sequential(
            BaseConv(base_c * 2, base_c * 4, 3, 2),
            CSPLayer(base_c * 4, base_c * 4, n=base_d * 3),
        )
        self.dark4 = nn.Sequential(
            BaseConv(base_c * 4, base_c * 8, 3, 2),
            CSPLayer(base_c * 8, base_c * 8, n=base_d * 3),
        )
        self.dark5 = nn.Sequential(
            BaseConv(base_c * 8, base_c * 16, 3, 2),
            SPPBottleneck(base_c * 16, base_c * 16),
            CSPLayer(base_c * 16, base_c * 16, n=base_d, shortcut=False),
        )

    def forward(self, x):
        x = self.stem(x)
        x = self.dark2(x)
        d3 = self.dark3(x)
        d4 = self.dark4(d3)
        d5 = self.dark5(d4)
        return d3, d4, d5


class YOLOPAFPN(nn.Module):
    def __init__(self, depth=1.0, width=1.0, in_channels=(256, 512, 1024)):
        super().__init__()
        self.backbone = CSPDarknet(depth, width)
        w = width
        d = depth
        self.upsample = nn.Upsample(scale_factor=2, mode="nearest")
        self.lateral_conv0 = BaseConv(int(in_channels[2] * w), int(in_channels[1] * w), 1, 1)
        self.C3_p4 = CSPLayer(int(2 * in_channels[1] * w), int(in_channels[1] * w), n=round(3 * d), shortcut=False)
        self.reduce_conv1 = BaseConv(int(in_channels[1] * w), int(in_channels[0] * w), 1, 1)
        self.C3_p3 = CSPLayer(int(2 * in_channels[0] * w), int(in_channels[0] * w), n=round(3 * d), shortcut=False)
        self.bu_conv2 = BaseConv(int(in_channels[0] * w), int(in_channels[0] * w), 3, 2)
        self.C3_n3 = CSPLayer(int(2 * in_channels[0] * w), int(in_channels[1] * w), n=round(3 * d), shortcut=False)
        self.bu_conv1 = BaseConv(int(in_channels[1] * w), int(in_channels[1] * w), 3, 2)
        self.C3_n4 = CSPLayer(int(2 * in_channels[1] * w), int(in_channels[2] * w), n=round(3 * d), shortcut=False)

    def forward(self, x):
        x2, x1, x0 = self.backbone(x)
        f0 = self.lateral_conv0(x0)
        f0_up = torch.cat([self.upsample(f0), x1], 1)
        f0_up = self.C3_p4(f0_up)
        f1 = self.reduce_conv1(f0_up)
        f1_up = torch.cat([self.upsample(f1), x2], 1)
        p2 = self.C3_p3(f1_up)
        n3 = torch.cat([self.bu_conv2(p2), f1], 1)
        p1 = self.C3_n3(n3)
        n4 = torch.cat([self.bu_conv1(p1), f0], 1)
        p0 = self.C3_n4(n4)
        return p2, p1, p0


class YOLOXHead(nn.Module):
    def __init__(self, num_classes=80, width=1.0, strides=(8, 16, 32), in_channels=(256, 512, 1024)):
        super().__init__()
        self.num_classes = num_classes
        self.strides = strides
        self.stems = nn.ModuleList()
        self.cls_convs = nn.ModuleList()
        self.reg_convs = nn.ModuleList()
        self.cls_preds = nn.ModuleList()
        self.reg_preds = nn.ModuleList()
        self.obj_preds = nn.ModuleList()
        for in_c in in_channels:
            self.stems.append(BaseConv(int(in_c * width), int(256 * width), 1, 1))
            self.cls_convs.append(nn.Sequential(
                BaseConv(int(256 * width), int(256 * width), 3, 1),
                BaseConv(int(256 * width), int(256 * width), 3, 1),
            ))
            self.reg_convs.append(nn.Sequential(
                BaseConv(int(256 * width), int(256 * width), 3, 1),
                BaseConv(int(256 * width), int(256 * width), 3, 1),
            ))
            self.cls_preds.append(nn.Conv2d(int(256 * width), num_classes, 1, 1, 0))
            self.reg_preds.append(nn.Conv2d(int(256 * width), 4, 1, 1, 0))
            self.obj_preds.append(nn.Conv2d(int(256 * width), 1, 1, 1, 0))

    def forward(self, inputs):
        outputs = []
        for i, x in enumerate(inputs):
            x = self.stems[i](x)
            cls_out = self.cls_preds[i](self.cls_convs[i](x))
            reg_out = self.reg_preds[i](self.reg_convs[i](x))
            obj_out = self.obj_preds[i](self.reg_convs[i](x))
            out = torch.cat([reg_out, obj_out, cls_out], 1)
            bs, _, h, w = out.shape
            outputs.append(out.view(bs, -1, h * w).permute(0, 2, 1))
        return torch.cat(outputs, dim=1)


class YOLOXTiny(nn.Module):
    def __init__(self, num_classes=80):
        super().__init__()
        self.backbone = YOLOPAFPN(depth=0.33, width=0.375)
        self.head = YOLOXHead(num_classes=num_classes, width=0.375)

    def forward(self, x):
        return self.head(self.backbone(x))


# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Export YOLOX-Tiny to ONNX (no yolox package needed)")
    parser.add_argument("--weights", default="yolox_tiny.pth")
    parser.add_argument("--output", default="models/yolox_tiny.onnx")
    parser.add_argument("--input-size", default=416, type=int)
    parser.add_argument("--opset", default=11, type=int)
    parser.add_argument("--num-classes", default=80, type=int)
    args = parser.parse_args()

    out_dir = os.path.dirname(args.output)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    model = YOLOXTiny(num_classes=args.num_classes)
    ckpt = torch.load(args.weights, map_location="cpu")
    state = ckpt.get("model", ckpt)
    model.load_state_dict(state)
    model.eval()

    dummy = torch.zeros(1, 3, args.input_size, args.input_size)

    # Use the legacy exporter explicitly — the new dynamo-based exporter in
    # torch >= 2.x has additional deps (onnxscript) and changed behaviour.
    torch.onnx.export(
        model,
        dummy,
        args.output,
        input_names=["images"],
        output_names=["output"],
        opset_version=args.opset,
        dynamic_axes={"images": {0: "batch_size"}},
        dynamo=False,
    )

    size_mb = os.path.getsize(args.output) / 1_048_576
    print(f"Saved: {args.output}  ({size_mb:.1f} MB)")
    print(f"Input shape : [1, 3, {args.input_size}, {args.input_size}]")
    print(f"Output shape: [1, num_preds, {4 + 1 + args.num_classes}]  (reg4 + obj1 + cls{args.num_classes})")


if __name__ == "__main__":
    main()
