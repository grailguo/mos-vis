
#pragma once
#include <atomic>
#include <cstddef>
#include <string>
#include <vector>
#include "mos/vis/common/types.h"

namespace mos::vis {

class AudioRingBuffer {
 public:
  explicit AudioRingBuffer(std::size_t capacity_samples);

  std::uint64_t Write(const Sample* data, std::size_t samples);
  bool Read(std::uint64_t start_pos, Sample* out, std::size_t samples) const;
  std::uint64_t write_pos() const;
  std::uint64_t OldestPos() const;
  std::size_t AvailableFrom(std::uint64_t reader_pos) const;

 private:
  std::vector<Sample> buffer_;
  std::size_t capacity_samples_;
  std::atomic<std::uint64_t> write_pos_{0};
};

class AudioReader {
 public:
  AudioReader(const AudioRingBuffer* ring, std::string name);

  bool Has(std::size_t samples) const;
  bool ReadAndAdvance(Sample* out, std::size_t samples);
  bool ReadWindow(Sample* out, std::size_t window_samples, std::size_t hop_samples);
  void Seek(std::uint64_t pos);
  std::uint64_t pos() const;

 private:
  const AudioRingBuffer* ring_;
  std::string name_;
  std::atomic<std::uint64_t> reader_pos_{0};
};

}  // namespace mos::vis
