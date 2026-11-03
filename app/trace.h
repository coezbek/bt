#pragma once
#include <string>

namespace bt {
    /**
     * @brief Appends a timestamped line to trace.log in the config folder when the BT_TRACE
     * environment variable is set. For diagnosing the resident process; costs one getenv otherwise.
     */
    void trace(const std::string& line);
}
