
#include "mos/vis/vad/vad_engine.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "mos/vis/common/logging.h"

#ifdef MOS_VIS_HAS_RKNNRT
#include <rknn_api.h>
#endif

namespace mos::vis {
namespace {

#ifdef MOS_VIS_HAS_RKNNRT

class RknnVadEngine final : public VadEngine {
 public:
  RknnVadEngine() = default;
  ~RknnVadEngine() override {
    if (ctx_ != 0) {
      rknn_destroy(ctx_);
      ctx_ = 0;
    }
  }

  Status Initialize(const VadConfig& config) override {
    config_ = config;

    std::ifstream ifs(config.model_path, std::ios::binary | std::ios::ate);
    if (!ifs) {
      return Status::NotFound("failed to open vad model: " + config.model_path);
    }

    const std::streamsize size = ifs.tellg();
    if (size <= 0) {
      return Status::Internal("vad model file is empty: " + config.model_path);
    }
    ifs.seekg(0, std::ios::beg);

    model_data_.resize(static_cast<std::size_t>(size));
    if (!ifs.read(reinterpret_cast<char*>(model_data_.data()), size)) {
      return Status::Internal("failed to read vad model: " + config.model_path);
    }

    int ret = rknn_init(&ctx_,
                        model_data_.data(),
                        static_cast<uint32_t>(model_data_.size()),
                        0,
                        nullptr);
    if (ret != RKNN_SUCC) {
      return Status::Internal("rknn_init failed for vad model, ret=" + std::to_string(ret));
    }

    ResetState();
    rknn_tensor_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.index = 0;
    rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, &attr, sizeof(attr));

    GetLogger()->info("VAD input attr: type={} qnt_type={} scale={} zp={}",
                      attr.type,
                      attr.qnt_type,
                      attr.scale,
                      attr.zp);
    GetLogger()->info("VAD initialized with RKNN model: {}", config.model_path);
    return Status::Ok();
  }

  Status Reset() override {
    ResetState();
    return Status::Ok();
  }

  Status Process(const float* samples, std::size_t count, VadResult* result) override {
    if (result == nullptr) {
      return Status::InvalidArgument("vad result is null");
    }
    if (samples == nullptr) {
      return Status::InvalidArgument("vad input samples is null");
    }
    if (count != 512) {
      return Status::InvalidArgument("vad expects 512 samples, got " + std::to_string(count));
    }
    if (ctx_ == 0) {
      return Status::Internal("vad rknn context not initialized");
    }

    input_samples_fp16_.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
      input_samples_fp16_[i] = Fp32ToFp16(samples[i]);
    }
    for (std::size_t i = 0; i < h_.size(); ++i) {
      h_fp16_[i] = Fp32ToFp16(h_[i]);
      c_fp16_[i] = Fp32ToFp16(c_[i]);
    }

    rknn_input inputs[3];
    std::memset(inputs, 0, sizeof(inputs));

    inputs[0].index = 0;
    inputs[0].buf = input_samples_fp16_.data();
    inputs[0].size = static_cast<uint32_t>(input_samples_fp16_.size() * sizeof(uint16_t));
    inputs[0].pass_through = 0;
    inputs[0].type = RKNN_TENSOR_FLOAT16;
    inputs[0].fmt = RKNN_TENSOR_UNDEFINED;

    inputs[1].index = 1;
    inputs[1].buf = h_fp16_.data();
    inputs[1].size = static_cast<uint32_t>(h_fp16_.size() * sizeof(uint16_t));
    inputs[1].pass_through = 0;
    inputs[1].type = RKNN_TENSOR_FLOAT16;
    inputs[1].fmt = RKNN_TENSOR_UNDEFINED;

    inputs[2].index = 2;
    inputs[2].buf = c_fp16_.data();
    inputs[2].size = static_cast<uint32_t>(c_fp16_.size() * sizeof(uint16_t));
    inputs[2].pass_through = 0;
    inputs[2].type = RKNN_TENSOR_FLOAT16;
    inputs[2].fmt = RKNN_TENSOR_UNDEFINED;

    int ret = rknn_inputs_set(ctx_, 3, inputs);
    if (ret != RKNN_SUCC) {
      return Status::Internal("rknn_inputs_set failed for vad, ret=" + std::to_string(ret));
    }

    ret = rknn_run(ctx_, nullptr);
    if (ret != RKNN_SUCC) {
      return Status::Internal("rknn_run failed for vad, ret=" + std::to_string(ret));
    }

    std::vector<uint16_t> prob_fp16(1, 0);
    std::vector<uint16_t> next_h_fp16(128, 0);
    std::vector<uint16_t> next_c_fp16(128, 0);

    rknn_output outputs[3];
    std::memset(outputs, 0, sizeof(outputs));

    outputs[0].want_float = 0;
    outputs[0].is_prealloc = 1;
    outputs[0].index = 0;
    outputs[0].buf = prob_fp16.data();
    outputs[0].size = static_cast<uint32_t>(prob_fp16.size() * sizeof(uint16_t));

