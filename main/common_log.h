#pragma once
#include "esp_log.h"
#include "esp_system.h"
#include <cstdio>

// --- Macros for Logging ---
// --- Macros for Logging (Direct Printf) ---
// We use printf directly to bypass the disabled ESP_LOG system, which is disabled in the sdkconfig
// This keeps the binary small by not including library log strings.
#define INFO(format, ...) printf("I (%lu) %s: " format "\r\n", esp_log_timestamp(), TAG, ##__VA_ARGS__)
#define WARN(format, ...) printf("W (%lu) %s: " format "\r\n", esp_log_timestamp(), TAG, ##__VA_ARGS__)
#define FAIL(format, ...) do { printf("E (%lu) %s: " format "\r\n", esp_log_timestamp(), TAG, ##__VA_ARGS__); esp_restart(); } while (0)
