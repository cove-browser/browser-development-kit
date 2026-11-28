/* The external automation endpoint's transport (T-AUT-04): a local stream socket that only the user
   who runs the application can connect to, carrying length-prefixed frames. Platform code of the
   runtime core, as data_directory_lock.h is: local_socket_posix.cc implements it with a Unix-domain
   socket for macOS and Linux. A Windows implementation (an AF_UNIX socket or a named pipe with a
   user-only security descriptor) replaces that one file. It never opens a network socket. */
#ifndef BDK_LOCAL_SOCKET_H_
#define BDK_LOCAL_SOCKET_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace bdkcore {

class LocalServer {
  public:
    struct Events {
        /* Each is called on the UI thread, through the core's post(), never after the server was
           destroyed. A connection's frames arrive in order, after its on_connect. */
        std::function<void(uint64_t connection)> on_connect;
        std::function<void(uint64_t connection, const std::string& frame)> on_frame;
        std::function<void(uint64_t connection)> on_disconnect;
    };

    /* Listens in a new directory only the user can enter: `directory` when it is not empty (it must
       exist, belong to the user and be writable by no one else), otherwise a fresh 0700 directory
       under the temporary directory. The socket itself is 0600 and every connection's peer must
       have the user's identity. Fills out_path, or returns null with out_error. */
    static std::unique_ptr<LocalServer> Create(const std::string& directory, Events events,
                                               std::string* out_path, std::string* out_error);

    /* Stops the transport thread, closes every connection and removes the socket (and the
       directory, if it made one). */
    virtual ~LocalServer();

    /* Queues one frame. False when the connection is gone or would have more than the hard limit
       waiting (a reader that does not keep up): the caller closes it. A frame with a coalescing
       key replaces the frame still queued under the same key, and is queued after every frame
       queued so far, once the connection has more than the soft limit waiting; frames the
       transport has begun to write are never taken back. An empty key is never coalesced. */
    virtual bool Send(uint64_t connection, const std::string& frame, const std::string& key) = 0;
    /* Replaces the limits (zero keeps the default); for tests. */
    virtual void SetLimits(size_t soft_bytes, size_t hard_bytes) = 0;
    /* Closes one connection after the frames already queued for it are written. */
    virtual void Disconnect(uint64_t connection) = 0;

    /* A connection that falls this far behind is closed. */
    static constexpr size_t kMaxQueuedBytes = size_t{64} * 1024 * 1024;
    /* Past this much waiting, a connection's state events are coalesced. */
    static constexpr size_t kSoftQueuedBytes = size_t{4} * 1024 * 1024;
    /* Connections that have not been answered: more are refused at once. */
    static constexpr size_t kMaxConnections = 64;
};

} // namespace bdkcore

#endif /* BDK_LOCAL_SOCKET_H_ */
