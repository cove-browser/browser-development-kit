// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/ui_presenters.h"

#include <map>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/task/sequenced_task_runner.h"
#include "bdk/engine/json_util.h"

namespace bdk {

namespace {

struct Script {
  base::DictValue answer;
  PresenterRecord record;
};

std::map<std::string, Script>& Scripts() {
  static base::NoDestructor<std::map<std::string, Script>> scripts;
  return *scripts;
}

// The records outlive a script being cleared, so a test can read what was
// presented after it.
std::map<std::string, PresenterRecord>& Records() {
  static base::NoDestructor<std::map<std::string, PresenterRecord>> records;
  return *records;
}

// A find bar that shows nothing.
class ScriptedFindBar : public FindBar {
 public:
  void SetResult(int ordinal, int count) override {}
  void Focus() override {}
};

// What a scripted presenter does for one request: records it and, unless the
// answer says "none", answers later. Returns the dismisser.
class Scripted {
 public:
  // Returns null when `surface` is not scripted.
  static Script* Find(const std::string& surface) {
    auto it = Scripts().find(surface);
    return it == Scripts().end() ? nullptr : &it->second;
  }

  template <typename Answer>
  static Dismisser Present(const std::string& surface,
                           const base::DictValue& payload,
                           Answer answer) {
    Script* script = Find(surface);
    PresenterRecord& record = Records()[surface];
    ++record.shown;
    record.last_payload = ToJson(payload);
    if (script->answer.FindBool("none").value_or(false)) {
      return base::BindOnce(
          [](std::string surface) { ++Records()[surface].dismissed; }, surface);
    }
    auto state = std::make_shared<bool>(true);
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](std::shared_ptr<bool> live, base::DictValue answer, Answer run) {
              if (*live) {
                run(answer);
              }
            },
            state, script->answer.Clone(), std::move(answer)));
    return base::BindOnce(
        [](std::shared_ptr<bool> live, std::string surface) {
          *live = false;
          ++Records()[surface].dismissed;
        },
        state, surface);
  }
};

// Chooses between the scripted answer and the platform's presenters.
class RoutingPresenters : public UiPresenters {
 public:
  RoutingPresenters() : platform_(CreatePlatformPresenters()) {}

  Dismisser ShowPermission(
      HostView* view,
      const base::DictValue& payload,
      base::OnceCallback<void(PermissionChoice)> answer) override {
    if (!Scripted::Find("permissions")) {
      return platform_->ShowPermission(view, payload, std::move(answer));
    }
    return Scripted::Present(
        "permissions", payload,
        [answer = std::move(answer)](const base::DictValue& a) mutable {
          const std::string* choice = a.FindString("choice");
          std::string c = choice ? *choice : "dismiss";
          std::move(answer).Run(c == "allow" ? PermissionChoice::kAllow
                                : c == "allowThisVisit"
                                    ? PermissionChoice::kAllowThisVisit
                                : c == "block" ? PermissionChoice::kBlock
                                               : PermissionChoice::kDismiss);
        });
  }

  Dismisser ShowJavaScriptDialog(
      HostView* view,
      const base::DictValue& payload,
      base::OnceCallback<void(JavaScriptDialogAnswer)> answer) override {
    if (!Scripted::Find("javascript_dialogs")) {
      return platform_->ShowJavaScriptDialog(view, payload, std::move(answer));
    }
    return Scripted::Present(
        "javascript_dialogs", payload,
        [answer = std::move(answer)](const base::DictValue& a) mutable {
          JavaScriptDialogAnswer result;
          result.accepted = a.FindBool("accept").value_or(false);
          if (const std::string* text = a.FindString("text")) {
            result.text = *text;
          }
          result.suppress_further = a.FindBool("suppress").value_or(false);
          std::move(answer).Run(std::move(result));
        });
  }

  Dismisser ShowAuthPrompt(
      HostView* view,
      const base::DictValue& payload,
      base::OnceCallback<void(std::optional<AuthAnswer>)> answer) override {
    if (!Scripted::Find("http_auth")) {
      return platform_->ShowAuthPrompt(view, payload, std::move(answer));
    }
    return Scripted::Present(
        "http_auth", payload,
        [answer = std::move(answer)](const base::DictValue& a) mutable {
          const std::string* user = a.FindString("username");
          const std::string* password = a.FindString("password");
          if (!user || !password) {
            std::move(answer).Run(std::nullopt);
            return;
          }
          std::move(answer).Run(AuthAnswer{*user, *password});
        });
  }

  Dismisser ShowFilePicker(
      HostView* view,
      const base::DictValue& payload,
      base::OnceCallback<void(std::vector<base::FilePath>)> answer) override {
    if (!Scripted::Find("file_pickers")) {
      return platform_->ShowFilePicker(view, payload, std::move(answer));
    }
    return Scripted::Present(
        "file_pickers", payload,
        [answer = std::move(answer)](const base::DictValue& a) mutable {
          std::vector<base::FilePath> paths;
          if (const base::ListValue* list = a.FindList("paths")) {
            for (const base::Value& path : *list) {
              if (path.is_string()) {
                paths.push_back(
                    base::FilePath::FromUTF8Unsafe(path.GetString()));
              }
            }
          }
          std::move(answer).Run(std::move(paths));
        });
  }

