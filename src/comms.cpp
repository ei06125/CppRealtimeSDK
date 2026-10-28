#include "rtsdk/comms.hpp"

#include <cstdlib>
#include <new>

namespace rtsdk {

// Cache-line aligned, zeroed memory: same interface as shared memory so rings
// and mailboxes can be unit-tested without OS shm.
HeapRegion::HeapRegion(std::size_t bytes) : mem_(nullptr), size_(bytes) {
  const std::size_t rounded = (bytes + kCacheLine - 1) & ~(kCacheLine - 1);
  mem_ = std::aligned_alloc(kCacheLine, rounded ? rounded : kCacheLine);
  if (!mem_) throw std::bad_alloc();
  std::memset(mem_, 0, rounded);
}

HeapRegion::~HeapRegion() { std::free(mem_); }

namespace {
constexpr std::uint64_t kRingMagic = 0x52544b52494e4731ull;  // "RTKRING1"
}

// Slot = 4-byte length prefix + payload rounded up to 8 bytes.
std::size_t SpscRing::required_bytes(std::uint32_t capacity,
                                     std::uint32_t max_msg) {
  const std::size_t stride = sizeof(std::uint32_t) + ((max_msg + 7u) & ~7u);
  return sizeof(Header) + stride * capacity;
}

// Producer-side initialisation; magic is written last to mark the header valid.
SpscRing SpscRing::create(void* mem, std::uint32_t capacity,
                          std::uint32_t max_msg) {
  Header* h = new (mem) Header();
  h->capacity = capacity;
  h->max_msg = max_msg;
  h->head.store(0);
  h->tail.store(0);
  h->magic = kRingMagic;
  return SpscRing(h);
}

// Consumer/second process maps existing memory; validates magic and
// power-of-two capacity.
SpscRing SpscRing::attach(void* mem, bool* ok) {
  Header* h = static_cast<Header*>(mem);
  const bool good = h->magic == kRingMagic && h->capacity &&
                    (h->capacity & (h->capacity - 1)) == 0;
  if (ok) *ok = good;
  return SpscRing(h);
}

// Capacity is a power of two, so wrap-around is a mask instead of a modulo.
unsigned char* SpscRing::slot(std::uint64_t pos) const {
  return reinterpret_cast<unsigned char*>(hdr_ + 1) +
         (pos & (hdr_->capacity - 1)) * stride();
}

// Producer: write payload, then publish with a release store of tail. The
// consumer's acquire load of tail guarantees it sees the complete message.
bool SpscRing::try_push(const void* msg, std::uint32_t len) {
  if (len > hdr_->max_msg) return false;
  const auto tail = hdr_->tail.load(std::memory_order_relaxed);
  if (tail - hdr_->head.load(std::memory_order_acquire) >= hdr_->capacity)
    return false;  // full
  unsigned char* s = slot(tail);
  std::memcpy(s, &len, sizeof(len));
  std::memcpy(s + sizeof(len), msg, len);
  hdr_->tail.store(tail + 1, std::memory_order_release);
  return true;
}

// Consumer mirror of try_push; head is released only after the copy is done.
bool SpscRing::try_pop(void* out, std::uint32_t* len) {
  const auto head = hdr_->head.load(std::memory_order_relaxed);
  if (head == hdr_->tail.load(std::memory_order_acquire))
    return false;  // empty
  const unsigned char* s = slot(head);
  std::uint32_t n;
  std::memcpy(&n, s, sizeof(n));
  std::memcpy(out, s + sizeof(n), n);
  if (len) *len = n;
  hdr_->head.store(head + 1, std::memory_order_release);
  return true;
}

std::size_t SpscRing::size_approx() const {
  return static_cast<std::size_t>(hdr_->tail.load(std::memory_order_acquire) -
                                  hdr_->head.load(std::memory_order_acquire));
}

}  // namespace rtsdk
