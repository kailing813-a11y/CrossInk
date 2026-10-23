#pragma once

#include <LibraryIndexFile.h>
#include <LibraryPick.h>

#include <cstdint>
#include <string>

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"

// "Suggest a Book": shows one randomly chosen unfinished book from the Library
// index with its cover, title and metadata, and asks "Open Book?".
//
//   Confirm / tap Open    -> returns the book's path (FilePathResult) to open it
//   Next / tap Another    -> remembers the decline and suggests a different book
//   Back                  -> closes without learning anything
//
// The draw is weighted by BookPickStore: books you keep skipping come up less
// often, and authors/genres you open or skip shift the odds a little. Books you
// keep returning to (recent and opened several times) come up more often, and
// books opened once and then left fade (see library::engagementWeight). Only a
// small random sample of the index is scored per suggestion, so time and memory
// stay constant regardless of library size.
//
// Opened from Home or the Library menu. The caller must close its own handle on
// the Library index first: real SD hardware allows one open reader per file.
// From Home the existing index is used as-is (no rescan); books that have since
// disappeared from the card are skipped.
class SuggestBookActivity final : public Activity {
 public:
  SuggestBookActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Books scored per suggestion. Bounded so a huge library costs the same.
  static constexpr size_t SAMPLE_SIZE = 12;
  // Recently shown ordinals are not offered again this visit.
  static constexpr size_t SHOWN_HISTORY = 8;

  struct Suggestion {
    bool valid = false;
    uint16_t ordinal = UINT16_MAX;
    library::PickTaste::Candidate taste;
    std::string path;
    std::string title;
    std::string author;
    std::string series;
    std::string genre;
    std::string thumbPath;
    float progress = -1.0f;
    uint32_t fileSize = 0;
    uint8_t fileType = 0;
  };

  enum class State : uint8_t { Ready, NoIndex, NoBooks };

  library::LibraryIndexFile index;
  uint8_t visibleTypes = 0;
  Suggestion current;
  State state = State::Ready;
  uint16_t shown[SHOWN_HISTORY]{};
  uint8_t shownCount = 0;
  uint8_t shownNext = 0;
  bool ignoreConfirmRelease = false;
  int coverWidth = 0;
  int coverHeight = 0;
  // Touch targets, laid out by render() and read by loop().
  Rect coverRect;
  Rect openRect;
  Rect anotherRect;

  void computeCoverSize();
  bool wasShown(uint16_t ordinal) const;
  void rememberShown(uint16_t ordinal);
  bool readCandidate(uint16_t ordinal, Suggestion& out, bool full);
  bool isEligible(const Suggestion& candidate, library::PickEngagement& engagement);
  void suggestNext();
  void prepareThumbnail();
  void openCurrent();
  void skipCurrent();
  void drawCover() const;
  void drawDetails(int top, int bottom) const;
  void drawTouchButton(const Rect& rect, const char* label, bool primary) const;
};
