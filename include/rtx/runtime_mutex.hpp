// Select the synchronization primitive that matches the library and consumer ABI.
// FreeRTOS uses native mutexes, Linux uses priority inheritance, other hosts use std::mutex.
#pragma once
#include <mutex>
#ifdef RTX_FREERTOS
#include <FreeRTOS.h>
#include <semphr.h>
#include <new>
#include <stdexcept>
namespace rtx {
class RuntimeMutex {
 public:
 RuntimeMutex():handle_(xSemaphoreCreateMutex()){if(!handle_) throw std::bad_alloc();}
 ~RuntimeMutex(){vSemaphoreDelete(handle_);}
 RuntimeMutex(const RuntimeMutex&)=delete;
 RuntimeMutex& operator=(const RuntimeMutex&)=delete;
 void lock(){if(xSemaphoreTake(handle_,portMAX_DELAY)!=pdTRUE) throw std::runtime_error("FreeRTOS mutex take failed");}
 void unlock(){if(xSemaphoreGive(handle_)!=pdTRUE) std::terminate();}
 private:
 SemaphoreHandle_t handle_;
};
}
#elif defined(__linux__)
#include <pthread.h>
#include <system_error>
namespace rtx {
// Keep a low-priority state-lock owner runnable when a FIFO supervisor waits.
class RuntimeMutex {
 public:
 RuntimeMutex() {
  pthread_mutexattr_t attributes;
  check(pthread_mutexattr_init(&attributes));
  int result=pthread_mutexattr_setprotocol(&attributes,PTHREAD_PRIO_INHERIT);
  if(!result) result=pthread_mutex_init(&handle_,&attributes);
  pthread_mutexattr_destroy(&attributes);
  check(result);
 }
 ~RuntimeMutex(){if(pthread_mutex_destroy(&handle_)) std::terminate();}
 RuntimeMutex(const RuntimeMutex&)=delete;
 RuntimeMutex& operator=(const RuntimeMutex&)=delete;
 pthread_mutex_t* native_handle() noexcept {return &handle_;}
 void lock(){check(pthread_mutex_lock(&handle_));}
 void unlock() noexcept {if(pthread_mutex_unlock(&handle_)) std::terminate();}
 private:
 static void check(int result) {if(result) throw std::system_error(result,std::generic_category(),"priority-inheritance mutex");}
 pthread_mutex_t handle_;
};
}
#else
namespace rtx {using RuntimeMutex=std::mutex;}
#endif
