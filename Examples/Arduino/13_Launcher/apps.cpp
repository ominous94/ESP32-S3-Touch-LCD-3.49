// Glue file — Arduino only auto-compiles .cpp in the sketch root.
// Subdirectory sources must be #included from a root-level translation unit.
#include "apps/codex_status/app_codex_status.cpp"
#include "apps/imu_test/app_imu_test.cpp"
#include "apps/ocean_water/ocean_flip.cpp"
#include "apps/ocean_water/app_ocean_water.cpp"
#include "apps/sd_browser/app_sd_browser.cpp"
#include "apps/settings/app_settings.cpp"
#include "apps/wifi_config/app_wifi_config.cpp"
