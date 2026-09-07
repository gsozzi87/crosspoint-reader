#pragma once

#include <string>

// Rolling log on the SD (/.crosspoint/device.log, two files rotated at 64 KB):
// every LOG_* line also lands here, with the RTC time when it is set, so a
// problem that happened while the device was away from the cable can still be
// read afterwards. Uploaded to the server on each hub sync and shown at
// /board, and downloadable from the device's own web UI.
namespace devlog {
void begin();                 // opens the file, writes a boot banner
void write(const char* line); // called by Logging for every line
void flush();                 // called before sleeping / rebooting
// Whole log (current + previous), capped at maxBytes from the end.
std::string tail(size_t maxBytes = 32 * 1024);
size_t size();
}  // namespace devlog
