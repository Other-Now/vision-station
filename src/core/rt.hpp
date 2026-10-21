#pragma once
// Real-time knobs for the calling thread: SCHED_FIFO priority and CPU pinning.
// Linux only; elsewhere they report "unsupported" and do nothing.
#include <string>

namespace vs::rt {

struct Result {
  bool ok = false;
  std::string detail;  // e.g. "SCHED_FIFO 80" or "EPERM (needs root/CAP_SYS_NICE)"
};

Result set_fifo(int priority);   // 1..99
Result pin_to_cpu(int cpu);
Result lock_memory();            // mlockall(MCL_CURRENT|MCL_FUTURE)
std::string describe_self();     // current policy/priority/affinity

}  // namespace vs::rt
