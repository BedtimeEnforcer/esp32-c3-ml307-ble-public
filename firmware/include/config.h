#pragma once
#define ADV_INTERVAL_MS 250
#define SCAN_INTERVAL_MS 100
#define SCAN_WINDOW_MS 90
#define BUTTON_PIN 9
#if ROLE_MASTER
constexpr int SCREEN_SDA=6, SCREEN_SCL=7;
#else
constexpr int SCREEN_SDA=4, SCREEN_SCL=5;
#endif

// User-approved common first-setup credential; upgrades preserve existing NVS.
inline constexpr const char* DEFAULT_MAINTENANCE_PASSWORD="88888888";
