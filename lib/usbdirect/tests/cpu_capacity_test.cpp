/* bigCpus() on fixture directories. */
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "check.h"
#include "usbdirect/cpu_capacity.h"

using namespace usbdirect;

static std::string makeRoot() {
    char tmpl[] = "/tmp/usbdirect_cpucap_XXXXXX";
    char *d = mkdtemp(tmpl);
    CHECK(d != nullptr);
    return d;
}
static void cpu(const std::string &root, int n, const char *cap) {
    const std::string dir = root + "/cpu" + std::to_string(n);
    mkdir(dir.c_str(), 0755);
    if (cap) {
        std::FILE *f = std::fopen((dir + "/cpu_capacity").c_str(), "w");
        CHECK(f != nullptr);
        std::fprintf(f, "%s\n", cap);
        std::fclose(f);
    }
}
static void rmRoot(const std::string &root) {
    const std::string cmd = "rm -rf '" + root + "'";
    CHECK(std::system(cmd.c_str()) == 0);
}
typedef std::vector<int> V;

int main() {
    { /* BLU MT6835: cpu0-5 = 350, cpu6-7 = 1024 */
        const std::string r = makeRoot();
        for (int i = 0; i < 6; i++) cpu(r, i, "350");
        cpu(r, 6, "1024");
        cpu(r, 7, "1024");
        mkdir((r + "/cpufreq").c_str(), 0755); /* unrelated entry is ignored */
        CHECK(bigCpus(r) == (V{6, 7}));
        CHECK(cpuMaskOf(bigCpus(r)) == 0xC0u);
        rmRoot(r);
    }
    { /* Fold-like: one 1024, three mid, four little: single top CPU pulls in the mid tier */
        const std::string r = makeRoot();
        for (int i = 0; i < 4; i++) cpu(r, i, "300");
        for (int i = 4; i < 7; i++) cpu(r, i, "700");
        cpu(r, 7, "1024");
        CHECK(bigCpus(r) == (V{4, 5, 6, 7}));
        CHECK(cpuMaskOf(bigCpus(r)) == 0xF0u);
        rmRoot(r);
    }
    { /* all equal capacity: every CPU is "big" */
        const std::string r = makeRoot();
        for (int i = 0; i < 4; i++) cpu(r, i, "1024");
        CHECK(bigCpus(r) == (V{0, 1, 2, 3}));
        rmRoot(r);
    }
    { /* single CPU: nothing to add */
        const std::string r = makeRoot();
        cpu(r, 0, "1024");
        CHECK(bigCpus(r) == (V{0}));
        rmRoot(r);
    }
    { /* cpu_capacity files missing */
        const std::string r = makeRoot();
        for (int i = 0; i < 8; i++) cpu(r, i, nullptr);
        CHECK(bigCpus(r).empty());
        rmRoot(r);
    }
    { /* one file missing: unreliable, no pinning */
        const std::string r = makeRoot();
        for (int i = 0; i < 7; i++) cpu(r, i, "350");
        cpu(r, 7, nullptr);
        CHECK(bigCpus(r).empty());
        rmRoot(r);
    }
    { /* garbage content */
        const std::string r = makeRoot();
        cpu(r, 0, "abc");
        cpu(r, 1, "1024");
        CHECK(bigCpus(r).empty());
        rmRoot(r);
    }
    { /* directory missing / no cpuN entries */
        CHECK(bigCpus("/nonexistent/usbdirect_cpu").empty());
        const std::string r = makeRoot();
        CHECK(bigCpus(r).empty());
        rmRoot(r);
    }
    CHECK(cpuMaskOf({}) == 0 && cpuMaskOf({8, 9}) == 0 && cpuMaskOf({0, 7}) == 0x81u);
    PASS("cpu_capacity");
}
