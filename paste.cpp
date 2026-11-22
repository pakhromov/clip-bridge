#include "paste.hpp"
#include <algorithm>
#include <array>
#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>

struct KeyName { const char* name; unsigned short code; };
static constexpr KeyName names[] = {
    {"ctrl", KEY_LEFTCTRL}, {"control", KEY_LEFTCTRL}, {"shift", KEY_LEFTSHIFT},
    {"alt", KEY_LEFTALT}, {"super", KEY_LEFTMETA}, {"meta", KEY_LEFTMETA},
    {"a", KEY_A}, {"b", KEY_B}, {"c", KEY_C}, {"d", KEY_D}, {"e", KEY_E},
    {"f", KEY_F}, {"g", KEY_G}, {"h", KEY_H}, {"i", KEY_I}, {"j", KEY_J},
    {"k", KEY_K}, {"l", KEY_L}, {"m", KEY_M}, {"n", KEY_N}, {"o", KEY_O},
    {"p", KEY_P}, {"q", KEY_Q}, {"r", KEY_R}, {"s", KEY_S}, {"t", KEY_T},
    {"u", KEY_U}, {"v", KEY_V}, {"w", KEY_W}, {"x", KEY_X}, {"y", KEY_Y}, {"z", KEY_Z},
    {"0", KEY_0}, {"1", KEY_1}, {"2", KEY_2}, {"3", KEY_3}, {"4", KEY_4},
    {"5", KEY_5}, {"6", KEY_6}, {"7", KEY_7}, {"8", KEY_8}, {"9", KEY_9},
    {"insert", KEY_INSERT}, {"enter", KEY_ENTER}, {"return", KEY_ENTER},
    {"space", KEY_SPACE}, {"tab", KEY_TAB}, {"escape", KEY_ESC}, {"esc", KEY_ESC},
    {"backspace", KEY_BACKSPACE}, {"delete", KEY_DELETE}, {"home", KEY_HOME}, {"end", KEY_END},
    {"pageup", KEY_PAGEUP}, {"pagedown", KEY_PAGEDOWN},
    {"left", KEY_LEFT}, {"right", KEY_RIGHT}, {"up", KEY_UP}, {"down", KEY_DOWN},
    {"f1", KEY_F1}, {"f2", KEY_F2}, {"f3", KEY_F3}, {"f4", KEY_F4},
    {"f5", KEY_F5}, {"f6", KEY_F6}, {"f7", KEY_F7}, {"f8", KEY_F8},
    {"f9", KEY_F9}, {"f10", KEY_F10}, {"f11", KEY_F11}, {"f12", KEY_F12},
};

static bool modifier(unsigned short code) {
    return code == KEY_LEFTCTRL || code == KEY_LEFTSHIFT || code == KEY_LEFTALT || code == KEY_LEFTMETA;
}

PasteKeys parsePasteKeybind(const std::string& text) {
    PasteKeys keys;
    size_t start = 0;
    for (;;) {
        auto end = text.find('+', start);
        auto token = text.substr(start, end == std::string::npos ? end : end - start);
        auto first = token.find_first_not_of(" \t");
        if (first == std::string::npos) throw std::runtime_error("empty paste key");
        token = token.substr(first, token.find_last_not_of(" \t") - first + 1);
        for (char& c : token) c = std::tolower(static_cast<unsigned char>(c));
        auto found = std::ranges::find_if(names, [&](const auto& key) { return token == key.name; });
        if (found == std::end(names)) throw std::runtime_error("unknown paste key: " + token);
        if (std::ranges::find(keys, found->code) != keys.end()) throw std::runtime_error("duplicate paste key: " + token);
        if ((end == std::string::npos) == modifier(found->code))
            throw std::runtime_error("paste_keybind must be modifiers followed by one non-modifier key");
        keys.push_back(found->code);
        if (end == std::string::npos) return keys;
        start = end + 1;
    }
}

Keyboard::~Keyboard() { reset(); }
void Keyboard::reset() {
    if (device) { ioctl(device.get(), UI_DEV_DESTROY); device.reset(); }
}

bool Keyboard::ensure() {
    if (device) return false;
    FD candidate{open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC)};
    if (!candidate) throw systemError("opening /dev/uinput (check uinput device permissions)");
    if (ioctl(candidate.get(), UI_SET_EVBIT, EV_KEY) < 0) throw systemError("enabling virtual keyboard events");
    std::array<bool, KEY_MAX + 1> enabled{};
    for (const auto& key : names) {
        if (enabled[key.code]) continue;
        if (ioctl(candidate.get(), UI_SET_KEYBIT, key.code) < 0) throw systemError("enabling virtual keyboard key");
        enabled[key.code] = true;
    }
    uinput_setup setup{};
    setup.id.bustype = BUS_VIRTUAL;
    std::strcpy(setup.name, "clip-bridge virtual keyboard");
    if (ioctl(candidate.get(), UI_DEV_SETUP, &setup) < 0 || ioctl(candidate.get(), UI_DEV_CREATE) < 0)
        throw systemError("creating virtual keyboard");
    device = std::move(candidate);
    return true;
}

void Keyboard::press(const PasteKeys& keys) {
    std::vector<input_event> events;
    events.reserve(2 * keys.size() + 2);
    for (auto code : keys) events.push_back({{}, EV_KEY, code, 1});
    events.push_back({{}, EV_SYN, SYN_REPORT, 0});
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) events.push_back({{}, EV_KEY, *it, 0});
    events.push_back({{}, EV_SYN, SYN_REPORT, 0});
    auto data = reinterpret_cast<const char*>(events.data());
    size_t remaining = events.size() * sizeof(input_event);
    while (remaining) {
        auto n = write(device.get(), data, remaining);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            auto error = systemError("sending paste keys");
            // Destroying the device also releases any keys from a partial write.
            reset(); throw error;
        }
        data += n; remaining -= n;
    }
}
