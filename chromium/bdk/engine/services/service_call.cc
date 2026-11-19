// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/services/service_call.h"

#include <utility>

#include "base/json/json_reader.h"
#include "base/strings/string_number_conversions.h"
#include "bdk/engine/engine_contents.h"
#include "chrome/browser/profiles/profile.h"

namespace bdk {

std::string JsonString(std::string_view text) {
  return "\"" + bdkcore::json_escape(std::string(text)) + "\"";
}

std::string JsonTime(base::Time time) {
  return base::NumberToString(time.InMillisecondsSinceUnixEpoch());
}

std::string JsonStringOrNull(const std::string& text) {
  return text.empty() ? "null" : JsonString(text);
}

ServiceCall::ServiceCall(bdkcore::Runtime& runtime,
                         std::shared_ptr<bdkcore::BuiltinCall> call,
                         Profile* profile)
    : call_(std::move(call)), runtime_(&runtime), profile_(profile) {
  args_ = base::JSONReader::ReadDict(call_->arguments, base::JSON_PARSE_RFC)
              .value_or(base::DictValue());
}

std::optional<std::string> ServiceCall::String(const char* key) const {
  const std::string* value = args_.FindString(key);
  return value ? std::optional<std::string>(*value) : std::nullopt;
}

std::string ServiceCall::StringOr(const char* key,
                                  const std::string& fallback) const {
  return String(key).value_or(fallback);
}

std::optional<double> ServiceCall::Number(const char* key) const {
  return args_.FindDouble(key);
}

std::optional<bool> ServiceCall::Bool(const char* key) const {
  return args_.FindBool(key);
}

const base::ListValue* ServiceCall::List(const char* key) const {
  return args_.FindList(key);
}

const base::DictValue* ServiceCall::Dict(const char* key) const {
  return args_.FindDict(key);
}

Profile* ServiceCall::original_profile() const {
  return profile_ ? profile_->GetOriginalProfile() : nullptr;
}

bool ServiceCall::is_private() const {
  return profile_ && profile_->IsOffTheRecord();
}

void ServiceCall::Resolve(const std::string& json) const {
  call_->control->Resolve(json);
}

void ServiceCall::Reject(const char* domain,
                         const char* code,
                         const std::string& message) const {
  call_->control->Reject(domain, code, message);
}

void ServiceCall::InvalidArguments(const std::string& message) const {
  Reject(BDK_DOMAIN_BROKER, BDK_CODE_INVALID_ARGUMENTS, message);
}

void ServiceCall::Progress(double fraction, const std::string& message) const {
  call_->control->Progress(fraction, message);
}

std::shared_ptr<ServiceCall> MakeServiceCall(
    bdkcore::Runtime& runtime,
    std::shared_ptr<bdkcore::BuiltinCall> call) {
  Profile* profile = nullptr;
  if (call->profile) {
    auto* data = static_cast<EngineProfile*>(call->profile->data.get());
    if (!data || !data->profile) {
      call->control->Reject(BDK_DOMAIN_PROFILE, "notOpen",
                            "the profile is not open");
      return nullptr;
    }
    profile = data->profile;
  }
  return std::make_shared<ServiceCall>(runtime, std::move(call), profile);
}

}  // namespace bdk
