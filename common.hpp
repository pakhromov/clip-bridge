#pragma once
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unistd.h>

class FD {
    int value = -1;
  public:
    explicit FD(int fd = -1) : value(fd) {}
    ~FD() { reset(); }
    FD(const FD&) = delete;
    FD& operator=(const FD&) = delete;
    FD(FD&& other) noexcept : value(other.release()) {}
    FD& operator=(FD&& other) noexcept { if (this != &other) reset(other.release()); return *this; }
    int get() const { return value; }
    explicit operator bool() const { return value >= 0; }
    int release() { int fd = value; value = -1; return fd; }
    void reset(int fd = -1) { if (value >= 0) close(value); value = fd; }
};

inline std::runtime_error systemError(const std::string& what) {
    return std::runtime_error(what + ": " + std::strerror(errno));
}

inline std::string defaultSocket() {
    if (const char* path = std::getenv("CLIP_BRIDGE_SOCKET"); path && *path)
        return path;
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime || !*runtime)
        throw std::runtime_error("XDG_RUNTIME_DIR is not set");
    const char* display = std::getenv("WAYLAND_DISPLAY");
    std::string name = display && *display ? display : "wayland-0";
    for (char& c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
    return std::string(runtime) + "/clip-bridge-" + name + ".sock";
}

inline constexpr char COPY_MAGIC[] = "CLIPBR02";
inline constexpr char COPY_PASTE_MAGIC[] = "CLIPCP01";
inline constexpr char PASTE_MAGIC[] = "CLIPPA01";
inline constexpr const char* TEXT_MIME = "text/plain;charset=utf-8";

int clipboardRequest(const std::string& mime, const std::string& socketPath, bool copying, bool pasting);
