// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_UI_PRESENTERS_H_
#define BDK_ENGINE_UI_PRESENTERS_H_

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/values.h"
#include "bdk/core/include/bdk/bdk_base.h"

namespace bdk {

class HostView;

// The default presenters of the browser-owned surfaces (ui-requests.md
// section 6): what shows a request when the application has no handler for
// its surface, or returns `.useDefault`. Platform-neutral: no AppKit, Win32 or
// toolkit type appears here (architecture.md section 14). The implementation
// for the platform is presenters_mac.mm.
//
// Each Show* is given the payload of the request, the same facts a custom
// presenter receives (ui-requests.md section 3: the presenter shows exactly
// what the application would be shown), and the view of the page it concerns,
// which anchors it. It returns a dismisser: running it removes what was shown
// (the request was withdrawn), and the answer callback never runs afterwards.
// A page that is not in a window yet is shown when it is.
using Dismisser = base::OnceClosure;

enum class PermissionChoice { kAllow, kAllowThisVisit, kBlock, kDismiss };

struct JavaScriptDialogAnswer {
  bool accepted = false;
  std::string text;
  bool suppress_further = false;
};

struct AuthAnswer {
  std::string username;
  std::string password;
};

enum class NotificationEvent { kClicked, kClosedByUser, kClosedByTimeout };

// A question with a few answers, shown as a sheet by the prompts the engine
// builds itself (the password manager, downloads): each button has a key (what
// a test script answers with) and a title (shown).
struct AlertButton {
  std::string key;
  std::string title;
};

struct Alert {
  std::string title;
  std::string message;
  // The first is the default button. The last one is Escape's.
  std::vector<AlertButton> buttons;
};

// The find bar's side of the conversation: what the person typed and did.
class FindBarClient {
 public:
  virtual ~FindBarClient() = default;
  // The text or the options changed, or next/previous was pressed.
  virtual void Search(const std::string& text,
                      bool forward,
                      bool match_case,
                      bool find_next) = 0;
  // The bar was closed by the person.
  virtual void Closed() = 0;
};

// A find bar being shown. Destroying it removes it.
class FindBar {
 public:
  virtual ~FindBar() = default;
  // The result of the current search: the 1-based ordinal of the active
  // match (0 when none) and the number of matches.
  virtual void SetResult(int ordinal, int count) = 0;
  virtual void Focus() = 0;
};

class UiPresenters {
 public:
  virtual ~UiPresenters() = default;

  // The real presenters of the platform, or the scripted ones while a test
  // scripts them (ScriptPresenter in testing.cc, via bdk_testing.h).
  static UiPresenters& Get();

