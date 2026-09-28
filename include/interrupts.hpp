#pragma once
#include <cstdint>

class InterruptController {
public:
  // Interrupt Flags ($FF0F) and Interrupt Enable ($FFFF)
  // Bit 0: VBlank
  // Bit 1: LCD STAT
  // Bit 2: Timer
  // Bit 3: Serial
  // Bit 4: Joypad
  uint8_t if_reg{0x00};
  uint8_t ie_reg{0x00};

  void request_interrupt(const uint8_t bit_mask) { if_reg |= bit_mask; }

  uint8_t read_if() const { return if_reg; }
  void write_if(const uint8_t val) { if_reg = val; }

  uint8_t read_ie() const { return ie_reg; }
  void write_ie(const uint8_t val) { ie_reg = val; }
};
