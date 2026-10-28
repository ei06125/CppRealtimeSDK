// Translate scheduler snapshots and registered mailbox operations into RPC responses.
// External callers select application aliases, never arbitrary shared-memory object names.
#include "rtx/control.hpp"
namespace rtx {
grpc::Status ControlService::GetHealth(grpc::ServerContext*,const rpc::Empty*,rpc::HealthReport* report) {
 for(const auto& h:scheduler_.health()) {
  auto* node=report->add_nodes(); node->set_name(h.name);
  switch(h.state) {
   case State::configured:node->set_state("configured");break;
   case State::running:node->set_state("running");break;
   case State::stopped:node->set_state("stopped");break;
   case State::faulted:node->set_state("faulted");break;
  }
  node->set_ticks(h.ticks); node->set_deadline_misses(h.deadline_misses);
  node->set_skipped_releases(h.skipped_releases); node->set_heartbeat_ns(h.heartbeat.count()); node->set_fault(h.fault);
 }
 return grpc::Status::OK;
}
grpc::Status ControlService::Stop(grpc::ServerContext*,const rpc::Empty*,rpc::Empty*) {
 stop_(); return grpc::Status::OK;
}
MailboxService::MailboxService(std::unordered_map<std::string,SharedMemory*> channels):channels_(std::move(channels)) {
 for(const auto& [name,mailbox]:channels_) if(name.empty() || !mailbox || mailbox->capacity()>16*1024*1024)
  throw std::invalid_argument("invalid channel registration (maximum capacity 16 MiB)");
}
grpc::Status MailboxService::Read(grpc::ServerContext*,const rpc::Channel* request,rpc::Payload* response) {
 auto channel=channels_.find(request->name());
 if(channel==channels_.end()) return {grpc::StatusCode::NOT_FOUND,"unknown channel"};
 try {
  std::vector<std::byte> buffer(channel->second->capacity());
  const auto size=channel->second->read(buffer);
  response->set_channel(request->name());
  response->set_data(reinterpret_cast<const char*>(buffer.data()),size);
  return grpc::Status::OK;
 }catch(const std::exception&) {return {grpc::StatusCode::INTERNAL,"mailbox read failed"};}
}
grpc::Status MailboxService::Write(grpc::ServerContext*,const rpc::Payload* request,rpc::Empty*) {
 auto channel=channels_.find(request->channel());
 if(channel==channels_.end()) return {grpc::StatusCode::NOT_FOUND,"unknown channel"};
 if(request->data().size()>channel->second->capacity()) return {grpc::StatusCode::RESOURCE_EXHAUSTED,"mailbox capacity exceeded"};
 try {
  channel->second->write(std::as_bytes(std::span(request->data().data(),request->data().size())));
  return grpc::Status::OK;
 }catch(const std::exception&) {return {grpc::StatusCode::INTERNAL,"mailbox write failed"};}
}
}
