// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/ui_presenters.h"

#import <AppKit/AppKit.h>
#import <CoreImage/CoreImage.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/apple/foundation_util.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/strings/sys_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "bdk/core/include/bdk/bdk_ids.h"
#include "bdk/engine/host_view.h"
#include "bdk/engine/window_registry.h"

// The default presenters on macOS: small AppKit sheets, panels and views
// (T-MAC-04: Chromium's own dialog views are not used; see feasibility.md).
// Sheets belong to the window the page's host view is in and block nothing
// else; a page that is in no window gets its sheet when it is shown.

@interface BDKFindBarView : NSView <NSSearchFieldDelegate>
@property(nonatomic, copy) void (^search)
    (NSString*, BOOL forward, BOOL matchCase, BOOL findNext);
@property(nonatomic, copy) void (^closed)(void);
- (instancetype)initWithText:(NSString*)text;
- (void)setOrdinal:(int)ordinal count:(int)count;
- (void)focusField;
@end

@implementation BDKFindBarView {
  NSSearchField* _field;
  NSTextField* _count;
  NSButton* _matchCase;
}

@synthesize search = _search;
@synthesize closed = _closed;

- (instancetype)initWithText:(NSString*)text {
  self = [super initWithFrame:NSMakeRect(0, 0, 430, 36)];
  if (!self) {
    return nil;
  }
  self.wantsLayer = YES;
  self.layer.backgroundColor = NSColor.windowBackgroundColor.CGColor;
  self.layer.cornerRadius = 6;
  self.layer.borderColor = NSColor.separatorColor.CGColor;
  self.layer.borderWidth = 1;
  self.autoresizingMask = NSViewMinXMargin;

  _field = [[NSSearchField alloc] initWithFrame:NSMakeRect(8, 6, 190, 24)];
  _field.delegate = self;
  _field.stringValue = text ?: @"";
  _field.sendsSearchStringImmediately = NO;
  _field.sendsWholeSearchString = YES;
  _field.target = self;
  _field.action = @selector(next:);
  [self addSubview:_field];

  _count = [NSTextField labelWithString:@""];
  _count.frame = NSMakeRect(202, 8, 70, 18);
  _count.textColor = NSColor.secondaryLabelColor;
  [self addSubview:_count];

  NSButton* previous = [NSButton buttonWithTitle:@"‹"
                                          target:self
                                          action:@selector(previous:)];
  previous.frame = NSMakeRect(274, 6, 28, 24);
  previous.toolTip = @"Previous";
  [self addSubview:previous];
  NSButton* next = [NSButton buttonWithTitle:@"›"
                                      target:self
                                      action:@selector(next:)];
  next.frame = NSMakeRect(302, 6, 28, 24);
  next.toolTip = @"Next";
  [self addSubview:next];

  _matchCase = [NSButton checkboxWithTitle:@"Aa"
                                    target:self
                                    action:@selector(optionChanged:)];
  _matchCase.frame = NSMakeRect(336, 8, 44, 20);
  _matchCase.toolTip = @"Match case";
  [self addSubview:_matchCase];

  NSButton* done = [NSButton buttonWithTitle:@"Done"
                                      target:self
                                      action:@selector(done:)];
  done.frame = NSMakeRect(376, 6, 50, 24);
  [self addSubview:done];
  return self;
}

- (BOOL)isFlipped {
  return NO;
}

- (void)focusField {
  [self.window makeFirstResponder:_field];
}

- (void)setOrdinal:(int)ordinal count:(int)count {
  if (_field.stringValue.length == 0) {
    _count.stringValue = @"";
  } else if (count == 0) {
    _count.stringValue = @"No matches";
  } else {
    _count.stringValue =
        [NSString stringWithFormat:@"%d of %d", ordinal, count];
  }
}

- (void)runSearchForward:(BOOL)forward findNext:(BOOL)findNext {
  if (self.search) {
    self.search(_field.stringValue, forward,
                _matchCase.state == NSControlStateValueOn, findNext);
  }
}

- (void)controlTextDidChange:(NSNotification*)notification {
  [self runSearchForward:YES findNext:NO];
}

- (void)optionChanged:(id)sender {
  [self runSearchForward:YES findNext:NO];
}

- (void)next:(id)sender {
  [self runSearchForward:YES findNext:YES];
}

- (void)previous:(id)sender {
  [self runSearchForward:NO findNext:YES];
}

- (void)done:(id)sender {
  if (self.closed) {
    self.closed();
  }
}

- (BOOL)control:(NSControl*)control
               textView:(NSTextView*)textView
    doCommandBySelector:(SEL)selector {
  if (selector == @selector(cancelOperation:)) {
    [self done:nil];
    return YES;
  }
  return NO;
}

@end

// A prompt attached to a rectangle the application supplied for its role
// (bdk_window_set_anchor_provider): a message and a row of buttons, just under
// the anchor. An NSAlert sheet cannot be positioned, so a window with an
// anchor gets this instead.
@interface BDKAnchoredPromptView : NSView
@property(nonatomic, copy) void (^answered)(NSInteger index);
- (instancetype)initWithMessage:(NSString*)message
                        buttons:(NSArray<NSString*>*)buttons;
- (void)pressButton:(NSInteger)index;
@end

@implementation BDKAnchoredPromptView {
  NSArray<NSButton*>* _buttons;
}

@synthesize answered = _answered;

