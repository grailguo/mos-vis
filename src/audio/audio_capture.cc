#include "mos/vis/audio/audio_capture.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include <portaudio.h>

#include "mos/vis/audio/audio_device_selector.h"
#include "mos/vis/common/logging.h"

namespace mos::vis {
namespace {

constexpr float kSilenceSample = 0.0F;
constexpr int kMaxAnalyzeChannels = 8;
constexpr double kRmsEmaAlpha = 0.2;
constexpr double kSwitchRatioThreshold = 1.2;
constexpr double kSwitchRmsDeltaThreshold = 5e-4;

std::string ToLowerCopy(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

AudioCapture::ChannelSelectMode ParseChannelSelectMode(const std::string& mode) {
  const std::string lowered = ToLowerCopy(mode);
  if (lowered == "auto_once") {
    return AudioCapture::ChannelSelectMode::kAutoOnce;
  }
  if (lowered == "fixed") {
    return AudioCapture::ChannelSelectMode::kFixed;
  }
  return AudioCapture::ChannelSelectMode::kAutoTrack;
}

const char* ChannelSelectModeName(AudioCapture::ChannelSelectMode mode) {
  switch (mode) {
    case AudioCapture::ChannelSelectMode::kAutoOnce:
      return "auto_once";
    case AudioCapture::ChannelSelectMode::kAutoTrack:
      return "auto_track";
    case AudioCapture::ChannelSelectMode::kFixed:
      return "fixed";
  }
  return "auto_track";
}

// ALSA backend probing can emit many "Unknown PCM" lines to stderr on systems
// with partial PCM alias definitions. Silence only the probe window.
class ScopedStderrSilencer {
 public:
  ScopedStderrSilencer() {
    saved_stderr_fd_ = ::dup(STDERR_FILENO);
    if (saved_stderr_fd_ < 0) {
      return;
    }

    null_fd_ = ::open("/dev/null", O_WRONLY);
    if (null_fd_ < 0) {
      ::close(saved_stderr_fd_);
      saved_stderr_fd_ = -1;
      return;
    }

    if (::dup2(null_fd_, STDERR_FILENO) != 0) {
      ::close(null_fd_);
      ::close(saved_stderr_fd_);
      null_fd_ = -1;
      saved_stderr_fd_ = -1;
      return;
    }

    active_ = true;
  }

  ScopedStderrSilencer(const ScopedStderrSilencer&) = delete;
  ScopedStderrSilencer& operator=(const ScopedStderrSilencer&) = delete;

  ~ScopedStderrSilencer() {
    if (active_) {
      ::fflush(stderr);
      ::dup2(saved_stderr_fd_, STDERR_FILENO);
    }
    if (null_fd_ >= 0) {
      ::close(null_fd_);
    }
    if (saved_stderr_fd_ >= 0) {
      ::close(saved_stderr_fd_);
    }
  }

 private:
  int saved_stderr_fd_ = -1;
  int null_fd_ = -1;
  bool active_ = false;
};

void PrintSelectedDevice(const char* tag, const AudioDeviceInfo& device) {
  GetLogger()->info("{}: [{}] {} | in={} | out={} | default_sr={}",
                    tag,
                    device.index,
                    device.name,
                    device.max_input_channels,
                    device.max_output_channels,
                    device.default_sample_rate);
}

int ClampInputChannels(int device_channels) {
  if (device_channels <= 0) {
    return 1;
  }
  return std::min(device_channels, kMaxAnalyzeChannels);
}

}  // namespace

AudioCapture::AudioCapture(const AudioConfig& config,
                           std::shared_ptr<AudioRingBuffer> ring)
    : config_(config), ring_(std::move(ring)) {
  callback_context_.ring = ring_.get();
  callback_context_.requested_output_channels = 1;
  callback_context_.device_input_channels = 1;
  callback_context_.sample_rate_hz = config_.sample_rate;
  callback_context_.frames_per_buffer = config_.capture_chunk_samples;
  callback_context_.channel_select_mode = ParseChannelSelectMode(config_.channel_select_mode);
  callback_context_.fixed_channel_index = config_.fixed_channel_index;
  callback_context_.track_switch_consecutive = std::max(1, config_.track_switch_consecutive);
}

AudioCapture::~AudioCapture() { Stop(); }

Status AudioCapture::InitializePortAudioIfNeeded() {
  if (pa_initialized_) {
    return Status::Ok();
  }

  ScopedStderrSilencer silence_alsa_probe_noise;
  const PaError err = Pa_Initialize();
  if (err != paNoError) {
    return Status::Internal(std::string("Pa_Initialize failed: ") + Pa_GetErrorText(err));
  }

  pa_initialized_ = true;
  return Status::Ok();
}

void AudioCapture::PrintResolvedDevices() const {
  ScopedStderrSilencer silence_alsa_probe_noise;
  const std::vector<AudioDeviceInfo> devices = EnumerateAudioDevices();
  PrintAudioDevices(devices);

  GetLogger()->info("Configured input_device='{}'", config_.input_device);
  if (!config_.input_device.empty()) {
    const auto input = FindBestInputDevice(devices, config_.input_device);
    if (input.has_value()) {
      PrintSelectedDevice("Matched input device", input.value());
    } else {
      GetLogger()->info("Matched input device: <not found by configured name>");
    }
  }

  GetLogger()->info("Configured output_device='{}'", config_.output_device);
  if (!config_.output_device.empty()) {
    const auto output = FindBestOutputDevice(devices, config_.output_device);
    if (output.has_value()) {
      PrintSelectedDevice("Matched output device", output.value());
    } else {
      GetLogger()->info("Matched output device: <not found by configured name>");
    }
  }

  const int default_input = Pa_GetDefaultInputDevice();
  if (default_input != paNoDevice) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(default_input);
    if (info != nullptr) {
      GetLogger()->info("Default input device: [{}] {}", default_input, info->name);
    }
  }

