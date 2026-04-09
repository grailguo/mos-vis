#include "mos/vis/audio/audio_playback.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#endif

#include <portaudio.h>

#include "mos/vis/audio/audio_device_selector.h"
#include "mos/vis/common/logging.h"

namespace mos::vis {
namespace {

LogContext AudioPlaybackLogCtx() {
  LogContext ctx;
  ctx.module = "AudioPlayback";
  return ctx;
}

class PortAudioPlayback final : public AudioPlayback {
 public:
  PortAudioPlayback() = default;

  ~PortAudioPlayback() override {
    Stop();
    if (pa_initialized_) {
      (void)Pa_Terminate();
      pa_initialized_ = false;
    }
  }

  Status Initialize(const AudioConfig& config) override {
    config_ = config;
    return Status::Ok();
  }

  Status Start() override {
    return InitializePortAudioIfNeeded();
  }

  void Stop() override {
    if (stream_ != nullptr) {
      (void)Pa_StopStream(stream_);
      (void)Pa_CloseStream(stream_);
      stream_ = nullptr;
    }
    active_sample_rate_hz_ = 0;
  }

  Status PlaySamples(const float* samples, std::size_t count, int sample_rate_hz) override {
    if (samples == nullptr && count > 0U) {
      return Status::InvalidArgument("playback samples is null");
    }
    if (count == 0U) {
      return Status::Ok();
    }

    const int clamped_sample_rate = std::max(1, sample_rate_hz);
    Status st = InitializePortAudioIfNeeded();
    if (!st.ok()) {
      return st;
    }

    st = OpenOutputStream(clamped_sample_rate);
    if (!st.ok()) {
      return st;
    }
    TryBoostPlaybackThreadPriority();
    const float gain = OutputGain();
    const bool unity_gain = std::abs(gain - 1.0F) < 1e-4F;

    const std::size_t max_frames_per_write = 1024U;
    std::size_t offset = 0U;
    while (offset < count) {
      const std::size_t frames = std::min(max_frames_per_write, count - offset);
      PaError err = paNoError;
      if (sample_format_ == paFloat32 && output_channels_ == 1) {
        if (unity_gain) {
          err = Pa_WriteStream(stream_, samples + offset, static_cast<unsigned long>(frames));
        } else {
          std::vector<float> mono;
          mono.resize(frames, 0.0F);
          for (std::size_t i = 0; i < frames; ++i) {
            mono[i] = ClampSample(samples[offset + i] * gain);
          }
          err = Pa_WriteStream(stream_, mono.data(), static_cast<unsigned long>(frames));
        }
      } else if (sample_format_ == paFloat32 && output_channels_ == 2) {
        std::vector<float> stereo;
        stereo.resize(frames * 2U, 0.0F);
        for (std::size_t i = 0; i < frames; ++i) {
          const float s = unity_gain ? samples[offset + i]
                                     : ClampSample(samples[offset + i] * gain);
          stereo[i * 2U] = s;
          stereo[i * 2U + 1U] = s;
        }
        err = Pa_WriteStream(stream_, stereo.data(), static_cast<unsigned long>(frames));
      } else if (sample_format_ == paInt16 && output_channels_ == 1) {
        std::vector<std::int16_t> mono16;
        mono16.resize(frames, 0);
        for (std::size_t i = 0; i < frames; ++i) {
          const float s = unity_gain ? ClampSample(samples[offset + i])
                                     : ClampSample(samples[offset + i] * gain);
          mono16[i] = static_cast<std::int16_t>(s * 32767.0F);
        }
        err = Pa_WriteStream(stream_, mono16.data(), static_cast<unsigned long>(frames));
      } else if (sample_format_ == paInt16 && output_channels_ == 2) {
        std::vector<std::int16_t> stereo16;
        stereo16.resize(frames * 2U, 0);
        for (std::size_t i = 0; i < frames; ++i) {
          const float s = unity_gain ? ClampSample(samples[offset + i])
                                     : ClampSample(samples[offset + i] * gain);
          const std::int16_t v = static_cast<std::int16_t>(s * 32767.0F);
          stereo16[i * 2U] = v;
          stereo16[i * 2U + 1U] = v;
        }
        err = Pa_WriteStream(stream_, stereo16.data(), static_cast<unsigned long>(frames));
      } else {
        return Status::Internal("unsupported playback stream format");
      }
      if (err == paOutputUnderflowed) {
        LogWarn(logevent::kAudioOverrun, AudioPlaybackLogCtx(),
                {Kv("detail", "pa_write_underflow_continue")});
        offset += frames;
        continue;
      }
      if (err != paNoError) {
        return Status::Internal(std::string("Pa_WriteStream failed: ") + Pa_GetErrorText(err));
      }
      offset += frames;
    }
    return Status::Ok();
  }

