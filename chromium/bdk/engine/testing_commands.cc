// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// bdk_testing_command (bdk_testing.h): the one test hook for the engine's
// browser-owned surfaces and services. It scripts the default presenters,
// drives find in page, reads what Chromium holds (preferences, site settings),
// starts captures, sets what the operating system's prompt and the download
// policy answer, loads a reputation list and reads what the network audit
// needs (providers.settings, network.hosts).

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/feature_list.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/metrics/field_trial_params.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "base/values.h"
#include "bdk/core/runtime/core_internal.h"
#include "bdk/engine/bdk_testing.h"
#include "bdk/engine/client_cert_testing.h"
#include "bdk/engine/device_chooser_testing.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/extensions/extension_testing.h"
#include "bdk/engine/extensions/extension_ui_testing.h"
#include "bdk/engine/extensions/panel_testing.h"
#include "bdk/engine/host_context.h"
#include "bdk/engine/json_util.h"
#include "bdk/engine/providers/provider_settings.h"
#include "bdk/engine/providers/reputation_service.h"
#include "bdk/engine/services/authentication.h"
#include "bdk/engine/services/capture_service.h"
#include "bdk/engine/services/download_prompts.h"
#include "bdk/engine/services/permissions.h"
#include "bdk/engine/testing.h"
#include "bdk/engine/ui_find.h"
#include "bdk/engine/ui_presenters.h"
#include "bdk/engine/webauthn_testing.h"
#include "chrome/browser/after_startup_task_utils.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/external_protocol/external_protocol_handler.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/resource_coordinator/tab_lifecycle_unit_external.h"
#include "chrome/browser/ui/dialogs/browser_dialogs.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/heavy_ad_intervention/heavy_ad_features.h"
#include "components/input/native_web_keyboard_event.h"
#include "components/prefs/pref_service.h"
#include "content/browser/web_contents/web_contents_impl.h"  // nogncheck
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "google_apis/google_api_keys.h"
#include "services/device/public/cpp/device_features.h"
#include "services/device/public/mojom/geolocation_internals.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "ui/base/models/dialog_model.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "url/gurl.h"