  const int default_output = Pa_GetDefaultOutputDevice();
  if (default_output != paNoDevice) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(default_output);
    if (info != nullptr) {
      GetLogger()->info("Default output device: [{}] {}", default_output, info->name);
    }
  }
}

int AudioCapture::ResolveInputDeviceIndex() const {
  ScopedStderrSilencer silence_alsa_probe_noise;
  const std::vector<AudioDeviceInfo> devices = EnumerateAudioDevices();
  if (!config_.input_device.empty()) {
    const auto matched = FindBestInputDevice(devices, config_.input_device);
    if (matched.has_value()) {
      return matched->index;
    }
    GetLogger()->warn(
        "input device '{}' not found by partial match. Falling back to default input device.",
        config_.input_device);
  }

  return Pa_GetDefaultInputDevice();
}

Status AudioCapture::OpenInputStream() {
  const int input_device_index = ResolveInputDeviceIndex();
  if (input_device_index == paNoDevice) {
    return Status::NotFound("no input audio device available");
  }

  const PaDeviceInfo* input_info = Pa_GetDeviceInfo(input_device_index);
  if (input_info == nullptr) {
    return Status::Internal("Pa_GetDeviceInfo returned null for input device");
  }
  if (input_info->maxInputChannels <= 0) {
    return Status::InvalidArgument("selected device has no input channels");
  }

  opened_input_device_index_ = input_device_index;
  opened_device_input_channels_ = ClampInputChannels(input_info->maxInputChannels);

  callback_context_.device_input_channels = opened_device_input_channels_;
  callback_context_.selected_channel.store(0);
  callback_context_.callbacks_since_switch.store(0);
  callback_context_.callback_count.store(0);
  callback_context_.auto_once_locked = false;
  callback_context_.pending_better_channel = -1;
  callback_context_.pending_better_count = 0;
  callback_context_.min_callbacks_between_switches =
      std::max(1, std::min(10, config_.sample_rate / std::max(config_.capture_chunk_samples * 5, 1)));
  callback_context_.rms_accumulator.assign(
      static_cast<std::size_t>(opened_device_input_channels_), 0.0);
  callback_context_.rms_ema.assign(static_cast<std::size_t>(opened_device_input_channels_), 0.0);
  callback_context_.peak_accumulator.assign(
      static_cast<std::size_t>(opened_device_input_channels_), 0.0F);
  callback_context_.mono_buffer.assign(
      static_cast<std::size_t>(config_.capture_chunk_samples), 0.0F);

  PaStreamParameters input_params;
  std::memset(&input_params, 0, sizeof(input_params));
  input_params.device = input_device_index;
  input_params.channelCount = opened_device_input_channels_;
  input_params.sampleFormat = paFloat32;
  input_params.suggestedLatency = input_info->defaultLowInputLatency;
  input_params.hostApiSpecificStreamInfo = nullptr;

  const PaError open_err =
      Pa_OpenStream(&stream_,
                    &input_params,
                    nullptr,
                    static_cast<double>(config_.sample_rate),
                    static_cast<unsigned long>(config_.capture_chunk_samples),
                    paNoFlag,
                    &AudioCapture::PaCallback,
                    &callback_context_);
  if (open_err != paNoError) {
    return Status::Internal(std::string("Pa_OpenStream failed: ") + Pa_GetErrorText(open_err));
  }

  GetLogger()->info(
      "Opened input stream on device: [{}] {} | sample_rate={} | requested_output_channels={} "
      "| device_input_channels={} | frames_per_buffer={} | channel_select_mode={} "
      "| fixed_channel_index={} | track_switch_consecutive={}",
      input_device_index,
      input_info->name,
      config_.sample_rate,
      config_.channels,
      opened_device_input_channels_,
      config_.capture_chunk_samples,
      ChannelSelectModeName(callback_context_.channel_select_mode),
      callback_context_.fixed_channel_index,
      callback_context_.track_switch_consecutive);

  return Status::Ok();
}

