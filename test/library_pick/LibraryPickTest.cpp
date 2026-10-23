#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <vector>

#include "LibraryPick.h"

using library::PickTaste;

namespace {

PickTaste::Candidate book(uint64_t path, uint64_t author = 0, uint64_t genre = 0) {
  PickTaste::Candidate c;
  c.pathHash = path;
  c.authorHash = author;
  c.genreHash = genre;
  return c;
}

}  // namespace

TEST(LibraryPickTest, UnknownBookHasBaseWeight) {
  PickTaste taste;
  EXPECT_EQ(taste.weight(book(1)), PickTaste::BASE_WEIGHT);
}

TEST(LibraryPickTest, EachSkipHalvesWeightButNeverReachesZero) {
  PickTaste taste;
  const auto b = book(42);
  taste.recordSkip(b);
  EXPECT_EQ(taste.weight(b), PickTaste::BASE_WEIGHT / 2);
  taste.recordSkip(b);
  EXPECT_EQ(taste.weight(b), PickTaste::BASE_WEIGHT / 4);
  for (int i = 0; i < 300; i++) taste.recordSkip(b);
  EXPECT_EQ(taste.skipCount(b.pathHash), UINT8_MAX);  // saturates instead of wrapping to 0
  EXPECT_GE(taste.weight(b), 1);
  EXPECT_EQ(taste.weight(b), PickTaste::BASE_WEIGHT >> PickTaste::MAX_SKIP_SHIFT);
}

TEST(LibraryPickTest, SkipsDoNotAffectOtherBooks) {
  PickTaste taste;
  taste.recordSkip(book(1));
  EXPECT_EQ(taste.weight(book(2)), PickTaste::BASE_WEIGHT);
}

TEST(LibraryPickTest, AcceptingRaisesAndSkippingLowersAuthorAndGenreWeight) {
  PickTaste taste;
  const uint64_t author = 1001;
  const uint64_t genre = 2002;
  const auto liked = book(1, author, genre);
  taste.recordAccept(liked);
  taste.recordAccept(book(2, author, genre));
  const auto sibling = book(3, author, genre);
  EXPECT_GT(taste.weight(sibling), PickTaste::BASE_WEIGHT);

  PickTaste other;
  other.recordSkip(book(1, author, genre));
  other.recordSkip(book(2, author, genre));
  EXPECT_LT(other.weight(book(3, author, genre)), PickTaste::BASE_WEIGHT);
}

TEST(LibraryPickTest, UnknownAuthorAndGenreStayNeutral) {
  PickTaste taste;
  for (int i = 0; i < 20; i++) taste.recordSkip(book(100 + i, 0, 0));
  EXPECT_EQ(taste.weight(book(999, 0, 0)), PickTaste::BASE_WEIGHT);
  EXPECT_EQ(taste.authorAffinity(0), 0);
  EXPECT_EQ(taste.genreAffinity(0), 0);
}

TEST(LibraryPickTest, AffinityIsClamped) {
  PickTaste taste;
  for (int i = 0; i < 200; i++) taste.recordAccept(book(i + 1, 7, 7));
  EXPECT_EQ(taste.authorAffinity(7), PickTaste::AFFINITY_LIMIT);
  PickTaste low;
  for (int i = 0; i < 200; i++) low.recordSkip(book(i + 1, 7, 7));
  EXPECT_EQ(low.authorAffinity(7), -PickTaste::AFFINITY_LIMIT);
  // Even the most disliked author keeps a nonzero chance.
  EXPECT_GE(low.weight(book(100000, 7, 7)), 1);
}

TEST(LibraryPickTest, AcceptingForgetsThatBooksSkips) {
  PickTaste taste;
  const auto b = book(5);
  taste.recordSkip(b);
  taste.recordSkip(b);
  taste.recordAccept(b);
  EXPECT_EQ(taste.skipCount(b.pathHash), 0);
  EXPECT_EQ(taste.weight(b), PickTaste::BASE_WEIGHT);
}