namespace bdk {

namespace {

// 0 nothing started or still running, 1 written, 2 failed.
int g_capture_state = 0;

// Whether the task startup.post posted has run.
bool& AfterStartupTaskRan() {
  static bool ran = false;
  return ran;
}

// What happened to the last dialog dialogs.tab_modal showed: "accepted",
// "cancelled" or "closed" (empty while open), and whether the model is gone.
struct TabModalOutcome {
  std::string result;
  bool destroyed = false;
};

TabModalOutcome& LastTabModal() {
  static base::NoDestructor<TabModalOutcome> outcome;
  return *outcome;
}

// A dialog model as File System Access and DevTools build theirs: a title, a
// paragraph, OK and Cancel, given to chrome::ShowTabModal.
void ShowTestTabModal(content::WebContents* contents,
                      const std::string& title,
                      const std::string& text,
                      bool default_cancel) {
  LastTabModal() = TabModalOutcome();
  auto set = [](const char* result) {
    return base::BindOnce(
        [](const char* value) { LastTabModal().result = value; }, result);
  };
  ui::DialogModel::Builder builder;
  builder.SetTitle(base::UTF8ToUTF16(title))
      .AddParagraph(ui::DialogModelLabel(base::UTF8ToUTF16(text)))
      .AddOkButton(set("accepted"))
      .AddCancelButton(set("cancelled"))
      .SetCloseActionCallback(set("closed"))
      .SetDialogDestroyingCallback(
          base::BindOnce([] { LastTabModal().destroyed = true; }));
  if (default_cancel) {
    builder.OverrideDefaultButton(ui::mojom::DialogButton::kCancel);
  }
  chrome::ShowTabModal(builder.Build(), contents);
}

EngineContents* PageOf(const base::DictValue& args) {
  const std::string* handle = args.FindString("contents");
  uint64_t value = 0;
  if (!handle || !base::StringToUint64(*handle, &value)) {
    return nullptr;
  }
  std::shared_ptr<bdkcore::Contents> c =
      bdkcore::contents_of(static_cast<bdk_contents_t>(value));
  return c ? EngineContents::From(*c) : nullptr;
}

// Starts the capture of `page` to the file `path`; the state tells when it is
// written.
void Capture(EngineContents& page, bool pdf, const std::string& path) {
  g_capture_state = 0;
  const base::FilePath file = base::FilePath::FromUTF8Unsafe(path);
  CaptureCallback done = base::BindOnce(
      [](base::FilePath file, std::optional<std::vector<uint8_t>> bytes,
         std::string error) {
        if (!bytes) {
          g_capture_state = 2;
          return;
        }
        WriteCaptureFile(file, std::move(*bytes),
                         base::BindOnce([](bool written) {
                           g_capture_state = written ? 1 : 2;
                         }));
      },
      file);
  if (pdf) {
    CapturePdf(page.web_contents(), std::move(done));
  } else {
    CaptureScreenshot(page.web_contents(), std::move(done));
  }
}

// Runs the `find.*` commands; returns false for a command it does not know.
bool RunFind(const std::string& name,
             EngineContents& page,
             const base::DictValue& args,
             base::DictValue& result) {
  PageFind& find = page.find();
  if (name == "find.present") {
    find.Present();
  } else if (name == "find.start") {
    const std::string* text = args.FindString("text");
    find.Start(text ? *text : std::string(),
               args.FindBool("matchCase").value_or(false));
  } else if (name == "find.next") {
    find.Next(args.FindBool("forward").value_or(true));
  } else if (name == "find.stop") {
    find.Stop(args.FindBool("clearSelection").value_or(true));
  } else if (name != "find.result") {
    return false;
  }
  result.Set("ordinal", find.result().ordinal);
  result.Set("count", find.result().count);
  result.Set("presented", find.IsPresented());
  return true;
}

// Runs a command; returns false when it is unknown or its arguments are bad.
bool Run(const std::string& name,
         const base::DictValue& args,
         base::DictValue& result) {
  const std::string* answer = args.FindString("answer");
  const std::string* surface = args.FindString("surface");

  if (name == "present.script") {
    if (!surface) {
      return false;
    }
    const base::DictValue* script = args.FindDict("answer");
    ScriptPresenter(*surface, script ? script->Clone() : base::DictValue());
    return true;
  }
  if (name == "present.clear") {
    ClearPresenterScripts();
    return true;
  }
  if (name == "present.record") {
    if (!surface) {
      return false;
    }
    PresenterRecord record = PresenterRecordOf(*surface);
    result.Set("shown", record.shown);
    result.Set("dismissed", record.dismissed);
    result.Set("payload", record.last_payload);
    return true;
  }
  if (name.starts_with("find.")) {
    EngineContents* page = PageOf(args);
    return page && RunFind(name, *page, args, result);
  }
  if (name == "permission.get") {
    EngineContents* page = PageOf(args);
    const std::string* origin = args.FindString("origin");
    const std::string* kind = args.FindString("kind");
    const PermissionKind* permission =
        kind ? FindPermissionKind(*kind) : nullptr;
    if (!page || !origin || !permission) {
      return false;
    }
    const GURL url(*origin);
    result.Set("value",
               PermissionValueName(
                   HostContentSettingsMapFactory::GetForProfile(
                       ProfileOf(*page->Core()))
                       ->GetContentSetting(url, url, permission->type)));
    return true;
  }
  if (name == "pref.json") {
    // The JSON value of the Chromium preference `name` in the page's profile.
    EngineContents* page = PageOf(args);
    const std::string* pref_name = args.FindString("name");
    if (!page || !pref_name) {
      return false;
    }
    const PrefService::Preference* pref =
        ProfileOf(*page->Core())->GetPrefs()->FindPreference(*pref_name);
    if (!pref) {
      return false;
    }
    result.Set("value", pref->GetValue()->Clone());
    return true;
  }
  if (name == "heavy_ad.store") {
    // Where the heavy-ad intervention keeps its opt-out store, decided as
    // HeavyAdService::Initialize decides it: "disk" or "memory" (BDK keeps
    // it in memory; chromium_main_mac.mm).
    const bool on_disk =
        base::FeatureList::IsEnabled(
            heavy_ad_intervention::features::kHeavyAdPrivacyMitigations) &&
        !base::GetFieldTrialParamByFeatureAsBool(
            heavy_ad_intervention::features::kHeavyAdPrivacyMitigations,
            "OptOutStoreDisabled", false);
    result.Set("store", on_disk ? "disk" : "memory");
    return true;
  }
  if (name == "startup.post") {
    // Posts a task the way Chromium posts its after-start-up work
    // (AfterStartupTaskUtils::PostTask, T-ADP-10); startup.state says whether
    // it ran.
    AfterStartupTaskUtils::PostTask(
        FROM_HERE, base::SequencedTaskRunner::GetCurrentDefault(),
        base::BindOnce([] { AfterStartupTaskRan() = true; }));
    return true;
  }
  if (name == "protocols.permit") {
    // Chrome accepts one external-protocol launch until a user gesture
    // permits the next; a scenario permits it as such a gesture would.
    ExternalProtocolHandler::PermitLaunchUrl();
    return true;
  }
  if (name == "pages.count") {
    // What is alive in the process: every WebContents, BDK's pages and their
    // host contexts (T-REL-03), for scenarios that check nothing is left
    // behind.
    result.Set(
        "webContents",
        static_cast<int>(content::WebContentsImpl::GetAllWebContents().size()));
    result.Set("pages", static_cast<int>(EngineContents::All().size()));
    result.Set("hostContexts",
               static_cast<int>(HostContexts::Get().ContextCount()));
    return true;
  }
  if (name == "startup.state") {
    result.Set("complete", AfterStartupTaskUtils::IsBrowserStartupComplete());
    result.Set("ran", AfterStartupTaskRan());
    return true;
  }
  if (name == "capture.start") {
    // {"contents", "path", "format": "png" | "pdf"}; poll capture.state.
    EngineContents* page = PageOf(args);
    const std::string* path = args.FindString("path");
    const std::string* format = args.FindString("format");
    if (!page || !path || !format || (*format != "png" && *format != "pdf")) {
      return false;
    }
    Capture(*page, *format == "pdf", *path);
    return true;
  }
  if (name == "tab.chromiumDiscard") {
    // Chromium's own discard of the page's tab, as under memory pressure: it
    // swaps a WebContents with no renderer into the tab.
    EngineContents* page = PageOf(args);
    auto* unit =
        page ? resource_coordinator::TabLifecycleUnitExternal::FromWebContents(
                   page->web_contents())
             : nullptr;
    if (!unit) {
      return false;
    }
    result.Set("discarded",
               unit->DiscardTab(::mojom::LifecycleUnitDiscardReason::URGENT));
    return true;
  }
  if (name == "capture.state") {
    result.Set("state", g_capture_state);
    return true;
  }
  if (name == "authentication") {
    // What the operating system's prompt answers: "allow", "deny" or "ask"
    // (the real prompt).
    AuthenticationAnswer value = AuthenticationAnswer::kAsk;
    if (answer && *answer == "allow") {
      value = AuthenticationAnswer::kAllow;
    } else if (answer && *answer == "deny") {
      value = AuthenticationAnswer::kDeny;
    }
    SetAuthenticationAnswerForTesting(value);
    return true;
  }
  if (name == "authentication.count") {
    result.Set("count", AuthenticationRequestCountForTesting());
    return true;
  }
  if (name == "downloads.ask") {
    // Makes every loaded profile ask where to save, as the setting does.
    AskWhereToSaveForTesting(args.FindBool("on").value_or(true));
    return true;
  }
  if (name == "downloads.replaceDelegate") {
    // {"contents"}: BDK's download delegate is put in again on the page's
    // profile, whose download manager exists by now.
    EngineContents* page = PageOf(args);
    if (!page) {
      return false;
    }
    ReplaceDownloadDelegateForTesting(ProfileOf(*page->Core()));
    return true;
  }
  if (name == "downloads.danger") {
    ForceDangerousDownloadsForTesting(args.FindBool("on").value_or(true));
    return true;
  }
  // Windows, side panels, anchored prompts and loading extension fixtures
  // (extensions.load, extensions.eval); first, because every other extensions.
  // verb is the install tests'.
  if (std::optional<bool> handled =
          RunExtensionPanelCommand(name, args, result)) {
    return *handled;
  }
  if (name.starts_with("extensions.")) {
    return RunExtensionTestingCommand(name, args, result);
  }
  if (name.starts_with("extui.")) {
    return RunExtensionUiTestCommand(name, args, result);
  }
  if (name.starts_with("webauthn.")) {
    return RunWebAuthnTestingCommand(name, args, result);
  }
  if (name == "input.key") {
    // {"contents", "key": "Escape", "type": "down" | "up"}: a key event
    // through the page's render widget host, as the view sends one, so the
    // page's delegate sees it first (PreHandleKeyboardEvent).
    EngineContents* page = PageOf(args);
    const std::string* key = args.FindString("key");
    const std::string* type = args.FindString("type");
    if (!page || !page->web_contents() || !key || *key != "Escape" || !type) {
      return false;
    }
    content::RenderWidgetHostView* view =
        page->web_contents()->GetRenderWidgetHostView();
    if (!view) {
      return false;
    }
    input::NativeWebKeyboardEvent event(
        *type == "up" ? blink::WebInputEvent::Type::kKeyUp
                      : blink::WebInputEvent::Type::kRawKeyDown,
        blink::WebInputEvent::kNoModifiers, base::TimeTicks::Now());
    event.windows_key_code = ui::VKEY_ESCAPE;
    event.dom_key = ui::DomKey::ESCAPE;
    event.dom_code = static_cast<int>(ui::DomCode::ESCAPE);
    view->GetRenderWidgetHost()->ForwardKeyboardEvent(event);
    return true;
  }
  if (name.starts_with("clientcert.")) {
    return RunClientCertTestingCommand(name, args, result);
  }
  if (name.starts_with("chooser.")) {
    return RunChooserTestingCommand(name, PageOf(args), args, result);
  }
  if (name == "reputation.list") {
    // {"json"}: the default provider's list, as a checked download would load
    // it (providers/reputation_service.h).
    const std::string* json = args.FindString("json");
    std::string error;
    if (!json) {
      return false;
    }
    result.Set("loaded", LoadReputationList(*json, &error));
    result.Set("error", error);
    return true;
  }
  if (name == "providers.settings") {
    // What Chromium runs with for providers and privacy (T-PRV-07): the
    // geolocation mode, whether component updates and sync are off, whether
    // a Google key is configured.
    const base::CommandLine& line = *base::CommandLine::ForCurrentProcess();
    result.Set("componentUpdatesDisabled",
               line.HasSwitch("disable-component-update"));
    result.Set("syncDisabled", line.HasSwitch("disable-sync"));
    result.Set("platformOnlyGeolocation",
               ::features::kLocationProviderManagerParam.Get() ==
                   device::mojom::LocationProviderManagerMode::kPlatformOnly);
    result.Set("googleKey", google_apis::HasAPIKeyConfigured());
    result.Set("geolocation", CurrentProviderSettings().geolocation);
    result.Set("reputation", CurrentProviderSettings().reputation);
    return true;
  }
  if (name == "network.hosts") {
    // {"path"}: the hosts a net log (--log-net-log) shows requests to, or
    // host names resolved for, so far. The log is read as it is being
    // written: each event is one line.
    const std::string* path = args.FindString("path");
    std::string text;
    // A test hook, run on the UI thread: the file is small and local.
    base::ScopedAllowBlockingForTesting allow_blocking;
    if (!path || !base::ReadFileToString(base::FilePath(*path), &text)) {
      return false;
    }
    std::set<std::string> hosts;
    for (std::string_view line : base::SplitStringPiece(
             text, "\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
      for (std::string_view key : {"\"url\":\"", "\"host\":\""}) {
        size_t at = line.find(key);
        if (at == std::string_view::npos) {
          continue;
        }
        std::string_view value = line.substr(at + key.size());
        value = value.substr(0, value.find('"'));
        GURL url(value.find("://") == std::string_view::npos
                     ? "https://" + std::string(value)
                     : std::string(value));
        if (url.is_valid() && !url.host().empty()) {
          hosts.insert(std::string(url.host()));
        }
      }
    }
    base::ListValue list;
    for (const std::string& host : hosts) {
      list.Append(host);
    }
    result.Set("hosts", std::move(list));
    return true;
  }
  if (name == "pref.set") {
    // {"contents", "name", "value"}: sets the Chromium preference `name` of
    // the page's profile to the JSON value.
    EngineContents* page = PageOf(args);
    const std::string* pref_name = args.FindString("name");
    const base::Value* value = args.Find("value");
    if (!page || !pref_name || !value) {
      return false;
    }
    PrefService* prefs = ProfileOf(*page->Core())->GetPrefs();
    const PrefService::Preference* pref = prefs->FindPreference(*pref_name);
    if (!pref || pref->GetType() != value->type()) {
      return false;
    }
    prefs->Set(*pref_name, value->Clone());
    return true;
  }
  if (name == "dialogs.tab_modal") {
    // {"contents", "title", "text", "defaultCancel"}: a dialog model through
    // chrome::ShowTabModal; poll dialogs.tab_modal.result.
    EngineContents* page = PageOf(args);
    const std::string* title = args.FindString("title");
    const std::string* text = args.FindString("text");
    if (!page || !title || !text) {
      return false;
    }
    ShowTestTabModal(page->web_contents(), *title, *text,
                     args.FindBool("defaultCancel").value_or(false));
    return true;
  }
  if (name == "dialogs.tab_modal.result") {
    result.Set("result", LastTabModal().result);
    result.Set("destroyed", LastTabModal().destroyed);
    return true;
  }
  if (name == "hsts.add") {
    // {"contents", "host"}: the page's profile treats the host as HSTS, so a
    // certificate error there is not overridable (T-UIR-07).
    EngineContents* page = PageOf(args);
    const std::string* host = args.FindString("host");
    if (!page || !host) {
      return false;
    }
    ProfileOf(*page->Core())
        ->GetDefaultStoragePartition()
        ->GetNetworkContext()
        ->AddHSTS(*host, base::Time::Now() + base::Days(1),
                  /*include_subdomains=*/false, base::DoNothing());
    return true;
  }
  if (name == "network.passthrough") {
    // {"host"}: loads from the host go to the network, not to the scripted
    // pages (a test's own server on a name mapped to the loopback address).
    const std::string* host = args.FindString("host");
    if (!host) {
      return false;
    }
    PassThroughHostForTesting(*host);
    return true;
  }
  if (name == "script_page") {
    // A page with content: {"url", "body" or "bodyBase64" (binary, an image),
    // "contentType", "headers"}.
    const std::string* url = args.FindString("url");
    const std::string* body = args.FindString("body");
    const std::string* body_base64 = args.FindString("bodyBase64");
    std::string decoded;
    if (body_base64 && !base::Base64Decode(*body_base64, &decoded)) {
      return false;
    }
    if (!url || (!body && !body_base64)) {
      return false;
    }
    const std::string* content_type = args.FindString("contentType");
    const std::string* headers = args.FindString("headers");
    ScriptPageForTesting(*url, body ? *body : decoded,
                         content_type ? *content_type : "",
                         headers ? *headers : "");
    return true;
  }
  return false;
}

}  // namespace

}  // namespace bdk

BDK_EXPORT bdk_status_t bdk_testing_command(bdk_runtime_t runtime,
                                            bdk_string_t command,
                                            bdk_string_t args_json,
                                            char* out,
                                            size_t out_size) {
  if (bdk_status_t s = bdkcore::check_thread()) {
    return s;
  }
  if (!bdkcore::runtime_of(runtime)) {
    return BDK_ERR_STALE_HANDLE;
  }
  if (!bdkcore::string_ok(command) || !bdkcore::string_ok(args_json)) {
    return BDK_ERR_INVALID_ARGUMENT;
  }
  std::optional<base::DictValue> args =
      bdk::ParseJsonDict(args_json.length ? bdkcore::str(args_json) : "{}");
  base::DictValue result;
  if (!args || !bdk::Run(bdkcore::str(command), *args, result)) {
    return BDK_ERR_INVALID_ARGUMENT;
  }
  if (out && out_size > 0) {
    const std::string text = bdk::ToJson(result);
    if (text.size() + 1 > out_size) {
      return BDK_ERR_INVALID_ARGUMENT;
    }
    // The caller promises `out_size` bytes at `out`.
    base::span<char> buffer = UNSAFE_BUFFERS(base::span<char>(out, out_size));
    buffer.first(text.size()).copy_from(base::span(text));
    buffer[text.size()] = '\0';
  }
  return BDK_OK;
}
