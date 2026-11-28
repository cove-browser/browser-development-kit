// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BDK_ENGINE_UI_SELECT_FILE_H_
#define BDK_ENGINE_UI_SELECT_FILE_H_

#include <memory>

namespace content {
class WebContents;
}  // namespace content

namespace ui {
class SelectFilePolicy;
}  // namespace ui

namespace bdk {

// The pickers Chromium opens through its file dialog rather than a page's
// file input (ui-requests.md 5.4, T-UIR-13): the File System Access API's
// showOpenFilePicker, showSaveFilePicker and showDirectoryPicker, and anything
// else of a page's that asks for a ui::SelectFileDialog. Each is a
// file_pickers request of the page, with isFileSystemAccessAPI set; the
// default presenter is the file inputs' sheet. A dialog asked for by no page
// is Chromium's own.

// The policy BdkContentBrowserClient::CreateSelectFilePolicy returns: Chrome's,
// and the page the dialog belongs to.
std::unique_ptr<ui::SelectFilePolicy> CreateSelectFilePolicy(
    content::WebContents* web_contents);

// Installs the factory of BDK's file dialogs, once, as Chromium starts.
void InstallSelectFileDialogs();

}  // namespace bdk

#endif  // BDK_ENGINE_UI_SELECT_FILE_H_
