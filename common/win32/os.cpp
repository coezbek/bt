#include "os.h"
#include <Windows.h>
#include "../str.h"

using namespace std;

namespace grey::common::win32::os {
    std::string get_last_error_text() {
        wchar_t err[256]{};
        ::FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, ::GetLastError(),
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), err, 255, nullptr);
        return str::to_str(wstring(err));
    }
}
