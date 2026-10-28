// Provide bounded heap growth, allocator serialization and trusted QEMU semihosting services.
// These board stubs are reference integration code, not production ECU I/O or fault recovery.
#include <FreeRTOS.h>
#include <task.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <reent.h>
extern "C" char _end, _heap_end;
extern "C" void* _sbrk(std::ptrdiff_t increment) {
 static auto next=reinterpret_cast<std::uintptr_t>(&_end);
 const auto limit=reinterpret_cast<std::uintptr_t>(&_heap_end);
 if(increment<0 || next>limit || static_cast<std::size_t>(increment)>limit-next) {
  errno=ENOMEM;return reinterpret_cast<void*>(-1);
 }
 const auto result=next;next+=static_cast<std::size_t>(increment);return reinterpret_cast<void*>(result);
}
// newlib's shared allocator must remain serialized across task preemption.
extern "C" void __malloc_lock(struct _reent*) {vTaskSuspendAll();}
extern "C" void __malloc_unlock(struct _reent*) {xTaskResumeAll();}
extern "C" void board_print(const char* message) {
 register std::uint32_t operation asm("r0")=4;
 register const char* argument asm("r1")=message;
 asm volatile("bkpt 0xab" : "+r"(operation) : "r"(argument) : "memory");
}
extern "C" [[noreturn]] void board_exit(int code) {
 const std::uint32_t arguments[]{0x20026,static_cast<std::uint32_t>(code)};
 register std::uint32_t operation asm("r0")=0x20;
 register const void* argument asm("r1")=arguments;
 asm volatile("bkpt 0xab" : "+r"(operation) : "r"(argument) : "memory");
 for(;;) {}
}
extern "C" {void* __dso_handle=&__dso_handle;}
extern "C" void board_assert_failure() {
 board_print("FAIL: FreeRTOS assertion\n");board_exit(1);
}

extern "C" void board_fault() {board_print("FAIL: unhandled Cortex-M exception\n");board_exit(1);}

extern "C" void vApplicationMallocFailedHook() {
 board_print("FAIL: FreeRTOS allocation failure\n");board_exit(1);
}
extern "C" void vApplicationStackOverflowHook(TaskHandle_t,char*) {
 board_print("FAIL: FreeRTOS stack overflow\n");board_exit(1);
}
extern "C" void board_print_unsigned(unsigned value) {
 char digits[11];unsigned count=0;
 do {digits[count++]=static_cast<char>('0'+value%10);value/=10;} while(value);
 char output[11];
 for(unsigned i=0;i<count;++i) output[i]=digits[count-1-i];
 output[count]='\0';board_print(output);
}
