#pragma once
#include "common/str.h"
#include <string>
#include <Windows.h>

namespace bt {
    struct click_payload {
        std::string url;

        bool app_mode{false};

        HWND source_window_handle;  // handle of the system window where the click came from

        // everything below is populated from source_window_handle
        std::string window_title;
        std::string process_id;
        std::string process_path;
        std::string process_name;
        std::string process_description;

        /**
         * Original URL before any modifications.
         */
        std::string raw_url;

        /**
         * Used by ClearURLs to count the number of trackers removed from the URL
         */
        int trackers_removed{0};

        [[nodiscard]] bool empty() const {
            return url.empty() && window_title.empty() && process_name.empty();
        }

        void clear(bool leave_url = false) {
            if(!leave_url) {
                url.clear();
            }
            window_title.clear();
            process_name.clear();
        }

        void prettify() {
            grey::common::str::trim(window_title);
            grey::common::str::trim(process_description);
            if(!process_description.empty()) {
                grey::common::str::replace_all(process_description, "(GUI launcher)", "");
                grey::common::str::trim(process_description);
            }
        }
    };
}