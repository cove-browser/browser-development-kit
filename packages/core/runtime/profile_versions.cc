/* Profile snapshots and the downgrade policy. See profile_versions.h. */
#include "profile_versions.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#include "broker_internal.h"
#include "core_internal.h"

namespace bdkcore {
namespace {

namespace fs = std::filesystem;

/* Chromium's names (chrome/browser/downgrade/user_data_downgrade.cc). */
constexpr const char* kLastVersionFile = "Last Version";
constexpr const char* kSnapshotsDir = "Snapshots";

/* Top-level entries a snapshot leaves out, as Chromium's MoveUserData keeps them in place: the
   process singleton, crash and metrics state, and the snapshots themselves. */
constexpr const char* kNotSnapshotted[] = {
    kSnapshotsDir,     kLastVersionFile, "lockfile", "SingletonLock",  "SingletonSocket",
    "SingletonCookie", "Crashpad",       "crashpad", "BrowserMetrics", "First Run",
};

/* Directories at any depth a snapshot leaves out: caches Chromium rebuilds, which are most of a
 * profile's size. */
constexpr const char* kCaches[] = {
    "Cache",       "Code Cache",        "GPUCache",        "GrShaderCache",
    "ShaderCache", "DawnGraphiteCache", "DawnWebGPUCache", "component_crx_cache",
};

/* Where the newer data goes while a snapshot is restored, before it is removed. */
constexpr const char* kDiscardDir = ".bdk-discard";

template <size_t N> bool listed(const char* const (&names)[N], const std::string& name) {
    return std::any_of(std::begin(names), std::end(names),
                       [&](const char* n) { return name == n; });
}

/* Paths are UTF-8 strings at the ABI (conventions.md); char8_t keeps them UTF-8 on every platform.
 */
fs::path u8path(const std::string& s) {
    return fs::path(std::u8string(s.begin(), s.end()));
}

std::string name_of(const fs::path& p) {
    const std::u8string u = p.filename().u8string();
    return std::string(u.begin(), u.end());
}

std::string display(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::optional<EngineVersion> read_version_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parse_engine_version(text);
}

/* Written beside the target and renamed over it, so a crash leaves the old value or the new one. */
bool write_version_file(const fs::path& file, const std::string& version) {
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out << version;
        if (!out)
            return false;
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);
    return !ec;
}

/* Valid snapshots: `Snapshots/<version>/` with its own `Last Version` (Chromium's
 * GetAvailableSnapshots). */
std::vector<EngineVersion> list_snapshots(const fs::path& dir) {
    std::vector<EngineVersion> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir / kSnapshotsDir, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code type_ec;
        if (!it->is_directory(type_ec))
            continue;
        std::optional<EngineVersion> v = parse_engine_version(name_of(it->path()));
        if (v && fs::exists(it->path() / kLastVersionFile, type_ec))
            out.push_back(*v);
    }
    return out;
}

/* Copies a tree, keeping links as links and leaving caches out. */
bool copy_tree(const fs::path& from, const fs::path& to, std::error_code& ec) {
    fs::create_directory(to, ec);
    if (ec)
        return false;
    for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& src = it->path();
        const fs::path dst = to / src.filename();
        const fs::file_status st = it->symlink_status(ec);
        if (ec)
            return false;
        if (fs::is_symlink(st)) {
            fs::copy_symlink(src, dst, ec);
        } else if (fs::is_directory(st)) {
            if (listed(kCaches, name_of(src)))
                continue;
            copy_tree(src, dst, ec);
        } else if (fs::is_regular_file(st)) {
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        }
        if (ec)
            return false;
    }
    return !ec;
}

bdk_status_t migration_failed(bdk_error_t* out_error, const std::string& what,
                              const std::error_code& ec) {
    return fail(out_error, BDK_DOMAIN_RUNTIME, "profileMigrationFailed",
                what + (ec ? ": " + ec.message() : std::string()));
}

/* Snapshots/<last>/ from the data directory, written as <last>.tmp and renamed, then the oldest
   beyond the retention limit removed (Chromium's SnapshotManager::TakeSnapshot and
   PurgeInvalidAndOldSnapshots). */
