#ifndef USBDIRECT_CPU_CAPACITY_H
#define USBDIRECT_CPU_CAPACITY_H
/* Big-core discovery from /sys/devices/system/cpu/cpuN/cpu_capacity (header-only, no Android API).
 *
 * bigCpus(): CPUs with the maximum capacity, plus the next lower capacity tier when the top tier is a
 * single CPU. Returns an empty vector (= do not pin) when there is no cpuN directory, or any cpuN
 * directory lacks a readable, parseable cpu_capacity file. Result is sorted ascending.
 * cpuMaskOf(): bitmask of the CPUs below 8 (usbdirect::Options::cpuMask covers CPUs 0..7 only). */
#include <sys/stat.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace usbdirect {

inline std::vector<int> bigCpus(const std::string &cpuDir = "/sys/devices/system/cpu") {
    std::map<long, std::vector<int>> byCapacity;
    int found = 0;
    for (int cpu = 0; cpu < 64; cpu++) {
        const std::string dir = cpuDir + "/cpu" + std::to_string(cpu);
        struct stat st;
        if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        found++;
        std::FILE *f = std::fopen((dir + "/cpu_capacity").c_str(), "r");
        if (!f) return {};
        long cap = -1;
        const int n = std::fscanf(f, "%ld", &cap);
        std::fclose(f);
        if (n != 1 || cap <= 0) return {};
        byCapacity[cap].push_back(cpu);
    }
    if (!found || byCapacity.empty()) return {};
    auto top = byCapacity.rbegin();
    std::vector<int> out = top->second;
    if (out.size() == 1 && byCapacity.size() > 1) {
        const auto &next = std::next(top)->second;
        out.insert(out.end(), next.begin(), next.end());
    }
    std::sort(out.begin(), out.end());
    return out;
}

inline uint32_t cpuMaskOf(const std::vector<int> &cpus) {
    uint32_t m = 0;
    for (int c : cpus)
        if (c >= 0 && c < 8) m |= 1u << c;
    return m;
}

} // namespace usbdirect
#endif
