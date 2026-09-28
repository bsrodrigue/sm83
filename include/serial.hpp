#pragma once
#include <cstdint>

class Serial {
public:
  uint8_t sb{0x00}; // $FF01: Serial data
  uint8_t sc{0x7E}; // $FF02: Serial control

  uint8_t read(const uint16_t addr) const {
    if (addr == 0xFF01)
      return sb;
    if (addr == 0xFF02)
      return sc;
    return 0xFF;
  }

  void write(const uint16_t addr, const uint8_t val) {
    if (addr == 0xFF01)
      sb = val;
    if (addr == 0xFF02)
      sc = val;
  }
};
