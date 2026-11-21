// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/services/user_scripts.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/json_util.h"
#include "bdk/engine/page_protocol.h"
#include "bdk/engine/services/service_call.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_user_data.h"

namespace bdk {

namespace {

// The world the CSS match check runs in; nothing else uses it.
constexpr char kMatchWorld[] = "bdk-scripts";
// css_applied_ is a guard against applying twice; this bounds it.
constexpr size_t kMaxAppliedKeys = 2000;

// Chrome's match patterns (`<scheme>://<host>/<path>`, `<all_urls>`), the one
// place they are implemented, in JavaScript because that is where a script is
// registered: Page.addScriptToEvaluateOnNewDocument runs in every document,
// so the script has to decide whether it applies. The same code answers for
// the CSS (user_scripts.cc evaluates shouldRun in the frame), so the two never
// disagree. Only web-like URLs can match: an internal page's scheme is never
// in the list, whatever the pattern says.
constexpr char kMatcher[] = R"JS(
const webSchemes = ['http:', 'https:', 'file:', 'ftp:', 'ws:', 'wss:'];
const escapeRe = (s) => s.replace(/[.+?^${}()|[\]\\]/g, '\\$&');
const globToRe = (glob) =>
    new RegExp('^' + glob.split('*').map(escapeRe).join('.*') + '$');
const hostMatches = (pattern, host) => {
  pattern = pattern.toLowerCase();
  if (pattern === '*') return true;
  if (pattern.startsWith('*.')) {
    return host === pattern.slice(2) || host.endsWith(pattern.slice(1));
  }
  return host === pattern;
};
const patternMatches = (pattern, url) => {
  if (!webSchemes.includes(url.protocol)) return false;
  if (pattern === '<all_urls>') {
    return ['http:', 'https:', 'file:', 'ftp:'].includes(url.protocol);
  }
  const m = /^(\*|https?|file|ftp|wss?):\/\/([^\/]*)(\/.*)$/.exec(pattern);
  if (!m) return false;
  const scheme = m[1], host = m[2], path = m[3];
  if (scheme === '*') {
    if (url.protocol !== 'http:' && url.protocol !== 'https:') return false;
  } else if (url.protocol !== scheme + ':') {
    return false;
  }
  if (scheme !== 'file' && !hostMatches(host, url.hostname)) return false;
  return globToRe(path).test(url.pathname + url.search);
};
const shouldRun = (cfg) => {
  if (!cfg.allFrames && window !== window.top) return false;
  let url;
  try { url = new URL(location.href); } catch (e) { return false; }
  if (!cfg.matches.some((p) => patternMatches(p, url))) return false;
  return !cfg.excludes.some((p) => patternMatches(p, url));
};
)JS";

std::string ConfigJson(const bdkcore::UserScript& script) {
  base::DictValue config;
  base::ListValue matches;
  for (const std::string& pattern : script.matches) {
    matches.Append(pattern);
  }
  base::ListValue excludes;
  for (const std::string& pattern : script.excludes) {
    excludes.Append(pattern);
  }
  config.Set("matches", std::move(matches));
  config.Set("excludes", std::move(excludes));
  config.Set("allFrames", script.all_frames);
  return ToJson(config);
}

// One script of a registration as the page runs it: the match check, then the
// code at the requested time. The code is its own registration, so a syntax
// error in one script does not take the others with it; a runtime error is
// caught and logged to the page's console.
std::string WrapperSource(const bdkcore::UserScript& script,
                          const std::string& code) {
  std::string schedule;
  if (script.run_at == "documentStart") {
    schedule = "run();";
  } else if (script.run_at == "documentEnd") {
    schedule =
        "if (document.readyState === 'loading') {"
        "  document.addEventListener('DOMContentLoaded', run, {once: true});"
        "} else { run(); }";
  } else {
    // documentIdle, the default.
    schedule =
        "if (document.readyState === 'complete') { run(); } else {"
        "  window.addEventListener('load', run, {once: true}); }";
  }
  return std::string("(() => {") + kMatcher +
         "const cfg = " + ConfigJson(script) +
         ";if (!shouldRun(cfg)) return;const run = () => {"
         "try {\n" +
         code + "\n} catch (e) { console.error(" +
         JsonString("[bdk user script " + script.id + "]") + ", e); }};" +
         schedule + "})();";
}

bool Same(const bdkcore::UserScript& a, const bdkcore::UserScript& b) {
  return a.id == b.id && a.matches == b.matches && a.excludes == b.excludes &&
         a.run_at == b.run_at && a.world == b.world &&
         a.all_frames == b.all_frames && a.javascript == b.javascript &&
         a.css == b.css;
}

// The user scripts of one page's profile, applied to the page. Created for a
// page of a profile that has scripts, and not before: only such pages pay for
// the page's protocol client being attached, which it keeps (the scripts must
// be registered before the first document loads, so attaching when a command
// first needs it would be too late).
class PageScripts : public content::WebContentsUserData<PageScripts>,
                    public PageProtocol::SessionObserver {
 public:
  ~PageScripts() override {
    if (protocol_) {
      protocol_->RemoveSessionObserver(this);
    }
  }

  // The page's scripts are now `scripts`: what changed is registered again.
  void Update(const std::vector<bdkcore::UserScript>& scripts) {
    PageProtocol* protocol = protocol_.get();
    if (!protocol) {
      return;
    }
    if (!scripts.empty()) {
      protocol->KeepAttached();
    }
    std::map<std::string, bdkcore::UserScript> next;
    for (const bdkcore::UserScript& script : scripts) {
      next[script.id] = script;
    }
    for (auto it = scripts_.begin(); it != scripts_.end();) {
      auto fresh = next.find(it->first);
      if (fresh == next.end() || !Same(it->second, fresh->second)) {
        Unregister(it->first);
        it = scripts_.erase(it);
      } else {
        ++it;
      }
    }
    for (auto& entry : next) {
      if (scripts_.contains(entry.first)) {
        continue;
      }
      const bdkcore::UserScript& script =
          scripts_.emplace(entry.first, std::move(entry.second)).first->second;
      generation_[script.id] = ++last_generation_;
      for (const std::string& session : protocol->Sessions()) {
        Register(session, script);
      }
    }
  }

  // PageProtocol::SessionObserver:
  void OnSessionAttached(const std::string& session_id) override {
    for (const auto& entry : scripts_) {
      Register(session_id, entry.second);
    }
  }
  void OnSessionDetached(const std::string& session_id) override {
    registered_.erase(session_id);
  }
  void OnFrameLifecycle(const std::string& session_id,
                        const std::string& frame_id,
                        const std::string& loader_id,
                        const std::string& name) override {
    if (name != "DOMContentLoaded") {
      return;
    }
    for (const auto& entry : scripts_) {
      ApplyCss(entry.second, frame_id, loader_id);
    }
  }

 private:
  friend class content::WebContentsUserData<PageScripts>;

  explicit PageScripts(content::WebContents* web_contents)
      : content::WebContentsUserData<PageScripts>(*web_contents),
        protocol_(PageProtocol::For(web_contents)->GetWeakPtr()) {
    protocol_->AddSessionObserver(this);
  }

  // Registers the script's JavaScript in the session, where it matters: the
  // page's session always, an out-of-process iframe's only for a script that
  // runs in every frame.
  void Register(const std::string& session_id,
                const bdkcore::UserScript& script) {
    PageProtocol* protocol = protocol_.get();
    if (!protocol || (!session_id.empty() && !script.all_frames)) {
      return;
    }
    const int generation = generation_[script.id];
    for (const std::string& code : script.javascript) {
      base::DictValue params;
      params.Set("source", WrapperSource(script, code));
      // "page" is the page's own world; any other name is an isolated world,
      // by default one of the script's own.
      if (script.world != "page") {
        params.Set("worldName",
                   script.world.empty() ? script.id : script.world);
      }
      protocol->Send(
          session_id, "Page.addScriptToEvaluateOnNewDocument",
          std::move(params),
          base::BindOnce(
              [](base::WeakPtr<PageScripts> self, std::string session_id,
                 std::string script_id, int generation, ProtocolResult result) {
                const std::string* identifier =
                    result.ok ? result.result.FindString("identifier")
                              : nullptr;
                if (!self || !identifier) {
                  return;
                }
                // The session may have gone while the command was sent.
                PageProtocol* protocol = self->protocol_.get();
                if (!protocol) {
                  return;
                }
                const std::vector<std::string> sessions = protocol->Sessions();
                if (std::find(sessions.begin(), sessions.end(), session_id) ==
                    sessions.end()) {
                  return;
                }
                auto live = self->generation_.find(script_id);
                if (live == self->generation_.end() ||
                    live->second != generation) {
                  // The script was removed or replaced while this was sent.
                  self->RemoveRegistration(session_id, *identifier);
                  return;
                }
                self->registered_[session_id][script_id].push_back(*identifier);
              },
              weak_factory_.GetWeakPtr(), session_id, script.id, generation));
    }
  }

  void RemoveRegistration(const std::string& session_id,
                          const std::string& identifier) {
    PageProtocol* protocol = protocol_.get();
    if (!protocol) {
      return;
    }
    base::DictValue params;
    params.Set("identifier", identifier);
    protocol->Send(session_id, "Page.removeScriptToEvaluateOnNewDocument",
                   std::move(params), ProtocolCallback());
  }

  // Takes a script's JavaScript out of every session, and its CSS out of the
  // documents it was applied to.
  void Unregister(const std::string& script_id) {
    generation_.erase(script_id);
    for (auto& session : registered_) {
      auto found = session.second.find(script_id);
      if (found == session.second.end()) {
        continue;
      }
      for (const std::string& identifier : found->second) {
        RemoveRegistration(session.first, identifier);
      }
      session.second.erase(found);
    }
    auto css = css_ids_.find(script_id);
    if (css != css_ids_.end()) {
      if (PageProtocol* protocol = protocol_.get()) {
        for (const std::string& id : css->second) {
          protocol->RemoveCss(id, base::DoNothing());
        }
      }
      css_ids_.erase(css);
    }
    std::erase_if(css_applied_, [&](const std::string& key) {
      return key.starts_with(script_id + "|");
    });
  }

  // The script's CSS, once, in a frame whose document has just loaded its
  // DOM, if the frame matches. Through the same style sheet as insertCSS,
  // which the page's content security policy does not restrict.
  void ApplyCss(const bdkcore::UserScript& script,
                const std::string& frame_id,
                const std::string& loader_id) {
    PageProtocol* protocol = protocol_.get();
    if (!protocol || script.css.empty()) {
      return;
    }
    content::RenderFrameHost* main =
        protocol->GetWebContents().GetPrimaryMainFrame();
    if (!script.all_frames &&
        (!main || frame_id != PageProtocol::FrameId(main))) {
      return;
    }
    const std::string key = script.id + "|" + frame_id + "|" + loader_id;
    if (css_applied_.size() > kMaxAppliedKeys) {
      css_applied_.clear();
    }
    if (!css_applied_.insert(key).second) {
      return;
    }
    const int generation = generation_[script.id];
    protocol->Evaluate(
        frame_id, World::Isolated(kMatchWorld),
        std::string("(() => {") + kMatcher + "return shouldRun(" +
            ConfigJson(script) + ");})()",
        /*user_gesture=*/false,
        base::BindOnce(
            [](base::WeakPtr<PageScripts> self, std::string script_id,
               int generation, std::string frame_id, EvaluateResult result) {
              if (!self || result.status != EvaluateResult::Status::kValue ||
                  !result.value.is_bool() || !result.value.GetBool()) {
                return;
              }
              auto script = self->scripts_.find(script_id);
              auto live = self->generation_.find(script_id);
              if (script == self->scripts_.end() ||
                  live == self->generation_.end() ||
                  live->second != generation) {
                return;
              }
              self->InsertCss(script_id, generation, frame_id,
                              script->second.css);
            },
            weak_factory_.GetWeakPtr(), script.id, generation, frame_id));
  }

  void InsertCss(const std::string& script_id,
                 int generation,
                 const std::string& frame_id,
                 const std::vector<std::string>& css) {
    PageProtocol* protocol = protocol_.get();
    if (!protocol) {
      return;
    }
    for (const std::string& text : css) {
      protocol->InsertCss(
          frame_id, text,
          base::BindOnce(
              [](base::WeakPtr<PageScripts> self, std::string script_id,
                 int generation, std::string id, std::string error) {
                if (!self || id.empty()) {
                  return;
                }
                auto live = self->generation_.find(script_id);
                if (live == self->generation_.end() ||
                    live->second != generation) {
                  // Removed while the sheet was written.
                  if (PageProtocol* protocol = self->protocol_.get()) {
                    protocol->RemoveCss(id, base::DoNothing());
                  }
                  return;
                }
                self->css_ids_[script_id].push_back(id);
              },
              weak_factory_.GetWeakPtr(), script_id, generation));
    }
  }

  base::WeakPtr<PageProtocol> protocol_;
  // What is applied: by script id.
  std::map<std::string, bdkcore::UserScript> scripts_;
  // A script's registrations: session, then script id, then the protocol's
  // identifiers.
  std::map<std::string, std::map<std::string, std::vector<std::string>>>
      registered_;
  // Bumped when a script is added, so a late answer for a removed or
  // replaced script can be told from a current one.
  std::map<std::string, int> generation_;
  int last_generation_ = 0;
  // The CSS ids (page_protocol.h) of each script, and the documents already
  // given each script's CSS ("script|frame|loader").
  std::map<std::string, std::vector<std::string>> css_ids_;
  std::set<std::string> css_applied_;
  base::WeakPtrFactory<PageScripts> weak_factory_{this};
  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(PageScripts);

struct ProfileScripts {
  base::WeakPtr<Profile> profile;
  std::vector<bdkcore::UserScript> scripts;
};

// The scripts of each profile, for a page that appears later. Keyed by the
// profile; an entry whose profile is gone is ignored.
std::map<Profile*, ProfileScripts>& Registry() {
  static base::NoDestructor<std::map<Profile*, ProfileScripts>> registry;
  return *registry;
}

Profile* ProfileOfPage(content::WebContents* web_contents) {
  return Profile::FromBrowserContext(web_contents->GetBrowserContext());
}

}  // namespace

void ApplyUserScripts(Profile* profile,
                      const std::vector<bdkcore::UserScript>& scripts) {
  if (scripts.empty()) {
    Registry().erase(profile);
  } else {
    ProfileScripts& entry = Registry()[profile];
    entry.profile = profile->GetWeakPtr();
    entry.scripts = scripts;
  }
  for (EngineContents* page : EngineContents::All()) {
    content::WebContents* web_contents = page->web_contents();
    if (!web_contents || ProfileOfPage(web_contents) != profile) {
      continue;
    }
    if (!scripts.empty()) {
      PageScripts::GetOrCreateForWebContents(web_contents)->Update(scripts);
    } else if (PageScripts* existing =
                   PageScripts::FromWebContents(web_contents)) {
      existing->Update(scripts);
    }
  }
}

void ApplyUserScriptsToPage(content::WebContents* web_contents) {
  auto it = Registry().find(ProfileOfPage(web_contents));
  if (it == Registry().end() || !it->second.profile) {
    return;
  }
  PageScripts::GetOrCreateForWebContents(web_contents)
      ->Update(it->second.scripts);
}

}  // namespace bdk
