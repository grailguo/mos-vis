
#include "mos/vis/audio/audio_ring_buffer.h"

namespace mos::vis {

AudioRingBuffer::AudioRingBuffer(std::size_t capacity_samples)
    : buffer_(capacity_samples, 0.0F), capacity_samples_(capacity_samples) {}

std::uint64_t AudioRingBuffer::Write(const Sample* data, std::size_t samples) {
  const std::uint64_t begin = write_pos_.load();
  for (std::size_t i = 0; i < samples; ++i) {
    buffer_[(begin + i) % capacity_samples_] = data[i];
  }
  write_pos_.store(begin + samples);
  return begin + samples;
}

bool AudioRingBuffer::Read(std::uint64_t start_pos, Sample* out, std::size_t samples) const {
  if (start_pos < OldestPos()) return false;
  if (start_pos + samples > write_pos()) return false;
  for (std::size_t i = 0; i < samples; ++i) {
    out[i] = buffer_[(start_pos + i) % capacity_samples_];
  }
  return true;
}

std::uint64_t AudioRingBuffer::write_pos() const { return write_pos_.load(); }

std::uint64_t AudioRingBuffer::OldestPos() const {
  const std::uint64_t w = write_pos();
  return (w > capacity_samples_) ? (w - capacity_samples_) : 0;
}

std::size_t AudioRingBuffer::AvailableFrom(std::uint64_t reader_pos) const {
  const std::uint64_t w = write_pos();
  return (reader_pos < w) ? static_cast<std::size_t>(w - reader_pos) : 0;
}

AudioReader::AudioReader(const AudioRingBuffer* ring, std::string name)
    : ring_(ring), name_(std::move(name)) {}

bool AudioReader::Has(std::size_t samples) const {
  const std::uint64_t cur = reader_pos_.load();
  const std::uint64_t oldest = ring_->OldestPos();
  const std::uint64_t effective_pos = (cur < oldest) ? oldest : cur;
  return ring_->AvailableFrom(effective_pos) >= samples;
}

bool AudioReader::ReadAndAdvance(Sample* out, std::size_t samples) {
  std::uint64_t cur = reader_pos_.load();
  const std::uint64_t oldest = ring_->OldestPos();
  if (cur < oldest) {
    cur = oldest;
    reader_pos_.store(cur);
  }
  if (!ring_->Read(cur, out, samples)) return false;
  reader_pos_.store(cur + samples);
  return true;
}

bool AudioReader::ReadWindow(Sample* out, std::size_t window_samples, std::size_t hop_samples) {
  std::uint64_t cur = reader_pos_.load();
  const std::uint64_t oldest = ring_->OldestPos();
  if (cur < oldest) {
    cur = oldest;
    reader_pos_.store(cur);
  }
  if (!ring_->Read(cur, out, window_samples)) return false;
  reader_pos_.store(cur + hop_samples);
  return true;
}

void AudioReader::Seek(std::uint64_t pos) { reader_pos_.store(pos); }
std::uint64_t AudioReader::pos() const { return reader_pos_.load(); }

}  // namespace mos::vis
