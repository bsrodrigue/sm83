#pragma once
#include "interrupts.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

class PPU {
public:
  enum class Mode {
    HBLANK,   // HBLANK (87-204 cycles)
    VBLANK,   // VBLANK (4560 cycles)
    OAM_SCAN, // OAM SCAN (80 cycles)
    DRAW,     // DRAWING PIXELS (172-289 cycles)
  };

  static constexpr size_t VRAM_SIZE = 0x2000; // 8 KiB ($8000-$9FFF)
  static constexpr size_t OAM_SIZE = 0x00A0;  // 160 bytes ($FE00-$FE9F)
  static constexpr size_t SCANLINE_CYCLES = 456;

  uint32_t total_cycles = 0;

  std::array<uint8_t, VRAM_SIZE> vram{};
  std::array<uint8_t, OAM_SIZE> oam{};

  // I/O Registers ($FF40-$FF4B)
  uint8_t lcdc{0x91}; // $FF40
  uint8_t stat{0x85}; // $FF41
  uint8_t scy{0x00};  // $FF42
  uint8_t scx{0x00};  // $FF43
  uint8_t ly{0x00};   // $FF44
  uint8_t lyc{0x00};  // $FF45
  uint8_t dma{0xFF};  // $FF46
  uint8_t bgp{0xFC};  // $FF47
  uint8_t obp0{0xFF}; // $FF48
  uint8_t obp1{0xFF}; // $FF49
  uint8_t wy{0x00};   // $FF4A
  uint8_t wx{0x00};   // $FF4B

  Mode get_mode() const {
    const uint8_t bits = (stat & 0x03);
    if (bits == 0x00) return Mode::HBLANK;
    if (bits == 0x01) return Mode::VBLANK;
    if (bits == 0x02) return Mode::OAM_SCAN;
    return Mode::DRAW;
  }

  void set_mode(const Mode mode) {
    stat &= ~0x03;
    stat |= static_cast<uint8_t>(mode);
  }

  uint8_t read(const uint16_t addr) const {
    switch (addr) {
    case 0xFF40: return lcdc;
    case 0xFF41: return stat;
    case 0xFF42: return scy;
    case 0xFF43: return scx;
    case 0xFF44: return ly;
    case 0xFF45: return lyc;
    case 0xFF46: return dma;
    case 0xFF47: return bgp;
    case 0xFF48: return obp0;
    case 0xFF49: return obp1;
    case 0xFF4A: return wy;
    case 0xFF4B: return wx;
    default:     return 0xFF;
    }
  }

  void write(const uint16_t addr, const uint8_t val) {
    switch (addr) {
    case 0xFF40: lcdc = val; break;
    case 0xFF41: stat = (stat & 0x07) | (val & 0xF8); break;
    case 0xFF42: scy = val; break;
    case 0xFF43: scx = val; break;
    case 0xFF44: /* LY is read-only on DMG */ break;
    case 0xFF45: lyc = val; break;
    case 0xFF46: dma = val; break;
    case 0xFF47: bgp = val; break;
    case 0xFF48: obp0 = val; break;
    case 0xFF49: obp1 = val; break;
    case 0xFF4A: wy = val; break;
    case 0xFF4B: wx = val; break;
    }
  }

  uint8_t read_vram(const uint16_t addr) const { return vram[addr - 0x8000]; }
  void write_vram(const uint16_t addr, const uint8_t val) { vram[addr - 0x8000] = val; }

  uint8_t read_oam(const uint16_t addr) const { return oam[addr - 0xFE00]; }
  void write_oam(const uint16_t addr, const uint8_t val) { oam[addr - 0xFE00] = val; }

  void tick(const int t_cycles, InterruptController &ic) {
    total_cycles += t_cycles;

    if (total_cycles >= SCANLINE_CYCLES) {
      total_cycles -= SCANLINE_CYCLES;
      ly++;
      if ((ly >= 144) && (get_mode() != Mode::VBLANK)) {
        set_mode(Mode::VBLANK);
        ic.request_interrupt(0x01);
      }

      if ((ly >= 154) && (get_mode() == Mode::VBLANK)) {
        set_mode(Mode::OAM_SCAN);
        ly = 0;
      }
    }
  }
};
