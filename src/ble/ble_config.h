// Auto-generated from packages/ble-config/config-multibook.json - DO NOT EDIT
// Re-generate by running: pnpm setup (from monorepo root)
#pragma once

#include <cstdint>

namespace lesefluss::ble {

constexpr int PROTOCOL_VERSION = 1;
constexpr const char* DEVICE_NAME = "RSVP Nano";
constexpr const char* SERVICE_UUID = "58fc3a69-6f17-45e2-a9d1-575f33a76219";
constexpr const char* INFO_CHAR_UUID = "3249061a-2d65-434b-b710-0c701985a0ce";
constexpr const char* LIBRARY_CHAR_UUID = "63a9cd8f-d1bb-4e4d-bdf8-8cb81bf69cae";
constexpr const char* ACTIVE_CHAR_UUID = "e4454473-669a-46c1-8d45-32d49f528d88";
constexpr const char* POSITION_CHAR_UUID = "90320c4d-6aee-453b-be85-b73b34b74350";
constexpr const char* TRANSFER_CHAR_UUID = "b44dac85-2b67-4fbf-bdb3-a829b6a36a39";
constexpr const char* SETTINGS_CHAR_UUID = "2e79ab9f-4877-4cac-9927-973a4c1f9717";
constexpr const char* STORAGE_CHAR_UUID = "f5988368-a011-42db-bc3a-b61d0ec8e325";
constexpr const char* DELETE_CHAR_UUID = "fd6d4fe1-2269-469b-8917-3f152961e902";
constexpr uint32_t CHUNK_SIZE = 509;
constexpr uint32_t WINDOW_SIZE = 2;
constexpr uint32_t MAX_RETRIES = 3;
constexpr uint32_t ACK_TIMEOUT_MS = 5000;

} // namespace lesefluss::ble
