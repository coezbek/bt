#include "globals.h"
#include "common/fss.h"

bt::script_site g_script;
bt::state g_state;
bt::config_store g_config;
bt::url_pipeline g_pipeline{g_state};

void bt_init_core() {
    g_config.load(g_state);
    g_script.open_file(grey::common::fss::get_config_file_path(CONFIG_NAME, "scripts.lua"));
    g_pipeline.load();
}
