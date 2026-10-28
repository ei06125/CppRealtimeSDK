// Bounded latest-value transport for nodes sharing one address space.
// Keep payload storage inline; normal copying is serialized without allocating
// message buffers.
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <stdexcept>

#include "rtx/runtime_mutex.hpp"
namespace rtx {
// Latest-value bytes shared by tasks in one address space. Reads do not
// consume. Normal read/write operations do not allocate; lock acquisition can
// block. This object is not an inter-process shared-memory layout or an ISR
// API.
template <std::size_t Capacity>
class Mailbox {
  static_assert(Capacity > 0, "mailbox capacity must be positive");

 public:
  static constexpr std::size_t capacity() noexcept { return Capacity; }
  void write(std::span<const std::byte> data) {
    if (data.size() > Capacity)
      throw std::length_error("mailbox payload exceeds capacity");
    std::lock_guard lock(mutex_);
    std::copy(data.begin(), data.end(), payload_.begin());
    size_ = data.size();
  }
  std::size_t read(std::span<std::byte> destination) const {
    std::lock_guard lock(mutex_);
    if (destination.size() < size_)
      throw std::length_error("mailbox destination too small");
    std::copy_n(payload_.begin(), size_, destination.begin());
    return size_;
  }

 private:
  mutable RuntimeMutex mutex_;
  std::array<std::byte, Capacity> payload_{};
  std::size_t size_ = 0;
};
}  // namespace rtx
