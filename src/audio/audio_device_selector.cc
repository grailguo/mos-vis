
#include "mos/vis/audio/audio_device_selector.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include <portaudio.h>

namespace mos::vis {
namespace {

std::string Normalize(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });

  for (char& c : s) {
    if (c == '_' || c == '-' || c == ':') {
      c = ' ';
    }
  }

  std::string out;
  out.reserve(s.size());
  bool prev_space = false;
  for (char c : s) {
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      if (!prev_space) {
        out.push_back(' ');
      }
      prev_space = true;
    } else {
      out.push_back(c);
      prev_space = false;
    }
  }

  while (!out.empty() && out.front() == ' ') {
    out.erase(out.begin());
  }
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  return out;
}

bool MatchName(const std::string& device_name, const std::string& configured_name) {
  if (configured_name.empty()) {
    return false;
  }
  const std::string d = Normalize(device_name);
  const std::string c = Normalize(configured_name);
  return d.find(c) != std::string::npos;
}

}  // namespace

std::vector<AudioDeviceInfo> EnumerateAudioDevices() {
  std::vector<AudioDeviceInfo> out;
  const int count = Pa_GetDeviceCount();
  if (count < 0) {
    return out;
  }

  out.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
    if (info == nullptr) {
      continue;
    }
    out.push_back(AudioDeviceInfo{
        i, info->name, info->maxInputChannels, info->maxOutputChannels, info->defaultSampleRate});
  }
  return out;
}

std::optional<AudioDeviceInfo> FindBestInputDevice(
    const std::vector<AudioDeviceInfo>& devices, const std::string& configured_name) {
  for (const auto& d : devices) {
    if (d.max_input_channels > 0 && d.name == configured_name) {
      return d;
    }
  }
  for (const auto& d : devices) {
    if (d.max_input_channels > 0 && MatchName(d.name, configured_name)) {
      return d;
    }
  }
  return std::nullopt;
}

std::optional<AudioDeviceInfo> FindBestOutputDevice(
    const std::vector<AudioDeviceInfo>& devices, const std::string& configured_name) {
  for (const auto& d : devices) {
    if (d.max_output_channels > 0 && d.name == configured_name) {
      return d;
    }
  }
  for (const auto& d : devices) {
    if (d.max_output_channels > 0 && MatchName(d.name, configured_name)) {
      return d;
    }
  }
  return std::nullopt;
}

void PrintAudioDevices(const std::vector<AudioDeviceInfo>& devices) {
  std::cout << "==== PortAudio device list ====\n";
  if (devices.empty()) {
    std::cout << "(no devices)\n";
    return;
  }

  for (const auto& d : devices) {
    std::cout << "  [" << d.index << "] "
              << d.name
              << " | in=" << d.max_input_channels
              << " | out=" << d.max_output_channels
              << " | default_sr=" << std::fixed << std::setprecision(0)
              << d.default_sample_rate << "\n";
  }
  std::cout << "===============================\n";
}

}  // namespace mos::vis
