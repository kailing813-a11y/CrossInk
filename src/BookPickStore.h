#pragma once

#include <LibraryPick.h>

// Persisted taste model for "Suggest a book" (about 700 bytes at
// /.crosspoint/pick_taste.bin). Loaded on first use; changes are written once
// when the suggestion screen closes rather than on every skip.
//
// Preferences are best-effort: an unreadable or damaged file simply starts a
// fresh model.
class BookPickStore {
 public:
  static BookPickStore& getInstance();

  BookPickStore(const BookPickStore&) = delete;
  BookPickStore& operator=(const BookPickStore&) = delete;

  library::PickTaste& taste() {
    ensureLoaded();
    return model;
  }
  void markDirty() { dirty = true; }
  // Writes only when something changed since the last load or save.
  bool saveIfDirty();
  // Forget everything learned so far and persist the empty model.
  bool reset();

  static const char* getFilePath() { return "/.crosspoint/pick_taste.bin"; }

 private:
  BookPickStore() = default;
  void ensureLoaded();
  bool writeModel() const;

  library::PickTaste model;
  bool loaded = false;
  bool dirty = false;
};

#define BOOK_PICK BookPickStore::getInstance()
