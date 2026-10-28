// Verify latest-value bytes, transactional bounds rejection and concurrent
// payload integrity. Sequence/complement pairs expose torn messages without
// imposing host-dependent timing limits.
#include "rtx/mailbox.hpp"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <thread>
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
int main() {
  try {
    rtx::Mailbox<16> channel;
    std::array<std::byte, 17> buffer{};
    require(channel.capacity() == 16 && channel.read(buffer) == 0,
            "initial mailbox not empty");
    const std::array<std::byte, 3> bytes{std::byte{0}, std::byte{0xff},
                                         std::byte{0x55}};
    channel.write(bytes);
    require(channel.read(buffer) == 3 && buffer[1] == std::byte{0xff},
            "binary bytes changed");
    bool rejected = false;
    try {
      channel.write(buffer);
    } catch (const std::length_error&) {
      rejected = true;
    }
    require(rejected && channel.read(buffer) == 3,
            "oversized write changed stored value");
    rejected = false;
    try {
      channel.read(std::span(buffer).first(2));
    } catch (const std::length_error&) {
      rejected = true;
    }
    require(rejected && channel.read(buffer) == 3,
            "short read changed stored value");
    channel.write({});
    require(channel.read({}) == 0, "empty write did not clear mailbox");
    std::atomic<bool> done = false, invalid = false;
    std::array<std::jthread, 2> readers;
    for (auto& reader : readers)
      reader = std::jthread([&] {
        std::uint64_t previous = 0;
        do {
          std::array<std::uint64_t, 2> message{};
          const auto size =
              channel.read(std::as_writable_bytes(std::span(message)));
          if (size) {
            if (size != sizeof(message) || message[1] != ~message[0] ||
                message[0] < previous)
              invalid = true;
            previous = message[0];
          }
        } while (!done);
        std::array<std::uint64_t, 2> final{};
        if (channel.read(std::as_writable_bytes(std::span(final))) !=
                sizeof(final) ||
            final[0] != 10000 || final[1] != ~final[0])
          invalid = true;
      });
    for (std::uint64_t sequence = 1; sequence <= 10000; ++sequence) {
      const std::array<std::uint64_t, 2> message{sequence, ~sequence};
      channel.write(std::as_bytes(std::span(message)));
    }
    done = true;
    for (auto& reader : readers) reader.join();
    require(!invalid, "concurrent payload torn or regressed");
    std::cout << "bounded binary mailbox, bounds, clear and concurrent "
                 "integrity passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
