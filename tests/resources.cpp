// Report ABI object sizes and a newly created pthread stack/guard reservation.
// These host observations exclude callback stack high-water and application-owned allocations.
#include "rtx/scheduler.hpp"
#include "rtx/posix.hpp"
#include "../src/shared_memory_layout.hpp"
#include <iostream>
#include <thread>
#include <pthread.h>
int main() {
 std::size_t stack=0,guard=0;int error=0;
 std::jthread probe([&]{
  pthread_attr_t attr;
  error=pthread_getattr_np(pthread_self(),&attr);
  if(error) return;
  void* base=nullptr;
  error=pthread_attr_getstack(&attr,&base,&stack);
  if(!error) error=pthread_attr_getguardsize(&attr,&guard);
  pthread_attr_destroy(&attr);
 });
 probe.join();
 if(error || !stack) {std::cerr<<"cannot inspect worker stack attributes\n";return 1;}
 std::cout<<"{\n  \"node_object_bytes\":"<<sizeof(rtx::Node)<<",\n  \"health_object_bytes\":"<<sizeof(rtx::Health)<<",\n  \"scheduler_object_bytes\":"<<sizeof(rtx::Scheduler)<<",\n  \"mailbox_header_bytes\":"<<sizeof(rtx::detail::SharedMemoryHeader)<<",\n  \"default_worker_stack_bytes\":"<<stack<<",\n  \"default_worker_guard_bytes\":"<<guard<<"\n}\n";
}
