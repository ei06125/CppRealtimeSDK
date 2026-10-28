#include "rtsdk/posix_platform.hpp"

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace rtsdk {

TimeUs PosixClock::now_us() const {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<TimeUs>(ts.tv_sec) * 1000000u + static_cast<TimeUs>(ts.tv_nsec) / 1000u;
}

void PosixClock::sleep_until(TimeUs t) {
  timespec ts;
  ts.tv_sec = static_cast<time_t>(t / 1000000u);
  ts.tv_nsec = static_cast<long>((t % 1000000u) * 1000u);
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
  }
}

int PosixPlatform::map_priority(Priority p) {
  const int lo = sched_get_priority_min(SCHED_FIFO), hi = sched_get_priority_max(SCHED_FIFO);
  return lo + static_cast<int>((static_cast<long>(hi - lo) * p) / 255);
}

namespace {

struct Payload {
  std::function<void()> body;
  std::string name;
};

void* trampoline(void* arg) {
  std::unique_ptr<Payload> p(static_cast<Payload*>(arg));
  if (!p->name.empty()) pthread_setname_np(pthread_self(), p->name.substr(0, 15).c_str());
  p->body();
  return nullptr;
}

class PThread final : public IThread {
 public:
  explicit PThread(pthread_t t) : t_(t) {}
  ~PThread() override { join(); }
  void join() override {
    if (!joined_) {
      pthread_join(t_, nullptr);
      joined_ = true;
    }
  }

 private:
  pthread_t t_;
  bool joined_ = false;
};

}  // namespace

std::unique_ptr<IThread> PosixPlatform::spawn(const ThreadAttr& attr, std::function<void()> body) {
  auto launch = [&](bool realtime) -> std::unique_ptr<IThread> {
    pthread_attr_t pa;
    pthread_attr_init(&pa);
    if (attr.stack_bytes) pthread_attr_setstacksize(&pa, attr.stack_bytes);
    if (realtime) {
      sched_param sp;
      std::memset(&sp, 0, sizeof(sp));
      sp.sched_priority = map_priority(attr.priority);
      pthread_attr_setinheritsched(&pa, PTHREAD_EXPLICIT_SCHED);
      pthread_attr_setschedpolicy(&pa, SCHED_FIFO);
      pthread_attr_setschedparam(&pa, &sp);
    }
    auto* payload = new Payload{body, attr.name ? attr.name : ""};
    pthread_t t;
    const int rc = pthread_create(&t, &pa, trampoline, payload);
    pthread_attr_destroy(&pa);
    if (rc != 0) {
      delete payload;
      return nullptr;
    }
    return std::make_unique<PThread>(t);
  };
  if (auto t = launch(true)) {
    rt_ok_.store(true);
    return t;
  }
  return launch(false);  // EPERM etc.: run as SCHED_OTHER, priorities advisory
}

// ------------------------------------------------------------------- PosixShm

std::unique_ptr<PosixShm> PosixShm::open(const std::string& name, std::size_t bytes, bool create,
                                         std::string* error) {
  auto fail = [&](const char* what) -> std::unique_ptr<PosixShm> {
    if (error) *error = std::string(what) + ": " + std::strerror(errno);
    return nullptr;
  };
  const int fd = shm_open(name.c_str(), create ? (O_CREAT | O_RDWR | O_TRUNC) : O_RDWR, 0600);
  if (fd < 0) return fail("shm_open");
  if (create && ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
    close(fd);
    return fail("ftruncate");
  }
  void* mem = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (mem == MAP_FAILED) return fail("mmap");
  std::unique_ptr<PosixShm> r(new PosixShm());
  r->mem_ = mem;
  r->size_ = bytes;
  r->name_ = name;
  return r;
}

PosixShm::~PosixShm() {
  if (mem_) munmap(mem_, size_);
}

void PosixShm::unlink(const std::string& name) { shm_unlink(name.c_str()); }

}  // namespace rtsdk
