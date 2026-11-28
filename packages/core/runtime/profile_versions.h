/* Profile snapshots and the downgrade policy (T-PKG-05; architecture.md section 13;
   configuration.md 3.3).

   Before the first launch of a newer engine milestone the core snapshots the data directory; when
   an older engine meets a data directory a newer one used last, it restores the newest snapshot it
   can use, or applies
   `[runtime] downgrade`: `refuse` fails start with `runtime.downgradeRefused`, `fresh-profile`
   moves the data directory aside, untouched, and starts with an empty one.

   The files are Chromium's (chrome/browser/downgrade at the pin): `Last Version` in the data
   directory holds the version that used it last, and `Snapshots/<version>/` holds a snapshot, valid
   when it has its own `Last Version`. So a snapshot this module takes is one Chromium's own restore
   would accept, and the version Chromium writes at each start is the one read here. Chromium's
   DowngradeManager takes snapshots only for managed devices; with them off it starts an older
   milestone on newer data ("unsupported"), which this module prevents by running first.

   Nothing is read or changed before this process holds the data directory's lock
   (data_directory_lock.h): another holder is runtime.dataDirectoryInUse. A restore is journalled
   and finished by the next start if it stops part-way.

   Platform neutral C++23 (std::filesystem with error codes, no exceptions). A backend supplies the
   data directory and the engine version (Backend::ProfileDataDirectory, Backend::EngineVersion);
   when either is empty nothing here runs. */
#ifndef BDK_PROFILE_VERSIONS_H_
#define BDK_PROFILE_VERSIONS_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "bdk/bdk.h"

namespace bdkcore {

/* A dotted version, compared component by component ("158.0.8127.37"). The first component is the
 * milestone. */
using EngineVersion = std::vector<uint32_t>;

std::optional<EngineVersion> parse_engine_version(const std::string& text);
std::string engine_version_string(const EngineVersion& v);
/* <0, 0 or >0; a missing trailing component counts as 0. */
int compare_engine_versions(const EngineVersion& a, const EngineVersion& b);

enum class DowngradePolicy : uint8_t { kRefuse, kFreshProfile };

struct ProfileVersionPolicy {
    DowngradePolicy downgrade = DowngradePolicy::kRefuse;
    bool snapshot_on_upgrade = true;
    /* Chromium's default retention (prefs::kUserDataSnapshotRetentionLimit). */
    size_t max_snapshots = 3;
};

/* `runtime.downgrade` and `runtime.snapshot_on_upgrade` from the resolved configuration; absent
   means the defaults. A value of the wrong type or an unknown policy is config.resolvedInvalid. */
bdk_status_t profile_policy_configure(const std::string& resolved, ProfileVersionPolicy* out,
                                      bdk_error_t* out_error);

enum class ProfileAction : uint8_t {
    kNone,     /* same version, a first start, an upgrade within a milestone or a minor downgrade */
    kSnapshot, /* an upgrade to a newer milestone: snapshot what `version` left */
    kRestore,  /* a downgrade across milestones: restore the snapshot of `version` */
    kRefuse,   /* a downgrade across milestones, no snapshot, policy refuse */
    kFreshProfile, /* the same, policy fresh-profile */
};

struct ProfileDecision {
    ProfileAction action = ProfileAction::kNone;
    EngineVersion version; /* kSnapshot: the last version; kRestore: the snapshot's */
};

/* The rule, with no file access: `last` is what `Last Version` holds (nullopt when absent or
   unreadable) and `snapshots` the valid snapshot versions. Mirrors
   DowngradeManager::PrepareUserDataDirectoryForCurrentVersion with snapshots on: a snapshot on a
   milestone upgrade only (BDK takes none within a milestone), and the newest snapshot no newer than
   `current` restored on a milestone downgrade. */
ProfileDecision decide_profile_action(const EngineVersion& current,
                                      const std::optional<EngineVersion>& last,
                                      const ProfileVersionPolicy& policy,
                                      const std::vector<EngineVersion>& snapshots);

/* Takes the data directory's lock, finishes a restore an earlier start left, applies the decision
   to `data_dir` and records `current` in `Last Version`. A failure leaves the data directory as it
   was where it can (a snapshot is written aside and renamed into place) and is
   runtime.downgradeRefused for kRefuse, runtime.profileMigrationFailed otherwise. */
bdk_status_t profile_versions_apply(const std::string& data_dir, const std::string& current,
                                    const ProfileVersionPolicy& policy, bdk_error_t* out_error);

} // namespace bdkcore

#endif /* BDK_PROFILE_VERSIONS_H_ */
