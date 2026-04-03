
#pragma once
#include <optional>
#include <string>
#include <vector>

namespace mos::vis {

struct AudioDeviceInfo {
  int index = -1;
  std::string name;
  int max_input_channels = 0;
  int max_output_channels = 0;
  double default_sample_rate = 0.0;
};

std::vector<AudioDeviceInfo> EnumerateAudioDevices();
std::optional<AudioDeviceInfo> FindBestInputDevice(
    const std::vector<AudioDeviceInfo>& devices,
    const std::string& configured_name);
std::optional<AudioDeviceInfo> FindBestOutputDevice(
    const std::vector<AudioDeviceInfo>& devices,
    const std::string& configured_name);
void PrintAudioDevices(const std::vector<AudioDeviceInfo>& devices);

}  // namespace mos::vis
