#pragma once
#include "paste.hpp"
#include <algorithm>
#include <charconv>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

inline std::string trim(const std::string& text) {
    auto first = text.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? "" : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

struct Limits {
    size_t min = 0, max = 0;
    bool accepts(size_t n) const { return n >= min && (!max || n <= max); }
    bool exceeds(size_t n, size_t extra) const { return max && (n > max || extra > max - n); }
};

enum class Persistence { Lazy, Eager, None };

struct Config {
    std::vector<std::string> command = {"cliphist", "store"};
    std::vector<std::string> mimeTypes = {
        "image/jpeg", "image/*", "image/png", "text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "*"
    };
    bool forwardAll = false, forwardSensitive = false;
    bool regular = true, primary = false;
    Persistence persistence = Persistence::Lazy;
    PasteKeys pasteKeys = {KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_V};
    unsigned pasteDelay = 50;
    unsigned timeout = 30000;
    Limits limits;
};

inline size_t number(const std::string& value) {
    size_t n = 0;
    auto [end, err] = std::from_chars(value.data(), value.data() + value.size(), n);
    if (value.empty() || err != std::errc() || end != value.data() + value.size())
        throw std::runtime_error("expected a non-negative integer");
    return n;
}

inline bool boolean(const std::string& value) {
    if (value == "true") return true;
    if (value == "false") return false;
    throw std::runtime_error("expected true or false");
}

// Shell-style argument splitting, without expansion or a shell process.
inline std::vector<std::string> parseCommand(const std::string& text) {
    std::vector<std::string> args;
    std::string arg;
    char quote = 0;
    bool started = false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (quote == '\'') {
            if (c == '\'') quote = 0;
            else arg += c;
        } else if (c == '\\') {
            if (++i == text.size()) throw std::runtime_error("unfinished command escape");
            char next = text[i];
            if (quote == '"' && next != '"' && next != '\\' && next != '$' && next != '`' && next != '\n')
                arg += '\\';
            if (next != '\n' || quote) { arg += next; started = true; }
        } else if (quote == '"') {
            if (c == '"') quote = 0;
            else arg += c;
        } else if (c == '\'' || c == '"') {
            quote = c; started = true;
        } else if (c == '#' && (i == 0 || text[i - 1] == ' ' || text[i - 1] == '\n')) {
            if (i + 1 == text.size()) throw std::runtime_error("empty command");
            auto end = text.find('\n', i);
            if (end == std::string::npos) break;
            i = end;
        } else if (c == ' ' || c == '\t' || c == '\n') {
            if (started) { args.push_back(std::move(arg)); arg.clear(); started = false; }
        } else {
            arg += c; started = true;
        }
    }
    if (quote) throw std::runtime_error("unclosed command quote");
    if (started) args.push_back(std::move(arg));
    if (args.empty() || args.front().empty()) throw std::runtime_error("empty command executable");
    return args;
}

inline Config readConfig(const std::string& path, bool required) {
    Config config;
    struct File {
        FILE* stream;
        char* buffer = nullptr;
        size_t capacity = 0;
        ~File() { if (stream) std::fclose(stream); std::free(buffer); }
    } file{std::fopen(path.c_str(), "r")};
    if (!file.stream) {
        if (required || errno != ENOENT) throw std::runtime_error("cannot open config: " + path);
        return config;
    }
    size_t lineNumber = 0;
    ssize_t length;
    while ((length = getline(&file.buffer, &file.capacity, file.stream)) >= 0) {
        ++lineNumber;
        auto line = trim(std::string(file.buffer, length));
        if (line.empty() || line.front() == '#') continue;
        try {
            if (line.find('\0') != std::string::npos) throw std::runtime_error("NUL byte in configuration");
            auto split = line.find('=');
            if (split == std::string::npos) throw std::runtime_error("expected key = value");
            auto key = trim(line.substr(0, split)), value = trim(line.substr(split + 1));
            if (key == "command") {
                config.command = value.empty() ? std::vector<std::string>{} : parseCommand(value);
            } else if (key == "mime_types") {
                config.mimeTypes.clear();
                if (!value.empty()) {
                    size_t start = 0;
                    do {
                        auto end = value.find(',', start);
                        auto item = trim(value.substr(start, end == std::string::npos ? end : end - start));
                        if (item.empty()) throw std::runtime_error("empty MIME pattern");
                        config.mimeTypes.push_back(std::move(item));
                        if (end == std::string::npos) break;
                        start = end + 1;
                    } while (true);
                }
            } else if (key == "forward_all") config.forwardAll = boolean(value);
            else if (key == "forward_sensitive") config.forwardSensitive = boolean(value);
            else if (key == "paste_keybind") config.pasteKeys = parsePasteKeybind(value);
            else if (key == "paste_delay_ms") {
                auto n = number(value);
                if (n > INT_MAX) throw std::runtime_error("paste_delay_ms exceeds INT_MAX");
                config.pasteDelay = n;
            }
            else if (key == "timeout_ms") {
                auto n = number(value);
                if (n > INT_MAX) throw std::runtime_error("timeout_ms exceeds INT_MAX");
                config.timeout = n;
            } else if (key == "min_size") config.limits.min = number(value);
            else if (key == "max_size") config.limits.max = number(value);
            else if (key == "persistence") {
                if (value == "lazy") config.persistence = Persistence::Lazy;
                else if (value == "eager") config.persistence = Persistence::Eager;
                else if (value == "none") config.persistence = Persistence::None;
                else throw std::runtime_error("persistence must be lazy, eager, or none");
            }
            else if (key == "clipboard") {
                if (value != "regular" && value != "primary" && value != "both")
                    throw std::runtime_error("clipboard must be regular, primary, or both");
                config.regular = value != "primary"; config.primary = value != "regular";
            } else throw std::runtime_error("unknown option: " + key);
        } catch (const std::exception& error) {
            throw std::runtime_error(path + ":" + std::to_string(lineNumber) + ": " + error.what());
        }
    }
    if (std::ferror(file.stream)) throw std::runtime_error("cannot read config: " + path);
    if (config.limits.max && config.limits.min > config.limits.max)
        throw std::runtime_error("min_size exceeds max_size");
    return config;
}
