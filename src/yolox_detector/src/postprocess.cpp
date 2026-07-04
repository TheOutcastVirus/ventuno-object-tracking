#include "yolox_detector/postprocess.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <opencv2/imgproc.hpp>

namespace yolox_detector {

cv::Mat letterbox(const cv::Mat & src, int out_w, int out_h,
                  float & scale, float & pad_left, float & pad_top)
{
  float scale_x = static_cast<float>(out_w) / src.cols;
  float scale_y = static_cast<float>(out_h) / src.rows;
  scale = std::min(scale_x, scale_y);

  int new_w = static_cast<int>(std::round(src.cols * scale));
  int new_h = static_cast<int>(std::round(src.rows * scale));

  pad_left = (out_w - new_w) * 0.5f;
  pad_top  = (out_h - new_h) * 0.5f;

  cv::Mat resized;
  cv::resize(src, resized, {new_w, new_h}, 0, 0, cv::INTER_LINEAR);
  resized.convertTo(resized, CV_32FC3);

  cv::Mat dst(out_h, out_w, CV_32FC3, cv::Scalar(114.f, 114.f, 114.f));
  resized.copyTo(dst(cv::Rect(
    static_cast<int>(pad_left), static_cast<int>(pad_top), new_w, new_h)));

  return dst;
}

namespace {

float sigmoid(float x)
{
  return 1.f / (1.f + std::exp(-x));
}

float iou(const Detection & a, const Detection & b)
{
  float ix1 = std::max(a.x1, b.x1);
  float iy1 = std::max(a.y1, b.y1);
  float ix2 = std::min(a.x2, b.x2);
  float iy2 = std::min(a.y2, b.y2);

  float inter = std::max(0.f, ix2 - ix1) * std::max(0.f, iy2 - iy1);
  float area_a = (a.x2 - a.x1) * (a.y2 - a.y1);
  float area_b = (b.x2 - b.x1) * (b.y2 - b.y1);
  float uni = area_a + area_b - inter;
  return uni > 0.f ? inter / uni : 0.f;
}

// YOLOX anchor-free decode for a single stride level.
// grid_y, grid_x : top-left cell index; stride : feature map stride (8/16/32)
// row : pointer to the start of the 5+num_classes values for this anchor
void decode_anchor(const float * row, int grid_x, int grid_y, int stride,
                   int num_classes, float score_thr,
                   float scale, float pad_left, float pad_top,
                   std::vector<Detection> & out)
{
  float obj = sigmoid(row[4]);
  // quick reject before computing class scores
  if (obj < score_thr) return;

  int best_cls = 0;
  float best_score = 0.f;
  for (int c = 0; c < num_classes; ++c) {
    float s = obj * sigmoid(row[5 + c]);
    if (s > best_score) { best_score = s; best_cls = c; }
  }
  if (best_score < score_thr) return;

  // YOLOX decodes cx, cy relative to the grid cell; wh are predicted directly
  float cx = (row[0] + grid_x) * stride;
  float cy = (row[1] + grid_y) * stride;
  float w  = std::exp(row[2]) * stride;
  float h  = std::exp(row[3]) * stride;

  // Map back to original image coordinates
  float x1 = (cx - w * 0.5f - pad_left) / scale;
  float y1 = (cy - h * 0.5f - pad_top)  / scale;
  float x2 = (cx + w * 0.5f - pad_left) / scale;
  float y2 = (cy + h * 0.5f - pad_top)  / scale;

  out.push_back({x1, y1, x2, y2, best_score, best_cls});
}

std::vector<Detection> nms(std::vector<Detection> dets, float nms_thr)
{
  std::sort(dets.begin(), dets.end(),
            [](const Detection & a, const Detection & b) { return a.score > b.score; });

  std::vector<bool> suppressed(dets.size(), false);
  std::vector<Detection> result;

  for (size_t i = 0; i < dets.size(); ++i) {
    if (suppressed[i]) continue;
    result.push_back(dets[i]);
    for (size_t j = i + 1; j < dets.size(); ++j) {
      if (!suppressed[j] && iou(dets[i], dets[j]) > nms_thr) {
        suppressed[j] = true;
      }
    }
  }
  return result;
}

}  // namespace

std::vector<Detection> decode_and_nms(
  const std::vector<float> & raw_output,
  int num_anchors, int num_classes,
  int in_w, int in_h,
  float scale, float pad_left, float pad_top,
  float score_thr, float nms_thr)
{
  int row_stride = 5 + num_classes;
  // YOLOX strides: 8, 16, 32 — derive grid sizes from model input dimensions
  const int strides[] = {8, 16, 32};
  std::vector<Detection> dets;
  dets.reserve(256);

  int anchor_idx = 0;
  for (int s : strides) {
    int gw = in_w / s;
    int gh = in_h / s;
    for (int gy = 0; gy < gh; ++gy) {
      for (int gx = 0; gx < gw; ++gx, ++anchor_idx) {
        if (anchor_idx >= num_anchors) break;
        const float * row = raw_output.data() + anchor_idx * row_stride;
        decode_anchor(row, gx, gy, s, num_classes, score_thr,
                      scale, pad_left, pad_top, dets);
      }
    }
  }

  return nms(std::move(dets), nms_thr);
}

}  // namespace yolox_detector
