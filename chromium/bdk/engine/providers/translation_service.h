// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Page translation without Google (T-PRV-02, T-PRV-03; architecture.md 11).
// Chromium's own path injects Google's element.js; BDK replaces that step and
// keeps the rest of Chromium's translate machinery it can use without Google:
// the renderer's language detection (reported through ChromeTranslateClient's
// driver) and the translate preferences (always and never lists, sites never
// to offer, the target language).
//
// A page's text is read and written by two small scripts BDK runs in the
// translate isolated world of the main frame, where the page's own script
// cannot see them; the text goes to the provider in batches through the core
// (translation_job_start), so the application's provider and the default
// engine answer through the same contract. Offers and the application's own
// `present` are `translation` requests; with no handler the default presenter
// is a sheet on the page's window.

#ifndef BDK_ENGINE_PROVIDERS_TRANSLATION_SERVICE_H_
#define BDK_ENGINE_PROVIDERS_TRANSLATION_SERVICE_H_

#include "bdk/engine/services/services.h"

namespace content {
class WebContents;
}

namespace bdk {

// Starts watching a page's language and navigations. Called when the engine
// attaches the page.
void AttachTranslation(content::WebContents* contents);

// The translation.* commands.
void AddTranslationHandlers(HandlerTable& table);

}  // namespace bdk

#endif  // BDK_ENGINE_PROVIDERS_TRANSLATION_SERVICE_H_
