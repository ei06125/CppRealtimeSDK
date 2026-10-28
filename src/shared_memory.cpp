// Own Linux mappings and serialize payload access with a robust process-shared mutex.
// Setup failures release creator resources; owner-death recovery discards potentially partial data.
#include "rtx/shared_memory.hpp"
#include "shared_memory_layout.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <stdexcept>
#include <system_error>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits>
namespace rtx {
namespace {
using Header=detail::SharedMemoryHeader;
void check(int result) {if(result) throw std::system_error(result,std::generic_category(),"shared memory");}
struct Lock {
 Header* h;
 explicit Lock(void* p):h(static_cast<Header*>(p)) {
  int result=pthread_mutex_lock(&h->mutex);
  if(result==EOWNERDEAD) {
   h->length=0;
   const int consistent=pthread_mutex_consistent(&h->mutex);
   if(consistent) {pthread_mutex_unlock(&h->mutex);check(consistent);}
  }
  else check(result);
 }
 ~Lock(){pthread_mutex_unlock(&h->mutex);}
};
}
SharedMemory::SharedMemory(const std::string& name,std::size_t capacity,bool create):capacity_(capacity) {
 if(name.size()<2 || name[0]!='/' || name.find('/',1)!=std::string::npos || capacity==0 || capacity>std::numeric_limits<std::size_t>::max()-sizeof(Header)) throw std::invalid_argument("invalid shared memory name or capacity");
 size_=sizeof(Header)+capacity;
 if(size_>static_cast<std::size_t>(std::numeric_limits<off_t>::max())) throw std::invalid_argument("shared memory size exceeds file offset range");
 int fd=shm_open(name.c_str(),O_RDWR|(create?(O_CREAT|O_EXCL):0),0600);
 if(fd<0) check(errno);
 try {
  if(create && ftruncate(fd,static_cast<off_t>(size_))!=0) check(errno);
  struct stat status{}; if(fstat(fd,&status)!=0) check(errno);
  if(status.st_size!=static_cast<off_t>(size_)) throw std::invalid_argument("shared memory capacity mismatch");
  mapping_=mmap(nullptr,size_,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
  if(mapping_==MAP_FAILED) {mapping_=nullptr; check(errno);}
  if(create) {
   auto* h=static_cast<Header*>(mapping_); h->length=0;
   pthread_mutexattr_t attr; check(pthread_mutexattr_init(&attr));
   int result=pthread_mutexattr_setpshared(&attr,PTHREAD_PROCESS_SHARED);
   if(!result) result=pthread_mutexattr_setrobust(&attr,PTHREAD_MUTEX_ROBUST);
   if(!result) result=pthread_mutex_init(&h->mutex,&attr);
   pthread_mutexattr_destroy(&attr); check(result);
  }
 } catch(...) {if(mapping_) munmap(mapping_,size_); close(fd); if(create) shm_unlink(name.c_str()); throw;}
 close(fd);
}
SharedMemory::~SharedMemory(){if(mapping_) munmap(mapping_,size_);}
void SharedMemory::unlink(const std::string& name){if(shm_unlink(name.c_str())!=0 && errno!=ENOENT) check(errno);}
void SharedMemory::write(std::span<const std::byte> data) {
 if(data.size()>capacity_) throw std::length_error("mailbox capacity exceeded");
 Lock lock(mapping_); if(!data.empty()) std::memcpy(static_cast<char*>(mapping_)+sizeof(Header),data.data(),data.size()); lock.h->length=data.size();
}
std::size_t SharedMemory::read(std::span<std::byte> destination) {
 Lock lock(mapping_);
 if(lock.h->length>capacity_) throw std::runtime_error("corrupt mailbox length");
 if(destination.size()<lock.h->length) throw std::length_error("destination too small");
 if(lock.h->length) std::memcpy(destination.data(),static_cast<char*>(mapping_)+sizeof(Header),lock.h->length);
 return lock.h->length;
}
}
