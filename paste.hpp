#pragma once
#include "common.hpp"
#include <linux/input-event-codes.h>
#include <vector>

using PasteKeys = std::vector<unsigned short>;
PasteKeys parsePasteKeybind(const std::string& text);

class Keyboard {
    FD device;
  public:
    ~Keyboard();
    bool ensure();
    void press(const PasteKeys& keys);
    void reset();
};
