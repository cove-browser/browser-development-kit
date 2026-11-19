// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/session_state.h"

#include <utility>

#include "base/pickle.h"
#include "components/sessions/content/content_serialized_navigation_builder.h"
#include "components/sessions/core/serialized_navigation_entry.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"

namespace bdk {

namespace {

// "BDKC" then the format version. Version 1: entry count, current index,
// then each entry as written by SerializedNavigationEntry::WriteToPickle.
constexpr uint32_t kMagic = 0x43444b42;  // "BDKC" little-endian
constexpr int kVersion = 1;
// The size limit Chromium's session service uses for one entry.
constexpr int kMaxEntryBytes = 1024 * 1024;

}  // namespace

RestoredNavigation::RestoredNavigation() = default;
RestoredNavigation::RestoredNavigation(RestoredNavigation&&) = default;
RestoredNavigation& RestoredNavigation::operator=(RestoredNavigation&&) =
    default;
RestoredNavigation::~RestoredNavigation() = default;

std::string SerializeNavigation(content::NavigationController& controller) {
  base::Pickle pickle;
  pickle.WriteUInt32(kMagic);
  pickle.WriteInt(kVersion);
  std::vector<sessions::SerializedNavigationEntry> entries;
  for (int i = 0; i < controller.GetEntryCount(); ++i) {
    content::NavigationEntry* entry = controller.GetEntryAtIndex(i);
    if (entry->IsInitialEntry()) {
      continue;
    }
    entries.push_back(
        sessions::ContentSerializedNavigationBuilder::FromNavigationEntry(
            static_cast<int>(entries.size()), entry));
  }
  int current = controller.GetLastCommittedEntryIndex();
  if (entries.empty() || current >= static_cast<int>(entries.size())) {
    current = static_cast<int>(entries.size()) - 1;
  }
  pickle.WriteInt(static_cast<int>(entries.size()));
  pickle.WriteInt(current);
  for (const auto& entry : entries) {
    entry.WriteToPickle(kMaxEntryBytes, &pickle);
  }
  return std::string(reinterpret_cast<const char*>(pickle.data()),
                     pickle.size());
}

ParseResult ParseNavigation(base::span<const uint8_t> data,
                            content::BrowserContext* context,
                            RestoredNavigation* out) {
  base::Pickle pickle = base::Pickle::WithUnownedBuffer(data);
  base::PickleIterator it(pickle);
  uint32_t magic = 0;
  int version = 0;
  if (!it.ReadUInt32(&magic) || magic != kMagic || !it.ReadInt(&version) ||
      version < 1) {
    return ParseResult::kMalformed;
  }
  if (version > kVersion) {
    return ParseResult::kTooNew;
  }
  int count = 0;
  int current = -1;
  if (!it.ReadInt(&count) || !it.ReadInt(&current) || count < 0 ||
      current < -1 || current >= count ||
      static_cast<size_t>(count) > data.size()) {
    return ParseResult::kMalformed;
  }
  std::vector<sessions::SerializedNavigationEntry> navigations(
      static_cast<size_t>(count));
  for (auto& navigation : navigations) {
    if (!navigation.ReadFromPickle(&it)) {
      return ParseResult::kMalformed;
    }
  }
  out->current = count == 0 ? -1 : current;
  out->entries =
      sessions::ContentSerializedNavigationBuilder::ToNavigationEntries(
          navigations, context);
  return ParseResult::kOk;
}

}  // namespace bdk
