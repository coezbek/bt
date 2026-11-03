#include "startup_trace.h"
#include <string>
#include <vector>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#include <fstream>
#include <filesystem>
#endif

namespace bt::trace {
#ifdef _WIN32
    static std::vector<std::pair<std::string, double>>& marks() {
        static std::vector<std::pair<std::string, double>> m;
        return m;
    }

    static double since_start_ms() {
        FILETIME created, exited, kernel, user, now;
        ::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user);
        ::GetSystemTimePreciseAsFileTime(&now);
        ULARGE_INTEGER a, b;
        a.LowPart = created.dwLowDateTime; a.HighPart = created.dwHighDateTime;
        b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
        return (b.QuadPart - a.QuadPart) / 10000.0;
    }

    void mark(const char* name) { marks().emplace_back(name, since_start_ms()); }

    void flush() {
        if(marks().empty()) return;
        wchar_t tmp[MAX_PATH];
        ::GetTempPathW(MAX_PATH, tmp);
        std::ofstream f{std::filesystem::path{tmp} / "bt-startup.log", std::ios::app};
        f << "pid=" << ::GetCurrentProcessId();
        for(auto& m : marks()) f << " " << m.first << "=" << static_cast<int>(m.second);
        f << "\n";
        marks().clear();
    }
#else
    void mark(const char*) {}
    void flush() {}
#endif
}
