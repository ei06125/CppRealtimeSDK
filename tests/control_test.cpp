// Exercise health and stop requests through a real gRPC server/client round trip.
// Bound client requests and preserve referenced service objects until server shutdown.
#include "rtx/control.hpp"
#include <grpcpp/grpcpp.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <stdexcept>
using namespace std::chrono_literals;
void require(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
int main() {
 try {
  rtx::Scheduler scheduler({{"sensor",5,1ms,10ms,1s,{},[](const rtx::Context& c){c.heartbeat();}}});
  rtx::PosixRunner runner(scheduler);
  rtx::ControlService service(scheduler,runner);
  grpc::ServerBuilder builder;
  int port=0;
  builder.AddListeningPort("127.0.0.1:0",grpc::InsecureServerCredentials(),&port);
  builder.RegisterService(&service);
  auto server=builder.BuildAndStart();
  require(server && port>0,"server bind failed");
  std::exception_ptr failure;
  std::jthread execution([&]{try {runner.run();} catch(...) {failure=std::current_exception();}});
  // Always request stop before joining, including assertion and RPC failures.
  struct StopGuard {rtx::PosixRunner& runner; ~StopGuard(){runner.request_stop();}} guard{runner};
  auto channel=grpc::CreateChannel("127.0.0.1:"+std::to_string(port),grpc::InsecureChannelCredentials());
  auto stub=rtx::rpc::Control::NewStub(channel);
  rtx::rpc::Empty empty;
  rtx::rpc::HealthReport report;
  const auto limit=std::chrono::steady_clock::now()+3s;
  bool active=false;
  while(std::chrono::steady_clock::now()<limit) {
   grpc::ClientContext context;
   context.set_deadline(std::chrono::system_clock::now()+1s);
   report.Clear();
   require(stub->GetHealth(&context,empty,&report).ok(),"GetHealth RPC failed");
   require(report.nodes_size()==1 && report.nodes(0).name()=="sensor","health identity mismatch");
   if(report.nodes(0).state()=="running" && report.nodes(0).ticks()>0) {active=true;break;}
   std::this_thread::sleep_for(1ms);
  }
  require(active,"runner never became active");
  require(report.nodes(0).heartbeat_ns()>0,"heartbeat missing");
  grpc::ClientContext stop_context;
  stop_context.set_deadline(std::chrono::system_clock::now()+1s);
  rtx::rpc::Empty response;
  require(stub->Stop(&stop_context,empty,&response).ok(),"Stop RPC failed");
  execution.join();
  if(failure) std::rethrow_exception(failure);
  grpc::ClientContext final_context;
  final_context.set_deadline(std::chrono::system_clock::now()+1s);
  report.Clear();
  require(stub->GetHealth(&final_context,empty,&report).ok(),"final health RPC failed");
  require(report.nodes(0).state()=="stopped","remote stop did not stop scheduler");
  server->Shutdown();
  std::cout<<"gRPC health and stop round trip passed\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
