/* LocalServer over a Unix-domain stream socket (macOS and Linux). One transport thread polls the
   listening socket, every connection and a wake pipe; the UI thread only queues frames and asks for
   disconnects. Frames are a 4-byte big-endian length and that many bytes (automation_internal.h,
   kMaxFrameBytes). */
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "automation_internal.h"
#include "core_internal.h"
#include "local_socket.h"

namespace bdkcore {
namespace {

constexpr const char* kSocketName = "automation.sock";

/* The identity of the process at the other end. */
bool peer_is_user(int fd) {
#if defined(__linux__)
    struct ucred cred;
    socklen_t len = sizeof cred;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
        return false;
    return cred.uid == geteuid();
#else
    uid_t uid = 0;
    gid_t gid = 0;
    if (getpeereid(fd, &uid, &gid) != 0)
        return false;
    return uid == geteuid();
#endif
}

void set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL,
          static_cast<int>(static_cast<unsigned>(flags) | static_cast<unsigned>(O_NONBLOCK)));
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#if defined(SO_NOSIGPIPE)
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

ssize_t send_some(int fd, const char* data, size_t length) {
#if defined(MSG_NOSIGNAL)
    return send(fd, data, length, MSG_NOSIGNAL);
#else
    return send(fd, data, length, 0);
#endif
}

/* A directory only the user may use: it exists, is theirs and nobody else may write to it. */
bool private_directory(const std::string& path, std::string* why) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        *why = "'" + path + "' is not a directory";
        return false;
    }
    if (st.st_uid != geteuid()) {
        *why = "'" + path + "' belongs to another user";
        return false;
    }
    if ((static_cast<unsigned>(st.st_mode) & static_cast<unsigned>(S_IWGRP | S_IWOTH)) != 0) {
        *why = "'" + path + "' can be written by other users";
        return false;
    }
    return true;
}

/* One frame waiting its turn, length prefix included. */
struct Queued {
    std::string bytes;
    std::string key; /* what a newer frame replaces it under; empty: never */
};

struct Client {
    int fd = -1;
    std::string in;  /* bytes read and not yet framed */
    std::string out; /* bytes the transport thread took and has not finished writing */
    std::map<uint64_t, Queued> queue;                /* frames not taken yet, in order */
    std::unordered_map<std::string, uint64_t> keyed; /* a key's newest frame in queue */
    size_t queued = 0;                               /* bytes in queue */
    uint64_t next = 0;
    bool closing = false;

    size_t Backlog() const {
        return out.size() + queued;
    }
    bool HasOutput() const {
        return !out.empty() || !queue.empty();
    }
    /* Appends frames from the front of the queue to bytes until it holds limit bytes. */
    void Take(std::string* bytes, size_t limit) {
        while (!queue.empty() && bytes->size() < limit) {
            auto front = queue.begin();
            auto k = keyed.find(front->second.key);
            if (k != keyed.end() && k->second == front->first)
                keyed.erase(k);
            queued -= front->second.bytes.size();
            *bytes += front->second.bytes;
            queue.erase(front);
        }
    }
};

/* Whether a poll() event word has a flag (POSIX declares both as signed). */
bool has(short events, short flag) {
    return (static_cast<unsigned>(events) & static_cast<unsigned>(flag)) != 0;
}

class PosixServer final : public LocalServer {
  public:
    PosixServer(int listen_fd, int wake_read, int wake_write, std::string dir, std::string path,
                Events events)
        : listen_fd_(listen_fd), wake_read_(wake_read), wake_write_(wake_write),
          dir_(std::move(dir)), path_(std::move(path)), events_(std::move(events)),
          alive_(std::make_shared<int>(0)) {
        thread_ = std::thread([this] { Run(); });
    }

    ~PosixServer() override {
        stop_ = true;
        Wake();
        thread_.join();
        for (auto& kv : clients_)
            close(kv.second.fd);
        close(listen_fd_);
        close(wake_read_);
        close(wake_write_);
        unlink(path_.c_str());
        rmdir(dir_.c_str());
        alive_.reset(); /* events already posted find nothing to call */
    }

