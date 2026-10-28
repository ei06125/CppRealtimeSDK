// Linux cross-process latest-value transport with an explicitly managed object
// lifetime. Opening peers must agree on capacity and ABI after the creator
// publishes readiness.
#pragma once
#include <cstddef>
#include <span>
#include <string>
namespace rtx {
// Fixed-size latest-value mailbox. A process-shared robust mutex protects
// payloads. Creation is exclusive. Consumers open only after the creator
// publishes readiness.
class SharedMemory {
 public:
  SharedMemory(const std::string& name, std::size_t capacity, bool create);
  ~SharedMemory();
  SharedMemory(const SharedMemory&) = delete;
  SharedMemory& operator=(const SharedMemory&) = delete;
  std::size_t capacity() const noexcept { return capacity_; }
  void write(std::span<const std::byte> data);
  std::size_t read(std::span<std::byte> destination);
  static void unlink(const std::string& name);

 private:
  void* mapping_ = nullptr;
  std::size_t size_ = 0, capacity_ = 0;
};
}  // namespace rtx
