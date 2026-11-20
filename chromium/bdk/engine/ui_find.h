// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_UI_FIND_H_
#define BDK_ENGINE_UI_FIND_H_

#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/raw_ref.h"
#include "base/memory/weak_ptr.h"
#include "bdk/engine/ui_presenters.h"
#include "bdk/engine/ui_request.h"
#include "components/find_in_page/find_result_observer.h"

namespace content {
class WebContents;
}  // namespace content

namespace find_in_page {
class FindTabHelper;
}  // namespace find_in_page

namespace bdk {

class EngineContents;

// Find in page for one page (ui-requests.md section 5.11): the search itself
// runs on Chrome's FindTabHelper; the find bar is a `find` UI request that the
// application's handler or the default presenter shows. EngineContents owns
// it.
class PageFind : public FindBarClient, public find_in_page::FindResultObserver {
 public:
  struct Result {
    int ordinal = 0;  // 1-based; 0 when there is no active match
    int count = 0;
  };

  explicit PageFind(EngineContents& page);
  ~PageFind() override;

  // The page's WebContents changed: follow its find helper.
  void Attach(content::WebContents* contents);
  // The page navigated to another document: the find UI goes.
  void OnNavigated();

  // Asks for the find UI (a find request): the application's handler or the
  // default find bar. Does nothing while one is open, beyond focusing it.
  void Present();
  // The search, as `page.find` offers it to an application with its own field.
  void Start(const std::string& text, bool match_case);
  void Next(bool forward);
  void Stop(bool clear_selection);
  // The latest result of the search.
  Result result() const { return result_; }
  // Whether a find UI is open (a request outstanding, or a bar shown).
  bool IsPresented() const { return request_ || bar_; }

  // FindBarClient:
  void Search(const std::string& text,
              bool forward,
              bool match_case,
              bool find_next) override;
  void Closed() override;

  // find_in_page::FindResultObserver:
  void OnFindResultAvailable(content::WebContents* web_contents) override;
  void OnFindEmptyText(content::WebContents* web_contents) override;
  void OnFindTabHelperDestroyed(find_in_page::FindTabHelper* helper) override;

 private:
  void OnEnded(base::DictValue payload, const UiResult& result);
  // Removes the find UI and ends the search.
  void CloseUi();

  const raw_ref<EngineContents> page_;
  raw_ptr<find_in_page::FindTabHelper> helper_ = nullptr;
  std::shared_ptr<bdkcore::Request> request_;
  std::unique_ptr<FindBar> bar_;
  Result result_;
  bool match_case_ = false;
  base::WeakPtrFactory<PageFind> weak_factory_{this};
};

}  // namespace bdk

#endif  // BDK_ENGINE_UI_FIND_H_
