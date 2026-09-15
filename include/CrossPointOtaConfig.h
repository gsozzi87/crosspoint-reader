#pragma once

// Public endpoints are configuration, not credentials. Keep the WS397 default
// in a shared header because both OtaUpdater and ServerCredentialStore need the
// same value: OTA checks use the full manifest URL, while Hub/API calls derive
// its origin when the user has not saved a custom server URL.
#if defined(FREEINK_DEVICE_WS397) && !defined(CROSSPOINT_OTA_RELEASE_URL)
#define CROSSPOINT_OTA_RELEASE_URL "https://paper-esp32.up.railway.app/firmware/latest"
#endif
