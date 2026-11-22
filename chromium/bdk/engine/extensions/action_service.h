// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_EXTENSIONS_ACTION_SERVICE_H_
#define BDK_ENGINE_EXTENSIONS_ACTION_SERVICE_H_

#include <memory>
#include <string>

#include "base/values.h"
#include "bdk/core/include/bdk/bdk_base.h"
#include "bdk/core/runtime/core_internal.h"
#include "bdk/engine/extensions/extension_popup.h"
#include "bdk/engine/services/events.h"
#include "bdk/engine/services/services.h"

class Profile;

namespace content {
class WebContents;
}  // namespace content

namespace extensions {
class Extension;
}  // namespace extensions

namespace bdk {

class EngineContents;

// Extension actions (extensions.md section 4.1, T-EXT-02): the state of each
// action as data, what a click on one does, and the keyboard commands an
// extension declares.

// The `ExtensionActionState` of every action of `profile`'s extensions, in the
// user's order (pinned first, as pinned, then the rest by name), for `page`
// (a tab-independent default when null). Title, icon (a data: URL), badge,
// whether it is enabled and has a popup for that page, whether the extension
// is waiting for site access, and whether the user pinned it.
base::ListValue ExtensionActionStates(Profile* profile,
                                      content::WebContents* page);

// What a click did, as `bdk_extension_action_activate` reports it.
struct ActionActivation {
  // dispatchedClick, popupRequested, sidePanelRequested, accessRequested or
  // blocked.
  std::string outcome;
  // Why, for blocked.
  std::string reason;

  std::string Json() const;
};

// Clicks the action of `extension` for `page`, as the toolbar button does:
// grants activeTab for the page, and lets Chromium decide
// (ExtensionActionRunner::RunAction) whether that fires action.onClicked, shows
// the popup (as a popup request), toggles the side panel or runs what the
// extension had blocked on the page. The page becomes the active tab of its
// window first, because it is the page the user is looking at. The caller has
// checked the gesture.
ActionActivation RunExtensionAction(EngineContents& page,
                                    const extensions::Extension& extension,
                                    PopupTrigger trigger);

// Backend::ExtensionActivateAction and Backend::ExtensionPerformCommand: the
// gesture must be recent (ui.gestureRequired), the extension installed and
// enabled in the page's profile (extension.notFound). A command that is not the
// action's fires commands.onCommand for the page and is "dispatchedClick".
bdk_status_t ActivateExtensionAction(
    const std::shared_ptr<bdkcore::Contents>& page,
    const std::string& extension_id,
    bdk_native_handle_t gesture,
    std::string* result_json,
    bdk_error_t* out_error);
bdk_status_t PerformExtensionCommand(
    const std::shared_ptr<bdkcore::Contents>& page,
    const std::string& extension_id,
    const std::string& command,
    bdk_native_handle_t gesture,
    std::string* result_json,
    bdk_error_t* out_error);

// Observes one profile's extension actions (badge, title, icon, visibility)
// and emits `extensions.actionChanged` for each change, with the page's handle
// as a decimal string or null for a change to the defaults. Until destroyed,
// with the profile's engine data.
std::unique_ptr<ServiceEvents> CreateExtensionActionEvents(
    Profile* profile,
    std::weak_ptr<bdkcore::Profile> bdk_profile);

// The command `extensions.actions`.
void AddExtensionActionHandlers(HandlerTable& table);

}  // namespace bdk

#endif  // BDK_ENGINE_EXTENSIONS_ACTION_SERVICE_H_
