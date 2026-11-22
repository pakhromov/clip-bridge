#include "common.hpp"
#include "config.hpp"
#include "protocol.hpp"
#include <algorithm>
#include <array>
#include <csignal>
#include <deque>
#include <fcntl.h>
#include <fnmatch.h>
#include <functional>
#include <cstdio>
#include <poll.h>
#include <sys/random.h>
#include <time.h>
#include <list>
#include <memory>
#include <spawn.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unordered_map>
#include <arpa/inet.h>

class Bridge;
struct Transfer;
struct Upload;

static void logError(const std::string& message) { std::fprintf(stderr, "clip-bridge: %s\n", message.c_str()); }
static uint64_t monotonicMs() {
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) throw systemError("reading monotonic clock");
    return uint64_t(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

struct PayloadData {
    std::string mime;
    std::vector<unsigned char> bytes;
    bool finished = false, complete = false;
};
using Payload = std::shared_ptr<PayloadData>;

struct Offer {
    Protocol* protocol;
    void* raw;
    std::vector<std::string> mimes;
    Offer(Protocol& p, void* offer) : protocol(&p), raw(offer) { p.listenOffer(raw, &mimes); }
    ~Offer() { protocol->destroyOffer(raw); }
};

struct Capture {
    std::vector<Payload> payloads, order;
    std::vector<std::string> command;
    Limits limits;
    Persistence persistence = Persistence::Lazy;
    unsigned timeout = 0;
    size_t pending = 0, next = 0;
};

struct Channel;
struct Owned : SourceEvents {
    Bridge* bridge;
    Channel* channel;
    Protocol* protocol;
    void* raw = nullptr;
    std::vector<Payload> payloads;
    bool textAliases = false;
    ~Owned() override { if (raw) protocol->destroySource(raw); }
    void send(const char* mime, int fd) override;
    void cancelled() override;
};

struct Channel {
    bool primary = false, empty = true, clearConfirmed = false;
    uint64_t generation = 0;
    std::unique_ptr<Offer> offer;
    std::shared_ptr<Capture> capture;
    std::unique_ptr<Owned> owned;
};

struct Transfer : std::enable_shared_from_this<Transfer> {
    Bridge* bridge = nullptr;
    FD fd;
    Payload payload;
    Limits limits;
    unsigned timeout = 0;
    uint64_t deadline = 0;
    bool active = true;
    const void* group = nullptr;
    size_t offset = 0;
    bool reading = true;
    std::function<void(bool)> done;
    // 0: continue, 1: explicit end of framed input, -1: reject.
    std::function<int(Transfer&)> progress;
    void setTimeout(unsigned milliseconds);
    void finish(bool success);
    void dispatch(short condition);
};

struct Upload {
    FD socket;
    Payload payload = std::make_shared<PayloadData>();
    Config config;
    bool headerDone = false, complete = false, replied = false;
    bool pasteOnly = false, pasteAfter = false;
    uint32_t chunkRemaining = 0;
    void reply(const std::string& message) {
        if (replied) return;
        replied = true;
        ::send(socket.get(), message.data(), message.size(), MSG_NOSIGNAL);
        socket.reset();
    }
};

struct PasteRequest { uint64_t deadline; PasteKeys keys; };
struct Delivery { Payload payload; std::vector<std::string> command; bool primary; };
struct Barrier { wl_callback* callback; Bridge* bridge; std::function<void()> done; };

class Bridge : public DeviceEvents {
  public:
    Config config;
    std::string configPath, socketPath, marker;
    bool requiredConfig;
    bool running = true, commandPending = false;
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_seat* seat = nullptr;
    uint32_t seatName = 0, extName = 0, extVersion = 0, wlrName = 0, wlrVersion = 0;
    Protocol protocol;
    FD listener, signalFd;
    bool socketBound = false, failed = false;
    dev_t socketDevice = 0;
    ino_t socketInode = 0;
    sigset_t oldSignalMask{};
    bool signalMaskSet = false;
    Channel regular, primary;
    std::unordered_map<void*, std::unique_ptr<Offer>> offers;
    std::list<std::shared_ptr<Transfer>> transfers;
    std::list<std::unique_ptr<Barrier>> barriers;
    std::shared_ptr<Upload> upload;
    std::deque<Delivery> commands;
    Keyboard keyboard;
    uint64_t keyboardReadyAt = 0;
    std::list<PasteRequest> pastes;
    pid_t child = -1;
    std::weak_ptr<Transfer> commandWriter;

    Bridge(Config c, std::string path, bool required, std::string socket)
        : config(std::move(c)), configPath(std::move(path)), socketPath(std::move(socket)), requiredConfig(required) {
        primary.primary = true;
        std::array<unsigned char, 16> random;
        size_t received = 0;
        while (received < random.size()) {
            auto n = getrandom(random.data() + received, random.size() - received, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw systemError("generating clipboard marker");
            received += n;
        }
        marker = "application/x-clip-bridge-";
        for (auto byte : random) {
            marker += "0123456789abcdef"[byte >> 4];
            marker += "0123456789abcdef"[byte & 15];
        }
    }
    ~Bridge() override {
        if (child > 0) { kill(child, SIGKILL); while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {} }
        transfers.clear(); upload.reset();
        for (auto& barrier : barriers) wl_callback_destroy(barrier->callback);
        barriers.clear();
        regular.capture.reset(); primary.capture.reset();
        regular.owned.reset(); primary.owned.reset();
        regular.offer.reset(); primary.offer.reset(); offers.clear();
        protocol.reset();
        if (seat) wl_seat_destroy(seat);
        if (registry) wl_registry_destroy(registry);
        if (display) wl_display_disconnect(display);
        if (socketBound) {
            struct stat status{};
            if (lstat(socketPath.c_str(), &status) == 0 && status.st_dev == socketDevice && status.st_ino == socketInode)
                unlink(socketPath.c_str());
        }
        if (signalMaskSet) sigprocmask(SIG_SETMASK, &oldSignalMask, nullptr);
    }
    bool enabled(const Channel& channel) const { return channel.primary ? config.primary : config.regular; }
    void stop(const std::string& message) { logError(message); failed = true; running = false; }
    void start();
    void run();
    void makeSocket();
    void acceptUploads();
    void readUpload(FD socket);
    void signals();
    void reload();
    void newOffer(void* raw) override { offers.emplace(raw, std::make_unique<Offer>(protocol, raw)); }
    void selection(void* raw, bool isPrimary) override;
    void finished() override { stop("compositor ended data-control access"); }
    void defer(std::function<void()> done);
    void abandon(Channel& channel);
    void capture(Channel& channel);
    void captured(Channel& channel, const std::shared_ptr<Capture>& current, const Payload& payload, bool success);
    void restore(Channel& channel);
    void install(Channel& channel, std::vector<Payload> payloads, bool aliases);
    void forward(const Payload& payload, const std::vector<std::string>& argv, bool isPrimary);
    void commandLater();
    void startCommand();
    void reapCommand();
    void queuePaste(const PasteKeys& keys, unsigned delay);
    void pasteDue();
    void cancelUpload(const std::string& reason);
    void finishUpload(const std::shared_ptr<Upload>& current, bool success);
    std::vector<size_t> order(const std::vector<std::string>& mimes, const Config& settings) const;
    std::shared_ptr<Transfer> transfer(FD fd, Payload payload, bool reading, const void* group,
                                      unsigned timeout, Limits limits, std::function<void(bool)> done,
                                      std::function<int(Transfer&)> progress = {});
    void cancelTransfers(const void* group) {
        transfers.remove_if([&](const auto& t) {
            if (t->group != group) return false;
            t->active = false; return true;
        });
    }
};

void Transfer::finish(bool success) {
    auto keepAlive = shared_from_this();
    if (!active) return;
    active = false;
    auto callback = std::move(done);
    bridge->transfers.remove_if([&](const auto& t) { return t.get() == this; });
    callback(success);
}

void Transfer::setTimeout(unsigned milliseconds) {
    timeout = milliseconds;
    deadline = timeout ? monotonicMs() + timeout : 0;
}

void Transfer::dispatch(short condition) {
    if (!reading && (condition & (POLLHUP | POLLERR | POLLNVAL))) { finish(false); return; }
    std::array<unsigned char, 64 * 1024> buffer;
    bool advanced = false;
    for (size_t budget = 256 * 1024; budget;) {
        if (!reading && offset == payload->bytes.size()) { finish(true); return; }
        size_t amount = std::min(budget, reading ? buffer.size() : payload->bytes.size() - offset);
        if (reading && limits.max) {
            const auto remaining = limits.max - payload->bytes.size();
            if (remaining < amount) amount = remaining + 1;
        }
        ssize_t n = reading ? read(fd.get(), buffer.data(), amount) : write(fd.get(), payload->bytes.data() + offset, amount);
        if (n > 0) {
            if (reading) {
                if (limits.exceeds(payload->bytes.size(), n)) { finish(false); return; }
                payload->bytes.insert(payload->bytes.end(), buffer.begin(), buffer.begin() + n);
                if (progress) {
                    int result = progress(*this);
                    if (result) { finish(result > 0); return; }
                }
            } else offset += n;
            budget -= n; advanced = true;
        } else if (n < 0 && errno == EINTR) continue;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        else { finish(reading && n == 0 && limits.accepts(payload->bytes.size())); return; }
    }
    if (advanced && timeout) deadline = monotonicMs() + timeout;
}

std::shared_ptr<Transfer> Bridge::transfer(FD fd, Payload payload, bool reading, const void* group,
                                          unsigned timeout, Limits limits, std::function<void(bool)> done,
                                          std::function<int(Transfer&)> progress) {
    int flags = fcntl(fd.get(), F_GETFL);
    if (flags < 0 || fcntl(fd.get(), F_SETFL, flags | O_NONBLOCK) < 0) { done(false); return {}; }
    auto t = std::make_shared<Transfer>();
    t->bridge = this; t->fd = std::move(fd); t->payload = std::move(payload); t->reading = reading;
    t->group = group; t->timeout = timeout; t->limits = limits; t->done = std::move(done); t->progress = std::move(progress);
    transfers.push_back(t);
    t->setTimeout(timeout);
    return t;
}

void Bridge::start() {
    sigset_t mask; sigemptyset(&mask);
    for (int signal : {SIGINT, SIGTERM, SIGHUP, SIGCHLD}) sigaddset(&mask, signal);
    if (sigprocmask(SIG_BLOCK, &mask, &oldSignalMask) < 0) throw systemError("blocking signals");
    signalMaskSet = true;
    signalFd.reset(signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK));
    if (!signalFd) throw systemError("creating signal fd");
    std::signal(SIGPIPE, SIG_IGN);
    makeSocket();
    display = wl_display_connect(nullptr);
    if (!display) throw systemError("connecting to Wayland");
    registry = wl_display_get_registry(display);
    static const wl_registry_listener registryListener = {
        .global = [](void* p, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
            auto& self = *static_cast<Bridge*>(p);
            if (!std::strcmp(interface, wl_seat_interface.name) && !self.seat) {
                self.seatName = name; self.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
            } else if (!std::strcmp(interface, ext_data_control_manager_v1_interface.name)) {
                self.extName = name; self.extVersion = version;
            } else if (!std::strcmp(interface, zwlr_data_control_manager_v1_interface.name)) {
                self.wlrName = name; self.wlrVersion = version;
            }
        },
        .global_remove = [](void* p, wl_registry*, uint32_t name) {
            auto& self = *static_cast<Bridge*>(p);
            if (name == self.seatName || (self.protocol.ext ? name == self.extName : name == self.wlrName))
                self.stop("seat or data-control manager was removed");
        },
    };
    wl_registry_add_listener(registry, &registryListener, this);
    if (wl_display_roundtrip(display) < 0) throw systemError("reading Wayland globals");
    if (!seat) throw std::runtime_error("no Wayland seat");
    if (extName) protocol.bind(registry, extName, extVersion, true);
    else if (wlrName) protocol.bind(registry, wlrName, wlrVersion, false);
    else throw std::runtime_error("compositor supports neither ext-data-control nor wlr-data-control");
    if (config.primary && !protocol.primarySupported()) throw std::runtime_error("primary selection requires wlr-data-control v2 or ext-data-control");
    protocol.getDevice(seat, this);
    if (wl_display_roundtrip(display) < 0) throw systemError("binding data-control device");
    run();
}

// One blocking poll integrates Wayland, signals, helpers and active transfers.
// With no active transfer deadline, the wait is indefinite and no timer runs.
void Bridge::run() {
    std::vector<pollfd> fds;
    std::vector<std::weak_ptr<Transfer>> pending;
    while (running && !failed) {
        if (commandPending) { commandPending = false; startCommand(); }
        while (wl_display_prepare_read(display) != 0) {
            if (wl_display_dispatch_pending(display) < 0) { stop("Wayland connection closed"); return; }
            if (!running || failed) return;
        }
        short events = POLLIN;
        if (wl_display_flush(display) < 0) {
            if (errno == EAGAIN) events |= POLLOUT;
            else { wl_display_cancel_read(display); stop("Wayland connection closed"); return; }
        }
        fds.clear(); pending.clear();
        fds.push_back({wl_display_get_fd(display), events, 0});
        fds.push_back({signalFd.get(), POLLIN, 0});
        fds.push_back({listener.get(), POLLIN, 0});
        int timeout = commandPending ? 0 : -1;
        const auto now = transfers.empty() && pastes.empty() ? 0 : monotonicMs();
        if (!pastes.empty()) {
            auto remaining = pastes.front().deadline > now ? std::min<uint64_t>(pastes.front().deadline - now, INT_MAX) : 0;
            if (timeout < 0 || remaining < unsigned(timeout)) timeout = remaining;
        }
        for (const auto& t : transfers) {
            fds.push_back({t->fd.get(), short(t->reading ? POLLIN : POLLOUT), 0});
            pending.push_back(t);
            if (t->deadline) {
                auto remaining = t->deadline > now ? std::min<uint64_t>(t->deadline - now, INT_MAX) : 0;
                if (timeout < 0 || remaining < unsigned(timeout)) timeout = remaining;
            }
        }
        int result = poll(fds.data(), fds.size(), timeout);
        if (result < 0) {
            wl_display_cancel_read(display);
            if (errno == EINTR) continue;
            throw systemError("waiting for clipboard events");
        }
        if (fds[0].revents & POLLIN) {
            if (wl_display_read_events(display) < 0) { stop("Wayland connection closed"); return; }
        } else wl_display_cancel_read(display);
        if ((fds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) || wl_display_dispatch_pending(display) < 0) {
            stop("Wayland connection closed"); return;
        }
        if (fds[1].revents) signals();
        if (!running || failed) break;
        if (fds[2].revents) acceptUploads();
        for (size_t i = 0; i < pending.size(); ++i) {
            auto t = pending[i].lock();
            if (!t || !t->active) continue;
            if (fds[i + 3].revents) t->dispatch(fds[i + 3].revents);
            if (t->active && t->deadline && t->deadline <= monotonicMs()) t->finish(false);
        }
        if (!pastes.empty()) pasteDue();
    }
}

void Bridge::queuePaste(const PasteKeys& keys, unsigned delay) {
    bool created = keyboard.ensure();
    const auto now = monotonicMs();
    if (created) keyboardReadyAt = now + delay + 50;
    // Give a picker time to close and let the compositor restore keyboard focus.
    const auto deadline = std::max(now + delay, keyboardReadyAt);
    pastes.insert(std::ranges::upper_bound(pastes, deadline, {}, &PasteRequest::deadline), {deadline, keys});
}

void Bridge::pasteDue() {
    const auto now = monotonicMs();
    while (!pastes.empty() && pastes.front().deadline <= now) {
        auto keys = std::move(pastes.front().keys); pastes.pop_front();
        try { keyboard.press(keys); }
        catch (const std::exception& error) { logError(error.what()); }
    }
}

void Bridge::signals() {
    signalfd_siginfo info;
    while (read(signalFd.get(), &info, sizeof(info)) == sizeof(info)) {
        if (info.ssi_signo == SIGHUP) reload();
        else if (info.ssi_signo == SIGCHLD) reapCommand();
        else running = false;
    }
}

void Bridge::reload() {
    try {
        auto next = readConfig(configPath, requiredConfig);
        if (next.primary && !protocol.primarySupported()) throw std::runtime_error("primary selection is unsupported");
        bool oldRegular = config.regular, oldPrimary = config.primary;
        config = std::move(next);
        for (auto* channel : {&regular, &primary}) {
            bool wasEnabled = channel->primary ? oldPrimary : oldRegular;
            if (!enabled(*channel)) abandon(*channel);
            else if (!wasEnabled && channel->offer) capture(*channel);
        }
    } catch (const std::exception& error) { logError(std::string("config retained: ") + error.what()); }
}

void Bridge::defer(std::function<void()> done) {
    auto item = std::make_unique<Barrier>();
    item->callback = wl_display_sync(display); item->bridge = this; item->done = std::move(done);
    static const wl_callback_listener listener = {
        .done = [](void* p, wl_callback* callback, uint32_t) {
            auto item = static_cast<Barrier*>(p);
            auto bridge = item->bridge; auto done = std::move(item->done);
            wl_callback_destroy(callback);
            bridge->barriers.remove_if([&](const auto& b) { return b.get() == item; });
            done();
        },
    };
    wl_callback_add_listener(item->callback, &listener, item.get()); barriers.push_back(std::move(item));
}

void Bridge::abandon(Channel& channel) {
    if (channel.capture) { cancelTransfers(channel.capture.get()); channel.capture.reset(); }
}

void Bridge::selection(void* raw, bool isPrimary) {
    auto& channel = isPrimary ? primary : regular;
    ++channel.generation;
    channel.offer.reset(); channel.empty = !raw; channel.clearConfirmed = false;
    if (!raw) {
        if (!isPrimary) cancelUpload("clipboard changed during upload");
        const auto generation = channel.generation;
        // A NULL event may be followed immediately by a replacement. A sync
        // barrier lets that replacement invalidate this restore without polling.
        if (channel.capture && channel.capture->persistence != Persistence::None) defer([this, &channel, generation] {
            if (channel.generation != generation || !channel.empty) return;
            channel.clearConfirmed = true; restore(channel);
        });
        return;
    }
    auto found = offers.find(raw);
    if (found == offers.end()) { stop("selection references an unknown offer"); return; }
    channel.offer = std::move(found->second); offers.erase(found);
    bool own = std::ranges::find(channel.offer->mimes, marker) != channel.offer->mimes.end();
    if (!isPrimary && !own) cancelUpload("clipboard changed during upload");
    abandon(channel);
    if (!own && enabled(channel)) capture(channel);
}

std::vector<size_t> Bridge::order(const std::vector<std::string>& mimes, const Config& settings) const {
    std::vector<size_t> result;
    if (settings.command.empty()) return result;
    for (const auto& pattern : settings.mimeTypes) {
        for (size_t i = 0; i < mimes.size(); ++i) {
            if (fnmatch(pattern.c_str(), mimes[i].c_str(), 0) || std::ranges::find(result, i) != result.end()) continue;
            result.push_back(i);
            if (!settings.forwardAll) return result;
        }
    }
    return result;
}

void Bridge::capture(Channel& channel) {
    abandon(channel);
    if (!channel.offer) return;
    auto& mimes = channel.offer->mimes;
    if (std::ranges::find(mimes, marker) != mimes.end()) return;
    if (!config.forwardSensitive && std::ranges::find(mimes, "x-kde-passwordManagerHint") != mimes.end()) return;
    auto current = std::make_shared<Capture>();
    current->command = config.command; current->limits = config.limits; current->timeout = config.timeout;
    current->persistence = config.persistence;
    const auto indices = order(mimes, config);
    if (current->persistence == Persistence::None) {
        // Without persistence, read only representations needed for forwarding.
        for (auto i : indices) {
            auto payload = std::make_shared<PayloadData>(); payload->mime = mimes[i];
            current->payloads.push_back(payload); current->order.push_back(std::move(payload));
        }
    } else {
        for (const auto& mime : mimes) {
            auto payload = std::make_shared<PayloadData>(); payload->mime = mime; current->payloads.push_back(std::move(payload));
        }
        for (auto i : indices) current->order.push_back(current->payloads[i]);
    }
    if (current->payloads.empty()) return;
    current->pending = current->payloads.size(); channel.capture = current;
    for (const auto& payload : current->payloads) {
        int pipes[2];
        if (pipe2(pipes, O_CLOEXEC) < 0) { captured(channel, current, payload, false); continue; }
        FD reader{pipes[0]}, writer{pipes[1]};
        if (transfer(std::move(reader), payload, true, current.get(), current->timeout, current->limits,
            [this, &channel, current, payload](bool success) { captured(channel, current, payload, success); }))
            protocol.receive(channel.offer->raw, payload->mime.c_str(), writer.get());
    }
}

void Bridge::captured(Channel& channel, const std::shared_ptr<Capture>& current, const Payload& payload, bool success) {
    if (channel.capture != current) return;
    payload->finished = true; payload->complete = success; --current->pending;
    if (!success) std::vector<unsigned char>().swap(payload->bytes);
    while (current->next < current->order.size()) {
        const auto& next = current->order[current->next];
        if (!next->finished) break;
        ++current->next;
        if (next->complete) forward(next, current->command, channel.primary);
    }
    restore(channel);
}

void Bridge::restore(Channel& channel) {
    if (!enabled(channel) || !channel.capture || channel.capture->pending) return;
    auto mode = channel.capture->persistence;
    if (mode == Persistence::None) { abandon(channel); return; }
    if (channel.empty) {
        if (!channel.clearConfirmed) return;
    } else if (mode != Persistence::Eager) return;
    std::vector<Payload> complete;
    for (const auto& payload : channel.capture->payloads) if (payload->complete) complete.push_back(payload);
    if (!complete.empty()) install(channel, std::move(complete), false);
    abandon(channel);
}

void Bridge::install(Channel& channel, std::vector<Payload> payloads, bool aliases) {
    auto source = std::make_unique<Owned>(); source->bridge = this; source->channel = &channel;
    source->protocol = &protocol; source->payloads = std::move(payloads); source->textAliases = aliases;
    source->raw = protocol.createSource(source.get());
    for (const auto& payload : source->payloads) protocol.offer(source->raw, payload->mime.c_str());
    if (aliases) { protocol.offer(source->raw, "text/plain"); protocol.offer(source->raw, "UTF8_STRING"); }
    protocol.offer(source->raw, marker.c_str());
    protocol.set(source->raw, channel.primary);
    channel.owned = std::move(source);
}

void Owned::send(const char* mime, int fd) {
    FD writer{fd};
    for (const auto& payload : payloads) {
        if (payload->mime == mime || (textAliases && (!std::strcmp(mime, "text/plain") || !std::strcmp(mime, "UTF8_STRING")))) {
            bridge->transfer(std::move(writer), payload, false, nullptr, bridge->config.timeout, {},
                [](bool success) { if (!success) logError("paste transfer failed or timed out"); });
            return;
        }
    }
}
void Owned::cancelled() { if (channel->owned.get() == this) channel->owned.reset(); }

void Bridge::forward(const Payload& payload, const std::vector<std::string>& argv, bool isPrimary) {
    if (argv.empty()) return;
    commands.push_back({payload, argv, isPrimary}); commandLater();
}
void Bridge::commandLater() {
    if (child <= 0 && !commands.empty()) commandPending = true;
}

void Bridge::startCommand() {
    if (child > 0 || commands.empty()) return;
    const auto& command = commands.front();
    int pipes[2];
    if (pipe2(pipes, O_CLOEXEC) < 0) { logError("cannot create command pipe"); commands.pop_front(); commandLater(); return; }
    FD reader{pipes[0]}, writer{pipes[1]};
    extern char** environ;
    std::vector<char*> env;
    for (auto entry = environ; *entry; ++entry) {
        if (!std::strncmp(*entry, "CLIPBOARD_STATE=", sizeof("CLIPBOARD_STATE=") - 1) ||
            !std::strncmp(*entry, "CLIPBOARD_TYPE=", sizeof("CLIPBOARD_TYPE=") - 1) ||
            !std::strncmp(*entry, "CLIPBOARD_SELECTION=", sizeof("CLIPBOARD_SELECTION=") - 1)) continue;
        env.push_back(*entry);
    }
    std::string type = "CLIPBOARD_TYPE=" + command.payload->mime;
    env.push_back(const_cast<char*>("CLIPBOARD_STATE=data"));
    env.push_back(type.data());
    env.push_back(const_cast<char*>(command.primary ? "CLIPBOARD_SELECTION=primary" : "CLIPBOARD_SELECTION=regular"));
    env.push_back(nullptr);
    std::vector<char*> argv;
    for (const auto& arg : command.command) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    int result = posix_spawn_file_actions_init(&actions);
    bool actionsReady = !result, attrReady = false;
    if (!result) { result = posix_spawnattr_init(&attr); attrReady = !result; }
    if (!result) result = posix_spawn_file_actions_adddup2(&actions, reader.get(), STDIN_FILENO);
    if (!result && reader.get() != STDIN_FILENO) result = posix_spawn_file_actions_addclose(&actions, reader.get());
    if (!result) result = posix_spawn_file_actions_addclose(&actions, writer.get());
    sigset_t empty, defaults; sigemptyset(&empty); sigemptyset(&defaults); sigaddset(&defaults, SIGPIPE);
    if (!result) result = posix_spawnattr_setsigmask(&attr, &empty);
    if (!result) result = posix_spawnattr_setsigdefault(&attr, &defaults);
    if (!result) result = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    if (!result) result = posix_spawnp(&child, argv.front(), &actions, &attr, argv.data(), env.data());
    if (attrReady) posix_spawnattr_destroy(&attr);
    if (actionsReady) posix_spawn_file_actions_destroy(&actions);
    if (result) { child = -1; logError("cannot start command: " + std::string(std::strerror(result))); commands.pop_front(); commandLater(); return; }
    reader.reset();
    commandWriter = transfer(std::move(writer), command.payload, false, nullptr, config.timeout, {}, [this](bool success) {
        commandWriter.reset();
        if (!success && child > 0) { logError("command input stalled or failed; terminating command"); kill(child, SIGKILL); }
    });
}
void Bridge::reapCommand() {
    if (child <= 0) return;
    int status = 0;
    pid_t result;
    do { result = waitpid(child, &status, WNOHANG); } while (result < 0 && errno == EINTR);
    if (result != child) return;
    child = -1;
    if (auto writer = commandWriter.lock()) { writer->active = false; transfers.remove(writer); }
    commandWriter.reset();
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) logError("history command exited unsuccessfully");
    commands.pop_front(); commandLater();
}