  Status PlayWavFile(const std::string& path) override {
    if (path.empty()) {
      return Status::InvalidArgument("playback wav path is empty");
    }

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
      return Status::NotFound("wav file not found: " + path);
    }

    char riff[4];
    ifs.read(riff, 4);
    if (!ifs || std::memcmp(riff, "RIFF", 4) != 0) {
      return Status::InvalidArgument("invalid wav: missing RIFF header");
    }

    (void)ReadLe32(&ifs);  // file size

    char wave[4];
    ifs.read(wave, 4);
    if (!ifs || std::memcmp(wave, "WAVE", 4) != 0) {
      return Status::InvalidArgument("invalid wav: missing WAVE header");
    }

    std::uint16_t audio_format = 0U;
    std::uint16_t num_channels = 0U;
    std::uint32_t sample_rate = 0U;
    std::uint16_t bits_per_sample = 0U;
    std::vector<std::uint8_t> data_chunk;

    while (ifs.good() && !ifs.eof()) {
      char chunk_id[4];
      ifs.read(chunk_id, 4);
      if (!ifs) {
        break;
      }
      const std::uint32_t chunk_size = ReadLe32(&ifs);

      if (std::memcmp(chunk_id, "fmt ", 4) == 0) {
        audio_format = ReadLe16(&ifs);
        num_channels = ReadLe16(&ifs);
        sample_rate = ReadLe32(&ifs);
        (void)ReadLe32(&ifs);  // byte rate
        (void)ReadLe16(&ifs);  // block align
        bits_per_sample = ReadLe16(&ifs);

        const std::uint32_t consumed = 16U;
        if (chunk_size > consumed) {
          ifs.ignore(static_cast<std::streamsize>(chunk_size - consumed));
        }
      } else if (std::memcmp(chunk_id, "data", 4) == 0) {
        data_chunk.resize(static_cast<std::size_t>(chunk_size), 0U);
        ifs.read(reinterpret_cast<char*>(data_chunk.data()),
                 static_cast<std::streamsize>(chunk_size));
      } else {
        ifs.ignore(static_cast<std::streamsize>(chunk_size));
      }

      if ((chunk_size & 1U) != 0U) {
        ifs.ignore(1);
      }
    }

    if (data_chunk.empty()) {
      return Status::InvalidArgument("invalid wav: data chunk is empty");
    }
    if (num_channels == 0U || sample_rate == 0U) {
      return Status::InvalidArgument("invalid wav: invalid channels or sample rate");
    }

    std::vector<float> mono;
    if (audio_format == 1U && bits_per_sample == 16U) {
      const std::size_t bytes_per_sample = 2U;
      const std::size_t frame_bytes = static_cast<std::size_t>(num_channels) * bytes_per_sample;
      if (frame_bytes == 0U || data_chunk.size() % frame_bytes != 0U) {
        return Status::InvalidArgument("invalid wav: PCM16 data size mismatch");
      }
      const std::size_t frames = data_chunk.size() / frame_bytes;
      mono.assign(frames, 0.0F);

      for (std::size_t i = 0; i < frames; ++i) {
        float sum = 0.0F;
        for (std::uint16_t ch = 0; ch < num_channels; ++ch) {
          const std::size_t base = (i * frame_bytes) + (static_cast<std::size_t>(ch) * bytes_per_sample);
          const std::int16_t v = static_cast<std::int16_t>(
              static_cast<std::uint16_t>(data_chunk[base]) |
              (static_cast<std::uint16_t>(data_chunk[base + 1]) << 8U));
          sum += static_cast<float>(v) / 32768.0F;
        }
        mono[i] = sum / static_cast<float>(num_channels);
      }
    } else if (audio_format == 3U && bits_per_sample == 32U) {
      const std::size_t bytes_per_sample = 4U;
      const std::size_t frame_bytes = static_cast<std::size_t>(num_channels) * bytes_per_sample;
      if (frame_bytes == 0U || data_chunk.size() % frame_bytes != 0U) {
        return Status::InvalidArgument("invalid wav: float32 data size mismatch");
      }
      const std::size_t frames = data_chunk.size() / frame_bytes;
      mono.assign(frames, 0.0F);

      for (std::size_t i = 0; i < frames; ++i) {
        float sum = 0.0F;
        for (std::uint16_t ch = 0; ch < num_channels; ++ch) {
          const std::size_t base = (i * frame_bytes) + (static_cast<std::size_t>(ch) * bytes_per_sample);
          float v = 0.0F;
          std::memcpy(&v, data_chunk.data() + base, sizeof(float));
          sum += std::max(-1.0F, std::min(1.0F, v));
        }
        mono[i] = sum / static_cast<float>(num_channels);
      }
    } else {
      return Status::InvalidArgument("unsupported wav format (only PCM16/float32 are supported)");
    }

