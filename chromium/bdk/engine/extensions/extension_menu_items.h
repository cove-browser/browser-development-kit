// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_EXTENSIONS_EXTENSION_MENU_ITEMS_H_
#define BDK_ENGINE_EXTENSIONS_EXTENSION_MENU_ITEMS_H_

#include <map>
#include <memory>
#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "bdk/core/runtime/core_internal.h"
#include "chrome/browser/extensions/context_menu_matcher.h"
#include "content/public/browser/context_menu_params.h"
#include "content/public/browser/global_routing_id.h"
#include "ui/menus/simple_menu_model.h"

namespace content {
class RenderFrameHost;
class WebContents;
}  // namespace content

namespace bdk {

// The items the profile's extensions add to a page's context menu
// (extensions.md section 4.4), built as Chrome builds them: Chromium's own
// `ContextMenuMatcher` fills a menu model from the `MenuManager` for the
// `ContextMenuParams`, one submenu per extension with more than one item, and
// this class turns the model into the items of the `context_menus` request.
//
// Each item has "source":"extension" and "extensionId", and an id that
// `Execute` runs through `MenuManager::ExecuteCommand`, which grants activeTab
// for the page and fires contextMenus.onClicked naming its tab. The object
// lives until the next context menu of the page replaces it, so that an item
// can be performed after the request finished.
class ExtensionMenuItems : public ui::SimpleMenuModel::Delegate {
 public:
  // The extension items for `params`, or null when there are none or
  // [extensions.ui] context_menus is disabled for the page.
  static std::unique_ptr<ExtensionMenuItems> Create(
      const std::shared_ptr<bdkcore::Contents>& page,
      content::RenderFrameHost& frame,
      const content::ContextMenuParams& params,
      content::WebContents* web_contents);

  ~ExtensionMenuItems() override;

  // The items, in the shape of the context_menus request ("id", "kind",
  // "title", "enabled", "isChecked", "icon", "children", "source",
  // "extensionId").
  const base::ListValue& items() const { return items_; }

  // Runs the item `id` (one of items()) and returns true; false for an id that
  // is not an extension item, which the caller then handles itself.
  bool Execute(const std::string& id);

  // ui::SimpleMenuModel::Delegate:
  bool IsCommandIdChecked(int command_id) const override;
  bool IsCommandIdEnabled(int command_id) const override;
  bool IsCommandIdVisible(int command_id) const override;
  void ExecuteCommand(int command_id, int event_flags) override;

 private:
  ExtensionMenuItems(content::WebContents* web_contents,
                     content::GlobalRenderFrameHostId frame_id,
                     const content::ContextMenuParams& params);

  // Appends the visible items of `model` to `out`.
  void AppendItems(ui::MenuModel* model,
                   const std::map<int, extensions::MenuItem::Id>& owners,
                   base::ListValue& out) const;

  base::WeakPtr<content::WebContents> web_contents_;
  const content::GlobalRenderFrameHostId frame_id_;
  const content::ContextMenuParams params_;
  // The model is declared before the matcher that fills it: the matcher goes
  // first.
  ui::SimpleMenuModel model_;
  extensions::ContextMenuMatcher matcher_;
  base::ListValue items_;
};

}  // namespace bdk

#endif  // BDK_ENGINE_EXTENSIONS_EXTENSION_MENU_ITEMS_H_
