#pragma once
#include <cstdint>

class Joypad {
public:
  uint8_t joyp{0xCF}; // $FF00: Matrix select & button state

  uint8_t read() const { return joyp; }
  void write(const uint8_t val) {
    // Bits 4 and 5 are writable selection lines (P14 / P15)
    joyp = (joyp & 0x0F) | (val & 0x30);
  }
};
