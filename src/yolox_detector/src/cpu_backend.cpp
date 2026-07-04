#include "yolox_detector/cpu_backend.hpp"
#include <executorch/extension/module/module.h>
#include <executorch/extension/tensor/tensor.h>
#include <executorch/runtime/core/evalue.h>
#include <executorch/runtime/core/error.h>
#include <opencv2/imgproc.hpp>
#include <stdexcept>

using executorch::extension::Module;
using executorch::extension::TensorPtr;
using executorch::extension::from_blob;
using executorch::aten::SizesType;
using executorch::runtime::EValue;
using executorch::runtime::Error;

namespace yolox_detector {

CpuBackend::CpuBackend(int width, int height)
: width_(width), height_(height) {}

bool CpuBackend::load(const std::string & model_path)
{
  module_ = std::make_unique<Module>(model_path, Module::LoadMode::MmapUseMlock);
  return module_->load() == Error::Ok;
}

std::vector<float> CpuBackend::infer(const cv::Mat & input_f32)
{
  // input_f32 is a letterboxed float32 BGR image (height × width × 3).
  // ExecuTorch expects NCHW float32 with shape [1, 3, H, W].
  cv::Mat chw;
  // HWC → CHW by splitting then merging with the right layout
  std::vector<cv::Mat> channels(3);
  cv::split(input_f32, channels);

  // Build a contiguous CHW buffer
  size_t plane = width_ * height_;
  std::vector<float> chw_buf(3 * plane);
  for (int c = 0; c < 3; ++c) {
    std::memcpy(chw_buf.data() + c * plane, channels[c].ptr<float>(),
                plane * sizeof(float));
  }

  // Wrap as an ExecuTorch tensor [1, 3, H, W]
  std::vector<SizesType> shape = {1, 3, height_, width_};
  auto tensor = from_blob(chw_buf.data(), shape);

  auto result = module_->execute("forward", {EValue(tensor)});
  if (!result.ok()) {
    throw std::runtime_error("ExecuTorch XNNPACK inference failed");
  }

  auto & out_tensor = result->at(0).toTensor();
  const float * data = out_tensor.const_data_ptr<float>();
  return std::vector<float>(data, data + out_tensor.numel());
}

}  // namespace yolox_detector
