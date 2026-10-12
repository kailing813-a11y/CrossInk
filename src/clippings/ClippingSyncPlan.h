#pragma once

// Pure decision logic for two-way clipping sync with crosspoint-sync.
//
// No storage or network here, so the rules can be unit-tested on the host.
// ClippingsUpload::sync() feeds it local clippings, the device's deletion log,
// and one remote item at a time, then performs the store/network operations the
// plan asks for.
//
// Matching:
// - A remote item is the same clipping as a local one when their server ids
//   match, or (for clippings made independently on two devices, or ids derived
//   from slightly different text) when they quote the same text in the same
//   chapter. The text comparison ignores spacing, punctuation and case, so a
//   CrossInk clipping with line breaks matches KOReader's single-line text.
// - Deleting locally records the clipping's id and text key. A remote item
//   matching either is deleted on the server instead of being restored.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace clippingSyncPlan {

constexpr size_t ID_SIZE = 17;  // 16 hex characters + terminator (server ids may be up to 64)
constexpr size_t REMOTE_ID_MAX = 65;

// FNV-1a over letters and digits only, seeded by the spine index. ASCII is
// lowercased; whitespace, ASCII punctuation, and common Unicode punctuation
// (quotes, dashes, bullets, no-break and soft hyphens) are skipped.
inline uint32_t textKey(const uint16_t spine, const char* text, const size_t length) {
  uint32_t hash = 2166136261U;
  auto mix = [&hash](const uint8_t byte) {
    hash ^= byte;
    hash *= 16777619U;
  };
  mix(static_cast<uint8_t>(spine & 0xff));
  mix(static_cast<uint8_t>(spine >> 8));
  const auto* p = reinterpret_cast<const uint8_t*>(text);
  const auto* end = p + length;
  while (p < end) {
    const uint8_t c = *p;
    if (c < 0x80) {
      if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')) mix(c);
      if (c >= 'A' && c <= 'Z') mix(static_cast<uint8_t>(c - 'A' + 'a'));
      ++p;
      continue;
    }
    // Multi-byte UTF-8 sequence: decode just enough to classify it.
    size_t n = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
    if (static_cast<size_t>(end - p) < n) n = static_cast<size_t>(end - p);
    uint32_t cp = 0;
    if (n == 2) cp = ((c & 0x1FU) << 6) | (p[1] & 0x3FU);
    if (n == 3) cp = ((c & 0x0FU) << 12) | ((p[1] & 0x3FU) << 6) | (p[2] & 0x3FU);
    const bool punctuation = (cp >= 0x2000 && cp <= 0x206F) || cp == 0x00A0 || cp == 0x00AD || cp == 0x00AB ||
                             cp == 0x00BB || cp == 0x00B7 || cp == 0xFEFF || cp == 0x3000;
    if (!punctuation) {
      for (size_t i = 0; i < n; ++i) mix(p[i]);
    }
    p += n;
  }
  return hash;
}

struct LocalEntry {
  char id[ID_SIZE] = {};
  uint32_t key = 0;
  bool onServer = false;  // the server already holds this clipping (live)
};

struct DeletedEntry {
  char id[ID_SIZE] = {};
  uint32_t key = 0;
};

enum class Action : uint8_t {
  None,          // already in sync
  AddLocal,      // store the remote clipping on this device
  RemoveLocal,   // the clipping was deleted elsewhere; remove local[index]
  DeleteRemote,  // deleted on this device; send a tombstone for the remote id
};

struct Decision {
  Action action = Action::None;
  size_t index = 0;
};

class Plan {
 public:
  std::vector<LocalEntry> local;
  std::vector<DeletedEntry> deleted;

  Decision onRemote(const char* remoteId, const bool remoteDeleted, const uint32_t remoteKey) const {
    if (remoteDeleted) {
      for (size_t i = 0; i < local.size(); ++i) {
        if (std::strcmp(local[i].id, remoteId) == 0) return {Action::RemoveLocal, i};
      }
      return {};
    }
    for (const auto& d : deleted) {
      if (std::strcmp(d.id, remoteId) == 0 || d.key == remoteKey) return {Action::DeleteRemote, 0};
    }
    for (size_t i = 0; i < local.size(); ++i) {
      if (std::strcmp(local[i].id, remoteId) == 0 || local[i].key == remoteKey) return {Action::None, i};
    }
    return {Action::AddLocal, 0};
  }

  // Record the effect of a decision after the store operation succeeded.
  void markOnServer(const size_t index) {
    if (index < local.size()) local[index].onServer = true;
  }
  void removed(const size_t index) {
    if (index < local.size()) local.erase(local.begin() + static_cast<std::ptrdiff_t>(index));
  }
  void added(const char* id, const uint32_t key) {
    LocalEntry entry;
    const size_t n = std::min(std::strlen(id), ID_SIZE - 1);
    std::memcpy(entry.id, id, n);
    entry.id[n] = '\0';
    entry.key = key;
    entry.onServer = true;
    local.push_back(entry);
  }
};

}  // namespace clippingSyncPlan
