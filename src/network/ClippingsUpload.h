#pragma once

#include <cstdint>
#include <string>

#include "StatsUploadClient.h"

struct Clipping;

namespace ClippingsUpload {
// Server identity: first 16 lowercase hex characters of SHA256(decimal timestamp + exact UTF-8 text).
bool identity(uint32_t timestamp, const std::string& text, char (&id)[17]);
// Two-way sync of one book's clippings with crosspoint-sync: downloads clippings
// made on other devices, removes ones deleted elsewhere, sends this device's
// deletions, and uploads clippings the server does not have yet.
StatsUploadClient::Result sync(const std::string& path, const std::string& document);
// Kept for existing callers; same as sync().
StatsUploadClient::Result upload(const std::string& path, const std::string& document);
}  // namespace ClippingsUpload
