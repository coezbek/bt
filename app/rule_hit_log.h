#pragma once
#include <string>
#include "browser.h"
#include <optional>
#include <iterator> // due to bug in p-ranav/csv2
#include <csv2/writer.hpp>

#include "model.h"

namespace bt {
    class rule_hit_log {
    public:
        rule_hit_log();

        void write(const click_payload& up, const profile_selection& sel,
            std::optional<picker_invoked_reason> picker_reason,
            std::optional<browser_match_result> rule_match);

        std::string get_absolute_path() { return path; }

        /**
         * @brief Global instance. Opens the log file on first use.
         */
        static rule_hit_log& instance();

    private:
        std::string path;
        std::string rotation_date;
        std::ofstream stream;
        csv2::Writer<csv2::delimiter<','>> writer;

        void rotate_if_needed();
        void write_header();
    };
}
