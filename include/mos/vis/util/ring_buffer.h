#pragma once

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <vector>

namespace mos::vis {

template <typename T>
class RingBuffer {
 public:
  explicit RingBuffer(std::size_t capacity)
      : buffer_(capacity == 0 ? 1 : capacity), capacity_(capacity == 0 ? 1 : capacity) {}

  std::size_t Push(const T* data, std::size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t written = 0;
    for (; written < count; ++written) {
      if (size_ == capacity_) {
        break;
      }
      buffer_[(head_ + size_) % capacity_] = data[written];
      ++size_;
    }
    return written;
  }

  std::size_t Pop(T* out, std::size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t to_read = std::min(count, size_);
    for (std::size_t i = 0; i < to_read; ++i) {
      out[i] = buffer_[(head_ + i) % capacity_];
    }
    head_ = (head_ + to_read) % capacity_;
    size_ -= to_read;
    return to_read;
  }

  std::size_t Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return size_;
  }

  std::size_t Capacity() const { return capacity_; }

  bool Empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return size_ == 0;
  }

  void Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    head_ = 0;
    size_ = 0;
  }

 private:
  std::vector<T> buffer_;
  std::size_t capacity_ = 0;
  std::size_t head_ = 0;
  std::size_t size_ = 0;
  mutable std::mutex mutex_;
};

}  // namespace mos::vis
