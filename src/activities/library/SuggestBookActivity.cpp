#include "SuggestBookActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryFileTypes.h>
#include <LibraryText.h>
#include <Memory.h>
#include <Xtc.h>

#include <algorithm>
#include <cstdio>

#include "BookPickStore.h"
#include "RecentBooksStore.h"
#include "activities/home/BookActions.h"
#include "activities/home/RecentBookProgress.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int SECTION_GAP = 14;
constexpr int TOUCH_BUTTON_HEIGHT = 56;
constexpr int TOUCH_BUTTON_GAP = 16;
constexpr int TOUCH_BUTTON_RADIUS = 8;

// Folded so "Le Guin, Ursula K." and "le guin, ursula k." share preferences.
uint64_t foldedHash(const std::string& text) {
  if (text.empty()) return 0;
  const std::string folded = library::fold(text);
  return folded.empty() ? 0 : library::clixPathHash(folded.data(), folded.size());
}

const char* fileTypeLabel(const uint8_t type) {
  switch (type) {
    case library::FileEpub:
      return "EPUB";
    case library::FileXtc:
      return "XTC";
    case library::FileTxt:
      return "TXT";
    case library::FileMarkdown:
      return "Markdown";
    default:
      return "";
  }
}

// Arduino's random() is backed by the ESP32 hardware RNG on device and by
// std::rand in the simulator.
uint32_t randomWord() { return static_cast<uint32_t>(random(0x7FFFFFFF)); }

