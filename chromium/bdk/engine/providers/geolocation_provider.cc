// Copyright 2026 The BDK Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bdk/engine/providers/geolocation_provider.h"

#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/values.h"
#include "bdk/core/runtime/providers_internal.h"
#include "bdk/engine/json_util.h"
#include "bdk/engine/providers/geolocation_platform.h"
#include "bdk/engine/providers/provider_settings.h"
#include "build/build_config.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "services/device/public/cpp/geolocation/location_provider.h"
#include "services/device/public/mojom/geoposition.mojom.h"

#if BUILDFLAG(IS_APPLE)
#include "services/device/public/cpp/geolocation/geolocation_system_permission_manager.h"
#include "services/device/public/cpp/geolocation/location_system_permission_status.h"
#include "services/device/public/cpp/geolocation/system_geolocation_source.h"
#endif

namespace bdk {

namespace {

// A position or error as the contract reports it (providers_internal.h,
// checked by the core), as Chromium's result.
device::mojom::GeopositionResultPtr ResultOf(const std::string& json) {
  std::optional<base::DictValue> report = ParseJsonDict(json);
  if (!report) {
    return nullptr;
  }
  if (const std::string* error = report->FindString("error")) {
    const std::string* message = report->FindString("message");
    return device::mojom::GeopositionResult::NewError(
        device::mojom::GeopositionError::New(
            *error == "permissionDenied"
                ? device::mojom::GeopositionErrorCode::kPermissionDenied
                : device::mojom::GeopositionErrorCode::kPositionUnavailable,
            message ? *message : std::string(), std::string()));
  }
  auto position = device::mojom::Geoposition::New();
  position->latitude = report->FindDouble("latitude").value_or(0);
  position->longitude = report->FindDouble("longitude").value_or(0);
  position->accuracy = report->FindDouble("accuracy").value_or(0);
  position->altitude =
      report->FindDouble("altitude").value_or(device::mojom::kBadAltitude);
  position->altitude_accuracy = report->FindDouble("altitudeAccuracy")
                                    .value_or(device::mojom::kBadAccuracy);
  position->heading =
      report->FindDouble("heading").value_or(device::mojom::kBadHeading);
  position->speed =
      report->FindDouble("speed").value_or(device::mojom::kBadSpeed);
  std::optional<double> timestamp = report->FindDouble("timestamp");
  position->timestamp = timestamp ? base::Time::FromMillisecondsSinceUnixEpoch(
                                        static_cast<int64_t>(*timestamp))
                                  : base::Time::Now();
  return device::mojom::GeopositionResult::NewPosition(std::move(position));
}

// The watch on the UI thread. Created and destroyed through tasks posted
// there, so the core is only touched on its own thread.
class Watch : public base::RefCountedThreadSafe<Watch> {
 public:
  using Report = base::RepeatingCallback<void(const std::string&)>;

  // Runs on the UI thread. `report` posts back to the provider's sequence.
  void Start(bool high_accuracy, Report report) {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
    std::shared_ptr<bdkcore::Runtime> rt = bdkcore::current_runtime();
    if (!rt || stopped_) {
      return;
    }
    bdkcore::ProviderHandlers handlers;
    handlers.report = [report](const std::string& json) { report.Run(json); };
    handlers.done = [report](const bdkcore::ProviderReply& reply) {
      if (!reply.ok() && reply.code != "cancelled") {
        report.Run(R"({"error":"positionUnavailable","message":")" +
                   bdkcore::json_escape(reply.message) + "\"}");
      }
    };
    request_ =
        bdkcore::provider_invoke(rt, bdkcore::Service::kGeolocation, "start",
                                 high_accuracy ? R"({"highAccuracy":true})"
                                               : R"({"highAccuracy":false})",
                                 std::move(handlers));
  }

  void Stop() {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
    stopped_ = true;
    if (request_) {
      request_->Cancel();
    }
    request_.reset();
  }

 private:
  friend class base::RefCountedThreadSafe<Watch>;
  ~Watch() = default;

  std::shared_ptr<bdkcore::ProviderRequest> request_;
  bool stopped_ = false;
};

class ApplicationLocationProvider : public device::LocationProvider {
 public:
  ApplicationLocationProvider() = default;
  ~ApplicationLocationProvider() override { StopProvider(); }

  // device::LocationProvider:
  void FillDiagnostics(device::mojom::GeolocationDiagnostics&) override {}

  void SetUpdateCallback(
      const LocationProviderUpdateCallback& callback) override {
    callback_ = callback;
  }

  void StartProvider(bool high_accuracy) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    started_ = true;
    high_accuracy_ = high_accuracy;
    MaybeWatch();
  }

  void StopProvider() override {
    started_ = false;
    if (watch_) {
      content::GetUIThreadTaskRunner({})->PostTask(
          FROM_HERE, base::BindOnce(&Watch::Stop, std::move(watch_)));
    }
  }

  const device::mojom::GeopositionResult* GetPosition() override {
    return last_.get();
  }

