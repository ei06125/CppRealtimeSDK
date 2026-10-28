// Demonstrate that independent supervision detects a callback that has stopped making progress.
// A watchdog fault prevents future invocation; it does not cancel an active callback.
#include "rtx/posix.hpp"
#include <condition_variable>
#include <iostream>
#include <thread>
#include <stdexcept>
using namespace std::chrono_literals;
int main() {
 std::mutex mutex;
 std::condition_variable wake;
 bool entered=false,released=false;
 unsigned calls=0;
 rtx::Scheduler scheduler({{"blocked",1,1ms,1s,20ms,{},[&](const rtx::Context& c){
  c.heartbeat();
  std::unique_lock lock(mutex);++calls;entered=true;wake.notify_all();
  wake.wait(lock,[&]{return released;});
  c.heartbeat(); // A late heartbeat must not recover a faulted node.
  throw std::runtime_error("late callback failure");
 }}});
 rtx::PosixRunner runner(scheduler);
 std::exception_ptr failure;
 std::jthread execution([&]{try {runner.run();}catch(...) {failure=std::current_exception();}});
 bool observed=false,started=false;
 {
  std::unique_lock lock(mutex);
  started=wake.wait_for(lock,2s,[&]{return entered;});
 }
 const auto limit=std::chrono::steady_clock::now()+2s;
 while(started && std::chrono::steady_clock::now()<limit) {
  if(scheduler.health()[0].state==rtx::State::faulted) {observed=true;break;}
  std::this_thread::sleep_for(1ms);
 }
 // Release only after observing the fault; timeout detection must happen during blockage.
 {
  std::lock_guard lock(mutex);released=true;
 }
 wake.notify_all();
 runner.request_stop();execution.join();
 try {
  if(failure) std::rethrow_exception(failure);
  auto health=scheduler.health()[0];
  if(!started || !observed || calls!=1 || health.state!=rtx::State::faulted || health.fault!="heartbeat timeout")
   throw std::runtime_error("blocked callback watchdog supervision failed");
  std::cout<<"independent watchdog detected blocked callback\n";
 }catch(const std::exception& ex){std::cerr<<ex.what()<<'\n';return 1;}
}