bdk_status_t take_snapshot(const fs::path& dir, const EngineVersion& last, size_t keep,
                           bdk_error_t* out_error) {
    const std::string name = engine_version_string(last);
    const fs::path snapshots = dir / kSnapshotsDir;
    const fs::path tmp = snapshots / (name + ".tmp");
    const fs::path target = snapshots / name;
    std::error_code ec;
    fs::create_directories(snapshots, ec);
    fs::remove_all(tmp, ec);
    if (ec || !fs::create_directory(tmp, ec) || ec)
        return migration_failed(out_error, "cannot create " + display(tmp), ec);
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string entry = name_of(it->path());
        if (listed(kNotSnapshotted, entry) || entry.starts_with(".bdk-") ||
            entry.starts_with(std::string(kLastVersionFile) + "."))
            continue;
        const fs::file_status st = it->symlink_status(ec);
        if (ec)
            break;
        if (fs::is_symlink(st))
            fs::copy_symlink(it->path(), tmp / entry, ec);
        else if (fs::is_directory(st))
            copy_tree(it->path(), tmp / entry, ec);
        else if (fs::is_regular_file(st))
            fs::copy_file(it->path(), tmp / entry, ec);
        if (ec)
            break;
    }
    if (ec || !write_version_file(tmp / kLastVersionFile, name)) {
        std::error_code ignored;
        fs::remove_all(tmp, ignored);
        return migration_failed(out_error, "cannot snapshot " + display(dir), ec);
    }
    fs::remove_all(target, ec);
    fs::rename(tmp, target, ec);
    if (ec)
        return migration_failed(out_error, "cannot rename the snapshot to " + display(target), ec);

    std::vector<EngineVersion> all = list_snapshots(dir);
    std::sort(all.begin(), all.end(), [](const EngineVersion& a, const EngineVersion& b) {
        return compare_engine_versions(a, b) > 0;
    });
    for (size_t i = keep; i < all.size(); ++i)
        fs::remove_all(snapshots / engine_version_string(all[i]), ec);
    return BDK_OK;
}

/* Moves the newer data aside, moves the snapshot's entries in, and removes both the snapshot and
   the newer data (Chromium's ProcessDowngrade followed by RestoreSnapshot). */
bdk_status_t restore_snapshot(const fs::path& dir, const EngineVersion& version,
                              bdk_error_t* out_error) {
    const fs::path snapshot = dir / kSnapshotsDir / engine_version_string(version);
    const fs::path discard = dir / kDiscardDir;
    std::error_code ec;
    fs::remove_all(discard, ec);
    fs::create_directory(discard, ec);
    if (ec)
        return migration_failed(out_error, "cannot create " + display(discard), ec);
    std::vector<fs::path> newer;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string entry = name_of(it->path());
        if (entry != kSnapshotsDir && entry != kDiscardDir)
            newer.push_back(it->path());
    }
    for (const fs::path& p : newer) {
        if (!ec)
            fs::rename(p, discard / p.filename(), ec);
    }
    std::vector<fs::path> restored;
    for (fs::directory_iterator it(snapshot, ec), end; !ec && it != end; it.increment(ec))
        restored.push_back(it->path());
    for (const fs::path& p : restored) {
        if (!ec)
            fs::rename(p, dir / p.filename(), ec);
    }
    if (ec)
        return migration_failed(out_error, "cannot restore the snapshot " + display(snapshot), ec);
    fs::remove_all(snapshot, ec);
    fs::remove_all(discard, ec);
    return BDK_OK;
}

/* Renames the data directory to "<name>.newer-<last>" beside it, untouched, and makes an empty one.
 */
bdk_status_t start_fresh(const fs::path& dir, const EngineVersion& last, bdk_error_t* out_error) {
    const fs::path parent = dir.parent_path();
    const std::string base = name_of(dir) + ".newer-" + engine_version_string(last);
    fs::path aside = parent / u8path(base);
    std::error_code ec;
    for (int n = 2; fs::exists(aside, ec); ++n)
        aside = parent / u8path(base + "-" + std::to_string(n));
    fs::rename(dir, aside, ec);
    if (!ec)
        fs::create_directory(dir, ec);
    if (ec)
        return migration_failed(out_error, "cannot move " + display(dir) + " aside", ec);
    return BDK_OK;
}

} // namespace

std::optional<EngineVersion> parse_engine_version(const std::string& text) {
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return std::nullopt;
    const size_t end = text.find_last_not_of(" \t\r\n");
    const std::string s = text.substr(begin, end - begin + 1);
    EngineVersion v;
    size_t start = 0;
    for (;;) {
        const size_t dot = s.find('.', start);
        const std::string part = s.substr(start, dot == std::string::npos ? dot : dot - start);
        if (part.empty() || part.size() > 9 ||
            !std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return std::nullopt;
        v.push_back(static_cast<uint32_t>(std::stoul(part)));
        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
    return v;
}

std::string engine_version_string(const EngineVersion& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0)
            out += '.';
        out += std::to_string(v[i]);
    }
    return out;
}

int compare_engine_versions(const EngineVersion& a, const EngineVersion& b) {
    for (size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
        const uint32_t x = i < a.size() ? a[i] : 0;
        const uint32_t y = i < b.size() ? b[i] : 0;
        if (x != y)
            return x < y ? -1 : 1;
    }
    return 0;
}