    bool Send(uint64_t id, const std::string& frame, const std::string& key) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = clients_.find(id);
        if (it == clients_.end() || it->second.closing)
            return false;
        Client& c = it->second;
        /* A newer state replaces the older one that still waits, and goes behind everything sent
           since, so that no frame is delivered before one that was sent earlier. */
        if (!key.empty() && c.Backlog() > soft_) {
            auto k = c.keyed.find(key);
            if (k != c.keyed.end()) {
                auto old = c.queue.find(k->second);
                c.queued -= old->second.bytes.size();
                c.queue.erase(old);
                c.keyed.erase(k);
            }
        }
        if (c.Backlog() + frame.size() + 4 > hard_)
            return false;
        const auto n = static_cast<uint32_t>(frame.size());
        const char prefix[4] = {static_cast<char>((n >> 24U) & 0xffU),
                                static_cast<char>((n >> 16U) & 0xffU),
                                static_cast<char>((n >> 8U) & 0xffU), static_cast<char>(n & 0xffU)};
        Queued q;
        q.bytes.append(prefix, 4);
        q.bytes += frame;
        q.key = key;
        c.queued += q.bytes.size();
        if (!key.empty())
            c.keyed[key] = c.next;
        c.queue.emplace(c.next++, std::move(q));
        Wake();
        return true;
    }

    void SetLimits(size_t soft_bytes, size_t hard_bytes) override {
        std::lock_guard<std::mutex> lock(mutex_);
        soft_ = soft_bytes ? soft_bytes : kSoftQueuedBytes;
        hard_ = hard_bytes ? hard_bytes : kMaxQueuedBytes;
    }

    void Disconnect(uint64_t id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = clients_.find(id);
        if (it != clients_.end())
            it->second.closing = true;
        Wake();
    }

  private:
    struct Event {
        enum class Kind : uint8_t { kConnect, kFrame, kDisconnect };
        Kind kind = Kind::kConnect;
        uint64_t id = 0;
        std::string frame;
    };

    void Wake() const {
        char b = 1;
        (void)!write(wake_write_, &b, 1);
    }

    /* Posts an event to the UI thread unless the server is gone by then. */
    void Deliver(Event event) {
        std::weak_ptr<int> alive = alive_;
        const Events* events = &events_;
        post([alive, events, event = std::move(event)] {
            if (!alive.lock())
                return;
            switch (event.kind) {
            case Event::Kind::kConnect:
                if (events->on_connect)
                    events->on_connect(event.id);
                break;
            case Event::Kind::kFrame:
                if (events->on_frame)
                    events->on_frame(event.id, event.frame);
                break;
            case Event::Kind::kDisconnect:
                if (events->on_disconnect)
                    events->on_disconnect(event.id);
                break;
            }
        });
    }

    void Run() {
        while (!stop_) {
            std::vector<pollfd> fds;
            std::vector<uint64_t> ids;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                fds.push_back({wake_read_, POLLIN, 0});
                fds.push_back({listen_fd_, POLLIN, 0});
                for (auto& kv : clients_) {
                    fds.push_back(
                        {kv.second.fd, kv.second.HasOutput() ? kReadWrite : short{POLLIN}, 0});
                    ids.push_back(kv.first);
                }
            }
            if (poll(fds.data(), fds.size(), -1) < 0) {
                if (errno == EINTR)
                    continue;
                return;
            }
            if (has(fds[0].revents, POLLIN)) {
                char buf[64];
                while (read(wake_read_, buf, sizeof buf) > 0) {
                }
            }
            if (has(fds[1].revents, POLLIN))
                Accept();
            for (size_t i = 0; i < ids.size(); ++i)
                Service(ids[i], fds[i + 2].revents);
            ReapClosing();
        }
    }

    void Accept() {
        for (;;) {
            int fd = accept(listen_fd_, nullptr, nullptr);
            if (fd < 0)
                return;
            if (!peer_is_user(fd)) {
                close(fd);
                continue;
            }
            uint64_t id = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (clients_.size() >= kMaxConnections) {
                    id = 0;
                } else {
                    id = next_id_++;
                    clients_[id].fd = fd;
                }
            }
            if (id == 0) {
                close(fd);
                continue;
            }
            set_nonblocking(fd);
            Deliver(Event{Event::Kind::kConnect, id, {}});
        }
    }

    /* Writes what it can of bytes; returns what is left. */
    static std::string Flush(int fd, std::string bytes) {
        size_t sent = 0;
        while (sent < bytes.size()) {
            ssize_t n = send_some(fd, bytes.data() + sent, bytes.size() - sent);
            if (n <= 0)
                break;
            sent += static_cast<size_t>(n);
        }
        return bytes.substr(sent);
    }

    /* Only this thread closes a client's descriptor, so it is used outside the lock; the lock
       guards the queues the UI thread fills. */
    void Service(uint64_t id, short revents) {
        int fd = -1;
        std::string pending;
        const bool writing = has(revents, POLLOUT);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = clients_.find(id);
            if (it == clients_.end())
                return;
            fd = it->second.fd;
            if (writing) {
                pending.swap(it->second.out);
                it->second.Take(&pending, kTakeBytes); /* the rest stays open to coalescing */
            }
        }
        std::string left = pending.empty() ? std::string() : Flush(fd, std::move(pending));
        std::string incoming;
        bool drop = false;
        if (has(revents, POLLIN) || has(revents, POLLHUP) || has(revents, POLLERR)) {
            char buf[65536];
            for (;;) {
                ssize_t n = recv(fd, buf, sizeof buf, 0);
                if (n > 0) {
                    incoming.append(buf, static_cast<size_t>(n));
                    continue;
                }
                if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                    drop = true;
                break;
            }
        }
        std::vector<std::string> frames;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = clients_.find(id);
            if (it == clients_.end())
                return;
            Client& c = it->second;
            if (writing)
                c.out = std::move(left);
            c.in += incoming;
            while (c.in.size() >= 4) {
                const auto* b = reinterpret_cast<const unsigned char*>(c.in.data());
                const size_t len = (size_t{b[0]} << 24U) | (size_t{b[1]} << 16U) |
                                   (size_t{b[2]} << 8U) | size_t{b[3]};
                if (len > kMaxFrameBytes) {
                    drop = true;
                    break;
                }
                if (c.in.size() < len + 4)
                    break;
                frames.push_back(c.in.substr(4, len));
                c.in.erase(0, len + 4);
            }
            if (drop)
                clients_.erase(it);
        }
        for (auto& f : frames)
            Deliver(Event{Event::Kind::kFrame, id, std::move(f)});
        if (drop) {
            close(fd);
            Deliver(Event{Event::Kind::kDisconnect, id, {}});
        }
    }

    /* A connection asked to close goes once its queue is written (or cannot be). */
    void ReapClosing() {
        std::vector<std::pair<uint64_t, Client>> gone;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = clients_.begin(); it != clients_.end();) {
                if (it->second.closing) {
                    gone.emplace_back(it->first, std::move(it->second));
                    it = clients_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& [id, client] : gone) {
            client.Take(&client.out, std::string::npos);
            (void)Flush(client.fd, std::move(client.out));
            shutdown(client.fd, SHUT_RDWR);
            close(client.fd);
            Deliver(Event{Event::Kind::kDisconnect, id, {}});
        }
    }

    /* How much the transport takes from a connection's queue to write at once. */
    static constexpr size_t kTakeBytes = size_t{256} * 1024;
    static constexpr short kReadWrite =
        static_cast<short>(static_cast<unsigned>(POLLIN) | static_cast<unsigned>(POLLOUT));

    int listen_fd_, wake_read_, wake_write_;
    std::string dir_, path_;
    Events events_;
    std::shared_ptr<int> alive_;
    std::mutex mutex_;
    std::map<uint64_t, Client> clients_;
    uint64_t next_id_ = 1;
    size_t soft_ = kSoftQueuedBytes, hard_ = kMaxQueuedBytes; /* guarded by mutex_ */
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

} // namespace

