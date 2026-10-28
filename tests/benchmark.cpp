// Measure synthetic scheduler dispatch overhead over a fixed number of releases.
// The supplied clock removes OS wakeup noise; this is not an application latency measurement.
#include "rtx/posix.hpp"
#include <iostream>
using namespace std::chrono_literals;
int main() {
 rtx::Scheduler s({{"node",1,1ms,1ms,0ns,{},[](const rtx::Context&){}}});
 s.start(0ns);
 auto start=rtx::monotonic_now();
 for(int i=0;i<100000;++i) s.step(i*1ms,[i]{return i*1ms;});
 auto elapsed=rtx::monotonic_now()-start;
 std::cout<<"dispatch mean ns: "<<elapsed.count()/100000<<" (100000 synthetic releases)\n";
 return s.health()[0].ticks==100000 ? 0 : 1;
}
