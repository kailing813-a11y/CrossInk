#pragma once

// "Suggest a book": a tiny, bounded taste model plus a weighted random draw.
//
// Deliberately free of Arduino/HalStorage so it is host-testable
// (test/library_pick). The device layer supplies hashes and persistence.
//
// Memory is fixed (~700 bytes) no matter how large the library is. Instead of
// scoring every book, the caller samples a handful of random books, scores only
// those with weight(), and draws one with pickWeighted(). Cost therefore stays
// constant on a 30,000-book card.
//
// Signals:
//   - per-book skips: each time a suggested book is declined its weight halves,
//     down to a floor, so it is rarer but never impossible.
//   - author / genre affinity: small signed counters in 64 hashed buckets.
//     Accepting a suggestion nudges them up, declining nudges them down.
//     Hash collisions only blur the preference slightly, which is fine for a
//     soft ranking.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace library {

class PickTaste {
 public:
  static constexpr uint8_t BUCKETS = 64;
  static constexpr uint8_t SKIP_SLOTS = 48;
  static constexpr int8_t AFFINITY_LIMIT = 12;
  static constexpr uint8_t MAX_SKIP_SHIFT = 5;
  static constexpr uint16_t BASE_WEIGHT = 256;

  // A hash of 0 means "unknown" (no author or genre) and is treated as neutral.
  struct Candidate {
    uint64_t pathHash = 0;
    uint64_t authorHash = 0;
    uint64_t genreHash = 0;
  };

  // magic(4) version(1) tick(4) authors(64) genres(64) slots(48 * 13)
  static constexpr size_t SERIALIZED_SIZE = 4 + 1 + 4 + BUCKETS + BUCKETS + SKIP_SLOTS * 13;

  // Relative likelihood of offering this book; always >= 1.
  uint16_t weight(const Candidate& c) const {
    uint32_t w = BASE_WEIGHT;
    w >>= std::min<uint8_t>(skipCount(c.pathHash), MAX_SKIP_SHIFT);
    w = w * static_cast<uint32_t>(16 + affinity(authors, c.authorHash)) / 16;
    w = w * static_cast<uint32_t>(16 + affinity(genres, c.genreHash)) / 16;
    return static_cast<uint16_t>(std::max<uint32_t>(1, w));
  }

  // The user declined this suggestion.
  void recordSkip(const Candidate& c) {
    tick++;
    Slot* slot = findSlot(c.pathHash);
    if (!slot) slot = claimSlot();
    slot->hash = c.pathHash;
    if (slot->count < UINT8_MAX) slot->count++;
    slot->lastTick = tick;
    nudge(authors, c.authorHash, -1);
    nudge(genres, c.genreHash, -1);
  }

  // The user chose to open this suggestion.
  void recordAccept(const Candidate& c) {
    tick++;
    if (Slot* slot = findSlot(c.pathHash)) *slot = Slot{};
    nudge(authors, c.authorHash, 2);
    nudge(genres, c.genreHash, 2);
  }

  uint8_t skipCount(const uint64_t pathHash) const {
    if (pathHash == 0) return 0;
    for (const Slot& slot : slots) {
      if (slot.count != 0 && slot.hash == pathHash) return slot.count;
    }
    return 0;
  }

  int8_t authorAffinity(const uint64_t hash) const { return affinity(authors, hash); }
  int8_t genreAffinity(const uint64_t hash) const { return affinity(genres, hash); }

  void clear() { *this = PickTaste{}; }

  // Fixed little-endian layout so a card written by one build reads in another.
  size_t serialize(uint8_t* out, const size_t capacity) const {
    if (capacity < SERIALIZED_SIZE) return 0;
    size_t at = 0;
    std::memcpy(out, MAGIC, 4);
    at += 4;
    out[at++] = VERSION;
    put32(out, at, tick);
    for (const int8_t value : authors) out[at++] = static_cast<uint8_t>(value);
    for (const int8_t value : genres) out[at++] = static_cast<uint8_t>(value);
    for (const Slot& slot : slots) {
      put64(out, at, slot.hash);
      out[at++] = slot.count;
      put32(out, at, slot.lastTick);
    }
    return at;
  }

  // Rejects anything of the wrong size, magic or version and leaves *this empty.
  bool deserialize(const uint8_t* in, const size_t size) {
    clear();
    if (size != SERIALIZED_SIZE || std::memcmp(in, MAGIC, 4) != 0 || in[4] != VERSION) return false;
    size_t at = 5;
    tick = get32(in, at);
    for (int8_t& value : authors) value = clampAffinity(static_cast<int8_t>(in[at++]));
    for (int8_t& value : genres) value = clampAffinity(static_cast<int8_t>(in[at++]));
    for (Slot& slot : slots) {
      slot.hash = get64(in, at);
      slot.count = in[at++];
      slot.lastTick = get32(in, at);
    }
    return true;
  }

 private:
  struct Slot {
    uint64_t hash = 0;
    uint8_t count = 0;
    uint32_t lastTick = 0;
  };

  static constexpr char MAGIC[4] = {'P', 'K', 'T', '1'};
  static constexpr uint8_t VERSION = 1;

