// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import <AppKit/AppKit.h>
#import <CoreServices/CoreServices.h>
#import <Foundation/Foundation.h>

#include <utility>

#include "base/apple/foundation_util.h"
#include "base/apple/scoped_cftyperef.h"
#include "base/functional/bind.h"
#include "base/strings/sys_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "bdk/engine/services/system_platform.h"

namespace bdk {

namespace {

// Whether the application's Info.plist declares the URL scheme.
bool DeclaresScheme(NSString* scheme) {
  NSArray* types =
      [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleURLTypes"];
  for (NSDictionary* type in types) {
    for (NSString* declared in type[@"CFBundleURLSchemes"]) {
      if ([declared caseInsensitiveCompare:scheme] == NSOrderedSame) {
        return YES;
      }
    }
  }
  return NO;
}

// Whether the application the system opens URLs of this scheme with is this
// one.
bool IsDefaultHandlerOf(NSString* scheme, NSString* bundle_id) {
  NSURL* probe =
      [NSURL URLWithString:[scheme stringByAppendingString:@"://example.com/"]];
  NSURL* handler =
      [NSWorkspace.sharedWorkspace URLForApplicationToOpenURL:probe];
  NSString* handler_id =
      handler ? [NSBundle bundleWithURL:handler].bundleIdentifier : nil;
  return handler_id && bundle_id &&
         [handler_id caseInsensitiveCompare:bundle_id] == NSOrderedSame;
}

DefaultBrowserStatus ReadStatus() {
  DefaultBrowserStatus status;
  NSString* bundle_id = NSBundle.mainBundle.bundleIdentifier;
  status.is_default = IsDefaultHandlerOf(@"http", bundle_id) &&
                      IsDefaultHandlerOf(@"https", bundle_id);
  status.can_be_default = DeclaresScheme(@"http") && DeclaresScheme(@"https");
  return status;
}

}  // namespace

void GetDefaultBrowserStatus(
    base::OnceCallback<void(DefaultBrowserStatus)> done) {
  base::ThreadPool::PostTaskAndReplyWithResult(FROM_HERE, {base::MayBlock()},
                                               base::BindOnce(&ReadStatus),
                                               std::move(done));
}

void RequestDefaultBrowser(base::OnceCallback<void(std::string)> done) {
  NSURL* app = NSBundle.mainBundle.bundleURL;
  auto runner = base::SequencedTaskRunner::GetCurrentDefault();
  // The handler runs on a queue of the system's; both schemes are set one
  // after the other and the first failure is reported.
  __block base::OnceCallback<void(std::string)> block_done = std::move(done);
  [NSWorkspace.sharedWorkspace
      setDefaultApplicationAtURL:app
            toOpenURLsWithScheme:@"http"
               completionHandler:^(NSError* http_error) {
                 if (http_error) {
                   std::string reason =
                       base::SysNSStringToUTF8(http_error.localizedDescription);
                   runner->PostTask(FROM_HERE,
                                    base::BindOnce(std::move(block_done),
                                                   std::move(reason)));
                   return;
                 }
                 [NSWorkspace.sharedWorkspace
                     setDefaultApplicationAtURL:app
                           toOpenURLsWithScheme:@"https"
                              completionHandler:^(NSError* https_error) {
                                std::string reason =
                                    https_error
                                        ? base::SysNSStringToUTF8(
                                              https_error.localizedDescription)
                                        : std::string();
                                runner->PostTask(
                                    FROM_HERE,
                                    base::BindOnce(std::move(block_done),
                                                   std::move(reason)));
                              }];
               }];
}

}  // namespace bdk