  Dismisser ShowNotification(
      HostView* view,
      const base::DictValue& payload,
      base::RepeatingCallback<void(NotificationEvent,
                                   const std::optional<std::string>&)> event)
      override {
    if (!Scripted::Find("notifications")) {
      return platform_->ShowNotification(view, payload, std::move(event));
    }
    return Scripted::Present(
        "notifications", payload, [event](const base::DictValue& a) {
          const std::string* name = a.FindString("event");
          const std::string* action = a.FindString("action");
          std::optional<std::string> act;
          if (action) {
            act = *action;
          }
          if (name && *name == "clicked") {
            event.Run(NotificationEvent::kClicked, act);
          } else if (name && *name == "timeout") {
            event.Run(NotificationEvent::kClosedByTimeout, std::nullopt);
          } else {
            event.Run(NotificationEvent::kClosedByUser, std::nullopt);
          }
        });
  }

  Dismisser ShowAlert(const std::string& surface,
                      HostView* view,
                      Alert alert,
                      base::OnceCallback<void(int)> answer) override {
    if (!Scripted::Find(surface)) {
      return platform_->ShowAlert(surface, view, std::move(alert),
                                  std::move(answer));
    }
    base::DictValue shown;
    shown.Set("title", alert.title);
    shown.Set("message", alert.message);
    base::ListValue keys;
    for (const AlertButton& button : alert.buttons) {
      keys.Append(button.key);
    }
    shown.Set("buttons", std::move(keys));
    return Scripted::Present(
        surface, shown,
        [alert = std::move(alert),
         answer = std::move(answer)](const base::DictValue& a) mutable {
          const std::string* key = a.FindString("answer");
          int index = -1;
          for (size_t i = 0; key && i < alert.buttons.size(); ++i) {
            if (alert.buttons[i].key == *key) {
              index = static_cast<int>(i);
            }
          }
          std::move(answer).Run(index);
        });
  }

  Dismisser ShowSavePanel(
      const std::string& surface,
      HostView* view,
      const std::string& suggested_name,
      base::OnceCallback<void(std::optional<std::string>)> answer) override {
    if (!Scripted::Find(surface)) {
      return platform_->ShowSavePanel(surface, view, suggested_name,
                                      std::move(answer));
    }
    return Scripted::Present(
        surface, base::DictValue().Set("suggestedName", suggested_name),
        [answer = std::move(answer)](const base::DictValue& a) mutable {
          const std::string* path = a.FindString("answer");
          std::move(answer).Run(path && *path != "cancel"
                                    ? std::optional<std::string>(*path)
                                    : std::nullopt);
        });
  }

  Dismisser ShowPageWindow(const std::string& surface,
                           HostView* view,
                           const std::string& title,
                           int width,
                           int height,
                           base::OnceClosure closed) override {
    if (!Scripted::Find(surface)) {
      return platform_->ShowPageWindow(surface, view, title, width, height,
                                       std::move(closed));
    }
    return Scripted::Present(
        surface,
        base::DictValue()
            .Set("title", title)
            .Set("width", width)
            .Set("height", height),
        [closed = std::move(closed)](const base::DictValue& a) mutable {
          const std::string* answer = a.FindString("answer");
          if (answer && *answer == "close") {
            std::move(closed).Run();
          }
        });
  }

  Dismisser ShowWebAuthn(
      HostView* view,
      const base::DictValue& payload,
      base::OnceCallback<void(base::DictValue)> answer) override {
    if (!Scripted::Find("webauthn")) {
      return platform_->ShowWebAuthn(view, payload, std::move(answer));
    }
    const std::string* step = payload.FindString("step");
    return Scripted::Present(
        "webauthn", payload,
        [step = step ? *step : std::string(),
         answer = std::move(answer)](const base::DictValue& a) mutable {
          const base::DictValue* chosen = a.FindDict(step);
          if (!chosen) {
            chosen = &a;
          }
          if (chosen->FindBool("none").value_or(false)) {
            return;  // undecided at this step
          }
          std::move(answer).Run(chosen->Clone());
        });
  }

  std::unique_ptr<FindBar> ShowFindBar(HostView* view,
                                       const base::DictValue& payload,
                                       FindBarClient* client) override {
    if (!Scripted::Find("find")) {
      return platform_->ShowFindBar(view, payload, client);
    }
    PresenterRecord& record = Records()["find"];
    ++record.shown;
    record.last_payload = ToJson(payload);
    return std::make_unique<ScriptedFindBar>();
  }

  bool IsRecentGesture(const bdk_native_handle_t& gesture) override {
    return platform_->IsRecentGesture(gesture);
  }

 private:
  std::unique_ptr<UiPresenters> platform_;
};

}  // namespace

// static
UiPresenters& UiPresenters::Get() {
  static base::NoDestructor<RoutingPresenters> presenters;
  return *presenters;
}

void ScriptPresenter(const std::string& surface, base::DictValue answer) {
  Records()[surface] = PresenterRecord();
  Scripts()[surface].answer = std::move(answer);
}

void ClearPresenterScripts() {
  Scripts().clear();
  Records().clear();
}

bool PresenterScripted(const std::string& surface) {
  return Scripted::Find(surface) != nullptr;
}

std::optional<Dismisser> ScriptedPresent(
    const std::string& surface,
    const base::DictValue& payload,
    base::OnceCallback<void(const base::DictValue&)> answer) {
  if (!Scripted::Find(surface)) {
    return std::nullopt;
  }
  return Scripted::Present(
      surface, payload,
      [answer = std::move(answer)](const base::DictValue& a) mutable {
        std::move(answer).Run(a);
      });
}

void RecordPresented(const std::string& surface,
                     const base::DictValue& payload) {
  PresenterRecord& record = Records()[surface];
  ++record.shown;
  record.last_payload = ToJson(payload);
}

PresenterRecord PresenterRecordOf(const std::string& surface) {
  auto it = Records().find(surface);
  return it == Records().end() ? PresenterRecord() : it->second;
}

}  // namespace bdk