    return PlaySamples(mono.data(), mono.size(), static_cast<int>(sample_rate));
  }

 private:
  static std::uint16_t ReadLe16(std::ifstream* ifs) {
    char b[2];
    ifs->read(b, 2);
    if (!(*ifs)) {
      return 0U;
    }
    return static_cast<std::uint16_t>(static_cast<std::uint8_t>(b[0])) |
           (static_cast<std::uint16_t>(static_cast<std::uint8_t>(b[1])) << 8U);
  }

  static std::uint32_t ReadLe32(std::ifstream* ifs) {
    char b[4];
    ifs->read(b, 4);
    if (!(*ifs)) {
      return 0U;
    }
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[0])) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[1])) << 8U) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[2])) << 16U) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[3])) << 24U);
  }

  Status InitializePortAudioIfNeeded() {
    if (pa_initialized_) {
      return Status::Ok();
    }
    const PaError err = Pa_Initialize();
    if (err != paNoError) {
      return Status::Internal(std::string("Pa_Initialize failed: ") + Pa_GetErrorText(err));
    }
    pa_initialized_ = true;
    return Status::Ok();
  }

  int ResolveOutputDeviceIndex() const {
    const std::vector<AudioDeviceInfo> devices = EnumerateAudioDevices();
    if (!config_.output_device.empty()) {
      const auto matched = FindBestOutputDevice(devices, config_.output_device);
      if (matched.has_value()) {
        return matched->index;
      }
      LogWarn(logevent::kAudioDeviceOpen, AudioPlaybackLogCtx(),
              {Kv("detail", "output_device_not_found_fallback_default"),
               Kv("output_device", config_.output_device)});
    }
    return Pa_GetDefaultOutputDevice();
  }

  Status OpenOutputStream(int sample_rate_hz) {
    if (stream_ != nullptr && active_sample_rate_hz_ == sample_rate_hz) {
      return Status::Ok();
    }
    if (stream_ != nullptr) {
      (void)Pa_StopStream(stream_);
      (void)Pa_CloseStream(stream_);
      stream_ = nullptr;
      active_sample_rate_hz_ = 0;
    }

    const int preferred_device = ResolveOutputDeviceIndex();
    const int default_device = Pa_GetDefaultOutputDevice();

    std::vector<int> candidate_devices;
    if (preferred_device != paNoDevice) {
      candidate_devices.push_back(preferred_device);
    }
    if (default_device != paNoDevice &&
        std::find(candidate_devices.begin(), candidate_devices.end(), default_device) ==
            candidate_devices.end()) {
      candidate_devices.push_back(default_device);
    }
    if (candidate_devices.empty()) {
      return Status::NotFound("no output audio device available");
    }

    const unsigned long configured_fpb =
        static_cast<unsigned long>(std::max(1, config_.capture_chunk_samples));
    const unsigned long fpb_1024 = std::max<unsigned long>(configured_fpb, 1024UL);
    const unsigned long fpb_2048 = std::max<unsigned long>(configured_fpb, 2048UL);
    const std::vector<unsigned long> fpb_candidates = {
        fpb_2048,
        fpb_1024,
        configured_fpb,
        paFramesPerBufferUnspecified,
    };
    const std::vector<PaSampleFormat> format_candidates = {paFloat32, paInt16};
    std::string last_error = "Pa_OpenStream(output) failed";

    for (int device_index : candidate_devices) {
      const PaDeviceInfo* output_info = Pa_GetDeviceInfo(device_index);
      if (output_info == nullptr || output_info->maxOutputChannels <= 0) {
        continue;
      }
      const std::vector<int> channel_candidates =
          (output_info->maxOutputChannels >= 2) ? std::vector<int>{1, 2} : std::vector<int>{1};

      for (int channels : channel_candidates) {
        for (PaSampleFormat sample_format : format_candidates) {
          for (unsigned long frames_per_buffer : fpb_candidates) {
            PaStreamParameters output_params;
            std::memset(&output_params, 0, sizeof(output_params));
            output_params.device = device_index;
            output_params.channelCount = channels;
            output_params.sampleFormat = sample_format;
            output_params.suggestedLatency = output_info->defaultLowOutputLatency;
            output_params.hostApiSpecificStreamInfo = nullptr;

            stream_ = nullptr;
            const PaError open_err =
                Pa_OpenStream(&stream_,
                              nullptr,
                              &output_params,
                              static_cast<double>(sample_rate_hz),
                              frames_per_buffer,
                              paNoFlag,
                              nullptr,
                              nullptr);
            if (open_err != paNoError) {
              stream_ = nullptr;
              std::ostringstream oss;
              oss << "device=" << device_index
                  << " channels=" << channels
                  << " format=" << ((sample_format == paFloat32) ? "float32" : "int16")
                  << " fpb=" << ((frames_per_buffer == paFramesPerBufferUnspecified) ? 0UL : frames_per_buffer)
                  << " err=" << Pa_GetErrorText(open_err);
              const PaHostErrorInfo* host_error = Pa_GetLastHostErrorInfo();
              if (host_error != nullptr && host_error->errorText != nullptr) {
                oss << " host=" << host_error->errorText;
              }
              last_error = oss.str();
              continue;
            }

            const PaError start_err = Pa_StartStream(stream_);
            if (start_err != paNoError) {
              last_error = std::string("Pa_StartStream(output) failed: ") + Pa_GetErrorText(start_err);
              (void)Pa_CloseStream(stream_);
              stream_ = nullptr;
              continue;
            }

            active_sample_rate_hz_ = sample_rate_hz;
            output_channels_ = channels;
            sample_format_ = sample_format;
            LogInfo(logevent::kAudioDeviceOpen, AudioPlaybackLogCtx(),
                    {Kv("index", device_index),
                     Kv("name", output_info->name),
                     Kv("sr", sample_rate_hz),
                     Kv("ch", channels),
                     Kv("format", (sample_format == paFloat32) ? "float32" : "int16"),
                     Kv("buf", (frames_per_buffer == paFramesPerBufferUnspecified) ? 0UL : frames_per_buffer)});
            return Status::Ok();
          }
        }
      }
    }

    return Status::Internal("Pa_OpenStream(output) failed after fallbacks: " + last_error);
  }

  void TryBoostPlaybackThreadPriority() {
    if (priority_boost_attempted_) {
      return;
    }
    priority_boost_attempted_ = true;

#if defined(__linux__)
    sched_param sp{};
    const int max_prio = sched_get_priority_max(SCHED_FIFO);
    if (max_prio > 0) {
      sp.sched_priority = std::max(1, max_prio / 4);
      const int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
      if (rc == 0) {
        LogInfo(logevent::kAudioDeviceOpen, AudioPlaybackLogCtx(),
                {Kv("detail", "playback_thread_priority_boosted"),
                 Kv("policy", "SCHED_FIFO"),
                 Kv("prio", sp.sched_priority)});
        return;
      }
    }

    errno = 0;
    if (setpriority(PRIO_PROCESS, 0, -5) == 0) {
      LogInfo(logevent::kAudioDeviceOpen, AudioPlaybackLogCtx(),
              {Kv("detail", "playback_thread_nice_boosted"), Kv("nice", -5)});
      return;
    }

    LogDebug(logevent::kAudioOverrun, AudioPlaybackLogCtx(),
             {Kv("detail", "playback_thread_priority_boost_failed"),
              Kv("errno", errno)});
#endif
  }

  float OutputGain() const {
    return std::max(0.1F, std::min(4.0F, config_.playback_gain));
  }

  static float ClampSample(float v) {
    return std::max(-1.0F, std::min(1.0F, v));
  }

  AudioConfig config_;
  bool pa_initialized_ = false;
  PaStream* stream_ = nullptr;
  int active_sample_rate_hz_ = 0;
  int output_channels_ = 1;
  PaSampleFormat sample_format_ = paFloat32;
  bool priority_boost_attempted_ = false;
};

}  // namespace

std::unique_ptr<AudioPlayback> CreateAudioPlayback() {
  return std::make_unique<PortAudioPlayback>();
}

}  // namespace mos::vis
