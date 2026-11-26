// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_UI_DIALOGS_H_
#define BDK_ENGINE_UI_DIALOGS_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/memory/raw_ref.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "bdk/engine/ui_presenters.h"
#include "bdk/engine/ui_request.h"
#include "content/public/browser/file_select_listener.h"
#include "content/public/browser/javascript_dialog_manager.h"
#include "third_party/blink/public/mojom/choosers/file_chooser.mojom.h"

namespace content {
class RenderFrameHost;
}  // namespace content

namespace bdk {

class EngineContents;

// The dialogs a page asks for that wait for an answer: JavaScript dialogs
// (alert, confirm, prompt, beforeunload) and file pickers. Each becomes a UI
// request of its surface; with no handler, or after `.useDefault`, the
// default presenter shows it. One per page (EngineContents owns it), and the
// page's JavaScriptDialogManager.
class PageDialogs : public content::JavaScriptDialogManager {
 public:
  explicit PageDialogs(EngineContents& page);
  ~PageDialogs() override;

  // The main frame committed a navigation: "prevent further dialogs" ends.
  void OnNavigated();

  // content::JavaScriptDialogManager:
  void RunJavaScriptDialog(content::WebContents* web_contents,
                           content::RenderFrameHost* render_frame_host,
                           content::JavaScriptDialogType dialog_type,
                           const std::u16string& message_text,
                           const std::u16string& default_prompt_text,
                           DialogClosedCallback callback,
                           bool* did_suppress_message) override;
  void RunBeforeUnloadDialog(content::WebContents* web_contents,
                             content::RenderFrameHost* render_frame_host,
                             bool is_reload,
                             DialogClosedCallback callback) override;
  bool HandleJavaScriptDialog(content::WebContents* web_contents,
                              bool accept,
                              const std::u16string* prompt_override) override;
  void CancelDialogs(content::WebContents* web_contents,
                     bool reset_state) override;

  // WebContentsDelegate::RunFileChooser and EnumerateDirectory.
  void RunFileChooser(content::RenderFrameHost* render_frame_host,
                      scoped_refptr<content::FileSelectListener> listener,
                      const blink::mojom::FileChooserParams& params);
  void EnumerateDirectory(scoped_refptr<content::FileSelectListener> listener,
                          const base::FilePath& path);

  // A picker Chromium opens through its file dialog (the File System Access
  // API, ui_select_file.h): `payload` is the file_pickers request's, and
  // `chosen` gets the chosen paths, none when it was cancelled.
  using PickerCallback =
      base::OnceCallback<void(std::vector<base::FilePath> paths)>;
  void RunPicker(base::DictValue payload, PickerCallback chosen);

 private:
  // A dialog or picker that has not ended: a script dialog (`script`), a
  // file input's picker (`listener`) or another picker (`picker`).
  struct Pending {
    Pending();
    Pending(Pending&&);
    Pending& operator=(Pending&&);
    ~Pending();
    std::shared_ptr<bdkcore::Request> request;
    Dismisser dismiss;
    DialogClosedCallback script;
    scoped_refptr<content::FileSelectListener> listener;
    PickerCallback picker;
    blink::mojom::FileChooserParams::Mode mode =
        blink::mojom::FileChooserParams::Mode::kOpen;
  };

  // Raises the request and keeps `pending` until it ends.
  void Start(Pending pending, uint32_t surface, base::DictValue payload);
  void OnEnded(int id, base::DictValue payload, const UiResult& result);
  void OnScriptAnswered(int id, JavaScriptDialogAnswer answer);
  void OnPickerAnswered(int id, std::vector<base::FilePath> paths);
  // Ends the dialog: removes what is shown and answers the page.
  void Complete(int id,
                bool accepted,
                const std::string& text,
                std::vector<base::FilePath> paths);
  void OnFolderEnumerated(scoped_refptr<content::FileSelectListener> listener,
                          const base::FilePath& folder,
                          std::vector<base::FilePath> files);
  void CancelAll();

  const raw_ref<EngineContents> page_;
  std::map<int, Pending> pending_;
  int next_id_ = 1;
  // The page's dialogs since its last navigation, and whether the person
  // asked for no more.
  int dialogs_since_navigation_ = 0;
  bool suppressed_ = false;
  base::WeakPtrFactory<PageDialogs> weak_factory_{this};
};

}  // namespace bdk

#endif  // BDK_ENGINE_UI_DIALOGS_H_
