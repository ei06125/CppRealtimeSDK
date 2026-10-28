// Inter-node communication primitives that live in a flat memory region, so the
// same code works between threads (HeapRegion) and between processes
// (PosixShm). All primitives are lock-free and allocation-free after setup.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>

namespace rtsdk {

class MemoryRegion {
 public:
  virtual ~MemoryRegion() = default;
  virtual void* data() = 0;
  virtual std::size_t size() const = 0;
};

// In-process region (aligned heap allocation).
class HeapRegion final : public MemoryRegion {
 public:
  explicit HeapRegion(std::size_t bytes);
  ~HeapRegion() override;
  void* data() override { return mem_; }
  std::size_t size() const override { return size_; }

 private:
  void* mem_;
  std::size_t size_;
};

// POSIX named shared memory (shm_open + mmap). Implemented in the posix backend.
class PosixShm final : public MemoryRegion {
 public:
  // create=true: create/truncate and zero; create=false: attach to existing.
  static std::unique_ptr<PosixShm> open(const std::string& name, std::size_t bytes, bool create,
                                        std::string* error = nullptr);
  ~PosixShm() override;
  void* data() override { return mem_; }
  std::size_t size() const override { return size_; }
  static void unlink(const std::string& name);

 private:
  PosixShm() = default;
  void* mem_ = nullptr;
  std::size_t size_ = 0;
  std::string name_;
};

constexpr std::size_t kCacheLine = 64;

// Single-producer / single-consumer ring of variable-length messages (each
// <= max_msg bytes). Wait-free on both sides.
class SpscRing {
 public:
  static std::size_t required_bytes(std::uint32_t capacity, std::uint32_t max_msg);
  // capacity must be a power of two. Initialises the region (producer side).
  static SpscRing create(void* mem, std::uint32_t capacity, std::uint32_t max_msg);
  // Attaches to an initialised region; ok=false if magic/layout mismatches.
  static SpscRing attach(void* mem, bool* ok = nullptr);

  bool try_push(const void* msg, std::uint32_t len);
  // Copies into out (size >= max_msg); returns false when empty.
  bool try_pop(void* out, std::uint32_t* len);
  std::uint32_t max_msg() const { return hdr_->max_msg; }
  std::size_t size_approx() const;

 private:
  struct Header {
    std::uint64_t magic;
    std::uint32_t capacity, max_msg;
    alignas(kCacheLine) std::atomic<std::uint64_t> head;  // consumer position
    alignas(kCacheLine) std::atomic<std::uint64_t> tail;  // producer position
  };
  explicit SpscRing(Header* h) : hdr_(h) {}
  unsigned char* slot(std::uint64_t pos) const;
  std::size_t stride() const { return sizeof(std::uint32_t) + ((hdr_->max_msg + 7u) & ~7u); }

  Header* hdr_;
};

// Latest-value mailbox (seqlock): one writer, any number of readers, readers
// never block the writer. T must be trivially copyable.
template <typename T>
class Mailbox {
  static_assert(std::is_trivially_copyable<T>::value, "Mailbox<T> requires trivially copyable T");

 public:
  static constexpr std::size_t kWords = (sizeof(T) + 7) / 8;
  static std::size_t required_bytes() { return sizeof(Block); }

  static Mailbox create(void* mem) {
    Block* b = new (mem) Block();
    return Mailbox(b);
  }
  static Mailbox attach(void* mem) { return Mailbox(static_cast<Block*>(mem)); }

  void write(const T& v) {
    std::uint64_t w[kWords] = {};
    std::memcpy(w, &v, sizeof(T));
    // Fence-free seqlock (also understood by ThreadSanitizer): the acq_rel RMW
    // keeps the data stores after the odd marker; readers use acquire data loads.
    const auto s = b_->seq.load(std::memory_order_relaxed);
    b_->seq.exchange(s + 1, std::memory_order_acq_rel);  // odd: write in progress
    for (std::size_t i = 0; i < kWords; ++i) b_->words[i].store(w[i], std::memory_order_relaxed);
    b_->seq.store(s + 2, std::memory_order_release);
  }

  // Returns false if nothing was ever written. Spins only while a write is in flight.
  bool read(T& out) const {
    std::uint64_t w[kWords];
    for (;;) {
      const auto s1 = b_->seq.load(std::memory_order_acquire);
      if (s1 == 0) return false;
      if (s1 & 1) continue;
      for (std::size_t i = 0; i < kWords; ++i) w[i] = b_->words[i].load(std::memory_order_acquire);
      if (b_->seq.load(std::memory_order_relaxed) == s1) break;
    }
    std::memcpy(&out, w, sizeof(T));
    return true;
  }

  std::uint64_t version() const { return b_->seq.load(std::memory_order_acquire) / 2; }

 private:
  struct Block {
    std::atomic<std::uint64_t> seq{0};
    std::atomic<std::uint64_t> words[kWords];
  };
  explicit Mailbox(Block* b) : b_(b) {}
  Block* b_;
};

}  // namespace rtsdk
