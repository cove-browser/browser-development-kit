// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_SERVICES_DOWNLOAD_PROMPTS_H_
#define BDK_ENGINE_SERVICES_DOWNLOAD_PROMPTS_H_

class Profile;

namespace bdk {

// Download prompts (ui-requests.md 5.7, T-UIR-03). For a profile the
// application opened:
//
// - where to save (the "ask where to save" setting, a save-as, a name
//   conflict) is a request of the `downloads` surface with needsLocation set,
//   answered by the application or by a native save panel;
// - a download Chromium holds as dangerous raises a dangerous-download
//   request, which only a completion with a user gesture can keep. A request
//   that is rejected, dropped or released discards the download;
// - with the surface disabled, downloads are cancelled when they start.
//
// Progress is not a request: the downloads service's events carry it.
//
// Call once per profile, as soon as it is loaded and before it can download.
void InstallDownloadPrompts(Profile* profile);

// For tests (bdk_testing_command "downloads.replaceDelegate"): puts a new BDK
// delegate in place of the profile's after its download manager exists, as
// happens to the profile Chromium loads at startup.
void ReplaceDownloadDelegateForTesting(Profile* profile);

// For tests (bdk_testing_command "downloads.ask"): makes every loaded profile
// ask where to save, as the setting does.
void AskWhereToSaveForTesting(bool ask);

// For tests (bdk_testing_command "downloads.danger"): every download is
// determined as dangerous content.
void ForceDangerousDownloadsForTesting(bool on);

}  // namespace bdk

#endif  // BDK_ENGINE_SERVICES_DOWNLOAD_PROMPTS_H_
