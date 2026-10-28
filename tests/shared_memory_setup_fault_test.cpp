// Force mapping and mutex setup failures through GNU linker wrappers.
// Verify creator cleanup while preserving pre-existing shared-memory objects owned by other peers.
#include "rtx/shared_memory.hpp"
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <pthread.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
namespace {int failure_stage=0;}
extern "C" int __real_ftruncate(int,off_t);
extern "C" void* __real_mmap(void*,std::size_t,int,int,int,off_t);
extern "C" int __real_pthread_mutex_init(pthread_mutex_t*,const pthread_mutexattr_t*);
extern "C" int __wrap_ftruncate(int fd,off_t size) {
 if(failure_stage==1) {errno=EIO;return -1;}
 return __real_ftruncate(fd,size);
}
extern "C" void* __wrap_mmap(void* address,std::size_t size,int protection,int flags,int fd,off_t offset) {
 if(failure_stage==2) {errno=ENOMEM;return MAP_FAILED;}
 return __real_mmap(address,size,protection,flags,fd,offset);
}
extern "C" int __wrap_pthread_mutex_init(pthread_mutex_t* mutex,const pthread_mutexattr_t* attributes) {
 if(failure_stage==3) return EAGAIN;
 return __real_pthread_mutex_init(mutex,attributes);
}
std::size_t descriptors() {
 std::size_t count=0;
 for(const auto& entry:std::filesystem::directory_iterator("/proc/self/fd")) {static_cast<void>(entry);++count;}
 return count;
}
int main() {
 const auto name="/rtx-setup-failure-"+std::to_string(getpid());
 struct Cleanup {std::string name;~Cleanup(){shm_unlink(name.c_str());}} cleanup{name};
 try {
  for(int stage=1;stage<=3;++stage) {
   const auto before=descriptors();bool rejected=false;failure_stage=stage;
   try {rtx::SharedMemory channel(name,64,true);}catch(const std::system_error&){rejected=true;}
   failure_stage=0;
   int fd=shm_open(name.c_str(),O_RDWR,0);const auto open_error=errno;
   if(fd>=0) close(fd);
   if(!rejected || fd>=0 || open_error!=ENOENT || descriptors()!=before) throw std::runtime_error("failed setup leaked object or descriptor");
  }
  rtx::SharedMemory owner(name,64,true);bool duplicate=false;
  try {rtx::SharedMemory second(name,64,true);}catch(const std::system_error& ex){duplicate=ex.code().value()==EEXIST;}
  rtx::SharedMemory peer(name,64,false);
  if(!duplicate || peer.capacity()!=64) throw std::runtime_error("duplicate creator damaged existing object");
  const auto before=descriptors();bool mismatch=false;
  try {rtx::SharedMemory bad_peer(name,63,false);}catch(const std::invalid_argument&){mismatch=true;}
  rtx::SharedMemory intact(name,64,false);
  if(!mismatch || descriptors()!=before || intact.capacity()!=64) throw std::runtime_error("failed peer open leaked or removed owner object");
  std::cout<<"shared-memory setup failures clean resources and preserve existing objects\n";
 }catch(const std::exception& ex){std::cerr<<ex.what()<<'\n';return 1;}
}
