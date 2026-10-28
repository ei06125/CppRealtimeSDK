// Validate native wait boundaries and interruption of a long dispatcher idle interval.
// Stop must notify the wait predicate rather than waiting for the polling resolution to expire.
#include "rtx/posix.hpp"
#include <atomic>
#include <future>
#include <iostream>
#include <thread>
using namespace std::chrono_literals;
int main() {
 try {
  // Native waits use CLOCK_MONOTONIC with checked absolute deadlines.
  rtx::RuntimeMutex mutex;
  rtx::detail::PosixConditionVariable wake;
  std::unique_lock lock(mutex);
  if(!wake.wait_for(lock,1s,[]{return true;}) || wake.wait_for(lock,0ns,[]{return false;}))
   throw std::runtime_error("condition predicate or immediate timeout failed");
  const auto begin=rtx::monotonic_now();
  if(wake.wait_for(lock,2ms,[]{return false;}) || rtx::monotonic_now()-begin<2ms || !lock.owns_lock())
   throw std::runtime_error("condition timeout was early or lost mutex ownership");
  bool rejected=false;
  try {wake.wait_for(lock,rtx::Duration::max(),[]{return false;});}
  catch(const std::overflow_error&) {rejected=true;}
  if(!rejected) throw std::runtime_error("condition deadline overflow was accepted");
  lock.unlock();rejected=false;
  try {wake.wait(lock,[]{return false;});}catch(const std::logic_error&){rejected=true;}
  if(!rejected) throw std::runtime_error("unowned condition mutex was accepted");
  std::promise<void> entered;
  std::atomic<unsigned> calls=0;
  rtx::Scheduler scheduler({{"node",1,10s,1s,0ns,{},[&](auto&){if(++calls==1) entered.set_value();}}});
  rtx::PosixRunner runner(scheduler,5s);
  std::exception_ptr failure;
  std::jthread execution([&]{try {runner.run();}catch(...) {failure=std::current_exception();}});
  struct Guard {rtx::PosixRunner& runner;~Guard(){runner.request_stop();}} guard{runner};
  if(entered.get_future().wait_for(2s)!=std::future_status::ready) throw std::runtime_error("callback never started");
  // Allow the dispatcher to enter its five-second idle wait.
  std::this_thread::sleep_for(20ms);
  const auto start=std::chrono::steady_clock::now();
  runner.request_stop();execution.join();
  const auto elapsed=std::chrono::steady_clock::now()-start;
  if(failure) std::rethrow_exception(failure);
  if(elapsed>1s || calls.load()!=1 || scheduler.health()[0].state!=rtx::State::stopped) throw std::runtime_error("stop waited for dispatcher resolution");
  std::cout<<"stop interrupted a five-second idle wait in "<<std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()<<" us\n";
 }catch(const std::exception& ex){std::cerr<<ex.what()<<'\n';return 1;}
}
