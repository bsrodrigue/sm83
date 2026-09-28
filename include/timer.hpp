#pragma once
#include "interrupts.hpp"
#include <cstddef>
#include <cstdint>

class Timer {
public:
  static constexpr size_t MASTER_CLOCK_CYCLES = 256;

  // Registers: $FF04-$FF07
  uint8_t div{0x00};  // $FF04: Divider Register
  uint8_t tima{0x00}; // $FF05: Timer Counter
  uint8_t tma{0x00};  // $FF06: Timer Modulo
  uint8_t tac{0x00};  // $FF07: Timer Control

  uint64_t div_cycles = 0;
  uint64_t tima_cycles = 0;

  bool is_tac_enabled() const {
    const uint8_t bits = tac & 0x04;
    return (bits != 0);
  }

  uint16_t get_tac_increment() const {
    const uint8_t bits = tac & 0x03;
    if (bits == 0x00) {
      return 1024;
    } else if (bits == 0x01) {
      return 16;
    } else if (bits == 0x02) {
      return 64;
    } else if (bits == 0x03) {
      return 256;
    }
    return 0;
  }

  uint8_t read(const uint16_t addr) const {
    switch (addr) {
    case 0xFF04:
      return div;
    case 0xFF05:
      return tima;
    case 0xFF06:
      return tma;
    case 0xFF07:
      return tac | 0xF8; // Upper 5 bits unused, return 1
    default:
      return 0xFF;
    }
  }

  void write(const uint16_t addr, const uint8_t val) {
    switch (addr) {
    case 0xFF04:
      // Any write resets DIV / internal counter
      div = 0x00;
      div_cycles = 0;
      break;
    case 0xFF05:
      tima = val;
      break;
    case 0xFF06:
      tma = val;
      break;
    case 0xFF07:
      tac = val & 0x07;
      break;
    }
  }

  void tick(const int t_cycles, InterruptController &ic) {
    div_cycles += t_cycles;

    while (div_cycles >= MASTER_CLOCK_CYCLES) {
      div_cycles -= MASTER_CLOCK_CYCLES;
      div++;
    }

    if (is_tac_enabled()) {
      tima_cycles += t_cycles;
      while (tima_cycles >= get_tac_increment()) {
        tima_cycles -= get_tac_increment();

        // Check overflow and interrupt
        if (tima == 0xFF) {
          tima = tma;
          ic.request_interrupt(0x04);
          continue;
        }
        tima++;
      }
    }
  }
};