  static int8_t clampAffinity(const int8_t value) {
    return static_cast<int8_t>(std::clamp<int>(value, -AFFINITY_LIMIT, AFFINITY_LIMIT));
  }
  static int8_t affinity(const int8_t (&table)[BUCKETS], const uint64_t hash) {
    return hash == 0 ? 0 : table[hash % BUCKETS];
  }
  static void nudge(int8_t (&table)[BUCKETS], const uint64_t hash, const int delta) {
    if (hash == 0) return;
    int8_t& value = table[hash % BUCKETS];
    value = static_cast<int8_t>(std::clamp<int>(value + delta, -AFFINITY_LIMIT, AFFINITY_LIMIT));
  }

  Slot* findSlot(const uint64_t hash) {
    for (Slot& slot : slots) {
      if (slot.count != 0 && slot.hash == hash) return &slot;
    }
    return nullptr;
  }
  // Free slot first; otherwise evict the least-skipped, then least-recent entry.
  Slot* claimSlot() {
    Slot* victim = &slots[0];
    for (Slot& slot : slots) {
      if (slot.count == 0) return &slot;
      if (slot.count < victim->count || (slot.count == victim->count && slot.lastTick < victim->lastTick)) {
        victim = &slot;
      }
    }
    *victim = Slot{};
    return victim;
  }

  static void put32(uint8_t* out, size_t& at, const uint32_t v) {
    for (int i = 0; i < 4; i++) out[at++] = static_cast<uint8_t>(v >> (8 * i));
  }
  static void put64(uint8_t* out, size_t& at, const uint64_t v) {
    for (int i = 0; i < 8; i++) out[at++] = static_cast<uint8_t>(v >> (8 * i));
  }
  static uint32_t get32(const uint8_t* in, size_t& at) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(in[at++]) << (8 * i);
    return v;
  }
  static uint64_t get64(const uint8_t* in, size_t& at) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= static_cast<uint64_t>(in[at++]) << (8 * i);
    return v;
  }

  int8_t authors[BUCKETS]{};
  int8_t genres[BUCKETS]{};
  Slot slots[SKIP_SLOTS]{};
  uint32_t tick = 0;
};

// How much the reader has engaged with a book, from data the device already
// keeps: the per-book session count (reading stats) and the book's position in
// the recent-books list.
struct PickEngagement {
  // Times the book was opened; 0 when never opened or stats are not tracked.
  uint16_t sessions = 0;
  // 0 = most recently opened; RECENT_NONE when not in the recent-books list.
  uint8_t recentRank = RECENT_NONE;

  static constexpr uint8_t RECENT_NONE = UINT8_MAX;
};

// Scales a taste weight by engagement so sampling many openings still lets the
// books you keep coming back to rise:
//   - never opened: unchanged, so new books stay in steady rotation
//   - in the recent list and returned to: x1.5 at two sessions, rising to x2.5
//     at four or more; a single sampled opening stays unchanged (the most
//     recent book is left unchanged too: it is already one tap away on Home,
//     and suggesting it would just repeat the last session)
//   - opened but fallen out of the recent list: faded, x1/4 if opened once,
//     x1/2 if returned to; rarer but never impossible
// Always returns >= 1.
inline uint16_t engagementWeight(const uint16_t tasteWeight, const PickEngagement& e) {
  const bool recent = e.recentRank != PickEngagement::RECENT_NONE;
  // A book in the recent list has been opened at least once, even with stats off.
  const uint16_t sessions = recent ? std::max<uint16_t>(e.sessions, 1) : e.sessions;
  uint32_t w = tasteWeight;
  if (sessions == 0 || e.recentRank == 0) {
    // unchanged
  } else if (recent) {
    w = w * (1 + std::min<uint16_t>(sessions, 4)) / 2;
  } else {
    w = sessions == 1 ? w / 4 : w / 2;
  }
  return static_cast<uint16_t>(std::clamp<uint32_t>(w, 1, UINT16_MAX));
}

// Draw an index in [0, count) with probability proportional to weights[i].
// `randomValue` is any uniform 32-bit random number. Returns `count` when there
// is nothing to draw from.
inline size_t pickWeighted(const uint16_t* weights, const size_t count, const uint32_t randomValue) {
  uint32_t total = 0;
  for (size_t i = 0; i < count; i++) total += weights[i];
  if (total == 0) return count;
  uint32_t target = randomValue % total;
  for (size_t i = 0; i < count; i++) {
    if (target < weights[i]) return i;
    target -= weights[i];
  }
  return count - 1;
}

// Fill `out` with up to `want` distinct ordinals in [0, bookCount). Returns how
// many were written. Small `want` only: the duplicate check is linear.
template <typename Rng>
inline size_t drawDistinctOrdinals(const uint16_t bookCount, const size_t want, uint16_t* out, Rng&& nextRandom) {
  const size_t target = std::min<size_t>(want, bookCount);
  size_t written = 0;
  // Bounded attempts: a tiny library would otherwise spin on duplicates.
  for (size_t attempt = 0; written < target && attempt < target * 8 + 8; attempt++) {
    const uint16_t ordinal = static_cast<uint16_t>(nextRandom() % bookCount);
    bool seen = false;
    for (size_t i = 0; i < written; i++) seen = seen || out[i] == ordinal;
    if (!seen) out[written++] = ordinal;
  }
  return written;
}

}  // namespace library
