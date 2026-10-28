// Small launch-file-driven capture/perception example with a bounded execution lifetime.
// The registered callbacks model node activity; they do not implement ADAS algorithms.
#include <thread>
#include <fstream>
#include "rtx/launch.hpp"
#include "rtx/posix.hpp"
#include <iostream>
using namespace std::chrono_literals;
int main(int argc,char** argv) {
 try {
 unsigned frames=0;
 if(argc!=2) {std::cerr<<"usage: adas_demo CONFIG.launch\n";return 2;}
 std::ifstream config(argv[1]);
 if(!config) throw std::runtime_error("cannot open launch configuration");
 rtx::CallbackRegistry callbacks{
  {"capture",[&](const rtx::Context& c){++frames;c.heartbeat();}},
  {"infer",[](const rtx::Context& c){c.heartbeat();}}
 };
 rtx::Scheduler scheduler(rtx::load_launch(config,callbacks));
 rtx::PosixRunner runner(scheduler);
 // Bounded example without a signal handler or detached threads.
 std::jthread timer([&]{std::this_thread::sleep_for(100ms);runner.request_stop();});
 runner.run();
 std::cout<<"camera frames: "<<frames<<'\n';
 }catch(const std::exception& ex){std::cerr<<ex.what()<<'\n';return 1;}
}