- (instancetype)initWithMessage:(NSString*)message
                        buttons:(NSArray<NSString*>*)titles {
  self = [super initWithFrame:NSMakeRect(0, 0, 340, 96)];
  if (!self) {
    return nil;
  }
  self.wantsLayer = YES;
  self.layer.backgroundColor = NSColor.windowBackgroundColor.CGColor;
  self.layer.cornerRadius = 8;
  self.layer.borderColor = NSColor.separatorColor.CGColor;
  self.layer.borderWidth = 1;
  NSTextField* label = [NSTextField wrappingLabelWithString:message];
  label.font = [NSFont systemFontOfSize:12];
  label.frame = NSMakeRect(12, 40, 316, 48);
  [self addSubview:label];
  NSMutableArray<NSButton*>* buttons = [NSMutableArray array];
  CGFloat x = 12;
  for (NSUInteger i = 0; i < titles.count; ++i) {
    NSButton* button = [NSButton buttonWithTitle:titles[i]
                                          target:self
                                          action:@selector(pressed:)];
    button.tag = static_cast<NSInteger>(i);
    button.controlSize = NSControlSizeSmall;
    [button sizeToFit];
    button.frame = NSMakeRect(x, 8, NSWidth(button.frame), 24);
    x += NSWidth(button.frame) + 6;
    [self addSubview:button];
    [buttons addObject:button];
  }
  _buttons = buttons;
  return self;
}

- (void)pressed:(NSButton*)sender {
  [self pressButton:sender.tag];
}

- (void)pressButton:(NSInteger)index {
  if (self.answered) {
    self.answered(index);
  }
}

@end

// A web notification shown inside the application's window.
@interface BDKNotificationView : NSView
@property(nonatomic, copy) void (^clicked)(NSString* action);
@property(nonatomic, copy) void (^closedByUser)(void);
- (instancetype)initWithTitle:(NSString*)title
                         body:(NSString*)body
                       origin:(NSString*)origin
                      actions:(NSArray<NSString*>*)actions;
@end

@implementation BDKNotificationView {
  NSArray<NSString*>* _actions;
}

@synthesize clicked = _clicked;
@synthesize closedByUser = _closedByUser;

- (instancetype)initWithTitle:(NSString*)title
                         body:(NSString*)body
                       origin:(NSString*)origin
                      actions:(NSArray<NSString*>*)actions {
  const CGFloat height = actions.count ? 96 : 72;
  self = [super initWithFrame:NSMakeRect(0, 0, 330, height)];
  if (!self) {
    return nil;
  }
  _actions = actions;
  self.wantsLayer = YES;
  self.layer.backgroundColor = NSColor.windowBackgroundColor.CGColor;
  self.layer.cornerRadius = 8;
  self.layer.borderColor = NSColor.separatorColor.CGColor;
  self.layer.borderWidth = 1;
  self.autoresizingMask = NSViewMinXMargin | NSViewMinYMargin;

  NSTextField* origin_label = [NSTextField labelWithString:origin];
  origin_label.font = [NSFont systemFontOfSize:10];
  origin_label.textColor = NSColor.secondaryLabelColor;
  origin_label.frame = NSMakeRect(12, height - 20, 280, 14);
  [self addSubview:origin_label];
  NSTextField* title_label = [NSTextField labelWithString:title];
  title_label.font = [NSFont boldSystemFontOfSize:13];
  title_label.frame = NSMakeRect(12, height - 40, 280, 18);
  [self addSubview:title_label];
  NSTextField* body_label = [NSTextField wrappingLabelWithString:body];
  body_label.font = [NSFont systemFontOfSize:12];
  body_label.frame = NSMakeRect(12, height - 66, 300, 26);
  [self addSubview:body_label];

  NSButton* close = [NSButton buttonWithTitle:@"×"
                                       target:self
                                       action:@selector(closeTapped:)];
  close.bordered = NO;
  close.frame = NSMakeRect(300, height - 28, 24, 22);
  [self addSubview:close];

  CGFloat x = 12;
  for (NSUInteger i = 0; i < actions.count; ++i) {
    NSButton* button = [NSButton buttonWithTitle:actions[i]
                                          target:self
                                          action:@selector(actionTapped:)];
    button.tag = static_cast<NSInteger>(i);
    button.controlSize = NSControlSizeSmall;
    [button sizeToFit];
    button.frame = NSMakeRect(x, 6, NSWidth(button.frame), 22);
    x += NSWidth(button.frame) + 6;
    [self addSubview:button];
  }
  return self;
}

- (void)mouseUp:(NSEvent*)event {
  if (self.clicked) {
    self.clicked(nil);
  }
}

- (void)closeTapped:(id)sender {
  if (self.closedByUser) {
    self.closedByUser();
  }
}

- (void)actionTapped:(NSButton*)sender {
  if (self.clicked) {
    self.clicked(_actions[sender.tag]);
  }
}

@end

namespace bdk {

namespace {

NSString* Str(const base::DictValue& dict, const char* key) {
  const std::string* value = dict.FindString(key);
  return value ? base::SysUTF8ToNSString(*value) : @"";
}

NSString* TextOf(const base::DictValue& payload, const char* key) {
  const base::DictValue* text = payload.FindDict("text");
  return text ? Str(*text, key) : @"";
}

// What a sheet needs to be taken down again.
struct SheetState {
  bool alive = true;
  NSWindow* __strong parent = nil;
};

// Calls `show` with the window of `anchor` now, or as soon as it has one.
void WhenInWindow(NSView* anchor,
                  std::shared_ptr<SheetState> state,
                  void (^show)(NSWindow*)) {
  if (!state->alive) {
    return;
  }
  if (anchor.window) {
    show(anchor.window);
    return;
  }
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC),
                 dispatch_get_main_queue(), ^{
                   WhenInWindow(anchor, state, show);
                 });
}