TEST(LibraryPickTest, FullSkipTableEvictsLeastSkippedThenOldest) {
  PickTaste taste;
  // One heavily skipped book that must survive churn.
  const auto stubborn = book(1);
  for (int i = 0; i < 4; i++) taste.recordSkip(stubborn);
  // Fill the table, then overflow it with single-skip books.
  for (uint64_t i = 0; i < PickTaste::SKIP_SLOTS * 3; i++) taste.recordSkip(book(1000 + i));
  EXPECT_EQ(taste.skipCount(stubborn.pathHash), 4);
  // The oldest single-skip entries are gone; the newest are still remembered.
  EXPECT_EQ(taste.skipCount(1000), 0);
  EXPECT_EQ(taste.skipCount(1000 + PickTaste::SKIP_SLOTS * 3 - 1), 1);
}

TEST(LibraryPickTest, SerializationRoundTrips) {
  PickTaste taste;
  taste.recordSkip(book(11, 3, 4));
  taste.recordSkip(book(11, 3, 4));
  taste.recordAccept(book(12, 5, 6));

  uint8_t buffer[PickTaste::SERIALIZED_SIZE];
  ASSERT_EQ(taste.serialize(buffer, sizeof(buffer)), PickTaste::SERIALIZED_SIZE);

  PickTaste restored;
  ASSERT_TRUE(restored.deserialize(buffer, sizeof(buffer)));
  EXPECT_EQ(restored.skipCount(11), 2);
  EXPECT_EQ(restored.authorAffinity(3), taste.authorAffinity(3));
  EXPECT_EQ(restored.genreAffinity(6), taste.genreAffinity(6));
  EXPECT_EQ(restored.weight(book(11, 3, 4)), taste.weight(book(11, 3, 4)));
  EXPECT_EQ(restored.weight(book(13, 5, 6)), taste.weight(book(13, 5, 6)));
}

TEST(LibraryPickTest, SerializeNeedsEnoughRoom) {
  PickTaste taste;
  uint8_t small[8];
  EXPECT_EQ(taste.serialize(small, sizeof(small)), 0u);
}

TEST(LibraryPickTest, DeserializeRejectsDamageAndStaysEmpty) {
  PickTaste taste;
  taste.recordSkip(book(11, 3, 4));
  uint8_t buffer[PickTaste::SERIALIZED_SIZE];
  ASSERT_EQ(taste.serialize(buffer, sizeof(buffer)), PickTaste::SERIALIZED_SIZE);

  PickTaste target;
  target.recordSkip(book(99));
  EXPECT_FALSE(target.deserialize(buffer, sizeof(buffer) - 1));
  EXPECT_EQ(target.skipCount(99), 0);  // failure leaves a clean model, not a half-loaded one

  buffer[0] = 'X';
  EXPECT_FALSE(target.deserialize(buffer, sizeof(buffer)));
  buffer[0] = 'P';
  buffer[4] = 99;  // unknown version
  EXPECT_FALSE(target.deserialize(buffer, sizeof(buffer)));
}

TEST(LibraryPickTest, DeserializeClampsOutOfRangeAffinity) {
  PickTaste taste;
  uint8_t buffer[PickTaste::SERIALIZED_SIZE];
  ASSERT_EQ(taste.serialize(buffer, sizeof(buffer)), PickTaste::SERIALIZED_SIZE);
  buffer[9] = 120;  // first author bucket, far beyond the limit
  PickTaste restored;
  ASSERT_TRUE(restored.deserialize(buffer, sizeof(buffer)));
  EXPECT_EQ(restored.authorAffinity(PickTaste::BUCKETS), PickTaste::AFFINITY_LIMIT);  // hash % 64 == 0
}

TEST(LibraryPickTest, PickWeightedIsProportional) {
  const uint16_t weights[] = {1, 255, 0, 256};
  // 512 total; sweeping every possible target must hit each index weight-many times.
  int hits[4] = {};
  for (uint32_t r = 0; r < 512; r++) hits[library::pickWeighted(weights, 4, r)]++;
  EXPECT_EQ(hits[0], 1);
  EXPECT_EQ(hits[1], 255);
  EXPECT_EQ(hits[2], 0);
  EXPECT_EQ(hits[3], 256);
}

