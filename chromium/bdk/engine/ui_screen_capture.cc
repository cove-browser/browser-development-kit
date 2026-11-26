// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/ui_screen_capture.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "bdk/core/include/bdk/bdk_ids.h"
#include "bdk/core/runtime/core_internal.h"
#include "bdk/engine/engine_contents.h"
#include "bdk/engine/json_util.h"
#include "bdk/engine/ui_presenters.h"
#include "bdk/engine/ui_request.h"
#include "chrome/browser/media/webrtc/desktop_media_list.h"
#include "chrome/browser/media/webrtc/desktop_media_list_observer.h"
#include "chrome/browser/media/webrtc/desktop_media_picker.h"
#include "chrome/browser/media/webrtc/embedder_screen_capture_ui.h"  // nogncheck
#include "chrome/browser/media/webrtc/media_capture_devices_dispatcher.h"
#include "chrome/browser/media/webrtc/media_stream_capture_indicator.h"
#include "chrome/browser/media/webrtc/media_stream_ui.h"
#include "content/public/browser/desktop_media_id.h"
#include "content/public/browser/media_stream_request.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/mojom/mediastream/media_stream.mojom.h"
#include "ui/base/webui/web_ui_util.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/image/image_skia.h"

namespace bdk {

namespace {

using blink::mojom::MediaStreamRequestResult;

MediaStreamCaptureIndicator* Indicator() {
  return MediaCaptureDevicesDispatcher::GetInstance()
      ->GetMediaStreamCaptureIndicator()
      .get();
}

const char* KindOf(DesktopMediaList::Type type) {
  switch (type) {
    case DesktopMediaList::Type::kScreen:
      return "screen";
    case DesktopMediaList::Type::kWindow:
      return "window";
    case DesktopMediaList::Type::kWebContents:
    case DesktopMediaList::Type::kCurrentTab:
      return "page";
    case DesktopMediaList::Type::kNone:
      break;
  }
  return "other";
}

const char* KindOf(content::DesktopMediaID::Type type) {
  switch (type) {
    case content::DesktopMediaID::TYPE_SCREEN:
      return "screen";
    case content::DesktopMediaID::TYPE_WINDOW:
      return "window";
    case content::DesktopMediaID::TYPE_WEB_CONTENTS:
      return "page";
    case content::DesktopMediaID::TYPE_NONE:
      break;
  }
  return "other";
}

class PagePicker;

// Tells the picker that one of its lists changed (the observer interface
// does not say which list).
class ListWatcher : public DesktopMediaListObserver {
 public:
  explicit ListWatcher(base::RepeatingClosure changed)
      : changed_(std::move(changed)) {}

  void OnSourceAdded(int index) override { changed_.Run(); }
  void OnSourceRemoved(int index) override { changed_.Run(); }
  void OnSourceMoved(int old_index, int new_index) override { changed_.Run(); }
  void OnSourceNameChanged(int index) override { changed_.Run(); }
  void OnSourceThumbnailChanged(int index) override { changed_.Run(); }
  void OnSourcePreviewChanged(size_t index) override {}
  void OnDelegatedSourceListSelection() override {}
  void OnDelegatedSourceListDismissed() override {}

 private:
  base::RepeatingClosure changed_;
};

// The picker: Chrome's source lists, shown as the page's screen_capture
// request.
class PagePicker : public DesktopMediaPicker {
 public:
  PagePicker() = default;
  ~PagePicker() override {
    if (request_ && !ended_) {
      bdkcore::withdraw_ui_request(*request_);
    }
    // Each list holds its watcher: the lists go first.
    lists_.clear();
    if (on_destroying_) {
      on_destroying_.Run();
    }
  }

  void Show(const Params& params,
            std::vector<std::unique_ptr<DesktopMediaList>> source_lists,
            DoneCallback done_callback) override {
    done_ = std::move(done_callback);
    request_audio_ = params.request_audio;
    on_destroying_ = params.on_picker_destroying;
    content::WebContents* contents = params.web_contents.get();
    EngineContents* page =
        contents ? EngineContents::FromWebContents(contents) : nullptr;
    if (!page) {
      // Not one of BDK's pages (an extension asked for no tab): nothing to
      // ask in.
      Finish(std::nullopt, MediaStreamRequestResult::PERMISSION_DENIED);
      return;
    }
    page_ = page->GetWeakPtr();
    lists_ = std::move(source_lists);
    for (const std::unique_ptr<DesktopMediaList>& list : lists_) {
      list->SetThumbnailSize(gfx::Size(160, 100));
      list->SetUpdatePeriod(base::Seconds(2));
      watchers_.push_back(std::make_unique<ListWatcher>(base::BindRepeating(
          &PagePicker::Changed, weak_factory_.GetWeakPtr())));
      list->StartUpdating(watchers_.back().get());
    }
    base::DictValue payload;
    payload.Set(
        "origin",
        contents->GetPrimaryMainFrame()->GetLastCommittedOrigin().Serialize());
    if (params.request_source == Params::RequestSource::kExtension) {
      payload.Set("extensionName", base::UTF16ToUTF8(params.app_name));
    }
    payload.Set("requestsAudio", params.request_audio);
    payload.Set("sources", Sources());
    request_ = RaiseUiRequest(
        *page, BDK_SURFACE_SCREEN_CAPTURE, payload,
        base::BindOnce(&PagePicker::OnEnded, weak_factory_.GetWeakPtr(),
                       payload.Clone()));
    if (!request_ && !ended_) {
      Finish(std::nullopt, MediaStreamRequestResult::PERMISSION_DENIED);
    }
  }