void EndSheet(std::shared_ptr<SheetState> state, NSWindow* sheet) {
  if (!state->alive) {
    return;
  }
  state->alive = false;
  if (state->parent && state->parent.attachedSheet == sheet) {
    [state->parent endSheet:sheet returnCode:NSModalResponseAbort];
  }
}

// Presents a sheet on the window `anchor` is in. `begin` starts it on the
// parent window with the completion handler to pass on; `done` runs with the
// response unless the sheet was dismissed first.
Dismisser PresentSheet(NSView* anchor,
                       NSWindow* sheet,
                       void (^begin)(NSWindow*, void (^)(NSModalResponse)),
                       void (^done)(NSModalResponse)) {
  auto state = std::make_shared<SheetState>();
  WhenInWindow(anchor, state, ^(NSWindow* parent) {
    state->parent = parent;
    begin(parent, ^(NSModalResponse response) {
      if (!state->alive) {
        return;
      }
      state->alive = false;
      done(response);
    });
  });
  return base::BindOnce(&EndSheet, state, sheet);
}

Dismisser PresentAlert(NSView* anchor,
                       NSAlert* alert,
                       void (^done)(NSModalResponse)) {
  return PresentSheet(
      anchor, alert.window,
      ^(NSWindow* parent, void (^completion)(NSModalResponse)) {
        [alert beginSheetModalForWindow:parent completionHandler:completion];
      },
      done);
}

BDKAnchoredPromptView* __weak g_anchored_prompt = nil;

// Puts a prompt under `anchor` (window coordinates, origin top left) in the
// window `view` is in. `done` runs with the pressed button's index unless the
// prompt was dismissed first. Null when the page is in no window.
Dismisser PresentAnchored(NSView* view,
                          std::array<double, 4> anchor,
                          NSString* message,
                          NSArray<NSString*>* buttons,
                          void (^done)(NSInteger)) {
  NSView* content = view.window.contentView;
  if (!content) {
    return base::DoNothing();
  }
  BDKAnchoredPromptView* prompt =
      [[BDKAnchoredPromptView alloc] initWithMessage:message buttons:buttons];
  const CGFloat width = NSWidth(prompt.frame);
  const CGFloat height = NSHeight(prompt.frame);
  const CGFloat x = std::clamp<CGFloat>(
      anchor[0], 0, std::max<CGFloat>(0, NSWidth(content.bounds) - width));
  const CGFloat top = anchor[1] + anchor[3] + 4;
  prompt.frame =
      NSMakeRect(x, NSHeight(content.bounds) - top - height, width, height);
  prompt.autoresizingMask = NSViewMinYMargin;
  [content addSubview:prompt positioned:NSWindowAbove relativeTo:nil];
  g_anchored_prompt = prompt;
  auto live = std::make_shared<bool>(true);
  __weak BDKAnchoredPromptView* weak_prompt = prompt;
  auto remove = [live, weak_prompt] {
    *live = false;
    [weak_prompt removeFromSuperview];
  };
  prompt.answered = ^(NSInteger index) {
    if (!*live) {
      return;
    }
    remove();
    done(index);
  };
  return base::BindOnce([](std::function<void()> remove) { remove(); },
                        std::function<void()>(remove));
}

NSView* AnchorOf(HostView* view) {
  if (!view) {
    return nil;
  }
  return (__bridge NSView*)view->NativeHandle().pointer;
}

// The window a notification without a page appears in.
NSWindow* ForemostWindow() {
  NSWindow* window = NSApp.keyWindow ?: NSApp.mainWindow;
  if (window) {
    return window;
  }
  for (NSWindow* candidate in NSApp.orderedWindows) {
    if (candidate.visible && candidate.contentView) {
      return candidate;
    }
  }
  return nil;
}

NSString* ExtensionOf(NSString* type) {
  return [type hasPrefix:@"."] ? [type substringFromIndex:1] : type;
}

NSArray<UTType*>* ContentTypesOf(const base::ListValue* accepted) {
  NSMutableArray<UTType*>* types = [NSMutableArray array];
  if (!accepted) {
    return types;
  }
  for (const base::Value& entry : *accepted) {
    if (!entry.is_string()) {
      continue;
    }
    NSString* type = base::SysUTF8ToNSString(entry.GetString());
    UTType* resolved = nil;
    if ([type isEqualToString:@"image/*"]) {
      resolved = UTTypeImage;
    } else if ([type isEqualToString:@"audio/*"]) {
      resolved = UTTypeAudio;
    } else if ([type isEqualToString:@"video/*"]) {
      resolved = UTTypeMovie;
    } else if ([type containsString:@"/"]) {
      resolved = [UTType typeWithMIMEType:type];
    } else {
      resolved = [UTType typeWithFilenameExtension:ExtensionOf(type)];
    }
    if (resolved) {
      [types addObject:resolved];
    }
  }
  return types;
}

class FindBarMac : public FindBar {
 public:
  FindBarMac(NSView* anchor, NSString* text, FindBarClient* client)
      : anchor_(anchor), view_([[BDKFindBarView alloc] initWithText:text]) {
    view_.search =
        ^(NSString* query, BOOL forward, BOOL match_case, BOOL find_next) {
          client->Search(base::SysNSStringToUTF8(query), forward, match_case,
                         find_next);
        };
    view_.closed = ^{
      client->Closed();
    };
    NSRect bounds = anchor_.bounds;
    view_.frame = NSMakeRect(NSWidth(bounds) - NSWidth(view_.frame) - 12, 12,
                             NSWidth(view_.frame), NSHeight(view_.frame));
    [anchor_ addSubview:view_];
    Focus();
  }

