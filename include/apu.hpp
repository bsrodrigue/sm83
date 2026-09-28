#pragma once
#include <array>
#include <cstdint>

class APU {
public:
  std::array<uint8_t, 0x30> registers{}; // $FF10-$FF3F

  uint8_t read(const uint16_t addr) const {
    if (addr >= 0xFF10 && addr <= 0xFF3F) {
      return registers[addr - 0xFF10];
    }
    return 0xFF;
  }

  void write(const uint16_t addr, const uint8_t val) {
    if (addr >= 0xFF10 && addr <= 0xFF3F) {
      registers[addr - 0xFF10] = val;
    }
  }
};
