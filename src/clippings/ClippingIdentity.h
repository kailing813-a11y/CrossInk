#pragma once

#include <cstdint>
#include <string>

namespace clippingIdentity {
// Server identity shared with crosspoint-sync and KOReader: the first 16
// lowercase hex characters of SHA256(decimal Unix timestamp + exact UTF-8 text).
// Pass clippingUnixTimestamp(saved) so clippings without a calendar date use 0.
bool compute(uint32_t timestamp, const std::string& text, char (&id)[17]);
}  // namespace clippingIdentity