  ~FindBarMac() override {
    view_.search = nil;
    view_.closed = nil;
    [view_ removeFromSuperview];
  }

  void SetResult(int ordinal, int count) override {
    [view_ setOrdinal:ordinal count:count];
  }

  void Focus() override { [view_ focusField]; }

 private:
  NSView* __strong anchor_;
  BDKFindBarView* __strong view_;
};

// Answers from a task, as every presenter does: never from inside the call
// that showed it.
template <typename T>
void AnswerLater(base::OnceCallback<void(T)> answer, T value) {
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(std::move(answer), std::move(value)));
}

// The default device chooser: a sheet with the devices in a pop-up menu,
// kept to the chooser's list, and Connect, which needs a device, and Cancel.
class DeviceChooserSheetMac : public DeviceChooserSheet {
 public:
  DeviceChooserSheetMac(
      HostView* view,
      const base::DictValue& payload,
      base::OnceCallback<void(std::optional<std::string>)> answer)
      : answer_(std::move(answer)) {
    NSDictionary<NSString*, NSString*>* what = @{
      @"usb" : @"a USB device",
      @"hid" : @"a HID device",
      @"serial" : @"a serial port",
      @"bluetooth" : @"a Bluetooth device"
    };
    alert_ = [[NSAlert alloc] init];
    alert_.messageText = [NSString
        stringWithFormat:@"%@ wants to connect to %@", Str(payload, "origin"),
                         what[Str(payload, "kind")] ?: @"a device"];
    [alert_ addButtonWithTitle:@"Connect"];
    [alert_ addButtonWithTitle:@"Cancel"];
    menu_ = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 300, 26)
                                       pullsDown:NO];
    alert_.accessoryView = menu_;
    if (const base::ListValue* devices = payload.FindList("devices")) {
      SetDevices(*devices);
    } else {
      SetDevices(base::ListValue());
    }
    NSPopUpButton* menu = menu_;
    __block base::OnceCallback<void(std::optional<std::string>)>* callback =
        &answer_;
    dismiss_ =
        PresentAlert(AnchorOf(view), alert_, ^(NSModalResponse response) {
          std::optional<std::string> chosen;
          NSString* id = menu.selectedItem.representedObject;
          if (response == NSAlertFirstButtonReturn && id) {
            chosen = base::SysNSStringToUTF8(id);
          }
          if (*callback) {
            AnswerLater(std::move(*callback), std::move(chosen));
          }
        });
  }

  ~DeviceChooserSheetMac() override {
    answer_.Reset();
    if (dismiss_) {
      std::move(dismiss_).Run();
    }
  }

  void SetDevices(const base::ListValue& devices) override {
    NSString* selected = menu_.selectedItem.representedObject;
    [menu_ removeAllItems];
    for (const base::Value& device : devices) {
      const base::DictValue* dict = device.GetIfDict();
      if (!dict) {
        continue;
      }
      NSString* title = Str(*dict, "name");
      if (dict->FindBool("paired").value_or(false)) {
        title = [title stringByAppendingString:@" (paired)"];
      }
      // Added to the menu, not the button, which would merge two devices of
      // the same name; the item stands for the device's id.
      NSMenuItem* item = [menu_.menu addItemWithTitle:title
                                               action:nil
                                        keyEquivalent:@""];
      item.representedObject = Str(*dict, "id");
      if ([selected isEqualToString:item.representedObject]) {
        [menu_ selectItem:item];
      }
    }
    const bool any = menu_.numberOfItems > 0;
    if (!any) {
      [menu_ addItemWithTitle:@"Looking for devices…"];
    }
    menu_.enabled = any;
    alert_.buttons.firstObject.enabled = any;
  }

 private:
  NSAlert* __strong alert_;
  NSPopUpButton* __strong menu_;
  base::OnceCallback<void(std::optional<std::string>)> answer_;
  Dismisser dismiss_;
};

class PresentersMac : public UiPresenters {
 public:
  Dismisser ShowPermission(
      HostView* view,
      const base::DictValue& payload,
      base::OnceCallback<void(PermissionChoice)> answer) override {
    const bool one_time = payload.FindBool("offersOneTime").value_or(false);
    if (std::optional<std::array<double, 4>> anchor =
            ViewWindowAnchor(view, BDK_ANCHOR_ADDRESS_BAR)) {
      NSMutableArray<NSString*>* titles =
          [NSMutableArray arrayWithObject:@"Allow"];
      if (one_time) {
        [titles addObject:@"Allow This Time"];
      }
      [titles addObjectsFromArray:@[ @"Block", @"Not Now" ]];
      __block base::OnceCallback<void(PermissionChoice)> callback =
          std::move(answer);
      return PresentAnchored(
          AnchorOf(view), *anchor,
          [NSString stringWithFormat:@"%@ %@", TextOf(payload, "title"),
                                     TextOf(payload, "message")],
          titles, ^(NSInteger index) {
            PermissionChoice choice = PermissionChoice::kDismiss;
            if (index == 0) {
              choice = PermissionChoice::kAllow;
            } else if (one_time && index == 1) {
              choice = PermissionChoice::kAllowThisVisit;
            } else if (index == (one_time ? 2 : 1)) {
              choice = PermissionChoice::kBlock;
            }
            std::move(callback).Run(choice);
          });
    }
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText =
        [NSString stringWithFormat:@"%@ %@", TextOf(payload, "title"),
                                   TextOf(payload, "message")];
    [alert addButtonWithTitle:@"Allow"];
    if (one_time) {
      [alert addButtonWithTitle:@"Allow This Time"];
    }
    [alert addButtonWithTitle:@"Block"];
    [alert addButtonWithTitle:@"Not Now"];
    __block base::OnceCallback<void(PermissionChoice)> callback =
        std::move(answer);
    return PresentAlert(AnchorOf(view), alert, ^(NSModalResponse response) {
      const NSInteger index = response - NSAlertFirstButtonReturn;
      PermissionChoice choice = PermissionChoice::kDismiss;
      if (index == 0) {
        choice = PermissionChoice::kAllow;
      } else if (one_time && index == 1) {
        choice = PermissionChoice::kAllowThisVisit;
      } else if (index == (one_time ? 2 : 1)) {
        choice = PermissionChoice::kBlock;
      }
      std::move(callback).Run(choice);
    });
  }