    outputs[1].want_float = 0;
    outputs[1].is_prealloc = 1;
    outputs[1].index = 1;
    outputs[1].buf = next_h_fp16.data();
    outputs[1].size = static_cast<uint32_t>(next_h_fp16.size() * sizeof(uint16_t));

    outputs[2].want_float = 0;
    outputs[2].is_prealloc = 1;
    outputs[2].index = 2;
    outputs[2].buf = next_c_fp16.data();
    outputs[2].size = static_cast<uint32_t>(next_c_fp16.size() * sizeof(uint16_t));

    ret = rknn_outputs_get(ctx_, 3, outputs, nullptr);
    if (ret != RKNN_SUCC) {
      return Status::Internal("rknn_outputs_get failed for vad, ret=" + std::to_string(ret));
    }

    result->probability = Fp16ToFp32(prob_fp16[0]);
    result->speech = (result->probability >= config_.threshold);

    for (std::size_t i = 0; i < h_.size(); ++i) {
      h_[i] = Fp16ToFp32(next_h_fp16[i]);
      c_[i] = Fp16ToFp32(next_c_fp16[i]);
    }

    ret = rknn_outputs_release(ctx_, 3, outputs);
    if (ret != RKNN_SUCC) {
      return Status::Internal("rknn_outputs_release failed for vad, ret=" + std::to_string(ret));
    }

    return Status::Ok();
  }

 private:
  static uint16_t Fp32ToFp16(float f) {
    uint32_t x = 0;
    std::memcpy(&x, &f, sizeof(x));

    const uint32_t sign = (x >> 16U) & 0x8000U;
    uint32_t mantissa = x & 0x007FFFFFU;
    int32_t exp = static_cast<int32_t>((x >> 23U) & 0xFFU) - 127 + 15;

    if (exp <= 0) {
      if (exp < -10) {
        return static_cast<uint16_t>(sign);
      }
      mantissa |= 0x00800000U;
      const uint32_t shift = static_cast<uint32_t>(14 - exp);
      uint32_t half_mant = mantissa >> shift;
      const uint32_t round_bit = (mantissa >> (shift - 1U)) & 1U;
      half_mant += round_bit;
      return static_cast<uint16_t>(sign | half_mant);
    }

    if (exp >= 31) {
      if (mantissa == 0U) {
        return static_cast<uint16_t>(sign | 0x7C00U);
      }
      return static_cast<uint16_t>(sign | 0x7E00U);
    }

    uint16_t half_exp = static_cast<uint16_t>(static_cast<uint16_t>(exp) << 10U);
    uint16_t half_mant = static_cast<uint16_t>(mantissa >> 13U);
    if ((mantissa & 0x00001000U) != 0U) {
      ++half_mant;
      if (half_mant == 0x0400U) {
        half_mant = 0;
        half_exp = static_cast<uint16_t>(half_exp + 0x0400U);
        if (half_exp >= 0x7C00U) {
          return static_cast<uint16_t>(sign | 0x7C00U);
        }
      }
    }
    return static_cast<uint16_t>(sign | half_exp | half_mant);
  }

  static float Fp16ToFp32(uint16_t h) {
    const uint32_t sign = (static_cast<uint32_t>(h & 0x8000U)) << 16U;
    uint32_t exp = (h & 0x7C00U) >> 10U;
    uint32_t mant = h & 0x03FFU;

    uint32_t f = 0;
    if (exp == 0) {
      if (mant == 0) {
        f = sign;
      } else {
        exp = 1;
        while ((mant & 0x0400U) == 0U) {
          mant <<= 1U;
          --exp;
        }
        mant &= 0x03FFU;
        exp = exp + (127U - 15U);
        f = sign | (exp << 23U) | (mant << 13U);
      }
    } else if (exp == 0x1FU) {
      f = sign | 0x7F800000U | (mant << 13U);
    } else {
      exp = exp + (127U - 15U);
      f = sign | (exp << 23U) | (mant << 13U);
    }

    float out = 0.0F;
    std::memcpy(&out, &f, sizeof(out));
    return out;
  }

  void ResetState() {
    std::fill(h_.begin(), h_.end(), 0.0F);
    std::fill(c_.begin(), c_.end(), 0.0F);
  }

  VadConfig config_;
  rknn_context ctx_ = 0;
  std::vector<unsigned char> model_data_;
  std::vector<uint16_t> input_samples_fp16_;
  std::vector<uint16_t> h_fp16_ = std::vector<uint16_t>(128, 0);
  std::vector<uint16_t> c_fp16_ = std::vector<uint16_t>(128, 0);
  std::vector<float> h_ = std::vector<float>(128, 0.0F);
  std::vector<float> c_ = std::vector<float>(128, 0.0F);
};

#else

class RknnVadEngine final : public VadEngine {
 public:
  Status Initialize(const VadConfig&) override {
    return Status::Internal("RKNN runtime not enabled");
  }
  Status Reset() override { return Status::Ok(); }
  Status Process(const float*, std::size_t, VadResult*) override {
    return Status::Internal("RKNN runtime not enabled");
  }
};

#endif

}  // namespace

std::unique_ptr<VadEngine> CreateVadEngine() {
  return std::make_unique<RknnVadEngine>();
}

}  // namespace mos::vis
