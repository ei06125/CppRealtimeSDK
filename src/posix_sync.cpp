// Implement native monotonic condition waits around the Linux priority-inheritance mutex.
// Report setup and deadline errors explicitly rather than silently changing clock or lock semantics.
#include "rtx/posix_sync.hpp"
#include "rtx/posix.hpp"
#include <cerrno>
#include <limits>
#include <system_error>
namespace rtx::detail {
namespace {
void check(int result) {
 if(result) throw std::system_error(result,std::generic_category(),"POSIX condition variable");
}
pthread_mutex_t* mutex_handle(std::unique_lock<RuntimeMutex>& lock) {
 if(!lock.owns_lock()) throw std::logic_error("condition wait requires locked mutex");
 return lock.mutex()->native_handle();
}
}
PosixConditionVariable::PosixConditionVariable() {
 pthread_condattr_t attributes;
 check(pthread_condattr_init(&attributes));
 int result=pthread_condattr_setclock(&attributes,CLOCK_MONOTONIC);
 if(!result) result=pthread_cond_init(&handle_,&attributes);
 pthread_condattr_destroy(&attributes);check(result);
}
PosixConditionVariable::~PosixConditionVariable() {if(pthread_cond_destroy(&handle_)) std::terminate();}
void PosixConditionVariable::notify_all() noexcept {if(pthread_cond_broadcast(&handle_)) std::terminate();}
Time PosixConditionVariable::deadline_after(Duration interval) {
 const auto now=monotonic_now();
 if(interval>Time::max()-now) throw std::overflow_error("condition wait deadline overflow");
 return now+interval;
}
void PosixConditionVariable::wait_once(std::unique_lock<RuntimeMutex>& lock) {
 check(pthread_cond_wait(&handle_,mutex_handle(lock)));
}
bool PosixConditionVariable::wait_until(std::unique_lock<RuntimeMutex>& lock,Time deadline) {
 const auto seconds=deadline.count()/1000000000;
 if(seconds>std::numeric_limits<time_t>::max()) throw std::overflow_error("condition wait exceeds time_t");
 const timespec absolute{static_cast<time_t>(seconds),static_cast<long>(deadline.count()%1000000000)};
 const auto result=pthread_cond_timedwait(&handle_,mutex_handle(lock),&absolute);
 if(result==ETIMEDOUT) return false;
 check(result);return true;
}
}
