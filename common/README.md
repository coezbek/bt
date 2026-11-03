# common

Small non-UI helpers (strings, files, processes, registry, HTTP) taken from
[grey](https://github.com/aloneguid/grey) at tag `202610`, under its MIT licence (`LICENSE-grey`).

Browser Tamer used to pull all of grey, including its ImGui UI layer. The Win32 build only needs
these helpers, so they live here and the ImGui dependency is gone. `colour.h` replaces grey's
ImGui-based `rgb_colour`.