void formatSize(const uint32_t bytes, char* out, const size_t len) {
  if (bytes >= 1024u * 1024u) {
    snprintf(out, len, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else {
    snprintf(out, len, "%u KB", static_cast<unsigned>((bytes + 1023u) / 1024u));
  }
}
}  // namespace

SuggestBookActivity::SuggestBookActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("SuggestBook", renderer, mappedInput) {}

void SuggestBookActivity::onEnter() {
  Activity::onEnter();
  // Confirm opened the menu entry that launched us; its release is not an answer.
  ignoreConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  // Same file types the Library shows.
  visibleTypes = (SETTINGS.libraryShowEpub ? library::FileEpub : 0) | (SETTINGS.libraryShowXtc ? library::FileXtc : 0) |
                 (SETTINGS.libraryShowTxt ? library::FileTxt : 0) |
                 (SETTINGS.libraryShowMarkdown ? library::FileMarkdown : 0);
  computeCoverSize();
  {
    RenderLock lock;
    current.path.reserve(256);
    if (!index.open(library::libraryIndexPath())) {
      LOG_ERR("PICK", "Cannot open library index (%s)", library::clixValidityName(index.validity()));
      state = State::NoIndex;
    }
  }
  if (state == State::Ready) {
    // Generating a first-time cover can take a moment on large images.
    {
      RenderLock lock;
      GUI.drawPopup(renderer, tr(STR_LOADING));
    }
    suggestNext();
  }
  requestUpdate(true);
}

void SuggestBookActivity::onExit() {
  if (!BOOK_PICK.saveIfDirty()) LOG_ERR("PICK", "Cannot save suggestion preferences");
  index.close();
  Activity::onExit();
}

void SuggestBookActivity::computeCoverSize() {
  int top = 0;
  int right = 0;
  int bottom = 0;
  int left = 0;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const int usableHeight = renderer.getScreenHeight() - top - bottom;
  const int usableWidth = renderer.getScreenWidth() - left - right;
  // Roughly the top 45% of the page, at the 2:3 ratio used for book thumbnails.
  coverHeight = std::max(120, usableHeight * 45 / 100);
  coverWidth = coverHeight * 2 / 3;
  if (coverWidth > usableWidth * 3 / 4) {
    coverWidth = usableWidth * 3 / 4;
    coverHeight = coverWidth * 3 / 2;
  }
}

bool SuggestBookActivity::wasShown(const uint16_t ordinal) const {
  for (uint8_t i = 0; i < shownCount; i++) {
    if (shown[i] == ordinal) return true;
  }
  return false;
}

void SuggestBookActivity::rememberShown(const uint16_t ordinal) {
  shown[shownNext] = ordinal;
  shownNext = static_cast<uint8_t>((shownNext + 1) % SHOWN_HISTORY);
  if (shownCount < SHOWN_HISTORY) shownCount++;
}

// `full` also reads display strings; scoring only needs the hashes.
bool SuggestBookActivity::readCandidate(const uint16_t ordinal, Suggestion& out, const bool full) {
  library::ClixRecord record{};
  std::string name;
  if (!index.readRecord(ordinal, record) || !index.readName(record, name)) return false;
  out.fileType = library::fileTypeFor(name);
  if ((out.fileType & visibleTypes) == 0) return false;
  out.ordinal = ordinal;
  out.fileSize = record.fileSize;
  if (!index.readPathHash(record, out.taste.pathHash) || !index.readPath(record, out.path) ||
      !index.readDisplayText(record, out.title, out.author) || !index.readGenre(record, out.genre)) {
    LOG_ERR("PICK", "Cannot read library record %u", static_cast<unsigned>(ordinal));
    return false;
  }
  out.taste.authorHash = foldedHash(out.author);
  out.taste.genreHash = foldedHash(out.genre);
  if (full && !index.readSeries(record, out.series)) out.series.clear();
  return true;
}

bool SuggestBookActivity::isEligible(const Suggestion& candidate) {
  // A finished book is a poor answer to "what should I read next?".
  return Storage.exists(candidate.path.c_str()) && !BookActions::isBookCompletedForList(candidate.path);
}

void SuggestBookActivity::suggestNext() {
  const uint16_t count = index.bookCount();
  if (count == 0) {
    RenderLock lock;
    current = Suggestion{};
    state = State::NoBooks;
    return;
  }

  uint16_t sample[SAMPLE_SIZE];
  uint16_t weights[SAMPLE_SIZE];
  uint16_t eligible[SAMPLE_SIZE];
  size_t eligibleCount = 0;
  Suggestion scratch;
  scratch.path.reserve(256);
  const library::PickTaste& taste = BOOK_PICK.taste();
  // Several rounds so a library that is mostly finished or filtered still finds
  // something; each round is a fresh random sample. The last rounds allow books
  // shown recently (even the current one) so the only unread book is not lost.
  constexpr int STRICT_ROUNDS = 4;
  constexpr int TOTAL_ROUNDS = 6;
  for (int round = 0; round < TOTAL_ROUNDS && eligibleCount == 0; round++) {
    const bool strict = round < STRICT_ROUNDS;
    const size_t drawn = library::drawDistinctOrdinals(count, SAMPLE_SIZE, sample, [] { return randomWord(); });
    for (size_t i = 0; i < drawn; i++) {
      if (strict && wasShown(sample[i])) continue;
      if (strict && current.valid && sample[i] == current.ordinal) continue;
      if (!readCandidate(sample[i], scratch, false) || !isEligible(scratch)) continue;
      eligible[eligibleCount] = sample[i];
      weights[eligibleCount] = taste.weight(scratch.taste);
      eligibleCount++;
    }
  }

  if (eligibleCount == 0) {
    RenderLock lock;
    current = Suggestion{};
    state = State::NoBooks;
    return;
  }
  const size_t chosen = library::pickWeighted(weights, eligibleCount, randomWord());
  Suggestion next;
  next.path.reserve(256);
  if (chosen >= eligibleCount || !readCandidate(eligible[chosen], next, true)) {
    RenderLock lock;
    current = Suggestion{};
    state = State::NoBooks;
    return;
  }
  next.valid = true;
  RecentBook progressKey;
  progressKey.path = next.path;
  next.progress = next.fileType == library::FileEpub ? RecentBookProgress::loadCachedEpubPercent(progressKey)
                                                     : RecentBookProgress::loadPercent(progressKey);
  rememberShown(next.ordinal);
  {
    RenderLock lock;
    current = std::move(next);
    state = State::Ready;
  }
  prepareThumbnail();
}

// Creates (or reuses) a cover thumbnail sized for this screen. Text formats have
// no cover and get a drawn placeholder instead.
void SuggestBookActivity::prepareThumbnail() {
  std::string thumb;
  if (current.fileType == library::FileEpub) {
    auto epub = makeUniqueNoThrow<Epub>(current.path, "/.crosspoint");
    if (!epub) {
      LOG_ERR("PICK", "Cannot allocate EPUB for thumbnail");
    } else {
      thumb = epub->getThumbBmpPath(coverWidth, coverHeight);
      if (!Storage.exists(thumb.c_str()) &&
          !epub->generateThumbBmpFromSource(coverWidth, coverHeight, &renderer, SETTINGS.getReaderFontId())) {
        thumb.clear();
      }
    }
  } else if (current.fileType == library::FileXtc) {
    auto xtc = makeUniqueNoThrow<Xtc>(current.path, "/.crosspoint");
    if (!xtc) {
      LOG_ERR("PICK", "Cannot allocate XTC for thumbnail");
    } else {
      const auto w = static_cast<uint16_t>(coverWidth);
      const auto h = static_cast<uint16_t>(coverHeight);
      thumb = xtc->getThumbBmpPath(w, h);
      if (!Storage.exists(thumb.c_str()) && (!xtc->load() || !xtc->generateThumbBmp(w, h))) thumb.clear();
    }
  }
  if (!thumb.empty() && !Storage.exists(thumb.c_str())) thumb.clear();
  RenderLock lock;
  current.thumbPath = std::move(thumb);
}

void SuggestBookActivity::openCurrent() {
  if (!current.valid) return;
  BOOK_PICK.taste().recordAccept(current.taste);
  BOOK_PICK.markDirty();
  FilePathResult picked;
  picked.path = current.path;
  setResult(ActivityResult(std::move(picked)));
  finish();
}

void SuggestBookActivity::skipCurrent() {
  if (current.valid) {
    BOOK_PICK.taste().recordSkip(current.taste);
    BOOK_PICK.markDirty();
  }
  if (state != State::NoIndex) {
    // The next cover may take a moment to generate on first use.
    {
      RenderLock lock;
      GUI.drawPopup(renderer, tr(STR_LOADING));
    }
    suggestNext();
  }
  requestUpdate();
}

void SuggestBookActivity::loop() {
  using Button = MappedInputManager::Button;
  if (ignoreConfirmRelease) {
    if (mappedInput.wasReleased(Button::Confirm) || !mappedInput.isPressed(Button::Confirm)) {
      ignoreConfirmRelease = false;
    }
    return;
  }

  if (mappedInput.wasReleased(Button::Back) || TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    ActivityResult cancelled;
    cancelled.isCancelled = true;
    setResult(std::move(cancelled));
    finish();
    return;
  }

  if (mappedInput.wasReleased(Button::Confirm)) {
    openCurrent();
    return;
  }
  if (mappedInput.wasReleased(Button::Right) || mappedInput.wasReleased(Button::Down)) {
    skipCurrent();
    return;
  }

  if (mappedInput.hasTouch()) {
    int x = 0;
    int y = 0;
    if (!mappedInput.wasScreenTapped(x, y)) return;
    const auto inside = [x, y](const Rect& r) {
      return r.width > 0 && x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
    };
    if (inside(openRect) || inside(coverRect)) {
      openCurrent();
    } else if (inside(anotherRect)) {
      skipCurrent();
    }
  }
}

void SuggestBookActivity::drawCover() const {
  renderer.drawRect(coverRect.x - 1, coverRect.y - 1, coverRect.width + 2, coverRect.height + 2);
  if (!current.thumbPath.empty()) {
    FsFile file;
    if (Storage.openFileForRead("PICK", current.thumbPath, file)) {
      Bitmap bitmap(file);
      const bool drawn = bitmap.parseHeaders() == BmpReaderError::Ok &&
                         renderer.drawBitmap(bitmap, coverRect.x, coverRect.y, coverRect.width, coverRect.height);
      file.close();
      if (drawn) return;
    }
  }
  // No artwork: a plain card with the title, like a cloth-bound spine label.
  renderer.fillRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, false);
  const int pad = 12;
  const int fontId = renderer.filenameFontId(UI_12_FONT_ID);
  const auto lines =
      renderer.wrappedText(fontId, current.title.c_str(), coverRect.width - 2 * pad, 5, EpdFontFamily::BOLD);
  const int lineHeight = renderer.getLineHeight(fontId);
  int y = coverRect.y + (coverRect.height - static_cast<int>(lines.size()) * lineHeight) / 2;
  for (const auto& line : lines) {
    const int w = renderer.getTextWidth(fontId, line.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(fontId, coverRect.x + (coverRect.width - w) / 2, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += lineHeight;
  }
}

void SuggestBookActivity::drawDetails(const int top, const int bottom) const {
  int left = 0;
  int right = 0;
  int unusedTop = 0;
  int unusedBottom = 0;
  renderer.getOrientedViewableTRBL(&unusedTop, &right, &unusedBottom, &left);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int textWidth = renderer.getScreenWidth() - left - right - 2 * metrics.contentSidePadding;
  int y = top;
  // Book metadata can be in any script; the filename copy of each UI font
  // fills glyphs the built-in font lacks (S3 builds with a UI Font chosen).
  const auto centered = [&](const int baseFontId, const std::string& text, const EpdFontFamily::Style style,
                            const int maxLines) {
    if (text.empty()) return;
    const int fontId = renderer.filenameFontId(baseFontId);
    for (const auto& line : renderer.wrappedText(fontId, text.c_str(), textWidth, maxLines, style)) {
      if (y + renderer.getLineHeight(fontId) > bottom) return;
      renderer.drawCenteredText(fontId, y, line.c_str(), true, style);
      y += renderer.getLineHeight(fontId);
    }
  };

  centered(UI_12_FONT_ID, current.title, EpdFontFamily::BOLD, 2);
  centered(UI_10_FONT_ID, current.author, EpdFontFamily::REGULAR, 1);
  y += SECTION_GAP / 2;

  std::string line;
  if (!current.series.empty()) {
    line = std::string(tr(STR_LIBRARY_SERIES)) + ": " + current.series;
    centered(SMALL_FONT_ID, line, EpdFontFamily::REGULAR, 1);
  }
  if (!current.genre.empty()) {
    line = std::string(tr(STR_LIBRARY_GENRE)) + ": " + current.genre;
    centered(SMALL_FONT_ID, line, EpdFontFamily::REGULAR, 1);
  }
  char size[24];
  formatSize(current.fileSize, size, sizeof(size));
  char facts[96];
  if (RecentBookProgress::hasPercent(current.progress)) {
    snprintf(facts, sizeof(facts), "%s  ·  %s  ·  %s", fileTypeLabel(current.fileType), size,
             RecentBookProgress::formatPercent(current.progress).c_str());
  } else {
    snprintf(facts, sizeof(facts), "%s  ·  %s  ·  %s", fileTypeLabel(current.fileType), size,
             tr(STR_SUGGEST_NOT_STARTED));
  }
  centered(SMALL_FONT_ID, facts, EpdFontFamily::REGULAR, 1);
}

void SuggestBookActivity::drawTouchButton(const Rect& rect, const char* label, const bool primary) const {
  if (primary) {
    renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, TOUCH_BUTTON_RADIUS, Color::Black);
  } else {
    renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 2, TOUCH_BUTTON_RADIUS, true);
  }
  const int textW = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD);
  const int textY = rect.y + (rect.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
  renderer.drawText(UI_10_FONT_ID, rect.x + (rect.width - textW) / 2, textY, label, !primary, EpdFontFamily::BOLD);
}

void SuggestBookActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const bool touch = mappedInput.hasTouchHardware();
  const auto header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (touch)
    TouchHeaderBackButton::draw(renderer, header, tr(STR_SUGGEST_BOOK), false);
  else
    GUI.drawHeader(renderer, header, tr(STR_SUGGEST_BOOK));

  int top = 0;
  int right = 0;
  int bottom = 0;
  int left = 0;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int screenW = renderer.getScreenWidth();
  const int contentLeft = left + metrics.contentSidePadding;
  const int contentWidth = screenW - left - right - 2 * metrics.contentSidePadding;
  int contentBottom = renderer.getScreenHeight() - bottom - (touch ? 0 : metrics.buttonHintsHeight) - SECTION_GAP;

  coverRect = Rect{};
  openRect = Rect{};
  anotherRect = Rect{};

  if (state != State::Ready || !current.valid) {
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2,
                              I18N.get(state == State::NoIndex ? StrId::STR_SUGGEST_NO_INDEX : StrId::STR_SUGGEST_NO_BOOKS));
    if (!touch) {
      const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
    renderer.displayBuffer();
    return;
  }

  // Bottom-up: buttons (touch), the question, then the details fill the gap.
  if (touch) {
    const int buttonW = (contentWidth - TOUCH_BUTTON_GAP) / 2;
    const int buttonY = contentBottom - TOUCH_BUTTON_HEIGHT;
    anotherRect = Rect{contentLeft, buttonY, buttonW, TOUCH_BUTTON_HEIGHT};
    openRect = Rect{contentLeft + buttonW + TOUCH_BUTTON_GAP, buttonY, buttonW, TOUCH_BUTTON_HEIGHT};
    drawTouchButton(anotherRect, tr(STR_SUGGEST_ANOTHER), false);
    drawTouchButton(openRect, tr(STR_OPEN), true);
    contentBottom = buttonY - SECTION_GAP;
  }
  const int questionHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int questionY = contentBottom - questionHeight;
  renderer.drawCenteredText(UI_12_FONT_ID, questionY, tr(STR_SUGGEST_OPEN_BOOK), true, EpdFontFamily::BOLD);

  const int coverTop = header.y + header.height + SECTION_GAP;
  // Shrink the cover on short screens so the details always get some room.
  const int detailsMin = renderer.getLineHeight(UI_12_FONT_ID) * 2 + renderer.getLineHeight(UI_10_FONT_ID) +
                         renderer.getLineHeight(SMALL_FONT_ID) * 2 + SECTION_GAP * 2;
  int drawH = std::min(coverHeight, questionY - SECTION_GAP - detailsMin - coverTop);
  drawH = std::max(drawH, 60);
  const int drawW = drawH == coverHeight ? coverWidth : drawH * 2 / 3;
  coverRect = Rect{(screenW - drawW) / 2, coverTop, drawW, drawH};
  drawCover();
  drawDetails(coverRect.y + coverRect.height + SECTION_GAP, questionY - SECTION_GAP / 2);

  if (!touch) {
    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_OPEN), "",
                                              tr(STR_SUGGEST_ANOTHER));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}
