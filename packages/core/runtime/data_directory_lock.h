/* Exclusive ownership of a data directory across processes (T-PKG-05). Two copies of an application
   may name the same data directory; the snapshot and downgrade policy (profile_versions.h) changes
   that directory before Chromium starts and before Chromium's own process singleton exists, so the
   core takes this lock first, and keeps it for the rest of the process, through Chromium's start.

   The lock is a file beside the data directory, `<data dir>.bdk-lock`, so the fresh-profile policy,
   which renames the directory, does not move it. A platform file of the runtime core:
   data_directory_lock_posix.cc implements it with flock(2) for macOS and Linux; a Windows
   implementation (LockFileEx) replaces that one file. */
#ifndef BDK_DATA_DIRECTORY_LOCK_H_
#define BDK_DATA_DIRECTORY_LOCK_H_

#include <string>

namespace bdkcore {

enum class DataDirectoryLock {
    kHeld,  /* this process holds it, from now or from an earlier start */
    kInUse, /* another process holds it */
    kFailed /* the lock file cannot be opened or locked; out_error says why */
};

/* Takes the lock for `data_dir` (UTF-8, an absolute path with no trailing separator) without
   waiting. Safe to call again for the same directory: a lock this process holds is kept. */
DataDirectoryLock lock_data_directory(const std::string& data_dir, std::string* out_error);

} // namespace bdkcore

#endif /* BDK_DATA_DIRECTORY_LOCK_H_ */
