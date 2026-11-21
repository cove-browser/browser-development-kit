// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/extensions/extension_menu_items.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "bdk/engine/extensions/extension_ui_util.h"
#include "bdk/engine/ui_request.h"
#include "chrome/browser/extensions/context_menu_helpers.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/models/image_model.h"
#include "ui/base/models/menu_model.h"

namespace bdk {

namespace {

// The prefix of an extension item's id in the request, followed by the
// command id of its menu model.
constexpr char kItemPrefix[] = "ext:";

// The extension that owns the first command in `model`, for the submenu of one
// extension.
std::string FirstOwner(ui::MenuModel* model,
                       const std::map<int, extensions::MenuItem::Id>& owners) {
  for (size_t i = 0; i < model->GetItemCount(); ++i) {
    if (ui::MenuModel* submenu = model->GetSubmenuModelAt(i)) {
      std::string owner = FirstOwner(submenu, owners);
      if (!owner.empty()) {
        return owner;
      }
      continue;
    }
    auto it = owners.find(model->GetCommandIdAt(i));
    if (it != owners.end()) {
      return it->second.extension_key.extension_id;
    }
  }
  return std::string();
}

}  // namespace

// static
std::unique_ptr<ExtensionMenuItems> ExtensionMenuItems::Create(
    const std::shared_ptr<bdkcore::Contents>& page,
    content::RenderFrameHost& frame,
    const content::ContextMenuParams& params,
    content::WebContents* web_contents) {
  if (!SurfaceAvailable(page, BDK_SURFACE_EXTENSION_CONTEXT_MENUS)) {
    return nullptr;
  }
  std::unique_ptr<ExtensionMenuItems> items(
      new ExtensionMenuItems(web_contents, frame.GetGlobalId(), params));
  extensions::context_menu_helpers::PopulateExtensionItems(
      web_contents->GetBrowserContext(), items->params_, items->matcher_);
  if (items->model_.GetItemCount() == 0) {
    return nullptr;
  }
  const std::map<int, extensions::MenuItem::Id> owners =
      items->matcher_.extension_item_map();
  items->AppendItems(&items->model_, owners, items->items_);
  if (items->items_.empty()) {
    return nullptr;
  }
  return items;
}

ExtensionMenuItems::ExtensionMenuItems(
    content::WebContents* web_contents,
    content::GlobalRenderFrameHostId frame_id,
    const content::ContextMenuParams& params)
    : web_contents_(web_contents->GetWeakPtr()),
      frame_id_(frame_id),
      params_(params),
      model_(this),
      matcher_(web_contents->GetBrowserContext(),
               this,
               &model_,
               base::BindRepeating(
                   extensions::context_menu_helpers::MenuItemMatchesParams,
                   params_)) {}

ExtensionMenuItems::~ExtensionMenuItems() = default;

void ExtensionMenuItems::AppendItems(
    ui::MenuModel* model,
    const std::map<int, extensions::MenuItem::Id>& owners,
    base::ListValue& out) const {
  for (size_t i = 0; i < model->GetItemCount(); ++i) {
    if (!model->IsVisibleAt(i)) {
      continue;
    }
    const ui::MenuModel::ItemType type = model->GetTypeAt(i);
    if (type == ui::MenuModel::TYPE_SEPARATOR) {
      out.Append(base::DictValue().Set("id", "").Set("kind", "separator"));
      continue;
    }
    if (type == ui::MenuModel::TYPE_TITLE) {
      continue;
    }
    base::DictValue item;
    item.Set("title", base::UTF16ToUTF8(model->GetLabelAt(i)));
    item.Set("enabled", model->IsEnabledAt(i));
    item.Set("source", "extension");
    ui::ImageModel icon = model->GetIconAt(i);
    if (icon.IsImage()) {
      const std::string url = PngDataUrl(icon.GetImage());
      if (!url.empty()) {
        item.Set("icon", url);
      }
    }
    if (ui::MenuModel* submenu = model->GetSubmenuModelAt(i)) {
      // One submenu per extension with more than one item, as in Chrome.
      item.Set("id", "");
      item.Set("kind", "submenu");
      base::ListValue children;
      AppendItems(submenu, owners, children);
      if (children.empty()) {
        continue;
      }
      item.Set("children", std::move(children));
      const std::string owner = FirstOwner(submenu, owners);
      if (!owner.empty()) {
        item.Set("extensionId", owner);
        item.Set("extension", owner);
      }
      out.Append(std::move(item));
      continue;
    }
    const int command_id = model->GetCommandIdAt(i);
    item.Set("id", kItemPrefix + base::NumberToString(command_id));
    switch (type) {
      case ui::MenuModel::TYPE_CHECK:
        item.Set("kind", "checkbox");
        item.Set("isChecked", model->IsItemCheckedAt(i));
        break;
      case ui::MenuModel::TYPE_RADIO:
        item.Set("kind", "radio");
        item.Set("isChecked", model->IsItemCheckedAt(i));
        break;
      default:
        item.Set("kind", "command");
        break;
    }
    auto owner = owners.find(command_id);
    if (owner != owners.end()) {
      const std::string& id = owner->second.extension_key.extension_id;
      item.Set("extensionId", id);
      // The Swift request reads the owner under this name.
      item.Set("extension", id);
    }
    out.Append(std::move(item));
  }
}

bool ExtensionMenuItems::Execute(const std::string& id) {
  if (!id.starts_with(kItemPrefix)) {
    return false;
  }
  int command_id = 0;
  if (!base::StringToInt(id.substr(sizeof(kItemPrefix) - 1), &command_id)) {
    return true;  // Ours by its prefix, and nothing to run.
  }
  ExecuteCommand(command_id, /*event_flags=*/0);
  return true;
}

bool ExtensionMenuItems::IsCommandIdChecked(int command_id) const {
  return matcher_.IsCommandIdChecked(command_id);
}

bool ExtensionMenuItems::IsCommandIdEnabled(int command_id) const {
  return matcher_.IsCommandIdEnabled(command_id);
}

bool ExtensionMenuItems::IsCommandIdVisible(int command_id) const {
  return matcher_.IsCommandIdVisible(command_id);
}

void ExtensionMenuItems::ExecuteCommand(int command_id, int event_flags) {
  content::RenderFrameHost* frame = content::RenderFrameHost::FromID(frame_id_);
  if (!web_contents_ || !frame) {
    return;  // The page or its frame went while the menu was open.
  }
  matcher_.ExecuteCommand(command_id, web_contents_.get(), frame, params_);
}

}  // namespace bdk
