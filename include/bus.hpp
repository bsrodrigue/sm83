#pragma once
#include "apu.hpp"
#include "interrupts.hpp"
#include "joypad.hpp"
#include "ppu.hpp"
#include "serial.hpp"
#include "timer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// The address bus is 16-bits wide, therefore 64KiB from 0x0000 to 0xFFFF
class Bus {
public:
  static constexpr size_t ROM_SIZE  = 0x8000; // 32 KiB Cartridge ROM ($0000-$7FFF)
  static constexpr size_t ERAM_SIZE = 0x2000; // 8 KiB External Cartridge RAM ($A000-$BFFF)
  static constexpr size_t WRAM_SIZE = 0x2000; // 8 KiB Work RAM ($C000-$DFFF)
  static constexpr size_t IO_SIZE   = 0x0080; // 128 bytes I/O Registers ($FF00-$FF7F)
  static constexpr size_t HRAM_SIZE = 0x007F; // 127 bytes High RAM ($FF80-$FFFE)

  std::vector<uint8_t> rom;
  std::array<uint8_t, ERAM_SIZE> eram{};
  std::array<uint8_t, WRAM_SIZE> wram{};
  std::array<uint8_t, IO_SIZE> io{};
  std::array<uint8_t, HRAM_SIZE> hram{};

  // Hardware subsystems
  InterruptController interrupts;
  Joypad joypad;
  Serial serial;
  Timer timer;
  APU apu;
  PPU ppu;

  Bus() : rom(ROM_SIZE, 0) {}

  void load_rom(const std::vector<uint8_t> &rom_buffer) {
    if (rom_buffer.size() > rom.size()) {
      rom.resize(rom_buffer.size());
    }
    std::copy(rom_buffer.begin(), rom_buffer.end(), rom.begin());
  }

  void tick(const int t_cycles) {
    timer.tick(t_cycles, interrupts);
    ppu.tick(t_cycles, interrupts);
  }

  uint8_t read(const uint16_t addr) const {
    if (addr < 0x8000) { // Cartridge ROM ($0000 - $7FFF)
      if (addr < rom.size()) {
        return rom[addr];
      }
      return 0xFF;
    }
    if (addr < 0xA000) { // VRAM ($8000 - $9FFF)
      return ppu.read_vram(addr);
    }
    if (addr < 0xC000) { // External Cartridge RAM ($A000 - $BFFF)
      return eram[addr - 0xA000];
    }
    if (addr < 0xE000) { // Work RAM ($C000 - $DFFF)
      return wram[addr - 0xC000];
    }
    if (addr < 0xFE00) { // Echo RAM ($E000 - $FDFF, mirrors $C000 - $DDFF)
      return wram[addr - 0xE000];
    }
    if (addr < 0xFEA0) { // OAM ($FE00 - $FE9F)
      return ppu.read_oam(addr);
    }
    if (addr < 0xFF00) { // Unusable memory ($FEA0 - $FEFF)
      return 0xFF;
    }
    // I/O Registers ($FF00 - $FF7F)
    if (addr == 0xFF00) {
      return joypad.read();
    }
    if (addr == 0xFF01 || addr == 0xFF02) {
      return serial.read(addr);
    }
    if (addr >= 0xFF04 && addr <= 0xFF07) {
      return timer.read(addr);
    }
    if (addr == 0xFF0F) {
      return interrupts.read_if();
    }
    if (addr >= 0xFF10 && addr <= 0xFF3F) {
      return apu.read(addr);
    }
    if (addr >= 0xFF40 && addr <= 0xFF4B) {
      return ppu.read(addr);
    }
    if (addr < 0xFF80) {
      return io[addr - 0xFF00];
    }
    if (addr < 0xFFFF) { // High RAM / Zero Page ($FF80 - $FFFE)
      return hram[addr - 0xFF80];
    }
    return interrupts.read_ie(); // Interrupt Enable Register ($FFFF)
  }

  void write(const uint16_t addr, const uint8_t val) {
    if (addr < 0x8000) { // Cartridge ROM ($0000 - $7FFF)
      if (addr < rom.size()) {
        rom[addr] = val;
      }
      return;
    }
    if (addr < 0xA000) { // VRAM ($8000 - $9FFF)
      ppu.write_vram(addr, val);
      return;
    }
    if (addr < 0xC000) { // External Cartridge RAM ($A000 - $BFFF)
      eram[addr - 0xA000] = val;
      return;
    }
    if (addr < 0xE000) { // Work RAM ($C000 - $DFFF)
      wram[addr - 0xC000] = val;
      return;
    }
    if (addr < 0xFE00) { // Echo RAM ($E000 - $FDFF, mirrors $C000 - $DDFF)
      wram[addr - 0xE000] = val;
      return;
    }
    if (addr < 0xFEA0) { // OAM ($FE00 - $FE9F)
      ppu.write_oam(addr, val);
      return;
    }
    if (addr < 0xFF00) { // Unusable memory ($FEA0 - $FEFF)
      return;
    }
    // I/O Registers ($FF00 - $FF7F)
    if (addr == 0xFF00) {
      joypad.write(val);
      return;
    }
    if (addr == 0xFF01 || addr == 0xFF02) {
      serial.write(addr, val);
      return;
    }
    if (addr >= 0xFF04 && addr <= 0xFF07) {
      timer.write(addr, val);
      return;
    }
    if (addr == 0xFF0F) {
      interrupts.write_if(val);
      return;
    }
    if (addr >= 0xFF10 && addr <= 0xFF3F) {
      apu.write(addr, val);
      return;
    }
    if (addr >= 0xFF40 && addr <= 0xFF4B) {
      ppu.write(addr, val);
      return;
    }
    if (addr < 0xFF80) {
      io[addr - 0xFF00] = val;
      return;
    }
    if (addr < 0xFFFF) { // High RAM / Zero Page ($FF80 - $FFFE)
      hram[addr - 0xFF80] = val;
      return;
    }
    interrupts.write_ie(val); // Interrupt Enable Register ($FFFF)
  }
};
