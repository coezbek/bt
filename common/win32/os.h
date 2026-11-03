#pragma once
#include <string>

namespace grey::common::win32::os {
    /**
     * @brief Calls ::GetLastError() and returns the error text.
     */
    std::string get_last_error_text();
}