int AudioCapture::PaCallback(const void* input,
                             void* /*output*/,
                             unsigned long frame_count,
                             const PaStreamCallbackTimeInfo* /*time_info*/,
                             PaStreamCallbackFlags /*status_flags*/,
                             void* user_data) {
  auto* ctx = static_cast<CallbackContext*>(user_data);
  if (ctx == nullptr || ctx->ring == nullptr) {
    return paAbort;
  }

  const std::size_t frames = static_cast<std::size_t>(frame_count);
  const int channels = ctx->device_input_channels;
  auto& mono = ctx->mono_buffer;

  if (mono.size() < frames) {
    mono.resize(frames, kSilenceSample);
  }

  const auto* input_samples = static_cast<const float*>(input);
  if (input_samples == nullptr) {
    std::fill(mono.begin(), mono.begin() + static_cast<std::ptrdiff_t>(frames), kSilenceSample);
    ctx->ring->Write(mono.data(), frames);
    return paContinue;
  }

  // 每个回调统计所有通道的 RMS / Peak。
  std::vector<double> rms_values(static_cast<std::size_t>(channels), 0.0);
  std::vector<float> peak_values(static_cast<std::size_t>(channels), 0.0F);

  for (int ch = 0; ch < channels; ++ch) {
    double sum = 0.0;
    float peak = 0.0F;
    for (std::size_t i = 0; i < frames; ++i) {
      const float v = input_samples[i * static_cast<std::size_t>(channels) +
                                    static_cast<std::size_t>(ch)];
      sum += static_cast<double>(v) * static_cast<double>(v);
      peak = std::max(peak, std::abs(v));
    }
    rms_values[static_cast<std::size_t>(ch)] =
        std::sqrt(sum / static_cast<double>(std::max<std::size_t>(frames, 1)));
    peak_values[static_cast<std::size_t>(ch)] = peak;
  }

  // 用 EMA 平滑每个通道能量，并通过滞回条件降低通道抖动。
  int best_channel = 0;
  double best_rms = 0.0;
  for (int ch = 0; ch < channels; ++ch) {
    const std::size_t idx = static_cast<std::size_t>(ch);
    const double prev = ctx->rms_ema[idx];
    const double ema = (kRmsEmaAlpha * rms_values[idx]) + ((1.0 - kRmsEmaAlpha) * prev);
    ctx->rms_ema[idx] = ema;
    if (ch == 0 || ema > best_rms) {
      best_rms = ema;
      best_channel = ch;
    }
  }

  int selected_channel = ctx->selected_channel.load();
  if (selected_channel < 0 || selected_channel >= channels) {
    selected_channel = best_channel;
  }

  const double current_rms = ctx->rms_ema[static_cast<std::size_t>(selected_channel)];
  const double candidate_rms = ctx->rms_ema[static_cast<std::size_t>(best_channel)];
  const std::uint64_t callbacks_since_switch =
      ctx->callbacks_since_switch.fetch_add(1U) + 1U;
  const bool enough_gap =
      callbacks_since_switch >= static_cast<std::uint64_t>(ctx->min_callbacks_between_switches);
  const bool clearly_stronger =
      (candidate_rms > (current_rms * kSwitchRatioThreshold)) &&
      ((candidate_rms - current_rms) > kSwitchRmsDeltaThreshold);

  switch (ctx->channel_select_mode) {
    case ChannelSelectMode::kFixed: {
      const int clamped = std::max(0, std::min(ctx->fixed_channel_index, channels - 1));
      selected_channel = clamped;
      ctx->selected_channel.store(selected_channel);
      ctx->callbacks_since_switch.store(0);
      ctx->pending_better_channel = -1;
      ctx->pending_better_count = 0;
      break;
    }
    case ChannelSelectMode::kAutoOnce: {
      if (!ctx->auto_once_locked) {
        selected_channel = best_channel;
        ctx->selected_channel.store(selected_channel);
        ctx->callbacks_since_switch.store(0);
        ctx->auto_once_locked = true;
      } else {
        ctx->selected_channel.store(selected_channel);
      }
      ctx->pending_better_channel = -1;
      ctx->pending_better_count = 0;
      break;
    }
    case ChannelSelectMode::kAutoTrack: {
      if (best_channel == selected_channel || !clearly_stronger) {
        ctx->pending_better_channel = -1;
        ctx->pending_better_count = 0;
      } else if (ctx->pending_better_channel == best_channel) {
        ++ctx->pending_better_count;
      } else {
        ctx->pending_better_channel = best_channel;
        ctx->pending_better_count = 1;
      }

      const bool enough_consecutive = ctx->pending_better_count >= ctx->track_switch_consecutive;
      if (best_channel != selected_channel && enough_gap && enough_consecutive) {
        selected_channel = best_channel;
        ctx->selected_channel.store(selected_channel);
        ctx->callbacks_since_switch.store(0);
        ctx->pending_better_channel = -1;
        ctx->pending_better_count = 0;
      } else {
        ctx->selected_channel.store(selected_channel);
      }
      break;
    }
  }

  for (int ch = 0; ch < channels; ++ch) {
    ctx->rms_accumulator[static_cast<std::size_t>(ch)] += rms_values[static_cast<std::size_t>(ch)];
    ctx->peak_accumulator[static_cast<std::size_t>(ch)] = std::max(
        ctx->peak_accumulator[static_cast<std::size_t>(ch)],
        peak_values[static_cast<std::size_t>(ch)]);
  }

  for (std::size_t i = 0; i < frames; ++i) {
    mono[i] = input_samples[i * static_cast<std::size_t>(channels) +
                            static_cast<std::size_t>(selected_channel)];
  }

  ctx->ring->Write(mono.data(), frames);

  const std::uint64_t callback_count = ctx->callback_count.fetch_add(1) + 1U;
  const int callbacks_per_second =
      std::max(1, ctx->sample_rate_hz / std::max(ctx->frames_per_buffer, 1));

  if (callback_count % static_cast<std::uint64_t>(callbacks_per_second) == 0U) {
    if (ctx->channel_select_mode == ChannelSelectMode::kFixed) {
      const std::size_t idx = static_cast<std::size_t>(selected_channel);
      const double avg_rms =
          ctx->rms_accumulator[idx] / static_cast<double>(callbacks_per_second);
      const float peak = ctx->peak_accumulator[idx];
      GetLogger()->info("capture channel(fixed): ch{}(rms={},peak={}) selected_channel={}",
                        selected_channel,
                        avg_rms,
                        peak,
                        selected_channel);
    } else {
      std::ostringstream oss;
      oss << "capture channels: ";
      for (int ch = 0; ch < channels; ++ch) {
        const double avg_rms =
            ctx->rms_accumulator[static_cast<std::size_t>(ch)] /
            static_cast<double>(callbacks_per_second);
        const float peak = ctx->peak_accumulator[static_cast<std::size_t>(ch)];
        oss << "ch" << ch
            << "(rms=" << avg_rms
            << ",peak=" << peak << ")";
        if (ch == selected_channel) {
          oss << "*";
        }
        if (ch + 1 != channels) {
          oss << " ";
        }
      }

      GetLogger()->info("{} selected_channel={}", oss.str(), selected_channel);
    }

    for (int ch = 0; ch < channels; ++ch) {
      ctx->rms_accumulator[static_cast<std::size_t>(ch)] = 0.0;
      ctx->peak_accumulator[static_cast<std::size_t>(ch)] = 0.0F;
    }
  }

  return paContinue;
}