  // payload: the permissions request payload (origin, kinds, text, ...).
  virtual Dismisser ShowPermission(
      HostView* view,
      const base::DictValue& payload,
      base::OnceCallback<void(PermissionChoice)> answer) = 0;
  // payload: the javascript_dialogs request payload.
  virtual Dismisser ShowJavaScriptDialog(
      HostView* view,
      const base::DictValue& payload,
      base::OnceCallback<void(JavaScriptDialogAnswer)> answer) = 0;
  // payload: the http_auth request payload. No answer means cancel.
  virtual Dismisser ShowAuthPrompt(
      HostView* view,
      const base::DictValue& payload,
      base::OnceCallback<void(std::optional<AuthAnswer>)> answer) = 0;
  // payload: the file_pickers request payload. No path means cancel.
  virtual Dismisser ShowFilePicker(
      HostView* view,
      const base::DictValue& payload,
      base::OnceCallback<void(std::vector<base::FilePath>)> answer) = 0;
  // payload: the notifications request payload. `view` may be null (a
  // notification of a service worker belongs to no page): it then appears in
  // the application's foremost window. `event` gets the action that was
  // clicked, if any.
  virtual Dismisser ShowNotification(
      HostView* view,
      const base::DictValue& payload,
      base::RepeatingCallback<void(NotificationEvent,
                                   const std::optional<std::string>&)>
          event) = 0;
  // `surface` names the [ui.surfaces] key that is asking (for test scripts).
  // `answer` gets the chosen button's index, or -1 when the sheet is dismissed
  // or there is no window to show it in. Unlike the others it is not given a
  // request payload: the prompt is the engine's own (ui-requests.md section
  // 6, "small AppKit presenters").
  virtual Dismisser ShowAlert(const std::string& surface,
                              HostView* view,
                              Alert alert,
                              base::OnceCallback<void(int)> answer) = 0;
  // A save panel as a sheet. `answer` gets the chosen path, or nothing when
  // the person cancels or there is no window to show it in. A script answers
  // with the path, or "cancel".
  virtual Dismisser ShowSavePanel(
      const std::string& surface,
      HostView* view,
      const std::string& suggested_name,
      base::OnceCallback<void(std::optional<std::string>)> answer) = 0;
  // The devtools surface's default (ui-requests.md 5.15): an undocked window
  // of BDK's own that shows `view`, the DevTools front end's page, titled
  // `title`. `closed` runs when the person closes the window; running the
  // dismisser closes it without calling `closed`. A script answers
  // {"answer":"close"} to close it as the person would.
  virtual Dismisser ShowPageWindow(HostView* view,
                                   const std::string& title,
                                   base::OnceClosure closed) = 0;
  // payload: the find request payload (initialText).
  virtual std::unique_ptr<FindBar> ShowFindBar(HostView* view,
                                               const base::DictValue& payload,
                                               FindBarClient* client) = 0;

  // Whether `gesture` is the operating system's event that confirmed a grant:
  // a recent mouse-up or key-down delivered to a window of this application
  // (ui-requests.md section 3).
  virtual bool IsRecentGesture(const bdk_native_handle_t& gesture) = 0;
};

// Implemented by the platform (presenters_mac.mm).
std::unique_ptr<UiPresenters> CreatePlatformPresenters();

// Test scripting (bdk_testing.h), the one mechanism for every default
// presenter. While a surface is scripted its presenter shows nothing and
// answers as `answer` says, later, on the task runner; an answer with
// "none": true leaves the request unanswered, as a person who has not decided
// yet. Surfaces are named as in [ui.surfaces]. Records what was presented
// either way.
struct PresenterRecord {
  int shown = 0;
  int dismissed = 0;
  std::string last_payload;
};
void ScriptPresenter(const std::string& surface, base::DictValue answer);
void ClearPresenterScripts();
// Whether `surface` is scripted: its default presenter then shows nothing.
bool PresenterScripted(const std::string& surface);
// For a default presenter that is Chromium's own UI and so has no UiPresenters
// method (the system print panel, the suggestion popup). When `surface` is
// scripted, records the use and, unless the script says "none", gives
// `answer` the script's answer later, and returns the dismisser; the caller
// then shows nothing. Returns nothing when `surface` is not scripted.
std::optional<Dismisser> ScriptedPresent(
    const std::string& surface,
    const base::DictValue& payload,
    base::OnceCallback<void(const base::DictValue&)> answer);
// Records that the default presenter of `surface` showed something (Chromium's
// own UI, which no script replaced), so a test can see it ran.
void RecordPresented(const std::string& surface,
                     const base::DictValue& payload);
PresenterRecord PresenterRecordOf(const std::string& surface);

// Where the last prompt that attached to a window anchor
// (bdk_window_set_anchor_provider) was put: x, y, width, height in the
// window's content view, origin top left; nothing when no such prompt is up.
// For tests. A prompt (permission, password, autofill, download) attaches to
// the anchor of its role when the window's provider supplies one, and is a
// sheet otherwise.
std::optional<std::array<double, 4>> AnchoredPromptFrame();
// Presses the button at `index` of that prompt, as a person would. False when
// none is up.
bool AnswerAnchoredPromptForTesting(int index);

}  // namespace bdk

#endif  // BDK_ENGINE_UI_PRESENTERS_H_
