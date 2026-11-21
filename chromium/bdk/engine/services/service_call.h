// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// What every built-in command handler of the engine works with: the broker's
// call, its arguments parsed, the Chromium profile it addresses and the ways
// to answer. Handlers live in the files of this directory (one per service)
// and are found by command name in command_dispatch.cc.

#ifndef BDK_ENGINE_SERVICES_SERVICE_CALL_H_
#define BDK_ENGINE_SERVICES_SERVICE_CALL_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "base/memory/raw_ptr.h"
#include "base/time/time.h"
#include "base/values.h"
#include "bdk/core/runtime/broker_internal.h"

class Profile;

namespace bdk {

// Quotes and escapes a string as a JSON string.
std::string JsonString(std::string_view text);
// A timestamp as the interface writes it: milliseconds since the Unix epoch.
std::string JsonTime(base::Time time);
// "null" for an empty string, else the quoted string.
std::string JsonStringOrNull(const std::string& text);

class ServiceCall {
 public:
  ServiceCall(bdkcore::Runtime& runtime,
              std::shared_ptr<bdkcore::BuiltinCall> call,
              Profile* profile);
  ~ServiceCall();

  const std::string& name() const { return call_->name; }
  bdkcore::Runtime& runtime() const { return *runtime_; }
  bdkcore::BuiltinCall& builtin() const { return *call_; }
  bdkcore::CommandControl& control() const { return *call_->control; }
  bool cancelled() const { return call_->control->cancelled(); }

  // The arguments, already checked against the command's types.
  const base::DictValue& args() const { return args_; }
  std::optional<std::string> String(const char* key) const;
  std::string StringOr(const char* key, const std::string& fallback) const;
  std::optional<double> Number(const char* key) const;
  std::optional<bool> Bool(const char* key) const;
  const base::ListValue* List(const char* key) const;
  const base::DictValue* Dict(const char* key) const;

  // The profile the call addresses; null for a command with no profile. It may
  // be a private (off-the-record) profile.
  Profile* profile() const { return profile_; }
  // The profile whose history and bookmarks a call reads and writes: for a
  // private profile its parent (services.md section 3).
  Profile* original_profile() const;
  bool is_private() const;

  void Resolve(const std::string& json) const;
  void ResolveNull() const { Resolve("null"); }
  void Reject(const char* domain,
              const char* code,
              const std::string& message) const;
  void InvalidArguments(const std::string& message) const;
  void Progress(double fraction, const std::string& message) const;

 private:
  std::shared_ptr<bdkcore::BuiltinCall> call_;
  raw_ptr<bdkcore::Runtime> runtime_;
  base::DictValue args_;
  raw_ptr<Profile> profile_;
};

// Wraps the call, finds its profile and parses its arguments. Null when the
// call has been rejected (a profile that is not open in Chromium).
std::shared_ptr<ServiceCall> MakeServiceCall(
    bdkcore::Runtime& runtime,
    std::shared_ptr<bdkcore::BuiltinCall> call);

// A command handler. It answers through the call, now or later, on the UI
// thread.
using CommandHandler = void (*)(const std::shared_ptr<ServiceCall>&);

}  // namespace bdk

#endif  // BDK_ENGINE_SERVICES_SERVICE_CALL_H_