Status AudioCapture::Start() {
  if (running_.exchange(true)) {
    return Status::InvalidArgument("audio capture already running");
  }

  Status st = InitializePortAudioIfNeeded();
  if (!st.ok()) {
    running_.store(false);
    return st;
  }

  PrintResolvedDevices();

  st = OpenInputStream();
  if (!st.ok()) {
    running_.store(false);
    return st;
  }

  const PaError start_err = Pa_StartStream(stream_);
  if (start_err != paNoError) {
    if (stream_ != nullptr) {
      Pa_CloseStream(stream_);
      stream_ = nullptr;
    }
    running_.store(false);
    return Status::Internal(std::string("Pa_StartStream failed: ") + Pa_GetErrorText(start_err));
  }

  GetLogger()->info("AudioCapture started");
  return Status::Ok();
}

void AudioCapture::Stop() {
  if (!running_.exchange(false)) {
    if (stream_ != nullptr) {
      Pa_CloseStream(stream_);
      stream_ = nullptr;
    }
    if (pa_initialized_) {
      Pa_Terminate();
      pa_initialized_ = false;
    }
    return;
  }

  if (stream_ != nullptr) {
    if (Pa_IsStreamActive(stream_) == 1) {
      Pa_StopStream(stream_);
    }
    Pa_CloseStream(stream_);
    stream_ = nullptr;
  }

  if (pa_initialized_) {
    Pa_Terminate();
    pa_initialized_ = false;
  }

  GetLogger()->info("AudioCapture stopped");
}

}  // namespace mos::vis