void Bridge::makeSocket() {
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    if (socketPath.size() >= sizeof(address.sun_path)) throw std::runtime_error("socket path too long");
    std::memcpy(address.sun_path, socketPath.c_str(), socketPath.size() + 1);
    listener.reset(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (!listener) throw systemError("creating helper socket");
    if (bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        if (errno != EADDRINUSE) throw systemError("binding helper socket");
        FD probe{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
        int result = probe ? connect(probe.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) : -1;
        if (!probe || result == 0 || errno == EINPROGRESS || errno == EAGAIN || errno == EALREADY)
            throw std::runtime_error("another clip-bridge is already using " + socketPath);
        if (errno != ECONNREFUSED && errno != ENOENT) throw systemError("checking existing helper socket");
        struct stat status{};
        if (lstat(socketPath.c_str(), &status) < 0 || !S_ISSOCK(status.st_mode) || status.st_uid != getuid())
            throw std::runtime_error("refusing to replace an unrelated socket path");
        if (unlink(socketPath.c_str()) < 0 || bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
            throw systemError("replacing stale helper socket");
    }
    socketBound = true;
    struct stat status{};
    if (lstat(socketPath.c_str(), &status) < 0) throw systemError("checking helper socket");
    socketDevice = status.st_dev; socketInode = status.st_ino;
    if (chmod(socketPath.c_str(), 0600) < 0 || listen(listener.get(), SOMAXCONN) < 0) throw systemError("opening helper socket");

}

void Bridge::acceptUploads() {
    for (;;) {
        FD socket{accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK)};
        if (!socket) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) logError("cannot accept helper connection");
            return;
        }
        ucred credentials{}; socklen_t size = sizeof(credentials);
        if (getsockopt(socket.get(), SOL_SOCKET, SO_PEERCRED, &credentials, &size) < 0 || credentials.uid != getuid()) continue;
        readUpload(std::move(socket));
    }
}
void Bridge::readUpload(FD socket) {
    auto current = std::make_shared<Upload>(); current->socket = std::move(socket);
    FD reader{fcntl(current->socket.get(), F_DUPFD_CLOEXEC, 0)};
    if (!reader) { current->reply("ERR cannot read upload\n"); return; }
    transfer(std::move(reader), std::make_shared<PayloadData>(), true, current.get(), config.timeout, {},
        [this, current](bool success) { finishUpload(current, success); },
        [this, current](Transfer& t) -> int {
            auto& bytes = t.payload->bytes;
            if (!current->headerDone && bytes.size() >= 8 && !std::memcmp(bytes.data(), PASTE_MAGIC, 8)) {
                if (bytes.size() != 8) return -1;
                current->pasteOnly = current->complete = true;
                current->config = config;
                return 1;
            }
            if (!current->headerDone && bytes.size() >= 12) {
                uint32_t size; std::memcpy(&size, bytes.data() + 8, 4); size = ntohl(size);
                current->pasteAfter = !std::memcmp(bytes.data(), COPY_PASTE_MAGIC, 8);
                if ((!current->pasteAfter && std::memcmp(bytes.data(), COPY_MAGIC, 8)) || !size || size > 65519) return -1;
                if (bytes.size() < size_t(12) + size) return 0;
                current->payload->mime.assign(reinterpret_cast<const char*>(bytes.data()) + 12, size);
                if (current->payload->mime.find('\0') != std::string::npos) return -1;
                bytes.erase(bytes.begin(), bytes.begin() + 12 + size);
                current->headerDone = true;
                current->config = config;
                cancelUpload("superseded by another upload"); upload = current;
                t.setTimeout(current->config.timeout);
            }
            if (!current->headerDone) return 0;
            size_t consumed = 0;
            while (consumed < bytes.size()) {
                if (!current->chunkRemaining) {
                    if (bytes.size() - consumed < 4) break;
                    uint32_t size; std::memcpy(&size, bytes.data() + consumed, 4);
                    current->chunkRemaining = ntohl(size); consumed += 4;
                    if (!current->chunkRemaining) {
                        current->complete = consumed == bytes.size();
                        return current->complete ? 1 : -1;
                    }
                }
                size_t amount = std::min<size_t>(current->chunkRemaining, bytes.size() - consumed);
                auto& payload = current->payload->bytes;
                if (current->config.limits.exceeds(payload.size(), amount)) {
                    current->reply("ERR copy exceeds max_size\n");
                    return -1;
                }
                payload.insert(payload.end(), bytes.begin() + consumed, bytes.begin() + consumed + amount);
                consumed += amount; current->chunkRemaining -= amount;
            }
            bytes.erase(bytes.begin(), bytes.begin() + consumed);
            return 0;
        });
}

