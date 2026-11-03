#pragma once

// DEBUG ONLY - not for upstream.
// Records named timestamps (ms since process creation) and appends them as one line to
// %TEMP%\bt-startup.log, to see where start-up time goes.
namespace bt::trace {
    void mark(const char* name);
    void flush();
}