bdk_status_t profile_policy_configure(const std::string& resolved, ProfileVersionPolicy* out,
                                      bdk_error_t* out_error) {
    *out = ProfileVersionPolicy{};
    JsonValue root;
    if (!json_parse(resolved, &root))
        return BDK_OK; /* bdk_runtime_start has already refused text that is not JSON */
    const JsonValue* runtime = root.find("runtime");
    if (!runtime)
        return BDK_OK;
    if (runtime->type != JsonValue::Type::kObject)
        return fail(out_error, BDK_DOMAIN_CONFIG, BDK_CODE_RESOLVED_INVALID,
                    "runtime must be an object");
    if (const JsonValue* d = runtime->find("downgrade")) {
        if (d->type == JsonValue::Type::kString && d->text == "refuse")
            out->downgrade = DowngradePolicy::kRefuse;
        else if (d->type == JsonValue::Type::kString && d->text == "fresh-profile")
            out->downgrade = DowngradePolicy::kFreshProfile;
        else
            return fail(out_error, BDK_DOMAIN_CONFIG, BDK_CODE_RESOLVED_INVALID,
                        "runtime.downgrade must be \"refuse\" or \"fresh-profile\"");
    }
    if (const JsonValue* s = runtime->find("snapshot_on_upgrade")) {
        if (s->type != JsonValue::Type::kBool)
            return fail(out_error, BDK_DOMAIN_CONFIG, BDK_CODE_RESOLVED_INVALID,
                        "runtime.snapshot_on_upgrade must be a boolean");
        out->snapshot_on_upgrade = s->boolean;
    }
    return BDK_OK;
}

ProfileDecision decide_profile_action(const EngineVersion& current,
                                      const std::optional<EngineVersion>& last,
                                      const ProfileVersionPolicy& policy,
                                      const std::vector<EngineVersion>& snapshots) {
    if (!last || current.empty() || last->empty())
        return {};
    const int order = compare_engine_versions(current, *last);
    if (order == 0)
        return {};
    const bool new_milestone = current[0] != (*last)[0];
    if (order > 0) {
        if (new_milestone && policy.snapshot_on_upgrade)
            return {ProfileAction::kSnapshot, *last};
        return {};
    }
    if (!new_milestone)
        return {}; /* a minor downgrade: Chromium supports it within a milestone */
    const EngineVersion* best = nullptr;
    for (const EngineVersion& s : snapshots) {
        if (compare_engine_versions(s, current) <= 0 &&
            (!best || compare_engine_versions(s, *best) > 0))
            best = &s;
    }
    if (best)
        return {ProfileAction::kRestore, *best};
    return {policy.downgrade == DowngradePolicy::kRefuse ? ProfileAction::kRefuse
                                                         : ProfileAction::kFreshProfile,
            *last};
}

bdk_status_t profile_versions_apply(const std::string& data_dir, const std::string& current,
                                    const ProfileVersionPolicy& policy, bdk_error_t* out_error) {
    const std::optional<EngineVersion> now = parse_engine_version(current);
    if (!now)
        return fail(out_error, BDK_DOMAIN_RUNTIME, "profileMigrationFailed",
                    "the engine reports no version (\"" + current + "\")");
    fs::path dir = u8path(data_dir).lexically_normal();
    if (dir.filename().empty())
        dir = dir.parent_path(); /* a trailing separator */
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec)
        return fail(out_error, BDK_DOMAIN_RUNTIME, "noDataDirectory",
                    "cannot create " + data_dir + ": " + ec.message());
    const std::optional<EngineVersion> last = read_version_file(dir / kLastVersionFile);
    const ProfileDecision d = decide_profile_action(*now, last, policy, list_snapshots(dir));
    bdk_status_t s = BDK_OK;
    switch (d.action) {
    case ProfileAction::kNone:
        break;
    case ProfileAction::kSnapshot:
        s = take_snapshot(dir, d.version, policy.max_snapshots, out_error);
        break;
    case ProfileAction::kRestore:
        s = restore_snapshot(dir, d.version, out_error);
        break;
    case ProfileAction::kFreshProfile:
        s = start_fresh(dir, d.version, out_error);
        break;
    case ProfileAction::kRefuse:
        return fail(out_error, BDK_DOMAIN_RUNTIME, "downgradeRefused",
                    "the data directory " + data_dir + " was last used by engine " +
                        engine_version_string(d.version) +
                        ", a newer milestone than this runtime's " + engine_version_string(*now) +
                        ", and it has no snapshot to restore; [runtime] downgrade is \"refuse\"");
    }
    if (s != BDK_OK)
        return s;
    if (!write_version_file(dir / kLastVersionFile, engine_version_string(*now)))
        return fail(out_error, BDK_DOMAIN_RUNTIME, "profileMigrationFailed",
                    "cannot write " + display(dir / kLastVersionFile));
    return BDK_OK;
}

} // namespace bdkcore
