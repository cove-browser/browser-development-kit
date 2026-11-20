// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/ui_find.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/host_view.h"
#include "bdk/engine/json_util.h"
#include "components/find_in_page/find_tab_helper.h"
#include "components/find_in_page/find_types.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"

namespace bdk {

PageFind::PageFind(EngineContents& page) : page_(page) {}

PageFind::~PageFind() {
  if (helper_) {
    helper_->RemoveObserver(this);
  }
  if (request_) {
    bdkcore::withdraw_ui_request(*request_);
  }
}

void PageFind::Attach(content::WebContents* contents) {
  if (helper_) {
    helper_->RemoveObserver(this);
    helper_ = nullptr;
  }
  find_in_page::FindTabHelper* helper =
      find_in_page::FindTabHelper::FromWebContents(contents);
  if (!helper) {
    find_in_page::FindTabHelper::CreateForWebContents(contents);
    helper = find_in_page::FindTabHelper::FromWebContents(contents);
  }
  helper_ = helper;
  if (helper_) {
    helper_->AddObserver(this);
  }
}

void PageFind::OnNavigated() {
  if (IsPresented()) {
    CloseUi();
  }
  result_ = Result();
  active_ = false;
}

void PageFind::Present() {
  if (!helper_) {
    return;
  }
  if (bar_) {
    bar_->Focus();
    return;
  }
  if (request_) {
    return;
  }
  // What the person selected on the page, or else the last search.
  std::u16string initial;
  if (content::RenderWidgetHostView* view =
          page_->web_contents()->GetPrimaryMainFrame()->GetView()) {
    initial = view->GetSelectedText();
  }
  if (initial.empty()) {
    initial = helper_->find_text();
  }
  base::DictValue payload;
  if (!initial.empty()) {
    payload.Set("initialText", base::UTF16ToUTF8(initial));
  }
  base::DictValue shown = payload.Clone();
  request_ = RaiseUiRequest(
      page_->Core(), BDK_SURFACE_FIND, payload,
      base::BindOnce(&PageFind::OnEnded, weak_factory_.GetWeakPtr(),
                     std::move(shown)),
      &page_->host_view());
}

void PageFind::OnEnded(base::DictValue payload, const UiResult& result) {
  switch (result.outcome) {
    case bdkcore::UiOutcome::kDefaulted:
      // The default presenter is the engine's find bar, which lives until
      // the person closes it.
      request_.reset();
      bar_ =
          UiPresenters::Get().ShowFindBar(&page_->host_view(), payload, this);
      if (bar_) {
        bar_->SetResult(result_.ordinal, result_.count);
      }
      return;
    case bdkcore::UiOutcome::kCompleted:
      // The application closed its find UI.
      request_.reset();
      Stop(/*clear_selection=*/true);
      return;
    default:
      // Rejected, dropped, withdrawn or disabled: no find UI.
      request_.reset();
      bar_.reset();
      Stop(/*clear_selection=*/true);
      return;
  }
}

void PageFind::CloseUi() {
  if (request_) {
    // The application is told it is withdrawn; its end resets request_.
    std::shared_ptr<bdkcore::Request> request = std::move(request_);
    bdkcore::withdraw_ui_request(*request);
  }
  bar_.reset();
  Stop(/*clear_selection=*/true);
}

void PageFind::Start(const std::string& text, bool match_case) {
  Search(text, /*forward=*/true, match_case, /*find_next=*/false);
}

void PageFind::Next(bool forward) {
  if (!helper_) {
    return;
  }
  const std::u16string text = helper_->find_text();
  if (!active_ || text.empty()) {
    return;
  }
  helper_->StartFinding(text, forward, match_case_, /*find_match=*/true);
}

void PageFind::Stop(bool clear_selection) {
  if (!helper_) {
    return;
  }
  helper_->StopFinding(clear_selection ? find_in_page::SelectionAction::kClear
                                       : find_in_page::SelectionAction::kKeep);
  result_ = Result();
  active_ = false;
}

void PageFind::Search(const std::string& text,
                      bool forward,
                      bool match_case,
                      bool find_next) {
  if (!helper_) {
    return;
  }
  match_case_ = match_case;
  active_ = !text.empty();
  helper_->StartFinding(base::UTF8ToUTF16(text), forward, match_case,
                        /*find_match=*/true);
}

void PageFind::Closed() {
  // The bar is the caller: take it down after its own call returns.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&PageFind::CloseUi, weak_factory_.GetWeakPtr()));
}

void PageFind::OnFindResultAvailable(content::WebContents* web_contents) {
  if (!helper_) {
    return;
  }
  const find_in_page::FindNotificationDetails& details = helper_->find_result();
  result_.ordinal = std::max(details.active_match_ordinal(), 0);
  result_.count = std::max(details.number_of_matches(), 0);
  if (bar_) {
    bar_->SetResult(result_.ordinal, result_.count);
  }
  Report(result_.count, result_.ordinal, details.selection_rect(),
         details.final_update());
}

void PageFind::OnFindEmptyText(content::WebContents* web_contents) {
  result_ = Result();
  if (bar_) {
    bar_->SetResult(0, 0);
  }
  Report(0, 0, gfx::Rect(), /*final_update=*/true);
}

void PageFind::Report(int count,
                      int ordinal,
                      const gfx::Rect& rect,
                      bool final_update) {
  page_->Emit(BDK_CONTENTS_EVENT_FIND_RESULT, "",
              ToJson(base::DictValue()
                         .Set("matchCount", count)
                         .Set("activeIndex", ordinal)
                         .Set("rect", base::DictValue()
                                          .Set("x", rect.x())
                                          .Set("y", rect.y())
                                          .Set("width", rect.width())
                                          .Set("height", rect.height()))
                         .Set("final", final_update)),
              0, 0);
}

void PageFind::OnFindTabHelperDestroyed(find_in_page::FindTabHelper* helper) {
  if (helper_ == helper) {
    helper_ = nullptr;
  }
}

}  // namespace bdk
