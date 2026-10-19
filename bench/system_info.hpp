#pragma once

#include <fstream>
#include <string>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace lob::bench {

inline std::string cpu_model() {
#if defined(__APPLE__)
  char buf[256];
  std::size_t len = sizeof buf;
  if (sysctlbyname("machdep.cpu.brand_string", buf, &len, nullptr, 0) == 0) return buf;
#elif defined(__linux__)
  std::ifstream f("/proc/cpuinfo");
  for (std::string line; std::getline(f, line);) {
    if (line.rfind("model name", 0) == 0) return line.substr(line.find(':') + 2);
  }
#endif
  return "unknown CPU";
}

inline std::string compiler() {
#if defined(__clang__)
  return "clang " __clang_version__;
#elif defined(__GNUC__)
  return "gcc " __VERSION__;
#else
  return "unknown compiler";
#endif
}

}  // namespace lob::bench