  // No position reaches a page before its permission is granted
  // (location_provider.h).
  void OnPermissionGranted() override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    permission_ = true;
    MaybeWatch();
  }

 private:
  void MaybeWatch() {
    if (!started_ || !permission_ || watch_) {
      return;
    }
    watch_ = base::MakeRefCounted<Watch>();
    Watch::Report report = base::BindPostTask(
        base::SequencedTaskRunner::GetCurrentDefault(),
        base::BindRepeating(&ApplicationLocationProvider::OnReport,
                            weak_factory_.GetWeakPtr()));
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE,
        base::BindOnce(&Watch::Start, watch_, high_accuracy_, report));
  }

  void OnReport(const std::string& json) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    device::mojom::GeopositionResultPtr result = ResultOf(json);
    if (!result) {
      return;
    }
    last_ = result.Clone();
    if (callback_) {
      callback_.Run(this, std::move(result));
    }
  }

  SEQUENCE_CHECKER(sequence_checker_);
  LocationProviderUpdateCallback callback_;
  scoped_refptr<Watch> watch_;
  device::mojom::GeopositionResultPtr last_;
  bool started_ = false;
  bool permission_ = false;
  bool high_accuracy_ = false;
  base::WeakPtrFactory<ApplicationLocationProvider> weak_factory_{this};
};

#if BUILDFLAG(IS_APPLE)
// Chromium's view of the operating system's location permission. Chromium
// checks it before any provider starts (the device service, and Chrome's
// permission contexts through system_permission_settings), so a page of an
// application without the macOS location permission never reaches the
// application's provider. With the application's provider it is granted: it
// guards the operating system's provider, which is not used then, and the
// application asks for its own if its provider needs one (a page still needs
// the page's permission). Otherwise it is the operating system's. Everything
// else is the operating system's source, which still serves "system".
class BdkGeolocationSource : public device::SystemGeolocationSource {
 public:
  explicit BdkGeolocationSource(
      std::unique_ptr<device::SystemGeolocationSource> os)
      : os_(std::move(os)) {}

  // Tells the permission's observers when the runtime that started has
  // another provider.
  void ProviderChanged() { Report(); }

  void RegisterPermissionUpdateCallback(
      PermissionUpdateCallback callback) override {
    callback_ = std::move(callback);
    os_->RegisterPermissionUpdateCallback(base::BindRepeating(
        &BdkGeolocationSource::OnOsPermission, weak_factory_.GetWeakPtr()));
    Report();
  }
  void OpenSystemPermissionSetting() override {
    os_->OpenSystemPermissionSetting();
  }
  void AddPositionUpdateObserver(PositionObserver* observer) override {
    os_->AddPositionUpdateObserver(observer);
  }
  void RemovePositionUpdateObserver(PositionObserver* observer) override {
    os_->RemovePositionUpdateObserver(observer);
  }
  void StartWatchingPosition(bool high_accuracy) override {
    os_->StartWatchingPosition(high_accuracy);
  }
  void StopWatchingPosition() override { os_->StopWatchingPosition(); }
  void RequestPermission() override {
    if (CurrentProviderSettings().geolocation != "app") {
      os_->RequestPermission();
    }
  }

 private:
  void OnOsPermission(device::LocationSystemPermissionStatus status) {
    os_status_ = status;
    Report();
  }

  void Report() {
    if (callback_) {
      callback_.Run(CurrentProviderSettings().geolocation == "app"
                        ? device::LocationSystemPermissionStatus::kAllowed
                        : os_status_);
    }
  }

  std::unique_ptr<device::SystemGeolocationSource> os_;
  PermissionUpdateCallback callback_;
  device::LocationSystemPermissionStatus os_status_ =
      device::LocationSystemPermissionStatus::kNotDetermined;
  base::WeakPtrFactory<BdkGeolocationSource> weak_factory_{this};
};

// The source installed, which the manager owns until Chromium shuts down.
BdkGeolocationSource* g_source = nullptr;
#endif  // BUILDFLAG(IS_APPLE)

}  // namespace

void InstallGeolocationPermission() {
#if BUILDFLAG(IS_APPLE)
  // Chromium installs the operating system's manager as it starts
  // (BrowserProcessPlatformPart::PreMainMessageLoopRun) unless one is
  // installed already, and its permission contexts and the device service
  // read the one installed.
  if (device::GeolocationSystemPermissionManager::GetInstance()) {
    return;
  }
  auto source = std::make_unique<BdkGeolocationSource>(
      NewOperatingSystemGeolocationSource());
  g_source = source.get();
  device::GeolocationSystemPermissionManager::SetInstance(
      std::make_unique<device::GeolocationSystemPermissionManager>(
          std::move(source)));
#endif
}

void GeolocationProviderChanged() {
#if BUILDFLAG(IS_APPLE)
  if (g_source && device::GeolocationSystemPermissionManager::GetInstance()) {
    g_source->ProviderChanged();
  }
#endif
}

std::unique_ptr<device::LocationProvider> CreateApplicationLocationProvider() {
  if (CurrentProviderSettings().geolocation != "app") {
    return nullptr;
  }
  return std::make_unique<ApplicationLocationProvider>();
}

}  // namespace bdk