TEST(LibraryPickTest, PickWeightedHandlesEmptyAndZeroTotals) {
  const uint16_t zeros[] = {0, 0};
  EXPECT_EQ(library::pickWeighted(zeros, 2, 7), 2u);
  EXPECT_EQ(library::pickWeighted(zeros, 0, 7), 0u);
}

TEST(LibraryPickTest, PickWeightedRespectsLearnedSkips) {
  PickTaste taste;
  const auto skipped = book(1);
  for (int i = 0; i < 5; i++) taste.recordSkip(skipped);
  const uint16_t weights[] = {taste.weight(skipped), taste.weight(book(2))};
  int skippedPicks = 0;
  const uint32_t total = weights[0] + weights[1];
  for (uint32_t r = 0; r < total; r++) skippedPicks += library::pickWeighted(weights, 2, r) == 0;
  EXPECT_LT(skippedPicks, static_cast<int>(total) / 10);
  EXPECT_GT(skippedPicks, 0);
}

TEST(LibraryPickTest, DrawDistinctOrdinalsAreUniqueAndInRange) {
  uint32_t state = 12345;
  auto rng = [&state] { return state = state * 1664525u + 1013904223u; };
  uint16_t out[16];
  const size_t n = library::drawDistinctOrdinals(500, 16, out, rng);
  ASSERT_EQ(n, 16u);
  std::set<uint16_t> seen(out, out + n);
  EXPECT_EQ(seen.size(), n);
  for (size_t i = 0; i < n; i++) EXPECT_LT(out[i], 500);
}

TEST(LibraryPickTest, DrawDistinctOrdinalsOnTinyLibrariesTerminates) {
  uint32_t state = 99;
  auto rng = [&state] { return state = state * 1664525u + 1013904223u; };
  uint16_t out[16];
  EXPECT_EQ(library::drawDistinctOrdinals(1, 16, out, rng), 1u);
  EXPECT_EQ(out[0], 0);
  const size_t three = library::drawDistinctOrdinals(3, 16, out, rng);
  EXPECT_LE(three, 3u);
  EXPECT_GE(three, 1u);
  std::set<uint16_t> seen(out, out + three);
  EXPECT_EQ(seen.size(), three);
}

namespace {
library::PickEngagement engaged(uint16_t sessions, uint8_t rank = library::PickEngagement::RECENT_NONE) {
  library::PickEngagement e;
  e.sessions = sessions;
  e.recentRank = rank;
  return e;
}
}  // namespace

TEST(LibraryPickTest, NeverOpenedBooksKeepTheirWeight) {
  EXPECT_EQ(library::engagementWeight(256, engaged(0)), 256);
}

TEST(LibraryPickTest, RecentBooksRiseOnlyOnceReturnedTo) {
  EXPECT_EQ(library::engagementWeight(256, engaged(1, 3)), 256);
  EXPECT_EQ(library::engagementWeight(256, engaged(2, 3)), 384);
  EXPECT_EQ(library::engagementWeight(256, engaged(3, 3)), 512);
  EXPECT_EQ(library::engagementWeight(256, engaged(4, 3)), 640);
  EXPECT_EQ(library::engagementWeight(256, engaged(40, 3)), 640);
}

TEST(LibraryPickTest, RecentBookWithoutStatsIsNeutral) {
  EXPECT_EQ(library::engagementWeight(256, engaged(0, 5)), 256);
}

TEST(LibraryPickTest, MostRecentBookIsNotBoosted) {
  EXPECT_EQ(library::engagementWeight(256, engaged(9, 0)), 256);
}

TEST(LibraryPickTest, OpenedBooksOutsideRecentsFade) {
  EXPECT_EQ(library::engagementWeight(256, engaged(1)), 64);
  EXPECT_EQ(library::engagementWeight(256, engaged(3)), 128);
}

TEST(LibraryPickTest, EngagementWeightStaysPositiveAndBounded) {
  EXPECT_EQ(library::engagementWeight(1, engaged(1)), 1);
  EXPECT_EQ(library::engagementWeight(UINT16_MAX, engaged(4, 2)), UINT16_MAX);
}
