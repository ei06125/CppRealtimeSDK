// Verify binary RPC input, configured transformation and shared-memory RPC output end to end.
// Include zero and high-bit bytes so text-only transport mistakes cannot pass unnoticed.
#include <sys/mman.h>
#include "rtx/control.hpp"
#include "rtx/launch.hpp"
#include <grpcpp/grpcpp.h>
#include <array>
#include <sstream>
#include <thread>
#include <iostream>
#include <unistd.h>
using namespace std::chrono_literals;
void require(bool value,const char* message){if(!value) throw std::runtime_error(message);}
int main() {
 try {
  const auto base="/rtx-pipeline-"+std::to_string(getpid());
  struct Cleanup {std::string a,b;~Cleanup(){shm_unlink(a.c_str());shm_unlink(b.c_str());}} cleanup{base+"-in",base+"-out"};
  rtx::SharedMemory input(cleanup.a,8,true),output(cleanup.b,8,true);
  rtx::CallbackRegistry callbacks{{"transform",[&](const rtx::Context& c){
   std::array<std::byte,8> data{};
   const auto size=input.read(data);
   if(size) {for(std::size_t i=0;i<size;++i) data[i]^=std::byte{0x55};output.write(std::span(data).first(size));}
   c.heartbeat();
  }}};
  std::istringstream launch("rtx-launch 1\nnode transform transform 10 1000000 10000000 1000000000\n");
  rtx::Scheduler scheduler(rtx::load_launch(launch,callbacks));
  rtx::PosixTaskRunner runner(scheduler,false);
  rtx::ControlService control(scheduler,runner);
  rtx::MailboxService data({{"input",&input},{"output",&output}});
  grpc::ServerBuilder builder;int port=0;
  builder.AddListeningPort("127.0.0.1:0",grpc::InsecureServerCredentials(),&port);
  builder.RegisterService(&control);builder.RegisterService(&data);
  auto server=builder.BuildAndStart();require(server && port>0,"bind failed");
  std::exception_ptr failure;
  std::jthread execution([&]{try {runner.run();}catch(...) {failure=std::current_exception();}});
  struct Stop {rtx::PosixTaskRunner& runner;~Stop(){runner.request_stop();}} stop{runner};
  auto channel=grpc::CreateChannel("127.0.0.1:"+std::to_string(port),grpc::InsecureChannelCredentials());
  auto stub=rtx::rpc::Mailbox::NewStub(channel);
  auto management=rtx::rpc::Control::NewStub(channel);
  rtx::rpc::Payload message;message.set_channel("input");message.set_data(std::string("\0\x01\xff",3));
  rtx::rpc::Empty empty;
  grpc::ClientContext write;write.set_deadline(std::chrono::system_clock::now()+1s);
  require(stub->Write(&write,message,&empty).ok(),"application write failed");
  bool transformed=false;
  rtx::rpc::Channel request;request.set_name("output");
  const auto limit=std::chrono::steady_clock::now()+2s;
  while(std::chrono::steady_clock::now()<limit) {
   grpc::ClientContext read;read.set_deadline(std::chrono::system_clock::now()+1s);
   rtx::rpc::Payload result;
   require(stub->Read(&read,request,&result).ok(),"application read failed");
   if(result.data()==std::string("\x55\x54\xaa",3)) {transformed=true;break;}
   std::this_thread::sleep_for(1ms);
  }
  require(transformed,"node did not transform shared-memory input");
  request.set_name("unregistered");
  grpc::ClientContext unknown;unknown.set_deadline(std::chrono::system_clock::now()+1s);
  rtx::rpc::Payload ignored;
  require(stub->Read(&unknown,request,&ignored).error_code()==grpc::StatusCode::NOT_FOUND,"unknown channel accepted");
  message.set_data(std::string(9,'x'));
  grpc::ClientContext oversize;oversize.set_deadline(std::chrono::system_clock::now()+1s);
  require(stub->Write(&oversize,message,&empty).error_code()==grpc::StatusCode::RESOURCE_EXHAUSTED,"oversize channel write accepted");
  grpc::ClientContext shutdown;shutdown.set_deadline(std::chrono::system_clock::now()+1s);
  require(management->Stop(&shutdown,empty,&empty).ok(),"remote stop failed");
  execution.join();if(failure) std::rethrow_exception(failure);
  require(scheduler.health()[0].state==rtx::State::stopped,"pipeline did not stop");
  server->Shutdown();std::cout<<"gRPC binary input -> shared memory -> configured node -> gRPC output passed\n";
 }catch(const std::exception& ex){std::cerr<<ex.what()<<'\n';return 1;}
}
