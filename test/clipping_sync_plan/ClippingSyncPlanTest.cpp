#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "clippings/ClippingSyncPlan.h"

using clippingSyncPlan::Action;
using clippingSyncPlan::Plan;
using clippingSyncPlan::textKey;

namespace {
uint32_t key(const uint16_t spine, const std::string& text) { return textKey(spine, text.data(), text.size()); }

clippingSyncPlan::LocalEntry local(const char* id, const uint32_t k) {
  clippingSyncPlan::LocalEntry e;
  std::memcpy(e.id, id, std::min(std::strlen(id), sizeof(e.id) - 1));
  e.key = k;
  return e;
}

clippingSyncPlan::DeletedEntry deleted(const char* id, const uint32_t k) {
  clippingSyncPlan::DeletedEntry e;
  std::memcpy(e.id, id, std::min(std::strlen(id), sizeof(e.id) - 1));
  e.key = k;
  return e;
}
}  // namespace

TEST(ClippingTextKey, IgnoresLineBreaksSpacingCaseAndPunctuation) {
  // CrossInk stores layout line breaks; KOReader stores running text.
  EXPECT_EQ(key(13, "Accept your good ideas\ngratefully whenever they come"),
            key(13, "accept your good ideas gratefully whenever they come."));
  EXPECT_EQ(key(14, "the reader\xe2\x80\x99s needs"), key(14, "the readers needs"));
  EXPECT_EQ(key(15, "draft each section\xe2\x80\x94the opener"), key(15, "draft each section the opener"));
  EXPECT_EQ(key(16, "\xe2\x80\xa2 Is the structure"), key(16, "Is the structure"));
}

TEST(ClippingTextKey, DependsOnChapterAndWords) {
  EXPECT_NE(key(13, "same words"), key(14, "same words"));
  EXPECT_NE(key(13, "same words"), key(13, "other words"));
  // Non-punctuation Unicode letters still count.
  EXPECT_NE(key(1, "caf\xc3\xa9"), key(1, "caf"));
}

TEST(ClippingSyncPlan, AddsUnknownRemoteClipping) {
  Plan plan;
  plan.local.push_back(local("aaaaaaaaaaaaaaaa", key(1, "local")));
  const auto d = plan.onRemote("bbbbbbbbbbbbbbbb", false, key(1, "remote"));
  EXPECT_EQ(d.action, Action::AddLocal);
}

TEST(ClippingSyncPlan, KnownIdOrSameTextIsAlreadyInSync) {
  Plan plan;
  plan.local.push_back(local("aaaaaaaaaaaaaaaa", key(1, "first")));
  plan.local.push_back(local("cccccccccccccccc", key(2, "Second quote")));
  auto d = plan.onRemote("aaaaaaaaaaaaaaaa", false, key(1, "first"));
  EXPECT_EQ(d.action, Action::None);
  EXPECT_EQ(d.index, 0u);
  // Same quote highlighted on another device: different id, same text.
  d = plan.onRemote("dddddddddddddddd", false, key(2, "second  quote"));
  EXPECT_EQ(d.action, Action::None);
  EXPECT_EQ(d.index, 1u);
}

TEST(ClippingSyncPlan, RemoteTombstoneRemovesLocalById) {
  Plan plan;
  plan.local.push_back(local("aaaaaaaaaaaaaaaa", 1));
  plan.local.push_back(local("bbbbbbbbbbbbbbbb", 2));
  auto d = plan.onRemote("bbbbbbbbbbbbbbbb", true, 0);
  EXPECT_EQ(d.action, Action::RemoveLocal);
  EXPECT_EQ(d.index, 1u);
  plan.removed(d.index);
  ASSERT_EQ(plan.local.size(), 1u);
  // A tombstone for something this device never had is a no-op.
  d = plan.onRemote("eeeeeeeeeeeeeeee", true, 0);
  EXPECT_EQ(d.action, Action::None);
}

TEST(ClippingSyncPlan, LocallyDeletedClippingIsDeletedOnServerNotRestored) {
  Plan plan;
  plan.deleted.push_back(deleted("aaaaaaaaaaaaaaaa", key(3, "gone")));
  // By id.
  EXPECT_EQ(plan.onRemote("aaaaaaaaaaaaaaaa", false, key(3, "gone")).action, Action::DeleteRemote);
  // By text, when another device uploaded the same quote under its own id.
  EXPECT_EQ(plan.onRemote("ffffffffffffffff", false, key(3, "Gone.")).action, Action::DeleteRemote);
  // Unrelated clippings are unaffected.
  EXPECT_EQ(plan.onRemote("1111111111111111", false, key(3, "kept")).action, Action::AddLocal);
}

TEST(ClippingSyncPlan, OnlyClippingsMissingFromServerAreUploaded) {
  Plan plan;
  plan.local.push_back(local("aaaaaaaaaaaaaaaa", key(1, "on server")));
  plan.local.push_back(local("bbbbbbbbbbbbbbbb", key(1, "new here")));
  const auto d = plan.onRemote("aaaaaaaaaaaaaaaa", false, key(1, "on server"));
  plan.markOnServer(d.index);
  plan.added("cccccccccccccccc", key(2, "downloaded"));
  ASSERT_EQ(plan.local.size(), 3u);
  EXPECT_TRUE(plan.local[0].onServer);
  EXPECT_FALSE(plan.local[1].onServer);  // the only upload
  EXPECT_TRUE(plan.local[2].onServer);
  // A second copy of the downloaded item is recognised.
  EXPECT_EQ(plan.onRemote("cccccccccccccccc", false, key(2, "downloaded")).action, Action::None);
}
