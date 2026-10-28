// Exercise startup failure, pre-start stop and repeated concurrent stop sessions.
// Check final health after joins rather than assuming a stop request completed execution.
#include "rtx/posix.hpp"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace std::chrono_literals;
void require(bool value,const char* message){if(!value) throw std::runtime_error(message);}
int main() {
 try {
  // All callbacks remain suppressed after a pre-start stop, including repeated stops.
  unsigned calls=0;
  rtx::Scheduler early({{"early",1,1ms,1ms,0ns,{},[&](auto&){++calls;}}});
  rtx::PosixTaskRunner stopped(early,false);stopped.request_stop();stopped.run();
  require(calls==0 && early.health()[0].state==rtx::State::stopped,"pre-start stop failed");
  unsigned dependent=0;
  rtx::Scheduler launch({{"fails",1,1ms,1ms,0ns,{},[](auto&){throw std::runtime_error("launch failure");}},
                         {"dependent",1,1ms,1ms,0ns,{"fails"},[&](auto&){++dependent;}}});
  rtx::PosixTaskRunner broken(launch,false);bool rejected=false;
  try {broken.run();}catch(const std::runtime_error&){rejected=true;}
  auto health=launch.health();
  require(rejected && !dependent && health[0].state==rtx::State::faulted && health[1].state==rtx::State::stopped,"launch fault did not suppress dependent");
  // Repeatedly race stop against thread creation, launch gates, and health reads.
  for(unsigned iteration=0;iteration<150;++iteration) {
   rtx::Scheduler scheduler({{"a",1,100us,1ms,0ns,{},[](auto& c){c.heartbeat();}},
                             {"b",2,100us,1ms,0ns,{"a"},[](auto& c){c.heartbeat();}}});
   rtx::PosixTaskRunner runner(scheduler,false,50us);
   std::exception_ptr failure;std::atomic<bool> done=false;
   std::atomic<bool> valid=true;
   std::jthread observer([&]{while(!done.load()) {auto h=scheduler.health();if(h.size()!=2) valid=false;std::this_thread::yield();}});
   std::jthread execution([&]{try {runner.run();}catch(...) {failure=std::current_exception();}done=true;});
   std::jthread stop_one([&]{if(iteration%2) std::this_thread::sleep_for(100us);runner.request_stop();});
   std::jthread stop_two([&]{runner.request_stop();runner.request_stop();});
   stop_one.join();stop_two.join();execution.join();observer.join();
   if(failure) std::rethrow_exception(failure);
   require(valid.load(),"concurrent health inspection invalid");
   for(const auto& h:scheduler.health()) require(h.state==rtx::State::stopped,"worker did not stop");
  }
  std::cout<<"pre-start stop, launch failure, and 150 concurrent stop sessions passed\n";
 }catch(const std::exception& ex){std::cerr<<ex.what()<<'\n';return 1;}
}
