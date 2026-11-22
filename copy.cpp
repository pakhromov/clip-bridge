#include "common.hpp"
#include <array>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>

static void sendAll(int fd, const void* bytes, size_t size) {
    auto data = static_cast<const char*>(bytes);
    while (size) {
        auto n = send(fd, data, size, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw systemError("sending clipboard data");
        data += n; size -= n;
    }
}

int clipboardRequest(const std::string& mime, const std::string& socketPath, bool copying, bool pasting) {
    if (copying && (mime.empty() || mime.size() > UINT32_MAX)) throw std::runtime_error("invalid MIME type");
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    if (socketPath.size() >= sizeof(address.sun_path)) throw std::runtime_error("socket path too long");
    std::memcpy(address.sun_path, socketPath.c_str(), socketPath.size() + 1);
    FD socket{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (!socket || connect(socket.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
        throw systemError("connecting to clip-bridge");
    sendAll(socket.get(), copying ? (pasting ? COPY_PASTE_MAGIC : COPY_MAGIC) : PASTE_MAGIC, 8);
    if (copying) {
        uint32_t size = htonl(mime.size());
        sendAll(socket.get(), &size, sizeof(size));
        sendAll(socket.get(), mime.data(), mime.size());
        std::array<char, 64 * 1024> buffer;
        for (;;) {
            auto n = read(STDIN_FILENO, buffer.data(), buffer.size());
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) throw systemError("reading stdin");
            uint32_t chunk = htonl(n);
            sendAll(socket.get(), &chunk, sizeof(chunk));
            if (!n) break;
            sendAll(socket.get(), buffer.data(), n);
        }
    }
    std::string response;
    std::array<char, 1024> reply;
    while (response.find('\n') == std::string::npos) {
        auto n = read(socket.get(), reply.data(), reply.size());
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) throw systemError("reading daemon response");
        if (!n) break;
        response.append(reply.data(), n);
    }
    if (response != "OK\n")
        throw std::runtime_error(response.empty() ? "daemon closed without accepting the request" : response);
    return 0;
}
