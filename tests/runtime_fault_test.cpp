// Verify that a later callback failure faults its node while healthy workers keep running.
// This differs from an initial launch failure, which must abort startup.
#include "rtx/posix.hpp"
#include <iostream>
#include <atomic>
#include <stdexcept>
#include <thread>
using namespace std::chrono_literals;
int main() {
 std::atomic<bool> healthy_started=false;
 std::uint64_t failed_tick=0;
 rtx::Scheduler scheduler({{"faulty",1,1ms,1s,0ns,{},[&](const rtx::Context& c){if(c.tick>1 && healthy_started.load()) {failed_tick=c.tick;throw std::runtime_error("runtime failure");}}},
                           {"healthy",2,1ms,1s,0ns,{"faulty"},[&](const rtx::Context& c){c.heartbeat();healthy_started=true;}}});
 rtx::PosixTaskRunner runner(scheduler,false,100us);
 std::exception_ptr failure;
 std::jthread execution([&]{try {runner.run();}catch(...) {failure=std::current_exception();}});
 bool isolated=false;
 std::uint64_t baseline=0;
 const auto limit=std::chrono::steady_clock::now()+2s;
 while(std::chrono::steady_clock::now()<limit) {
  auto h=scheduler.health();
  if(h[0].state==rtx::State::faulted) {
   if(!baseline) baseline=h[1].ticks+1;
   if(h[1].ticks>=baseline+3 && h[1].state==rtx::State::running) {isolated=true;break;}
  }
  std::this_thread::sleep_for(1ms);
 }
 runner.request_stop();execution.join();
 auto h=scheduler.health();
 if(failure || !isolated || h[0].ticks!=failed_tick || failed_tick<2 || h[0].fault!="runtime failure" || h[1].state!=rtx::State::stopped) {
  std::cerr<<"runtime callback fault isolation failed\n";return 1;
 }
 std::cout<<"healthy worker continued after peer callback fault\n";
}