LocalServer::~LocalServer() = default;

std::unique_ptr<LocalServer> LocalServer::Create(const std::string& directory, Events events,
                                                 std::string* out_path, std::string* out_error) {
    std::string parent = directory;
    if (parent.empty()) {
        const char* tmp = getenv("TMPDIR");
        parent = tmp && *tmp ? tmp : "/tmp";
    } else if (!private_directory(parent, out_error)) {
        return nullptr;
    }
    while (parent.size() > 1 && parent.back() == '/')
        parent.pop_back();
    std::string templ = parent + "/bdk-XXXXXX";
    std::vector<char> buf(templ.begin(), templ.end());
    buf.push_back('\0');
    if (!mkdtemp(buf.data())) { /* mkdtemp makes it 0700 */
        *out_error = "could not create a directory in '" + parent + "': " + strerror(errno);
        return nullptr;
    }
    std::string dir = buf.data();
    std::string path = dir + "/" + kSocketName;
    sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    if (path.size() >= sizeof addr.sun_path) {
        rmdir(dir.c_str());
        *out_error = "the socket path '" + path + "' is too long; name a shorter directory";
        return nullptr;
    }
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path.c_str(), path.size() + 1);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    int wake[2] = {-1, -1};
    auto cleanup = [&](const std::string& why) {
        if (fd >= 0)
            close(fd);
        if (wake[0] >= 0) {
            close(wake[0]);
            close(wake[1]);
        }
        unlink(path.c_str());
        rmdir(dir.c_str());
        *out_error = why + ": " + strerror(errno);
        return nullptr;
    };
    if (fd < 0)
        return cleanup("could not create the socket");
    set_nonblocking(fd);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0)
        return cleanup("could not bind '" + path + "'");
    if (chmod(path.c_str(), static_cast<mode_t>(0600)) != 0)
        return cleanup("could not restrict '" + path + "'");
    if (listen(fd, 16) != 0)
        return cleanup("could not listen on '" + path + "'");
    if (pipe(wake) != 0)
        return cleanup("could not create the wake pipe");
    set_nonblocking(wake[0]);
    set_nonblocking(wake[1]);
    *out_path = path;
    return std::make_unique<PosixServer>(fd, wake[0], wake[1], dir, path, std::move(events));
}

} // namespace bdkcore