 private:
  // Every source of every list, in list order, each with an id that stays
  // its own while the picker is open.
  base::ListValue Sources() {
    base::ListValue sources;
    for (const std::unique_ptr<DesktopMediaList>& list : lists_) {
      for (int i = 0; i < list->GetSourceCount(); ++i) {
        const DesktopMediaList::Source& source = list->GetSource(i);
        if (source.is_sharing_blocked) {
          continue;
        }
        const std::string key = source.id.ToString();
        auto [it, added] = ids_.try_emplace(key, next_id_);
        if (added) {
          ++next_id_;
        }
        const std::string id = "source" + base::NumberToString(it->second);
        media_ids_[id] = source.id;
        base::DictValue entry;
        entry.Set("id", id);
        entry.Set("kind", KindOf(list->GetMediaListType()));
        entry.Set("name", base::UTF16ToUTF8(source.name));
        if (!source.thumbnail.isNull()) {
          entry.Set("thumbnail",
                    webui::GetBitmapDataUrl(*source.thumbnail.bitmap()));
        }
        sources.Append(std::move(entry));
      }
    }
    return sources;
  }

  // Lists change in bursts (a source and its thumbnail): one update after
  // each burst.
  void Changed() {
    if (ended_ || update_pending_) {
      return;
    }
    update_pending_ = true;
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&PagePicker::SendUpdate, weak_factory_.GetWeakPtr()));
  }

  void SendUpdate() {
    update_pending_ = false;
    if (ended_) {
      return;
    }
    base::ListValue sources = Sources();
    if (sheet_) {
      sheet_->SetSources(sources);
    }
    if (request_) {
      if (std::shared_ptr<bdkcore::Runtime> runtime =
              bdkcore::current_runtime()) {
        bdkcore::ui_send_update(
            runtime, request_,
            ToJson(base::DictValue().Set("sources", std::move(sources))));
      }
    }
  }

  void OnEnded(base::DictValue payload, const UiResult& result) {
    request_.reset();
    if (ended_) {
      return;
    }
    switch (result.outcome) {
      case bdkcore::UiOutcome::kCompleted: {
        const std::string* source = result.args.FindString("source");
        if (result.completion == "share" && source) {
          Finish(
              ScreenCaptureChoice{
                  *source, result.args.FindBool("audio").value_or(false)},
              MediaStreamRequestResult::PERMISSION_DENIED_BY_USER);
          return;
        }
        Finish(std::nullopt,
               MediaStreamRequestResult::PERMISSION_DENIED_BY_USER);
        return;
      }
      case bdkcore::UiOutcome::kDefaulted:
        if (!page_) {
          Finish(std::nullopt, MediaStreamRequestResult::PERMISSION_DENIED);
          return;
        }
        sheet_ = UiPresenters::Get().ShowScreenCapturePicker(
            &page_->host_view(), payload,
            base::BindOnce(&PagePicker::OnSheetAnswered,
                           weak_factory_.GetWeakPtr()));
        sheet_->SetSources(Sources());
        return;
      default:
        // Rejected, dropped, withdrawn or disabled.
        Finish(std::nullopt, MediaStreamRequestResult::PERMISSION_DENIED);
        return;
    }
  }

  void OnSheetAnswered(std::optional<ScreenCaptureChoice> choice) {
    Finish(std::move(choice),
           MediaStreamRequestResult::PERMISSION_DENIED_BY_USER);
  }

  // Shares the chosen source, or refuses with `refusal` when there is none
  // or it is no longer offered.
  void Finish(std::optional<ScreenCaptureChoice> choice,
              MediaStreamRequestResult refusal) {
    if (ended_) {
      return;
    }
    ended_ = true;
    sheet_.reset();
    DoneCallbackArgumentType result = base::unexpected(refusal);
    if (choice) {
      auto it = media_ids_.find(choice->source);
      if (it != media_ids_.end()) {
        content::DesktopMediaID id = it->second;
        id.audio_share = choice->audio && request_audio_;
        result = id;
      }
    }
    // Chromium's pickers answer from a task: the handler may destroy the
    // picker in the callback.
    if (done_) {
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(std::move(done_), std::move(result)));
    }
  }

  DoneCallback done_;
  bool request_audio_ = false;
  base::RepeatingClosure on_destroying_;
  base::WeakPtr<EngineContents> page_;
  std::vector<std::unique_ptr<DesktopMediaList>> lists_;
  std::vector<std::unique_ptr<ListWatcher>> watchers_;
  std::map<std::string, int> ids_;
  std::map<std::string, content::DesktopMediaID> media_ids_;
  int next_id_ = 0;
  std::shared_ptr<bdkcore::Request> request_;
  std::unique_ptr<ScreenCaptureSheet> sheet_;
  bool update_pending_ = false;
  bool ended_ = false;
  base::WeakPtrFactory<PagePicker> weak_factory_{this};
};

