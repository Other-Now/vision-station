#include "core/rt.hpp"

#include <cerrno>
#include <cstring>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#endif

namespace vs::rt {

#ifdef __linux__
static std::string err(int e) {
  std::string s = std::strerror(e);
  if (e == EPERM) s += " (needs root or CAP_SYS_NICE / RLIMIT_RTPRIO)";
  return s;
}

Result set_fifo(int priority) {
  sched_param sp{};
  sp.sched_priority = priority;
  int e = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
  if (e != 0) return {false, "SCHED_FIFO " + std::to_string(priority) + ": " + err(e)};
  return {true, "SCHED_FIFO " + std::to_string(priority)};
}

Result pin_to_cpu(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  int e = pthread_setaffinity_np(pthread_self(), sizeof set, &set);
  if (e != 0) return {false, "pin cpu" + std::to_string(cpu) + ": " + err(e)};
  return {true, "cpu" + std::to_string(cpu)};
}

Result lock_memory() {
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) return {false, "mlockall: " + err(errno)};
  return {true, "mlockall"};
}

std::string describe_self() {
  int policy = 0;
  sched_param sp{};
  pthread_getschedparam(pthread_self(), &policy, &sp);
  cpu_set_t set;
  CPU_ZERO(&set);
  pthread_getaffinity_np(pthread_self(), sizeof set, &set);
  std::string cpus;
  for (int i = 0; i < CPU_SETSIZE; ++i)
    if (CPU_ISSET(i, &set)) cpus += (cpus.empty() ? "" : ",") + std::to_string(i);
  const char* pn = policy == SCHED_FIFO ? "FIFO" : policy == SCHED_RR ? "RR" : "OTHER";
  return std::string(pn) + "/" + std::to_string(sp.sched_priority) + " cpus=" + cpus;
}
#else
Result set_fifo(int) { return {false, "unsupported on this OS"}; }
Result pin_to_cpu(int) { return {false, "unsupported on this OS"}; }
Result lock_memory() { return {false, "unsupported on this OS"}; }
std::string describe_self() { return "default"; }
#endif

}  // namespace vs::rt
