#include "yolox_detector/qnn_backend.hpp"
#include <executorch/extension/module/module.h>
#include <executorch/extension/tensor/tensor.h>
#include <executorch/runtime/core/evalue.h>
#include <executorch/runtime/core/error.h>
#include <opencv2/imgproc.hpp>
#include <cstring>
#include <stdexcept>
#include <dlfcn.h>  // dlopen / RTLD_NOW — needed to pre-load QNN libs

using executorch::extension::Module;
using executorch::extension::from_blob;
using executorch::aten::SizesType;
using executorch::runtime::EValue;
using executorch::runtime::Error;

namespace yolox_detector {

namespace {
// Pre-load the QNN shared libraries so ExecuTorch can find them.
void preload_qnn_libs(const std::string & lib_dir)
{
  const char * libs[] = {
    "libQnnSystem.so",
    "libQnnHtp.so",
    "libQnnHtpPrepare.so",
    nullptr
  };
  for (int i = 0; libs[i]; ++i) {
    std::string path = lib_dir.empty() ? libs[i] : (lib_dir + "/" + libs[i]);
    // RTLD_GLOBAL makes symbols visible to subsequently loaded libraries.
    if (!dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL)) {
      throw std::runtime_error(std::string("Failed to load QNN lib: ") + dlerror());
    }
  }
}
}  // namespace

QnnBackend::QnnBackend(int width, int height, const std::string & qnn_lib_dir)
: width_(width), height_(height), qnn_lib_dir_(qnn_lib_dir) {}

bool QnnBackend::load(const std::string & model_path)
{
  preload_qnn_libs(qnn_lib_dir_);
  // MmapUseMlock keeps the model file in memory, which helps warm latency.
  module_ = std::make_unique<Module>(model_path, Module::LoadMode::MmapUseMlock);
  return module_->load() == Error::Ok;
}

std::vector<float> QnnBackend::infer(const cv::Mat & input_f32)
{
  std::vector<cv::Mat> channels(3);
  cv::split(input_f32, channels);

  size_t plane = width_ * height_;
  std::vector<float> chw_buf(3 * plane);
  for (int c = 0; c < 3; ++c) {
    std::memcpy(chw_buf.data() + c * plane, channels[c].ptr<float>(),
                plane * sizeof(float));
  }

  std::vector<SizesType> shape = {1, 3, height_, width_};
  auto tensor = from_blob(chw_buf.data(), shape);

  auto result = module_->execute("forward", {EValue(tensor)});
  if (!result.ok()) {
    throw std::runtime_error("ExecuTorch QNN inference failed");
  }

  auto & out_tensor = result->at(0).toTensor();
  const float * data = out_tensor.const_data_ptr<float>();
  return std::vector<float>(data, data + out_tensor.numel());
}

}  // namespace yolox_detector