  Dismisser ShowJavaScriptDialog(
      HostView* view,
      const base::DictValue& payload,
      base::OnceCallback<void(JavaScriptDialogAnswer)> answer) override {
    const std::string kind =
        payload.FindString("kind") ? *payload.FindString("kind") : "alert";
    NSAlert* alert = [[NSAlert alloc] init];
    NSString* title = Str(payload, "pageTitle");
    NSString* origin = Str(payload, "origin");
    if (kind == "beforeUnload") {
      alert.messageText = @"Leave site?";
      alert.informativeText = Str(payload, "message");
      [alert addButtonWithTitle:@"Leave"];
      [alert addButtonWithTitle:@"Stay"];
    } else {
      // The page's title and origin come with the message, so a page cannot
      // pass its dialog off as the browser's.
      alert.messageText =
          title.length
              ? [NSString stringWithFormat:@"“%@” at %@ says", title, origin]
              : [NSString stringWithFormat:@"%@ says", origin];
      alert.informativeText = Str(payload, "message");
      [alert addButtonWithTitle:@"OK"];
      if (kind != "alert") {
        [alert addButtonWithTitle:@"Cancel"];
      }
    }
    NSTextField* field = nil;
    if (kind == "prompt") {
      field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 260, 24)];
      field.stringValue = Str(payload, "defaultPromptText");
      alert.accessoryView = field;
    }
    if (payload.FindBool("offersSuppression").value_or(false)) {
      alert.showsSuppressionButton = YES;
      alert.suppressionButton.title =
          @"Prevent this page from creating additional dialogs";
    }
    __block base::OnceCallback<void(JavaScriptDialogAnswer)> callback =
        std::move(answer);
    return PresentAlert(AnchorOf(view), alert, ^(NSModalResponse response) {
      JavaScriptDialogAnswer result;
      result.accepted = response == NSAlertFirstButtonReturn;
      if (field && result.accepted) {
        result.text = base::SysNSStringToUTF8(field.stringValue);
      }
      result.suppress_further =
          alert.showsSuppressionButton &&
          alert.suppressionButton.state == NSControlStateValueOn;
      std::move(callback).Run(std::move(result));
    });
  }

  Dismisser ShowAuthPrompt(
      HostView* view,
      const base::DictValue& payload,
      base::OnceCallback<void(std::optional<AuthAnswer>)> answer) override {
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = [NSString
        stringWithFormat:@"Sign in to %@", Str(payload, "challenger")];
    NSString* realm = Str(payload, "realm");
    alert.informativeText =
        realm.length
            ? [NSString stringWithFormat:@"The site says: “%@”.\n"
                                         @"Your connection to this site "
                                         @"may not be private.",
                                         realm]
            : @"Your connection to this site may not be private.";
    NSView* form = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 260, 56)];
    NSTextField* user =
        [[NSTextField alloc] initWithFrame:NSMakeRect(0, 32, 260, 24)];
    user.placeholderString = @"Username";
    NSSecureTextField* password =
        [[NSSecureTextField alloc] initWithFrame:NSMakeRect(0, 0, 260, 24)];
    password.placeholderString = @"Password";
    [form addSubview:user];
    [form addSubview:password];
    alert.accessoryView = form;
    [alert addButtonWithTitle:@"Sign In"];
    [alert addButtonWithTitle:@"Cancel"];
    __block base::OnceCallback<void(std::optional<AuthAnswer>)> callback =
        std::move(answer);
    return PresentAlert(AnchorOf(view), alert, ^(NSModalResponse response) {
      if (response != NSAlertFirstButtonReturn) {
        std::move(callback).Run(std::nullopt);
        return;
      }
      std::move(callback).Run(
          AuthAnswer{base::SysNSStringToUTF8(user.stringValue),
                     base::SysNSStringToUTF8(password.stringValue)});
    });
  }

  Dismisser ShowFilePicker(
      HostView* view,
      const base::DictValue& payload,
      base::OnceCallback<void(std::vector<base::FilePath>)> answer) override {
    const std::string mode =
        payload.FindString("mode") ? *payload.FindString("mode") : "open";
    NSSavePanel* panel = nil;
    if (mode == "save") {
      panel = [NSSavePanel savePanel];
      panel.nameFieldStringValue = Str(payload, "suggestedName");
    } else {
      NSOpenPanel* open = [NSOpenPanel openPanel];
      open.canChooseFiles = mode != "directory";
      open.canChooseDirectories = mode == "directory";
      open.allowsMultipleSelection = mode == "openMultiple";
      panel = open;
    }
    NSArray<UTType*>* types = ContentTypesOf(payload.FindList("acceptedTypes"));
    if (types.count && mode != "directory") {
      panel.allowedContentTypes = types;
    }
    __block base::OnceCallback<void(std::vector<base::FilePath>)> callback =
        std::move(answer);
    return PresentSheet(
        AnchorOf(view), panel,
        ^(NSWindow* parent, void (^completion)(NSModalResponse)) {
          [panel beginSheetModalForWindow:parent completionHandler:completion];
        },
        ^(NSModalResponse response) {
          std::vector<base::FilePath> paths;
          if (response == NSModalResponseOK) {
            if ([panel isKindOfClass:NSOpenPanel.class]) {
              for (NSURL* url in ((NSOpenPanel*)panel).URLs) {
                paths.push_back(base::apple::NSURLToFilePath(url));
              }
            } else if (panel.URL) {
              paths.push_back(base::apple::NSURLToFilePath(panel.URL));
            }
          }
          std::move(callback).Run(std::move(paths));
        });
  }

  Dismisser ShowNotification(
      HostView* view,
      const base::DictValue& payload,
      base::RepeatingCallback<void(NotificationEvent,
                                   const std::optional<std::string>&)> event)
      override {
    const base::DictValue* details = payload.FindDict("notification");
    if (!details) {
      return base::DoNothing();
    }
    NSMutableArray<NSString*>* actions = [NSMutableArray array];
    if (const base::ListValue* list = details->FindList("actions")) {
      for (const base::Value& action : *list) {
        if (action.is_string()) {
          [actions addObject:base::SysUTF8ToNSString(action.GetString())];
        }
      }
    }
    NSWindow* window = AnchorOf(view).window ?: ForemostWindow();
    if (!window || !window.contentView) {
      event.Run(NotificationEvent::kClosedByTimeout, std::nullopt);
      return base::DoNothing();
    }
    BDKNotificationView* banner =
        [[BDKNotificationView alloc] initWithTitle:Str(*details, "title")
                                              body:Str(*details, "body")
                                            origin:Str(*details, "origin")
                                           actions:actions];
    NSView* content = window.contentView;
    banner.frame =
        NSMakeRect(NSWidth(content.bounds) - NSWidth(banner.frame) - 16,
                   NSHeight(content.bounds) - NSHeight(banner.frame) - 16,
                   NSWidth(banner.frame), NSHeight(banner.frame));
    [content addSubview:banner positioned:NSWindowAbove relativeTo:nil];
    auto live = std::make_shared<bool>(true);
    __weak BDKNotificationView* weak_banner = banner;
    auto remove = [live, weak_banner] {
      *live = false;
      [weak_banner removeFromSuperview];
    };
    banner.clicked = ^(NSString* action) {
      if (!*live) {
        return;
      }
      std::optional<std::string> name;
      if (action) {
        name = base::SysNSStringToUTF8(action);
      }
      remove();
      event.Run(NotificationEvent::kClicked, name);
    };
    banner.closedByUser = ^{
      if (!*live) {
        return;
      }
      remove();
      event.Run(NotificationEvent::kClosedByUser, std::nullopt);
    };
    if (!details->FindBool("requireInteraction").value_or(false)) {
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_SEC),
                     dispatch_get_main_queue(), ^{
                       if (!*live) {
                         return;
                       }
                       remove();
                       event.Run(NotificationEvent::kClosedByTimeout,
                                 std::nullopt);
                     });
    }
    return base::BindOnce([](std::function<void()> remove) { remove(); },
                          std::function<void()>(remove));
  }

  Dismisser ShowAlert(const std::string& surface,
                      HostView* view,
                      Alert alert,
                      base::OnceCallback<void(int)> answer) override {
    NSView* anchor = AnchorOf(view);
    if (!anchor.window.visible) {
      AnswerLater(std::move(answer), -1);  // nowhere to show it
      return base::DoNothing();
    }
    // The engine's own prompts attach to the window's anchor for their role
    // when it supplies one: passwords and autofill at the address bar,
    // downloads at the downloads button.
    const uint32_t role = surface == "downloads"
                              ? BDK_ANCHOR_DOWNLOADS
                              : (surface == "passwords" || surface == "autofill"
                                     ? BDK_ANCHOR_ADDRESS_BAR
                                     : 0u);
    if (std::optional<std::array<double, 4>> position =
            role ? ViewWindowAnchor(view, role) : std::nullopt) {
      NSMutableArray<NSString*>* titles = [NSMutableArray array];
      for (const AlertButton& button : alert.buttons) {
        [titles addObject:base::SysUTF8ToNSString(button.title)];
      }
      const size_t button_count = alert.buttons.size();
      __block base::OnceCallback<void(int)> callback = std::move(answer);
      return PresentAnchored(
          anchor, *position,
          [NSString stringWithFormat:@"%@\n%@",
                                     base::SysUTF8ToNSString(alert.title),
                                     base::SysUTF8ToNSString(alert.message)],
          titles, ^(NSInteger index) {
            std::move(callback).Run(index >= 0 && static_cast<size_t>(index) <
                                                      button_count
                                        ? static_cast<int>(index)
                                        : -1);
          });
    }
    NSAlert* ns_alert = [[NSAlert alloc] init];
    ns_alert.messageText = base::SysUTF8ToNSString(alert.title);
    ns_alert.informativeText = base::SysUTF8ToNSString(alert.message);
    for (const AlertButton& button : alert.buttons) {
      [ns_alert addButtonWithTitle:base::SysUTF8ToNSString(button.title)];
    }
    const size_t count = alert.buttons.size();
    __block base::OnceCallback<void(int)> callback = std::move(answer);
    return PresentAlert(anchor, ns_alert, ^(NSModalResponse response) {
      const NSInteger index = response - NSAlertFirstButtonReturn;
      std::move(callback).Run(index >= 0 && static_cast<size_t>(index) < count
                                  ? static_cast<int>(index)
                                  : -1);
    });
  }

  Dismisser ShowSavePanel(
      const std::string& surface,
      HostView* view,
      const std::string& suggested_name,
      base::OnceCallback<void(std::optional<std::string>)> answer) override {
    NSView* anchor = AnchorOf(view);
    if (!anchor.window.visible) {
      AnswerLater(std::move(answer), std::optional<std::string>());
      return base::DoNothing();
    }
    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.nameFieldStringValue = base::SysUTF8ToNSString(suggested_name);
    __block base::OnceCallback<void(std::optional<std::string>)> callback =
        std::move(answer);
    return PresentSheet(
        anchor, panel,
        ^(NSWindow* parent, void (^completion)(NSModalResponse)) {
          [panel beginSheetModalForWindow:parent completionHandler:completion];
        },
        ^(NSModalResponse response) {
          if (response == NSModalResponseOK && panel.URL.path) {
            std::move(callback).Run(base::SysNSStringToUTF8(panel.URL.path));
          } else {
            std::move(callback).Run(std::nullopt);
          }
        });
  }

  // Draws a QR code for `text`, sharp at `side` points, or nil when the
  // generator is unavailable.
  static NSImage* QRCodeImage(NSString* text, CGFloat side) {
    CIFilter* filter = [CIFilter filterWithName:@"CIQRCodeGenerator"];
    NSData* data = [text dataUsingEncoding:NSISOLatin1StringEncoding];
    if (!filter || !data) {
      return nil;
    }
    [filter setValue:data forKey:@"inputMessage"];
    [filter setValue:@"M" forKey:@"inputCorrectionLevel"];
    CIImage* code = filter.outputImage;
    if (!code || code.extent.size.width < 1) {
      return nil;
    }
    const CGFloat scale =
        std::max<CGFloat>(1, std::floor(side * 2 / code.extent.size.width));
    CIImage* scaled = [code
        imageByApplyingTransform:CGAffineTransformMakeScale(scale, scale)];
    NSCIImageRep* rep = [NSCIImageRep imageRepWithCIImage:scaled];
    NSImage* image = [[NSImage alloc] initWithSize:NSMakeSize(side, side)];
    [image addRepresentation:rep];
    return image;
  }

  std::unique_ptr<DeviceChooserSheet> ShowDeviceChooser(
      HostView* view,
      const base::DictValue& payload,
      base::OnceCallback<void(std::optional<std::string>)> answer) override {
    return std::make_unique<DeviceChooserSheetMac>(view, payload,
                                                   std::move(answer));
  }

  Dismisser ShowWebAuthn(
      HostView* view,
      const base::DictValue& payload,
      base::OnceCallback<void(base::DictValue)> answer) override {
    // One sheet per step, in Chromium's wording: a button per mechanism or
    // account where the step is a choice, a PIN field where it asks for one,
    // the step's own button where it has one, and the step's cancel button
    // last (Escape's).
    const std::string step =
        payload.FindString("step") ? *payload.FindString("step") : "";
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = TextOf(payload, "title");
    NSString* message = TextOf(payload, "message");
    NSString* error = TextOf(payload, "error");
    alert.informativeText =
        error.length ? [NSString stringWithFormat:@"%@\n\n%@", message, error]
                     : message;
    // Each button's answer, in the order the buttons were added.
    auto answers = std::make_shared<std::vector<base::DictValue>>();
    auto add = [&](NSString* title, base::DictValue choice) {
      [alert addButtonWithTitle:title];
      answers->push_back(std::move(choice));
    };
    const char* list = step == "chooseMechanism" ? "mechanisms"
                       : step == "chooseAccount" ? "accounts"
                                                 : nullptr;
    if (const base::ListValue* options =
            list ? payload.FindList(list) : nullptr) {
      const bool account = step == "chooseAccount";
      for (const base::Value& option : *options) {
        if (!option.is_dict()) {
          continue;
        }
        const base::DictValue& entry = option.GetDict();
        NSString* name = Str(entry, "name");
        if (account && !name.length) {
          name = Str(entry, "displayName");
        }
        base::DictValue choice;
        choice.Set("completion", account ? "grant" : "accept");
        choice.Set(account ? "account" : "mechanism",
                   entry.FindString("id") ? *entry.FindString("id") : "");
        add(name, std::move(choice));
      }
    }
    NSSecureTextField* pin = nil;
    NSSecureTextField* confirm = nil;
    if (const base::DictValue* entry = payload.FindDict("pin")) {
      const bool twice = Str(*entry, "mode").length &&
                         ![Str(*entry, "mode") isEqualToString:@"enter"];
      NSView* form =
          [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 260, twice ? 56 : 24)];
      pin = [[NSSecureTextField alloc]
          initWithFrame:NSMakeRect(0, twice ? 32 : 0, 260, 24)];
      pin.placeholderString = @"PIN";
      [form addSubview:pin];
      if (twice) {
        confirm =
            [[NSSecureTextField alloc] initWithFrame:NSMakeRect(0, 0, 260, 24)];
        confirm.placeholderString = @"Confirm PIN";
        [form addSubview:confirm];
      }
      alert.accessoryView = form;
    }
    if (!pin) {
      // The phone pairs by scanning this code, so the sheet has to show it.
      NSString* qr = Str(payload, "qrCode");
      NSImage* image = qr.length ? QRCodeImage(qr, 200) : nil;
      if (image) {
        NSImageView* code = [NSImageView imageViewWithImage:image];
        code.frame = NSMakeRect(0, 0, 200, 200);
        alert.accessoryView = code;
      }
    }
    NSString* accept = Str(payload, "acceptLabel");
    if (accept.length && !list) {
      base::DictValue choice;
      choice.Set("completion", "accept");
      add(accept, std::move(choice));
    }
    NSString* other = Str(payload, "otherMechanismsLabel");
    if (other.length) {
      base::DictValue choice;
      choice.Set("completion", "accept");
      choice.Set("action", "otherMechanisms");
      add(other, std::move(choice));
    }
    NSString* cancel = Str(payload, "cancelLabel");
    base::DictValue deny;
    deny.Set("completion", "deny");
    add(cancel.length ? cancel : @"Cancel", std::move(deny));
    __block base::OnceCallback<void(base::DictValue)> callback =
        std::move(answer);
    return PresentAlert(AnchorOf(view), alert, ^(NSModalResponse response) {
      const NSInteger index = response - NSAlertFirstButtonReturn;
      if (index < 0 || static_cast<size_t>(index) >= answers->size()) {
        base::DictValue closed;
        closed.Set("completion", "deny");
        std::move(callback).Run(std::move(closed));
        return;
      }
      base::DictValue choice = std::move((*answers)[index]);
      const std::string* completion = choice.FindString("completion");
      if (pin && completion && *completion == "accept" &&
          !choice.FindString("action")) {
        choice.Set("pin", base::SysNSStringToUTF8(pin.stringValue));
        if (confirm) {
          // Chromium's sheet compares the two and says when they differ.
          choice.Set("pinConfirmation",
                     base::SysNSStringToUTF8(confirm.stringValue));
        }
      }
      std::move(callback).Run(std::move(choice));
    });
  }

  Dismisser ShowPageWindow(const std::string& surface,
                           HostView* view,
                           const std::string& title,
                           int width,
                           int height,
                           base::OnceClosure closed) override {
    NSView* page = AnchorOf(view);
    if (!page) {
      return base::DoNothing();
    }
    NSWindow* window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, width > 0 ? width : 960,
                                       height > 0 ? height : 640)
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable |
                            NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    window.releasedWhenClosed = NO;
    window.title = base::SysUTF8ToNSString(title);
    [page removeFromSuperview];
    page.frame = window.contentView.bounds;
    page.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [window.contentView addSubview:page];
    [window center];
    [window makeKeyAndOrderFront:nil];
    // The person closing the window closes the page; the dismisser closes
    // the window without asking again.
    auto pending = std::make_shared<base::OnceClosure>(std::move(closed));
    id observer = [NSNotificationCenter.defaultCenter
        addObserverForName:NSWindowWillCloseNotification
                    object:window
                     queue:nil
                usingBlock:^(NSNotification*) {
                  if (*pending) {
                    std::move(*pending).Run();
                  }
                }];
    return base::BindOnce(
        [](NSWindow* w, id o, std::shared_ptr<base::OnceClosure> p) {
          [NSNotificationCenter.defaultCenter removeObserver:o];
          p->Reset();
          [w orderOut:nil];
          [w close];
        },
        window, observer, pending);
  }

  std::unique_ptr<FindBar> ShowFindBar(HostView* view,
                                       const base::DictValue& payload,
                                       FindBarClient* client) override {
    NSView* anchor = AnchorOf(view);
    if (!anchor) {
      return nullptr;
    }
    return std::make_unique<FindBarMac>(anchor, Str(payload, "initialText"),
                                        client);
  }

  bool IsRecentGesture(const bdk_native_handle_t& gesture) override {
    if (gesture.kind != BDK_NATIVE_NSEVENT || !gesture.pointer) {
      return false;
    }
    NSEvent* event = (__bridge NSEvent*)gesture.pointer;
    switch (event.type) {
      case NSEventTypeLeftMouseUp:
      case NSEventTypeRightMouseUp:
      case NSEventTypeOtherMouseUp:
      case NSEventTypeKeyDown:
        break;
      default:
        return false;
    }
    constexpr NSTimeInterval kRecent = 10;
    if (NSProcessInfo.processInfo.systemUptime - event.timestamp > kRecent) {
      return false;
    }
    // Delivered to a window of this application.
    return event.window && [NSApp.windows containsObject:event.window];
  }
};

}  // namespace

std::optional<std::array<double, 4>> AnchoredPromptFrame() {
  BDKAnchoredPromptView* prompt = g_anchored_prompt;
  NSView* content = prompt.superview;
  if (!content) {
    return std::nullopt;
  }
  const NSRect frame = prompt.frame;
  return std::array<double, 4>{
      frame.origin.x,
      NSHeight(content.bounds) - frame.origin.y - frame.size.height,
      frame.size.width, frame.size.height};
}

bool AnswerAnchoredPromptForTesting(int index) {
  BDKAnchoredPromptView* prompt = g_anchored_prompt;
  if (!prompt.superview) {
    return false;
  }
  [prompt pressButton:index];
  return true;
}

std::unique_ptr<UiPresenters> CreatePlatformPresenters() {
  return std::make_unique<PresentersMac>();
}

}  // namespace bdk