void Bridge::cancelUpload(const std::string& reason) {
    if (!upload) return;
    auto current = std::move(upload);
    cancelTransfers(current.get()); current->reply("ERR " + reason + "\n");
}
void Bridge::finishUpload(const std::shared_ptr<Upload>& current, bool success) {
    if (current->pasteOnly) {
        try {
            queuePaste(current->config.pasteKeys, current->config.pasteDelay);
            current->reply("OK\n");
        } catch (const std::exception& error) { current->reply("ERR " + std::string(error.what()) + "\n"); }
        return;
    }
    if (!success || !current->complete || upload != current || !current->config.limits.accepts(current->payload->bytes.size())) {
        current->reply("ERR incomplete, cancelled, timed out, or out-of-range copy\n");
        if (upload == current) upload.reset();
        return;
    }
    auto payload = current->payload; payload->complete = payload->finished = true;
    abandon(regular);
    install(regular, {payload}, payload->mime == TEXT_MIME);
    // The barrier retains this completed request; only unfinished uploads can be superseded.
    upload.reset();
    if (!order({payload->mime}, current->config).empty()) forward(payload, current->config.command, false);
    defer([this, current] {
        try {
            if (current->pasteAfter) queuePaste(current->config.pasteKeys, current->config.pasteDelay);
            current->reply("OK\n");
        } catch (const std::exception& error) {
            current->reply("ERR copy succeeded, but paste failed: " + std::string(error.what()) + "\n");
        }
    });
}