std::unique_ptr<DesktopMediaPicker> CreatePicker(
    const content::MediaStreamRequest* request) {
  return std::make_unique<PagePicker>();
}

// The notification while a page shares: BDK's sharing bar, unless the
// application handles screen_capture and so shows its own indicator.
class SharingIndicator : public MediaStreamUI {
 public:
  SharingIndicator(content::WebContents* capturing,
                   const content::DesktopMediaID& captured)
      : capturing_(capturing ? capturing->GetWeakPtr() : nullptr),
        captured_(captured) {}
  // Chromium destroys the notification when the capture stops.
  ~SharingIndicator() override {
    if (dismiss_) {
      std::move(dismiss_).Run();
    }
  }

  gfx::NativeViewId OnStarted(
      base::OnceClosure stop,
      content::MediaStreamUI::SourceCallback source,
      const std::vector<content::DesktopMediaID>& media_ids) override {
    EngineContents* page =
        capturing_ ? EngineContents::FromWebContents(capturing_.get())
                   : nullptr;
    if (!page || SurfaceHasHandler(page->Core(), BDK_SURFACE_SCREEN_CAPTURE)) {
      return 0;
    }
    base::DictValue sharing;
    sharing.Set("origin", capturing_->GetPrimaryMainFrame()
                              ->GetLastCommittedOrigin()
                              .Serialize());
    sharing.Set("kind", KindOf(captured_.type));
    if (captured_.type == content::DesktopMediaID::TYPE_WEB_CONTENTS) {
      content::WebContents* shared = content::WebContents::FromRenderFrameHost(
          content::RenderFrameHost::FromID(
              captured_.web_contents_id.render_process_id,
              captured_.web_contents_id.main_render_frame_id));
      if (shared) {
        sharing.Set("name", base::UTF16ToUTF8(shared->GetTitle()));
      }
    }
    dismiss_ = UiPresenters::Get().ShowSharingBar(&page->host_view(), sharing,
                                                  std::move(stop));
    return 0;
  }

 private:
  base::WeakPtr<content::WebContents> capturing_;
  const content::DesktopMediaID captured_;
  Dismisser dismiss_;
};

std::unique_ptr<MediaStreamUI> CreateNotification(
    content::WebContents* capturing,
    const content::DesktopMediaID& captured,
    const std::u16string& application_title) {
  return std::make_unique<SharingIndicator>(capturing, captured);
}

// Tells a page when what it captures, or whether it is captured, changes.
class CaptureObserver : public MediaStreamCaptureIndicator::Observer {
 public:
  void OnIsCapturingVideoChanged(content::WebContents* contents,
                                 bool) override {
    Note(contents);
  }
  void OnIsCapturingAudioChanged(content::WebContents* contents,
                                 bool) override {
    Note(contents);
  }
  void OnIsBeingMirroredChanged(content::WebContents* contents, bool) override {
    Note(contents);
  }
  void OnIsCapturingTabChanged(content::WebContents* contents, bool) override {
    Note(contents);
  }
  void OnIsCapturingWindowChanged(content::WebContents* contents,
                                  bool) override {
    Note(contents);
  }
  void OnIsCapturingDisplayChanged(content::WebContents* contents,
                                   bool) override {
    Note(contents);
  }

 private:
  // The page that captures is told; the page it captures is not, so every
  // page looks again.
  static void Note(content::WebContents*) {
    for (EngineContents* page : EngineContents::All()) {
      page->NoteCaptureChanged();
    }
  }
};

}  // namespace

void InstallScreenCaptureUi() {
  static bool installed = false;
  if (installed) {
    return;
  }
  installed = true;
  embedder_screen_capture_ui::SetFactories(&CreatePicker, &CreateNotification);
  static base::NoDestructor<CaptureObserver> observer;
  Indicator()->AddObserver(observer.get());
}

uint32_t CaptureFlags(content::WebContents* contents) {
  MediaStreamCaptureIndicator* indicator = Indicator();
  if (!contents || !indicator) {
    return 0;
  }
  uint32_t flags = 0;
  if (indicator->IsCapturingVideo(contents)) {
    flags |= BDK_CAPTURE_CAMERA;
  }
  if (indicator->IsCapturingAudio(contents)) {
    flags |= BDK_CAPTURE_MICROPHONE;
  }
  if (indicator->IsCapturingTab(contents) ||
      indicator->IsCapturingWindow(contents) ||
      indicator->IsCapturingDisplay(contents)) {
    flags |= BDK_CAPTURE_DISPLAY;
  }
  if (contents->IsBeingCaptured()) {
    flags |= BDK_CAPTURE_CAPTURED;
  }
  return flags;
}

}  // namespace bdk
