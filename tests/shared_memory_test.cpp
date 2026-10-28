// Exercise Linux process-shared transfer, owner-death recovery and payload
// integrity. Child processes model real mapping peers rather than substituting
// thread-only synchronization.
#include "rtx/shared_memory.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

#include "../src/shared_memory_layout.hpp"
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
void wait_child(pid_t child) {
  int status = 0;
  require(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0,
          "child failed");
}
int main() {
  const auto name = "/rtx-recovery-" + std::to_string(getpid());
  struct Cleanup {
    const std::string& name;
    ~Cleanup() { shm_unlink(name.c_str()); }
  } cleanup{name};
  try {
    rtx::SharedMemory mailbox(name, 64, true);
    std::array<std::byte, 64> payload;
    payload.fill(std::byte{42});
    mailbox.write(payload);
    // Use the real internal layout to kill an owner with a corrupt partial
    // update. This intentionally bypasses the public API solely for
    // deterministic injection.
    pid_t child = fork();
    require(child >= 0, "fork failed");
    if (child == 0) {
      int fd = shm_open(name.c_str(), O_RDWR, 0);
      if (fd < 0) _exit(1);
      const auto size = sizeof(rtx::detail::SharedMemoryHeader) + 64;
      void* mapping =
          mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (mapping == MAP_FAILED) _exit(2);
      auto* header = static_cast<rtx::detail::SharedMemoryHeader*>(mapping);
      if (pthread_mutex_lock(&header->mutex) != 0) _exit(3);
      header->length = 1000;  // Not a valid committed message.
      static_cast<std::byte*>(mapping)[sizeof(*header)] = std::byte{99};
      _exit(0);  // No unlock: Linux reports EOWNERDEAD to the next locker.
    }
    wait_child(child);
    require(mailbox.read(payload) == 0,
            "partial owner-death payload was exposed");
    payload.fill(std::byte{7});
    mailbox.write(payload);
    rtx::SharedMemory peer(name, 64, false);
    std::array<std::byte, 64> read{};
    require(peer.read(read) == 64 && read == payload,
            "mailbox failed after owner recovery");
    bool rejected = false;
    try {
      rtx::SharedMemory mismatch(name, 63, false);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    require(rejected, "capacity mismatch accepted");
    std::array<std::byte, 65> oversized{};
    rejected = false;
    try {
      mailbox.write(oversized);
    } catch (const std::length_error&) {
      rejected = true;
    }
    require(rejected, "oversize payload accepted");
    require(peer.read(read) == 64 && read == payload,
            "rejected write changed committed data");
    child = fork();
    require(child >= 0, "fork failed");
    if (child == 0) {
      try {
        rtx::SharedMemory writer(name, 64, false);
        for (unsigned i = 0; i < 3000; ++i) {
          std::array<std::byte, 64> bytes;
          bytes.fill(static_cast<std::byte>(i % 256));
          writer.write(bytes);
        }
        _exit(0);
      } catch (...) {
        _exit(4);
      }
    }
    bool intact = true;
    for (unsigned i = 0; i < 3000; ++i) {
      if (mailbox.read(read) != 64 ||
          !std::all_of(read.begin(), read.end(),
                       [&](auto b) { return b == read[0]; }))
        intact = false;
    }
    wait_child(child);
    require(intact, "concurrent reader observed torn payload");
    std::cout << "owner death, recovery, bounds and process-shared payload "
                 "integrity passed\n";
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 1;
  }
}
