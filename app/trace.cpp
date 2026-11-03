#include "trace.h"
#include <windows.h>
#include <fstream>
#include <format>
#include "globals.h"
#include "common/fss.h"

namespace bt {
    void trace(const std::string& line) {
        static const bool enabled = ::GetEnvironmentVariableW(L"BT_TRACE", nullptr, 0) > 0;
        if(!enabled) return;
        static LARGE_INTEGER freq{}, start{};
        if(!freq.QuadPart) {
            ::QueryPerformanceFrequency(&freq);
            ::QueryPerformanceCounter(&start);
        }
        LARGE_INTEGER now;
        ::QueryPerformanceCounter(&now);
        double ms = static_cast<double>(now.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
        static std::ofstream f{grey::common::fss::get_config_file_path(CONFIG_NAME, "trace.log"), std::ios::app};
        f << std::format("[{}] {:9.2f} ms  {}\n", ::GetCurrentProcessId(), ms, line);
        f.flush();
    }
}