int main(int argc, char** argv) {
    try {
        std::string configPath, socketPath, mime = TEXT_MIME;
        bool required = false, copying = false, pasting = false, hasType = false;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--help" || arg == "-h") {
                std::puts("Usage: clip-bridge [--config PATH] [--socket PATH]\n"
                          "       clip-bridge -c [-t MIME] [--socket PATH] < data\n"
                          "       clip-bridge -p [--socket PATH]\n"
                          "       clip-bridge -cp [-t MIME] [--socket PATH] < data\n\n"
                          "-c copies stdin, -p pastes the clipboard, -cp copies then pastes.\n"
                          "Copy and paste requests use an existing daemon.\n"
                          "Copy mode accepts --type/-t; default MIME: text/plain;charset=utf-8.\n"
                          "Send SIGHUP to the daemon to reload configuration."); return 0;
            }
            if (arg == "--version") { std::puts("clip-bridge " CLIP_BRIDGE_VERSION); return 0; }
            if (arg.size() > 1 && arg[0] == '-' && arg.find_first_not_of("cp", 1) == std::string::npos) {
                copying = copying || arg.find('c') != std::string::npos;
                pasting = pasting || arg.find('p') != std::string::npos;
                continue;
            }
            if ((arg == "--config" || arg == "--socket" || arg == "--type" || arg == "-t") && i + 1 < argc) {
                if (arg == "--config") { configPath = argv[++i]; required = true; }
                else if (arg == "--socket") socketPath = argv[++i];
                else { mime = argv[++i]; hasType = true; }
            } else throw std::runtime_error("unknown or incomplete option: " + arg);
        }
        if (hasType && !copying) throw std::runtime_error("--type requires -c");
        if ((copying || pasting) && required) throw std::runtime_error("--config is only available in daemon mode");
        if (socketPath.empty()) socketPath = defaultSocket();
        if (copying || pasting) return clipboardRequest(mime, socketPath, copying, pasting);
        if (configPath.empty()) {
            const char* base = std::getenv("XDG_CONFIG_HOME");
            if (base && *base) configPath = std::string(base) + "/clip-bridge/config.ini";
            else {
                const char* home = std::getenv("HOME");
                if (!home) throw std::runtime_error("HOME is not set");
                configPath = std::string(home) + "/.config/clip-bridge/config.ini";
            }
        }
        Bridge bridge(readConfig(configPath, required), configPath, required, socketPath);
        bridge.start(); return bridge.failed ? 1 : 0;
    } catch (const std::exception& error) { logError(error.what()); return 1; }
}
