// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/extensions/action_service.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/scoped_observation.h"
#include "base/strings/string_number_conversions.h"
#include "bdk/engine/bdk_browser_window.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/extensions/extension_ui_util.h"
#include "bdk/engine/extensions/extensions_container.h"
#include "bdk/engine/host_context.h"
#include "bdk/engine/json_util.h"
#include "bdk/engine/ui_presenters.h"
#include "bdk/engine/ui_request.h"
#include "chrome/browser/extensions/extension_action_dispatcher.h"
#include "chrome/browser/extensions/extension_action_runner.h"
#include "chrome/browser/extensions/extension_view_host.h"
#include "chrome/browser/extensions/extension_view_host_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/extensions/extension_side_panel_utils.h"
#include "chrome/browser/ui/toolbar/toolbar_actions_model.h"
#include "content/public/browser/web_contents.h"
#include "extensions/browser/extension_action.h"
#include "extensions/browser/extension_action_icon_factory.h"
#include "extensions/browser/extension_action_manager.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_registry_observer.h"
#include "extensions/browser/permissions/site_permissions_helper.h"
#include "extensions/common/api/commands/commands_handler.h"
#include "extensions/common/command.h"
#include "extensions/common/extension.h"
#include "ui/gfx/image/image.h"

namespace bdk {

namespace {

using extensions::Extension;
using extensions::ExtensionAction;
using extensions::ExtensionActionManager;
using extensions::ExtensionActionRunner;

// Whether the action can be clicked for `page`: it is visible there, or the
// extension is waiting to be allowed to run on the site (what the toolbar
// button greys out for).
bool ActionEnabled(Profile* profile,
                   const Extension& extension,
                   const ExtensionAction& action,
                   content::WebContents* page) {
  const int tab_id =
      page ? ExtensionTabId(page) : ExtensionAction::kDefaultTabId;
  if (action.GetIsVisible(tab_id)) {
    return true;
  }
  if (!page) {
    return false;
  }
  switch (extensions::SitePermissionsHelper(profile).GetSiteInteraction(
      extension, page)) {
    case extensions::SitePermissionsHelper::SiteInteraction::kWithheld:
    case extensions::SitePermissionsHelper::SiteInteraction::kActiveTab:
      return true;
    default:
      return false;
  }
}

// Observes one profile's extension actions and tells the application when one
// changes. It also keeps the action icons: Chromium loads them asynchronously,
// so an icon is asked for once (the first state may still show the placeholder)
// and the change event reports when it arrives.
class ActionEvents : public ServiceEvents,
                     public extensions::ExtensionActionDispatcher::Observer,
                     public extensions::ExtensionRegistryObserver {
 public:
  ActionEvents(Profile* profile, std::weak_ptr<bdkcore::Profile> bdk_profile)
      : profile_(profile), bdk_profile_(std::move(bdk_profile)) {
    Registry()[profile_] = this;
    if (auto* dispatcher =
            extensions::ExtensionActionDispatcher::Get(profile_)) {
      dispatcher_observation_.Observe(dispatcher);
    }
    registry_observation_.Observe(extensions::ExtensionRegistry::Get(profile_));
  }

  ~ActionEvents() override { Registry().erase(profile_); }

  static ActionEvents* For(Profile* profile) {
    auto it = Registry().find(profile);
    return it == Registry().end() ? nullptr : it->second;
  }

  // The icon of `action` for the tab, or an empty image.
  gfx::Image IconFor(const Extension& extension,
                     ExtensionAction& action,
                     int tab_id) {
    std::unique_ptr<Icon>& icon = icons_[extension.id()];
    if (!icon) {
      icon = std::make_unique<Icon>(this, extension, &action);
    }
    return icon->factory.GetIcon(tab_id);
  }

  // Tells the application that the action of `extension_id` changed for
  // `page`, or for its defaults when `page` is null.
  void Notify(const std::string& extension_id, content::WebContents* page) {
    std::string contents = "null";
    if (page) {
      EngineContents* engine_page = EngineContents::FromWebContents(page);
      std::shared_ptr<bdkcore::Contents> core =
          engine_page ? engine_page->Core() : nullptr;
      if (core) {
        contents = "\"" + base::NumberToString(core->handle) + "\"";
      }
    }
    EmitEvent(bdk_profile_, "extensions.actionChanged",
              "{\"id\":" + JsonString(extension_id) +
                  ",\"contents\":" + contents + "}");
  }

  // extensions::ExtensionActionDispatcher::Observer:
  void OnExtensionActionUpdated(
      ExtensionAction* extension_action,
      content::WebContents* web_contents,
      content::BrowserContext* browser_context) override {
    // The dispatcher is shared by a profile and its private profiles.
    if (Profile::FromBrowserContext(browser_context) != profile_) {
      return;
    }
    Notify(extension_action->extension_id(), web_contents);
  }
  void OnShuttingDown() override { dispatcher_observation_.Reset(); }

  // extensions::ExtensionRegistryObserver:
  void OnExtensionUnloaded(
      content::BrowserContext* browser_context,
      const Extension* extension,
      extensions::UnloadedExtensionReason reason) override {
    icons_.erase(extension->id());
  }

 private:
  // One action's icon factory, which reports an icon that finished loading.
  struct Icon : public extensions::ExtensionActionIconFactory::Observer {
    Icon(ActionEvents* owner,
         const Extension& extension,
         ExtensionAction* action)
        : owner(owner), id(extension.id()), factory(&extension, action, this) {}

    // extensions::ExtensionActionIconFactory::Observer:
    void OnIconUpdated() override { owner->Notify(id, nullptr); }

    const raw_ptr<ActionEvents> owner;
    const std::string id;
    extensions::ExtensionActionIconFactory factory;
  };

  static std::map<Profile*, ActionEvents*>& Registry() {
    static base::NoDestructor<std::map<Profile*, ActionEvents*>> registry;
    return *registry;
  }

  const raw_ptr<Profile> profile_;
  const std::weak_ptr<bdkcore::Profile> bdk_profile_;
  std::map<std::string, std::unique_ptr<Icon>> icons_;
  base::ScopedObservation<extensions::ExtensionActionDispatcher,
                          extensions::ExtensionActionDispatcher::Observer>
      dispatcher_observation_{this};
  base::ScopedObservation<extensions::ExtensionRegistry,
                          extensions::ExtensionRegistryObserver>
      registry_observation_{this};
};

base::DictValue ActionState(Profile* profile,
                            const Extension& extension,
                            ExtensionAction& action,
                            content::WebContents* page) {
  const int tab_id =
      page ? ExtensionTabId(page) : ExtensionAction::kDefaultTabId;
  base::DictValue state;
  state.Set("id", extension.id());
  const std::string title = action.GetTitle(tab_id);
  state.Set("title", title.empty() ? extension.name() : title);
  std::string icon;
  if (ActionEvents* events = ActionEvents::For(profile)) {
    icon = PngDataUrl(events->IconFor(extension, action, tab_id));
  }
  if (icon.empty()) {
    state.Set("icon", base::Value());
  } else {
    state.Set("icon", icon);
  }
  const std::string badge = action.GetDisplayBadgeText(tab_id);
  if (badge.empty()) {
    state.Set("badgeText", base::Value());
    state.Set("badgeColor", base::Value());
  } else {
    state.Set("badgeText", badge);
    state.Set("badgeColor", ColorHex(action.GetBadgeBackgroundColor(tab_id)));
  }
  state.Set("isEnabled", ActionEnabled(profile, extension, action, page));
  state.Set("hasPopup", action.HasPopup(tab_id));
  ExtensionActionRunner* runner =
      ExtensionActionRunner::GetForWebContents(page);
  state.Set("wantsAccess", runner && runner->WantsToRun(&extension));
  // The user's pin choice lives in the profile's extension preferences, which
  // Chromium's toolbar model reads and writes (extensions.setPinned).
  ToolbarActionsModel* model = ToolbarActionsModel::Get(profile);
  state.Set("isPinned", model && model->IsActionPinned(extension.id()));
  return state;
}

// Reads the page and profile a call names. False when the call answered
// itself.
bool CallPage(const std::shared_ptr<ServiceCall>& call,
              content::WebContents** page) {
  *page = nullptr;
  const std::shared_ptr<bdkcore::Contents>& contents = call->builtin().contents;
  if (!contents) {
    return true;
  }
  if (contents->profile != call->builtin().profile) {
    call->InvalidArguments("the page belongs to another profile");
    return false;
  }
  EngineContents* engine_page = EngineContents::From(*contents);
  *page = engine_page ? engine_page->web_contents() : nullptr;
  return true;
}

void ActionsCommand(const std::shared_ptr<ServiceCall>& call) {
  if (!bdkcore::ui_surface_available(call->runtime(),
                                     BDK_SURFACE_EXTENSION_ACTIONS)) {
    call->Reject(BDK_DOMAIN_EXTENSION, BDK_CODE_SURFACE_DISABLED,
                 "action state is not available: [extensions.ui] actions is "
                 "disabled");
    return;
  }
  content::WebContents* page = nullptr;
  if (!CallPage(call, &page)) {
    return;
  }
  call->Resolve(ToJson(ExtensionActionStates(call->profile(), page)));
}

// The page and extension a function of bdk_extensions.h names, after the
// gesture is checked. Fails like the header says.
bool ResolveActivation(const std::shared_ptr<bdkcore::Contents>& page,
                       const std::string& extension_id,
                       bdk_native_handle_t gesture,
                       bdk_error_t* out_error,
                       EngineContents** engine_page,
                       const Extension** extension,
                       bdk_status_t* status) {
  // The core checked the gesture's kind; the engine checks that the operating
  // system delivered it recently to a window of this application.
  if (!UiPresenters::Get().IsRecentGesture(gesture)) {
    *status = bdkcore::fail(out_error, BDK_DOMAIN_UI, BDK_CODE_GESTURE_REQUIRED,
                            "the action needs the event that clicked it, "
                            "delivered recently to one of the application's "
                            "windows");
    return false;
  }
  *engine_page = EngineContents::From(*page);
  *extension = EnabledExtension(ProfileOf(*page), extension_id);
  if (!*extension) {
    *status = bdkcore::fail(out_error, BDK_DOMAIN_EXTENSION, BDK_CODE_NOT_FOUND,
                            "no extension \"" + extension_id +
                                "\" is installed and enabled in the page's "
                                "profile");
    return false;
  }
  return true;
}

}  // namespace

std::string ActionActivation::Json() const {
  base::DictValue dict;
  dict.Set("outcome", outcome);
  if (!reason.empty()) {
    dict.Set("reason", reason);
  }
  return ToJson(dict);
}

base::ListValue ExtensionActionStates(Profile* profile,
                                      content::WebContents* page) {
  extensions::ExtensionRegistry* registry =
      extensions::ExtensionRegistry::Get(profile);
  ExtensionActionManager* manager = ExtensionActionManager::Get(profile);
  ToolbarActionsModel* model = ToolbarActionsModel::Get(profile);
  std::vector<std::pair<const Extension*, ExtensionAction*>> actions;
  if (registry && manager) {
    for (const auto& extension : registry->enabled_extensions()) {
      if (ExtensionAction* action = manager->GetExtensionAction(*extension)) {
        actions.emplace_back(extension.get(), action);
      }
    }
  }
  // The user's order: what they pinned, in the order they pinned it, then the
  // rest by name.
  auto pin_position = [model](const Extension* extension) -> size_t {
    if (!model) {
      return std::numeric_limits<size_t>::max();
    }
    const std::vector<std::string>& pinned = model->pinned_action_ids();
    auto it = std::ranges::find(pinned, extension->id());
    return it == pinned.end() ? std::numeric_limits<size_t>::max()
                              : static_cast<size_t>(it - pinned.begin());
  };
  std::ranges::stable_sort(actions, [&](const auto& a, const auto& b) {
    const size_t pa = pin_position(a.first);
    const size_t pb = pin_position(b.first);
    if (pa != pb) {
      return pa < pb;
    }
    if (a.first->name() != b.first->name()) {
      return a.first->name() < b.first->name();
    }
    return a.first->id() < b.first->id();
  });
  base::ListValue states;
  for (const auto& [extension, action] : actions) {
    states.Append(ActionState(profile, *extension, *action, page));
  }
  return states;
}

ActionActivation RunExtensionAction(EngineContents& page,
                                    const Extension& extension,
                                    PopupTrigger trigger) {
  ActionActivation result;
  auto blocked = [&result](const char* reason) {
    result.outcome = "blocked";
    result.reason = reason;
    return result;
  };
  std::shared_ptr<bdkcore::Contents> core = page.Core();
  if (!core) {
    return blocked("pageGone");
  }
  Profile* profile = ProfileOf(*core);
  content::WebContents* contents = page.web_contents();
  ExtensionAction* action =
      ExtensionActionManager::Get(profile)->GetExtensionAction(extension);
  ExtensionActionRunner* runner =
      ExtensionActionRunner::GetForWebContents(contents);
  HostContext* context = HostContexts::Get().ContextOf(contents);
  if (!action || !runner || !context || !context->browser()) {
    return blocked("noAction");
  }
  if (!ActionEnabled(profile, extension, *action, contents)) {
    return blocked("actionDisabled");
  }
  const int tab_id = ExtensionTabId(contents);
  // A click on an action that has blocked work (it asked to run on the site
  // and was held) grants access and runs that work instead of its popup.
  const bool had_blocked_work = runner->GetBlockedActions(extension.id()) != 0;
  const bool has_popup = !had_blocked_work && action->HasPopup(tab_id) &&
                         action->GetIsVisible(tab_id);
  if (has_popup && !SurfaceAvailable(core, BDK_SURFACE_EXTENSION_POPUPS)) {
    return blocked("popupsDisabled");
  }
  if (has_popup && context->is_parking()) {
    return blocked("pageNotInWindow");
  }
  // The page is the one the user is looking at: the active tab of its window,
  // which is what tabs.query and the popup name.
  if (!context->IsActive(contents)) {
    context->Activate(contents);
  }
  switch (runner->RunAction(&extension, /*grant_tab_permissions=*/true)) {
    case ExtensionAction::ShowAction::kShowPopup: {
      BdkExtensionsContainer* container =
          context->browser_window()
              ? context->browser_window()->extensions_container()
              : nullptr;
      std::unique_ptr<extensions::ExtensionViewHost> host =
          extensions::ExtensionViewHostFactory::CreatePopupHost(
              extension, action->GetPopupUrl(tab_id), context->browser());
      if (!container || !host) {
        return blocked("popupUnavailable");
      }
      container->ShowPopup(std::move(host), trigger, ShowPopupCallback());
      result.outcome = "popupRequested";
      return result;
    }
    case ExtensionAction::ShowAction::kToggleSidePanel:
      if (!SurfaceAvailable(core, BDK_SURFACE_EXTENSION_SIDE_PANELS)) {
        return blocked("sidePanelsDisabled");
      }
      extensions::side_panel_util::ToggleExtensionSidePanel(context->browser(),
                                                            extension.id());
      result.outcome = "sidePanelRequested";
      return result;
    case ExtensionAction::ShowAction::kNone:
      break;
  }
  result.outcome = had_blocked_work ? "accessRequested" : "dispatchedClick";
  return result;
}

bdk_status_t ActivateExtensionAction(
    const std::shared_ptr<bdkcore::Contents>& page,
    const std::string& extension_id,
    bdk_native_handle_t gesture,
    std::string* result_json,
    bdk_error_t* out_error) {
  EngineContents* engine_page = nullptr;
  const Extension* extension = nullptr;
  bdk_status_t status = BDK_OK;
  if (!ResolveActivation(page, extension_id, gesture, out_error, &engine_page,
                         &extension, &status)) {
    return status;
  }
  *result_json =
      RunExtensionAction(*engine_page, *extension, PopupTrigger::kUserClick)
          .Json();
  return BDK_OK;
}

bdk_status_t PerformExtensionCommand(
    const std::shared_ptr<bdkcore::Contents>& page,
    const std::string& extension_id,
    const std::string& command,
    bdk_native_handle_t gesture,
    std::string* result_json,
    bdk_error_t* out_error) {
  EngineContents* engine_page = nullptr;
  const Extension* extension = nullptr;
  bdk_status_t status = BDK_OK;
  if (!ResolveActivation(page, extension_id, gesture, out_error, &engine_page,
                         &extension, &status)) {
    return status;
  }
  // The action's own command (_execute_action and its older names) is a click
  // on the action.
  if (extensions::Command::IsActionRelatedCommand(command)) {
    *result_json =
        RunExtensionAction(*engine_page, *extension, PopupTrigger::kCommand)
            .Json();
    return BDK_OK;
  }
  const ui::CommandMap* commands =
      extensions::CommandsInfo::GetNamedCommands(extension);
  if (!commands || !commands->contains(command)) {
    return bdkcore::fail(
        out_error, BDK_DOMAIN_EXTENSION, BDK_CODE_NOT_FOUND,
        "the extension declares no command \"" + command + "\"");
  }
  content::WebContents* contents = engine_page->web_contents();
  HostContext* context = HostContexts::Get().ContextOf(contents);
  BdkExtensionsContainer* container =
      context && context->browser_window()
          ? context->browser_window()->extensions_container()
          : nullptr;
  ActionActivation result;
  if (!container) {
    result.outcome = "blocked";
    result.reason = "pageNotInWindow";
  } else {
    // commands.onCommand names the window's active tab: the page the user is
    // looking at.
    if (!context->IsActive(contents)) {
      context->Activate(contents);
    }
    container->DispatchCommand(extension_id, command);
    result.outcome = "dispatchedClick";
  }
  *result_json = result.Json();
  return BDK_OK;
}

std::unique_ptr<ServiceEvents> CreateExtensionActionEvents(
    Profile* profile,
    std::weak_ptr<bdkcore::Profile> bdk_profile) {
  return std::make_unique<ActionEvents>(profile, std::move(bdk_profile));
}

void AddExtensionActionHandlers(HandlerTable& table) {
  table["extensions.actions"] = &ActionsCommand;
}

}  // namespace bdk
