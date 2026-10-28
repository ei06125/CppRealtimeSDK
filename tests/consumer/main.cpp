// Consume only installed public headers and exported targets from a relocated package.
// Exercise enabled components without relying on the source tree or build-tree include paths.
#include <rtx/launch.hpp>
#include <rtx/mailbox.hpp>
#include <sstream>
#ifdef HAS_POSIX
#include <rtx/posix.hpp>
#endif
#ifdef HAS_GRPC
#include <rtx/control.hpp>
#endif
int main() {
 rtx::Mailbox<8> mailbox;
 std::array<std::byte,8> payload{};
 mailbox.write(payload);
 if(mailbox.read(payload)!=payload.size()) return 3;
 unsigned ticks=0;
 rtx::CallbackRegistry callbacks{{"run",[&](auto&){++ticks;}}};
 std::istringstream config("rtx-launch 1\nnode example run 1 1000000 1000000 0\n");
 rtx::Scheduler scheduler(rtx::load_launch(config,callbacks));
 scheduler.start(rtx::Time{});
 scheduler.step(rtx::Time{},[]{return rtx::Time{};});
 scheduler.stop();
#ifdef HAS_POSIX
 rtx::PosixRunner runner(scheduler);
 if(rtx::monotonic_now().count()<=0) return 2;
#endif
#ifdef HAS_GRPC
 rtx::ControlService service(scheduler,runner);
#endif
 return ticks==1?0:1;
}
