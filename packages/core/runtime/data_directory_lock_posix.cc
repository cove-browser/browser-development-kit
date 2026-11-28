/* lock_data_directory with flock(2) (macOS and Linux). See data_directory_lock.h. */
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>

#include "data_directory_lock.h"

namespace bdkcore {
namespace {

/* The lock files this process holds open, and locked, by path. They close as the process exits,
   which releases them. */
struct Held {
    std::mutex mutex;
    std::map<std::string, int> files;
};

Held& H() {
    static Held* h = new Held;
    return *h;
}

/* Whether `fd` is still the file at `path`: one removed or replaced since no longer guards it. */
bool same_file(int fd, const std::string& path) {
    struct stat open_st, path_st;
    return fstat(fd, &open_st) == 0 && stat(path.c_str(), &path_st) == 0 &&
           open_st.st_dev == path_st.st_dev && open_st.st_ino == path_st.st_ino;
}

} // namespace

DataDirectoryLock lock_data_directory(const std::string& data_dir, std::string* out_error) {
    const std::string path = data_dir + ".bdk-lock";
    Held& h = H();
    std::lock_guard<std::mutex> lock(h.mutex);
    auto it = h.files.find(path);
    if (it != h.files.end()) {
        if (same_file(it->second, path))
            return DataDirectoryLock::kHeld;
        close(it->second);
        h.files.erase(it);
    }
    const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        *out_error = "cannot open " + path + ": " + std::strerror(errno);
        return DataDirectoryLock::kFailed;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int error = errno;
        close(fd);
        if (error == EWOULDBLOCK)
            return DataDirectoryLock::kInUse;
        *out_error = "cannot lock " + path + ": " + std::strerror(error);
        return DataDirectoryLock::kFailed;
    }
    h.files[path] = fd;
    return DataDirectoryLock::kHeld;
}

} // namespace bdkcore
