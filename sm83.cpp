#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <ios>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// GameBoy Memory Areas
// $FFFF	Interrupt Enable Flag
// $FF80-$FFFE	Zero Page - 127 bytes
// $FF00-$FF7F	Hardware I/O Registers
// $FEA0-$FEFF	Unusable Memory
// $FE00-$FE9F	OAM - Object Attribute Memory
// $E000-$FDFF	Echo RAM - Reserved, Do Not Use
// $D000-$DFFF	Internal RAM - Bank 1-7 (switchable - CGB only)
// $C000-$CFFF	Internal RAM - Bank 0 (fixed)
// $A000-$BFFF	Cartridge RAM (If Available)
// $9C00-$9FFF	BG Map Data 2
// $9800-$9BFF	BG Map Data 1
// $8000-$97FF	Character RAM
// $4000-$7FFF	Cartridge ROM - Switchable Banks 1-xx
// $0150-$3FFF	Cartridge ROM - Bank 0 (fixed)
// $0100-$014F	Cartridge Header Area
// $0000-$00FF	Restart and Interrupt Vectors

// Hardware Components
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

class Joypad {
public:
  uint8_t joyp{0xCF}; // $FF00: Matrix select & button state

  uint8_t read() const { return joyp; }
  void write(const uint8_t val) {
    // Bits 4 and 5 are writable selection lines (P14 / P15)
    joyp = (joyp & 0x0F) | (val & 0x30);
  }
};

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

  bool is_tac_enabled() {
    const uint8_t bits = tac & 0x04;

    return (bits != 0);
  }

  uint16_t get_tac_increment() {
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

    // Throw error here
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
      // Consume
      div_cycles -= MASTER_CLOCK_CYCLES;

      div++;
    }

    // Handle TIMA here ?
    if (is_tac_enabled()) {
      tima_cycles += t_cycles;
      while (tima_cycles >= get_tac_increment()) {
        // Consume
        tima_cycles -= get_tac_increment();

        // Check overflow and interrupt
        if (tima == 0xFF) {
          tima = tma;
          // Interrupt
          ic.request_interrupt(0x04);
          continue;
        }
        tima++;
      }
    }
  }
};

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
                                              //
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

  Mode get_mode() {
    // Extract first two bits
    const uint8_t bits = (stat & 0x03);

    if (bits == 0x00) {
      return Mode::HBLANK;
    }

    else if (bits == 0x01) {
      return Mode::VBLANK;
    }

    else if (bits == 0x02) {
      return Mode::OAM_SCAN;
    }

    else if (bits == 0x03) {
      return Mode::DRAW;
    }

    // Better to throw exception here...
    return Mode::OAM_SCAN;
  }

  void set_mode(const Mode mode) {
    stat &= ~0x03; // Clear bits
    stat |= static_cast<uint8_t>(mode);
  }

  uint8_t read(const uint16_t addr) const {
    switch (addr) {
    case 0xFF40:
      return lcdc;
    case 0xFF41:
      return stat;
    case 0xFF42:
      return scy;
    case 0xFF43:
      return scx;
    case 0xFF44:
      return ly;
    case 0xFF45:
      return lyc;
    case 0xFF46:
      return dma;
    case 0xFF47:
      return bgp;
    case 0xFF48:
      return obp0;
    case 0xFF49:
      return obp1;
    case 0xFF4A:
      return wy;
    case 0xFF4B:
      return wx;
    default:
      return 0xFF;
    }
  }

  void write(const uint16_t addr, const uint8_t val) {
    switch (addr) {
    case 0xFF40:
      lcdc = val;
      break;
    case 0xFF41:
      stat = (stat & 0x07) | (val & 0xF8);
      break;
    case 0xFF42:
      scy = val;
      break;
    case 0xFF43:
      scx = val;
      break;
    case 0xFF44: /* LY is read-only on DMG */
      break;
    case 0xFF45:
      lyc = val;
      break;
    case 0xFF46:
      dma = val;
      break;
    case 0xFF47:
      bgp = val;
      break;
    case 0xFF48:
      obp0 = val;
      break;
    case 0xFF49:
      obp1 = val;
      break;
    case 0xFF4A:
      wy = val;
      break;
    case 0xFF4B:
      wx = val;
      break;
    }
  }

  uint8_t read_vram(const uint16_t addr) const { return vram[addr - 0x8000]; }
  void write_vram(const uint16_t addr, const uint8_t val) {
    vram[addr - 0x8000] = val;
  }

  uint8_t read_oam(const uint16_t addr) const { return oam[addr - 0xFE00]; }
  void write_oam(const uint16_t addr, const uint8_t val) {
    oam[addr - 0xFE00] = val;
  }

  void tick(const int t_cycles, InterruptController &ic) {
    total_cycles += t_cycles;

    if (total_cycles >= SCANLINE_CYCLES) { // Scanline boundary

      // if (total_cycles <= 80) {
      //
      // }
      // else if (total_cycles >= 172 && total_cycles <= 289) {
      //
      // }
      // else if (total_cycles)

      total_cycles -= SCANLINE_CYCLES; // Consume
      ly++;
      if ((ly >= 144) && (get_mode() != Mode::VBLANK)) { // VBLANK MODE
        set_mode(Mode::VBLANK);
        ic.request_interrupt(0x01);
      }

      if ((ly >= 154) && (get_mode() == Mode::VBLANK)) { // RESET
        set_mode(Mode::OAM_SCAN);
        ly = 0;
      }
    }
  }

  void draw() {
    for (auto i = 0; i < 144; i++) {
      // MODE 2
      set_mode(Mode::OAM_SCAN);

      // MODE 3
      set_mode(Mode::DRAW);

      // MODE 0
      set_mode(Mode::HBLANK);
    }

    for (auto i = 144; i < 154; i++) {
      // MODE 1
      set_mode(Mode::VBLANK);
    }
  }
};

// The address bus is 16-bits wide, therefore 64KiB from 0x0000 to 0xFFFF
class Bus {
public:
  // Memory region sizes
  static constexpr size_t ROM_SIZE =
      0x8000; // 32 KiB Cartridge ROM ($0000-$7FFF)
  static constexpr size_t ERAM_SIZE =
      0x2000; // 8 KiB External Cartridge RAM ($A000-$BFFF)
  static constexpr size_t WRAM_SIZE = 0x2000; // 8 KiB Work RAM ($C000-$DFFF)
  static constexpr size_t IO_SIZE =
      0x0080; // 128 bytes I/O Registers ($FF00-$FF7F)
  static constexpr size_t HRAM_SIZE =
      0x007F; // 127 bytes High RAM ($FF80-$FFFE)

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
        // No cycle cost?
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

class SM83 {
private:
  // State
  bool is_on_halt = false;

  // Registers
  uint8_t reg_a; // Accumulator
  uint8_t reg_f; // Flag Register
  uint8_t reg_b;
  uint8_t reg_c;
  uint8_t reg_d;
  uint8_t reg_e;
  uint8_t reg_h;
  uint8_t reg_l;

  // Special Registers
  uint16_t reg_sp; // Stack Pointer
  uint16_t reg_pc; // Program Counter
  uint8_t reg_ir;  // Instruction Register
  uint8_t reg_ie;  // Interrupt Enable

  bool reg_ime; // Interrupt Master Enable (EI/DI)

  uint16_t reg_bc() const { return (reg_b << 8) | reg_c; }
  uint16_t reg_de() const { return (reg_d << 8) | reg_e; }
  uint16_t reg_hl() const { return (reg_h << 8) | reg_l; }

  void set_reg_bc(const uint16_t bc) {
    reg_b = bc >> 8;
    reg_c = (bc & 0x00FF);
  }

  void set_reg_de(const uint16_t de) {
    reg_d = de >> 8;
    reg_e = (de & 0x00FF);
  }

  void set_reg_hl(const uint16_t hl) {
    reg_h = hl >> 8;
    reg_l = (hl & 0x00FF);
  }

  bool z() const { // Bit 7
    return (reg_f & 0b10000000);
  }
  bool n() const { // Bit 6
    return (reg_f & 0b01000000);
  }
  bool h() const { // Bit 5
    return (reg_f & 0b00100000);
  }
  bool c() const { // Bit 4
    return (reg_f & 0b00010000);
  }

  void set_z() { // Bit 7
    reg_f |= 0b10000000;
  }
  void set_n() { // Bit 6
    reg_f |= 0b01000000;
  }
  void set_h() { // Bit 5
    reg_f |= 0b00100000;
  }
  void set_c() { // Bit 4
    reg_f |= 0b00010000;
  }

  void set_flags(const bool z, const bool n, const bool h, const bool c) {
    reg_f = (z ? 0b10000000 : 0) | (n ? 0b01000000 : 0) | (h ? 0b00100000 : 0) |
            (c ? 0b00010000 : 0);
  }

  // Fetch helpers (operands are read little-endian, low byte first)
  uint8_t fetch8() { return read8(reg_pc++); }

  uint16_t fetch16() {
    const uint8_t low = fetch8();
    const uint8_t high = fetch8();
    return (high << 8) | low;
  }

  uint8_t &
  reg_r8(const uint8_t index) { // r8: 0=B, 1=C, 2=D, 3=E, 4=H, 5=L, 7=A
    switch (index) {
    case 0:
      return reg_b;
    case 1:
      return reg_c;
    case 2:
      return reg_d;
    case 3:
      return reg_e;
    case 4:
      return reg_h;
    case 5:
      return reg_l;
    default:
      return reg_a; // index 6 ((HL)) is handled separately
    }
  }

  void alu8(const uint8_t op, const uint8_t src) { // 0x80 block: ALU op on r8
    const uint8_t value = src == 6 ? read8(reg_hl()) : reg_r8(src);
    switch (op) {
    case 0b000:
      add(value);
      break; // ADD A, r8
    case 0b001:
      adc(value);
      break; // ADC A, r8
    case 0b010:
      sub(value);
      break; // SUB r8
    case 0b011:
      sbc(value);
      break; // SBC A, r8
    case 0b100:
      and_a(value);
      break; // AND r8
    case 0b101:
      xor_a(value);
      break; // XOR r8
    case 0b110:
      or_a(value);
      break; // OR r8
    case 0b111:
      cp(value);
      break; // CP r8
    }
  }

  void execute_cb() {
    const uint8_t opcode = fetch8();
    const uint8_t op = opcode >> 3;
    const uint8_t target = opcode & 0x07;

    if (target == 6) { // (HL) target
      const uint16_t addr = reg_hl();
      const uint8_t val = read8(addr);
      uint8_t result = val;
      switch (op) {
      case 0b000: // RLC
      {
        const bool c_f = val & 0x80;
        result = (val << 1) | (val >> 7);
        set_flags(result == 0, false, false, c_f);
      } break;
      case 0b001: // RRC
      {
        const bool c_f = val & 0x01;
        result = (val >> 1) | (val << 7);
        set_flags(result == 0, false, false, c_f);
      } break;
      case 0b010: // RL
      {
        const bool c_f = val & 0x80;
        result = (val << 1) | (c() ? 1 : 0);
        set_flags(result == 0, false, false, c_f);
      } break;
      case 0b011: // RR
      {
        const bool c_f = val & 0x01;
        result = (val >> 1) | (c() ? 0x80 : 0);
        set_flags(result == 0, false, false, c_f);
      } break;
      case 0b100: // SLA
      {
        const bool c_f = val & 0x80;
        result = val << 1;
        set_flags(result == 0, false, false, c_f);
      } break;
      case 0b101: // SRA
      {
        const bool c_f = val & 0x01;
        result = (val >> 1) | (val & 0x80);
        set_flags(result == 0, false, false, c_f);
      } break;
      case 0b110: // SWAP
      {
        result = (val >> 4) | (val << 4);
        set_flags(result == 0, false, false, false);
      } break;
      case 0b111: // SRL
      {
        const bool c_f = val & 0x01;
        result = val >> 1;
        set_flags(result == 0, false, false, c_f);
      } break;
      case 8:
      case 9:
      case 10:
      case 11:
      case 12:
      case 13:
      case 14:
      case 15:
        bit(op - 8, val);
        return; // BIT doesn't write back
      case 16:
      case 17:
      case 18:
      case 19:
      case 20:
      case 21:
      case 22:
      case 23:
        res(op - 16, result);
        break;
      default:
        set(op - 24, result);
        break;
      }
      write8(addr, result);
      return;
    }

    uint8_t &reg = reg_r8(target);
    switch (op) {
    case 0b000:
      rlc(reg);
      break;
    case 0b001:
      rrc(reg);
      break;
    case 0b010:
      rl(reg);
      break;
    case 0b011:
      rr(reg);
      break;
    case 0b100:
      sla(reg);
      break;
    case 0b101:
      sra(reg);
      break;
    case 0b110:
      swap(reg);
      break;
    case 0b111:
      srl(reg);
      break;
    case 8:
    case 9:
    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
      bit(op - 8, reg);
      break;
    case 16:
    case 17:
    case 18:
    case 19:
    case 20:
    case 21:
    case 22:
    case 23:
      res(op - 16, reg);
      break;
    default:
      set(op - 24, reg);
      break;
    }
  }

  // Cycle Counting
  uint64_t total_cycles;

  void tick(const int t_cycles) {
    total_cycles += t_cycles;
    bus.tick(t_cycles);
  }

  // Bus
  Bus bus;

  // Memory Access (Every 8-bit access takes 4 T-cycles)
  uint8_t read8(const uint16_t addr) {
    tick(4);
    return bus.read(addr);
  }

  void write8(const uint16_t addr, const uint8_t val) {
    tick(4);
    bus.write(addr, val);
  }

  void inspect_tile_data() {
    // 0x8000 to 0x97FF
    uint16_t offset = 0x8000;

    while (offset < 0x97FF) {
      uint8_t data = read8(offset++);
      (void)data;
    }
  }

  // $8000–87FF	$8800–8FFF	$9000–97FF

  uint16_t get_tile_offset(const uint8_t tile_idx, const bool lcdc4) {
    const uint16_t base_offset = lcdc4 ? 0x8000 : 0x9000;

    if (lcdc4) {
      return base_offset + tile_idx * 16;
    }

    return base_offset + static_cast<int8_t>(tile_idx) * 16;
  }

  std::vector<uint8_t> decode_tile(uint16_t tile_offset) {
    std::vector<uint8_t> pixels{};

    for (auto i = 0; i < 8; i++) {
      const uint8_t lsb_byte = read8(tile_offset++); // 0b00000000
      const uint8_t msb_byte = read8(tile_offset++); // 0b00000000

      // Decode line
      for (auto bit_pos = 0; bit_pos < 8; bit_pos++) {
        const uint8_t lsb = (lsb_byte >> bit_pos) & 1;
        const uint8_t msb = (msb_byte >> bit_pos) & 1;

        uint8_t result = (msb << 1) | lsb;
        pixels.push_back(result);
      }
    }

    return pixels;
  }

  // Game ROM expresses interest in eventually using interrupts.
  uint8_t get_ie() { return read8(0xFFFF); }
  // Actually interrupts happening.
  uint8_t get_if() { return read8(0xFF0F); }

public:
  // Interrupts
  void handle_interrupts() {
    // Priorities:
    // 1. V-Blank (Bit 0)
    // 2. LCD Stat (Bit 1)
    // 3. Timer (Bit 2)
    // 4. Serial (Bit 3)
    // 5. Joypad (Bit 4)
    //
    // Bit	Interrupt	Vector Address
    // 0	VBlank	0x40
    // 1	LCD Stat	0x48
    // 2	Timer	0x50
    // 3	Serial	0x58
    // 4	Joypad	0x60

    const bool should_handle_interrupt = ((get_if() & get_ie()) > 0) && reg_ime;

    if (!should_handle_interrupt)
      return;

    // Disable interrupts for now...
    reg_ime = false;

    // Disable halting
    is_on_halt = false;

    uint8_t flags = get_if();

    // Check if V-Blank flag is set
    if (flags & 1) {
      // Clear flag bit
      flags &= ~(1);
      write8(0xFF0F, flags);
      tick(20);
      push(reg_pc);
      reg_pc = 0x40;
    }

    // Check if LCD flag is set
    else if (flags & (1 << 1)) {
      // Clear flag bit
      flags &= ~(1 << 1);

      write8(0xFF0F, flags);
      tick(20);
      push(reg_pc);
      reg_pc = 0x48;
    }

    // Check if Timer flag is set
    else if (flags & (1 << 2)) {
      // Clear flag bit
      flags &= ~(1 << 2);

      write8(0xFF0F, flags);
      tick(20);
      push(reg_pc);
      reg_pc = 0x50;
    }

    // Check if Serial flag is set
    else if (flags & (1 << 3)) {
      // Clear flag bit
      flags &= ~(1 << 3);

      write8(0xFF0F, flags);
      tick(20);
      push(reg_pc);
      reg_pc = 0x58;
    }

    // Check if Joypad flag is set
    else if (flags & (1 << 4)) {
      // Clear flag bit
      flags &= ~(1 << 4);

      write8(0xFF0F, flags);
      tick(20);
      push(reg_pc);
      reg_pc = 0x60;
    }
  }

  // Instructions
  void nop() const {}

  // 8-Bit Arithmetic
  void add(const uint8_t value) { // ADD A, value
    const uint16_t result = reg_a + value;
    const bool half_carry = (reg_a & 0x0F) + (value & 0x0F) > 0x0F;
    set_flags((result & 0xFF) == 0, false, half_carry, result > 0xFF);
    reg_a = result & 0xFF;
  }

  void adc(const uint8_t value) { // ADC A, value
    const bool carry = c();
    const uint16_t result = reg_a + value + (carry ? 1 : 0);
    const bool half_carry =
        (reg_a & 0x0F) + (value & 0x0F) + (carry ? 1 : 0) > 0x0F;
    set_flags((result & 0xFF) == 0, false, half_carry, result > 0xFF);
    reg_a = result & 0xFF;
  }

  void sub(const uint8_t value) { // SUB value
    const int result = reg_a - value;
    const bool half_borrow = (reg_a & 0x0F) < (value & 0x0F);
    set_flags(result == 0, true, half_borrow, result < 0);
    reg_a = result & 0xFF;
  }

  void sbc(const uint8_t value) { // SBC A, value
    const bool carry = c();
    const int result = reg_a - value - (carry ? 1 : 0);
    const bool half_borrow = (reg_a & 0x0F) < (value & 0x0F) + (carry ? 1 : 0);
    set_flags(result == 0, true, half_borrow, result < 0);
    reg_a = result & 0xFF;
  }

  void and_a(const uint8_t value) { // AND value
    reg_a &= value;
    set_flags(reg_a == 0, false, true, false);
  }

  void xor_a(const uint8_t value) { // XOR value
    reg_a ^= value;
    set_flags(reg_a == 0, false, false, false);
  }

  void or_a(const uint8_t value) { // OR value
    reg_a |= value;
    set_flags(reg_a == 0, false, false, false);
  }

  void cp(const uint8_t value) { // CP value (compares against A)
    const int result = reg_a - value;
    const bool half_borrow = (reg_a & 0x0F) < (value & 0x0F);
    set_flags(result == 0, true, half_borrow, result < 0);
  }

  void inc(uint8_t &reg) { // INC r8
    const bool half_carry = (reg & 0x0F) == 0x0F;
    reg++;
    set_flags(reg == 0, false, half_carry, c());
  }

  void dec(uint8_t &reg) { // DEC r8
    const bool half_borrow = (reg & 0x0F) == 0;
    reg--;
    set_flags(reg == 0, true, half_borrow, c());
  }

  void inc_hl_mem() { // INC [HL]
    const uint16_t addr = reg_hl();
    const uint8_t val = read8(addr);
    const bool half_carry = (val & 0x0F) == 0x0F;
    const uint8_t res = val + 1;
    write8(addr, res);
    set_flags(res == 0, false, half_carry, c());
  }

  void dec_hl_mem() { // DEC [HL]
    const uint16_t addr = reg_hl();
    const uint8_t val = read8(addr);
    const bool half_borrow = (val & 0x0F) == 0;
    const uint8_t res = val - 1;
    write8(addr, res);
    set_flags(res == 0, true, half_borrow, c());
  }

  void daa() { // DAA - adjust A to BCD
    const bool carry = c();
    if (n()) {
      if (h())
        reg_a -= 0x06;
      if (carry)
        reg_a -= 0x60;
    } else {
      if (h() || (reg_a & 0x0F) > 0x09)
        reg_a += 0x06;
      if (carry || reg_a > 0x99) {
        reg_a += 0x60;
        set_c();
      }
    }
    set_flags(reg_a == 0, n(), false, c());
  }

  void cpl() { // CPL - complement A
    reg_a = ~reg_a;
    set_n();
    set_h();
  }

  void ccf() { // CCF - complement carry
    const bool carry = c();
    reg_f = z() ? 0b10000000 : 0;
    if (!carry)
      set_c();
  }

  void scf() { // SCF - set carry
    reg_f = z() ? 0b10000000 : 0;
    set_c();
  }

  // 8-Bit Loads
  void ld(uint8_t &dst, const uint8_t src) { // LD r8, r8' / LD r8, n8
    dst = src;
  }

  void ld_hl_to_r8(uint8_t &dst) { // LD r8, (HL)
    dst = read8(reg_hl());
  }

  void ld_r8_to_hl(const uint8_t src) { // LD (HL), r8 / n8
    write8(reg_hl(), src);
  }

  void ld_a_bc() { // LD A, (BC)
    reg_a = read8(reg_bc());
  }

  void ld_a_de() { // LD A, (DE)
    reg_a = read8(reg_de());
  }

  void ld_a_hl_inc() { // LD A, (HL+)
    reg_a = read8(reg_hl());
    set_reg_hl(reg_hl() + 1);
  }

  void ld_a_hl_dec() { // LD A, (HL-)
    reg_a = read8(reg_hl());
    set_reg_hl(reg_hl() - 1);
  }

  void ld_a_nn(const uint16_t addr) { // LD A, (nn16)
    reg_a = read8(addr);
  }

  void ld_bc_a() { // LD (BC), A
    write8(reg_bc(), reg_a);
  }

  void ld_de_a() { // LD (DE), A
    write8(reg_de(), reg_a);
  }

  void ld_hl_inc_a() { // LD (HL+), A
    write8(reg_hl(), reg_a);
    set_reg_hl(reg_hl() + 1);
  }

  void ld_hl_dec_a() { // LD (HL-), A
    write8(reg_hl(), reg_a);
    set_reg_hl(reg_hl() - 1);
  }

  void ld_nn_a(const uint16_t addr) { // LD (nn16), A
    write8(addr, reg_a);
  }

  void ldh_a_n(const uint8_t offset) { // LDH A, (n8)
    reg_a = read8(0xFF00 + offset);
  }

  void ldh_n_a(const uint8_t offset) { // LDH (n8), A
    write8(0xFF00 + offset, reg_a);
  }

  void ld_a_c() { // LD A, (C)
    reg_a = read8(0xFF00 + reg_c);
  }

  void ld_c_a() { // LD (C), A
    write8(0xFF00 + reg_c, reg_a);
  }

  // 16-Bit Loads
  void ld_bc_nn(const uint16_t nn) { // LD BC, nn16
    set_reg_bc(nn);
  }

  void ld_de_nn(const uint16_t nn) { // LD DE, nn16
    set_reg_de(nn);
  }

  void ld_hl_nn(const uint16_t nn) { // LD HL, nn16
    set_reg_hl(nn);
  }

  void ld_sp_nn(const uint16_t nn) { // LD SP, nn16
    reg_sp = nn;
  }

  void ld_sp_hl() { // LD SP, HL
    reg_sp = reg_hl();
  }

  void ld_hl_sp_r8(const int8_t offset) { // LD HL, SP + r8 (sign-extended)
    const uint16_t sp = reg_sp;
    const bool half_carry = (sp & 0x0F) + (offset & 0x0F) > 0x0F;
    const bool carry = (sp & 0xFF) + (offset & 0xFF) > 0xFF;
    set_reg_hl(sp + offset);
    set_flags(false, false, half_carry, carry);
  }

  void ld_nn_sp(const uint16_t addr) { // LD (nn16), SP
    write8(addr, reg_sp & 0xFF);
    write8(addr + 1, reg_sp >> 8);
  }

  // 16-Bit Arithmetic
  void add_hl(const uint16_t value) { // ADD HL, rr
    tick(4);                          // Internal delay
    const uint32_t result = reg_hl() + value;
    const bool half_carry = (reg_hl() & 0x0FFF) + (value & 0x0FFF) > 0x0FFF;
    set_reg_hl(result & 0xFFFF);
    set_flags(z(), false, half_carry, result > 0xFFFF);
  }

  void inc_bc() {
    tick(4);
    set_reg_bc(reg_bc() + 1);
  } // INC BC
  void inc_de() {
    tick(4);
    set_reg_de(reg_de() + 1);
  } // INC DE
  void inc_hl() {
    tick(4);
    set_reg_hl(reg_hl() + 1);
  } // INC HL
  void inc_sp() {
    tick(4);
    reg_sp++;
  } // INC SP

  void dec_bc() {
    tick(4);
    set_reg_bc(reg_bc() - 1);
  } // DEC BC
  void dec_de() {
    tick(4);
    set_reg_de(reg_de() - 1);
  } // DEC DE
  void dec_hl() {
    tick(4);
    set_reg_hl(reg_hl() - 1);
  } // DEC HL
  void dec_sp() {
    tick(4);
    reg_sp--;
  } // DEC SP

  void add_sp_r8(const int8_t offset) { // ADD SP, r8
    const uint16_t sp = reg_sp;
    tick(4); // Wait
    const bool half_carry = (sp & 0x0F) + (offset & 0x0F) > 0x0F;
    const bool carry = (sp & 0xFF) + (offset & 0xFF) > 0xFF;
    reg_sp = sp + offset;
    set_flags(false, false, half_carry, carry);
    tick(4); // Wait
  }

  // Rotates (A only, base table)
  void rlca() { // RLCA
    const bool c_flag = reg_a & 0x80;
    reg_a = (reg_a << 1) | (reg_a >> 7);
    set_flags(false, false, false, c_flag);
  }

  void rla() { // RLA
    const bool c_flag = reg_a & 0x80;
    const bool carry = c();
    reg_a = (reg_a << 1) | (carry ? 1 : 0);
    set_flags(false, false, false, c_flag);
  }

  void rrca() { // RRCA
    const bool c_flag = reg_a & 0x01;
    reg_a = (reg_a >> 1) | (reg_a << 7);
    set_flags(false, false, false, c_flag);
  }

  void rra() { // RRA
    const bool c_flag = reg_a & 0x01;
    const bool carry = c();
    reg_a = (reg_a >> 1) | (carry ? 0x80 : 0);
    set_flags(false, false, false, c_flag);
  }

  // CB Rotates & Shifts (register target)
  void rlc(uint8_t &reg) { // RLC r8
    const bool c_flag = reg & 0x80;
    reg = (reg << 1) | (reg >> 7);
    set_flags(reg == 0, false, false, c_flag);
  }

  void rl(uint8_t &reg) { // RL r8
    const bool c_flag = reg & 0x80;
    const bool carry = c();
    reg = (reg << 1) | (carry ? 1 : 0);
    set_flags(reg == 0, false, false, c_flag);
  }

  void rrc(uint8_t &reg) { // RRC r8
    const bool c_flag = reg & 0x01;
    reg = (reg >> 1) | (reg << 7);
    set_flags(reg == 0, false, false, c_flag);
  }

  void rr(uint8_t &reg) { // RR r8
    const bool c_flag = reg & 0x01;
    const bool carry = c();
    reg = (reg >> 1) | (carry ? 0x80 : 0);
    set_flags(reg == 0, false, false, c_flag);
  }

  void sla(uint8_t &reg) { // SLA r8
    const bool c_flag = reg & 0x80;
    reg = reg << 1;
    set_flags(reg == 0, false, false, c_flag);
  }

  void sra(uint8_t &reg) { // SRA r8
    const bool c_flag = reg & 0x01;
    reg = (reg >> 1) | (reg & 0x80);
    set_flags(reg == 0, false, false, c_flag);
  }

  void srl(uint8_t &reg) { // SRL r8
    const bool c_flag = reg & 0x01;
    reg = reg >> 1;
    set_flags(reg == 0, false, false, c_flag);
  }

  void swap(uint8_t &reg) { // SWAP r8
    reg = (reg >> 4) | (reg << 4);
    set_flags(reg == 0, false, false, false);
  }

  // (Removed memory-specific CB methods, now handled in execute_cb)

  // CB Bit Operations
  void bit(const uint8_t pos, const uint8_t val) { // BIT b, r8
    set_flags(!(val & (1 << pos)), false, true, c());
  }

  void set(const uint8_t pos, uint8_t &reg) { // SET b, r8
    reg |= (1 << pos);
  }

  void res(const uint8_t pos, uint8_t &reg) { // RES b, r8
    reg &= ~(1 << pos);
  }

  // Stack
  void push(const uint16_t val) {
    tick(4); // Internal wait
    write8(--reg_sp, val >> 8);
    write8(--reg_sp, val & 0xFF);
  }

  uint16_t pop() {
    const uint8_t low = read8(reg_sp++);
    const uint8_t high = read8(reg_sp++);
    return low | (high << 8);
  }

  void push_bc() { push(reg_bc()); }
  void push_de() { push(reg_de()); }
  void push_hl() { push(reg_hl()); }
  void push_af() { push((reg_a << 8) | reg_f); }

  void pop_bc() { set_reg_bc(pop()); }
  void pop_de() { set_reg_de(pop()); }
  void pop_hl() { set_reg_hl(pop()); }
  void pop_af() {
    const uint16_t val = pop();
    reg_a = val >> 8;
    reg_f = val & 0xF0;
  }

  // Subroutines
  void call(const uint16_t addr) {
    push(reg_pc);
    reg_pc = addr;
  }

  void ret() {
    reg_pc = pop();
    tick(4); // Wait
  }

  void reti() {
    ret();
    reg_ime = true;
  }

  void rst(const uint8_t vector) {
    push(reg_pc);
    reg_pc = vector;
  }

  // Jumps
  void jp(const uint16_t addr) { reg_pc = addr; }

  void jr(const int8_t offset) {
    tick(4); // Internal wait for calculating offset
    reg_pc += offset;
  }

  void jr_nz(const int8_t offset) {
    if (!z())
      jr(offset);
  } // JR NZ, r8
  void jr_z(const int8_t offset) {
    if (z())
      jr(offset);
  } // JR Z, r8
  void jr_nc(const int8_t offset) {
    if (!c())
      jr(offset);
  } // JR NC, r8
  void jr_c(const int8_t offset) {
    if (c())
      jr(offset);
  } // JR C, r8

  // Misc Control
  // Interrupt switches
  void ei() { // EI - enable interrupts (has a 1-instruction delay on real HW)
    reg_ime = true;
  }

  void di() { // DI - disable interrupts
    reg_ime = false;
  }

  void halt() { // HALT - halts CPU until an interrupt occurs
    is_on_halt = true;
  }

  void stop() { // STOP - halts CPU and pauses LCD
    write8(0xFF04, 0x00);
  }

  // Decoder - fetch and execute a single instruction
  void execute() {
    if (is_on_halt) {
      return; // Skip execution
    }

    const uint8_t opcode = fetch8();

    switch (opcode) {
    case 0x00:
      nop();
      break; // NOP
    case 0x01:
      ld_bc_nn(fetch16());
      break; // LD BC, nn16
    case 0x02:
      ld_bc_a();
      break; // LD (BC), A
    case 0x03:
      inc_bc();
      break; // INC BC
    case 0x04:
      inc(reg_b);
      break; // INC B
    case 0x05:
      dec(reg_b);
      break; // DEC B
    case 0x06:
      ld(reg_b, fetch8());
      break; // LD B, n8
    case 0x07:
      rlca();
      break; // RLCA
    case 0x08:
      ld_nn_sp(fetch16());
      break; // LD (nn16), SP
    case 0x09:
      add_hl(reg_bc());
      break; // ADD HL, BC
    case 0x0A:
      ld_a_bc();
      break; // LD A, (BC)
    case 0x0B:
      dec_bc();
      break; // DEC BC
    case 0x0C:
      inc(reg_c);
      break; // INC C
    case 0x0D:
      dec(reg_c);
      break; // DEC C
    case 0x0E:
      ld(reg_c, fetch8());
      break; // LD C, n8
    case 0x0F:
      rrca();
      break; // RRCA

    case 0x10:
      fetch8();
      stop();
      break; // STOP n8
    case 0x11:
      ld_de_nn(fetch16());
      break; // LD DE, nn16
    case 0x12:
      ld_de_a();
      break; // LD (DE), A
    case 0x13:
      inc_de();
      break; // INC DE
    case 0x14:
      inc(reg_d);
      break; // INC D
    case 0x15:
      dec(reg_d);
      break; // DEC D
    case 0x16:
      ld(reg_d, fetch8());
      break; // LD D, n8
    case 0x17:
      rla();
      break; // RLA
    case 0x18:
      jr(static_cast<int8_t>(fetch8()));
      break; // JR r8
    case 0x19:
      add_hl(reg_de());
      break; // ADD HL, DE
    case 0x1A:
      ld_a_de();
      break; // LD A, (DE)
    case 0x1B:
      dec_de();
      break; // DEC DE
    case 0x1C:
      inc(reg_e);
      break; // INC E
    case 0x1D:
      dec(reg_e);
      break; // DEC E
    case 0x1E:
      ld(reg_e, fetch8());
      break; // LD E, n8
    case 0x1F:
      rra();
      break; // RRA

    case 0x20:
      jr_nz(static_cast<int8_t>(fetch8()));
      break; // JR NZ, r8
    case 0x21:
      ld_hl_nn(fetch16());
      break; // LD HL, nn16
    case 0x22:
      ld_hl_inc_a();
      break; // LD (HL+), A
    case 0x23:
      inc_hl();
      break; // INC HL
    case 0x24:
      inc(reg_h);
      break; // INC H
    case 0x25:
      dec(reg_h);
      break; // DEC H
    case 0x26:
      ld(reg_h, fetch8());
      break; // LD H, n8
    case 0x27:
      daa();
      break; // DAA
    case 0x28:
      jr_z(static_cast<int8_t>(fetch8()));
      break; // JR Z, r8
    case 0x29:
      add_hl(reg_hl());
      break; // ADD HL, HL
    case 0x2A:
      ld_a_hl_inc();
      break; // LD A, (HL+)
    case 0x2B:
      dec_hl();
      break; // DEC HL
    case 0x2C:
      inc(reg_l);
      break; // INC L
    case 0x2D:
      dec(reg_l);
      break; // DEC L
    case 0x2E:
      ld(reg_l, fetch8());
      break; // LD L, n8
    case 0x2F:
      cpl();
      break; // CPL

    case 0x30:
      jr_nc(static_cast<int8_t>(fetch8()));
      break; // JR NC, r8
    case 0x31:
      ld_sp_nn(fetch16());
      break; // LD SP, nn16
    case 0x32:
      ld_hl_dec_a();
      break; // LD (HL-), A
    case 0x33:
      inc_sp();
      break; // INC SP
    case 0x34:
      inc_hl_mem();
      break; // INC (HL)
    case 0x35:
      dec_hl_mem();
      break; // DEC (HL)
    case 0x36:
      ld_r8_to_hl(fetch8());
      break; // LD (HL), n8
    case 0x37:
      scf();
      break; // SCF
    case 0x38:
      jr_c(static_cast<int8_t>(fetch8()));
      break; // JR C, r8
    case 0x39:
      add_hl(reg_sp);
      break; // ADD HL, SP
    case 0x3A:
      ld_a_hl_dec();
      break; // LD A, (HL-)
    case 0x3B:
      dec_sp();
      break; // DEC SP
    case 0x3C:
      inc(reg_a);
      break; // INC A
    case 0x3D:
      dec(reg_a);
      break; // DEC A
    case 0x3E:
      ld(reg_a, fetch8());
      break; // LD A, n8
    case 0x3F:
      ccf();
      break; // CCF

    // LD r8, r8' (0x40-0x7F)
    case 0x40:
    case 0x41:
    case 0x42:
    case 0x43:
    case 0x44:
    case 0x45:
    case 0x46:
    case 0x47:
    case 0x48:
    case 0x49:
    case 0x4A:
    case 0x4B:
    case 0x4C:
    case 0x4D:
    case 0x4E:
    case 0x4F:
    case 0x50:
    case 0x51:
    case 0x52:
    case 0x53:
    case 0x54:
    case 0x55:
    case 0x56:
    case 0x57:
    case 0x58:
    case 0x59:
    case 0x5A:
    case 0x5B:
    case 0x5C:
    case 0x5D:
    case 0x5E:
    case 0x5F:
    case 0x60:
    case 0x61:
    case 0x62:
    case 0x63:
    case 0x64:
    case 0x65:
    case 0x66:
    case 0x67:
    case 0x68:
    case 0x69:
    case 0x6A:
    case 0x6B:
    case 0x6C:
    case 0x6D:
    case 0x6E:
    case 0x6F:
    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73:
    case 0x74:
    case 0x75:
    case 0x77:
    case 0x78:
    case 0x79:
    case 0x7A:
    case 0x7B:
    case 0x7C:
    case 0x7D:
    case 0x7E:
    case 0x7F: {
      const uint8_t dst = (opcode >> 3) & 0x07;
      const uint8_t src = opcode & 0x07;
      if (dst == 6) {
        ld_r8_to_hl(src == 6 ? reg_a : reg_r8(src)); // LD (HL), r8
      } else if (src == 6) {
        ld_hl_to_r8(reg_r8(dst)); // LD r8, (HL)
      } else {
        ld(reg_r8(dst), reg_r8(src)); // LD r8, r8'
      }
      break;
    }
    case 0x76:
      halt();
      break; // HALT

    // 8-bit ALU on A (0x80-0xBF)
    case 0x80:
    case 0x81:
    case 0x82:
    case 0x83:
    case 0x84:
    case 0x85:
    case 0x87:
    case 0x88:
    case 0x89:
    case 0x8A:
    case 0x8B:
    case 0x8C:
    case 0x8D:
    case 0x8F:
    case 0x90:
    case 0x91:
    case 0x92:
    case 0x93:
    case 0x94:
    case 0x95:
    case 0x97:
    case 0x98:
    case 0x99:
    case 0x9A:
    case 0x9B:
    case 0x9C:
    case 0x9D:
    case 0x9F:
    case 0xA0:
    case 0xA1:
    case 0xA2:
    case 0xA3:
    case 0xA4:
    case 0xA5:
    case 0xA7:
    case 0xA8:
    case 0xA9:
    case 0xAA:
    case 0xAB:
    case 0xAC:
    case 0xAD:
    case 0xAF:
    case 0xB0:
    case 0xB1:
    case 0xB2:
    case 0xB3:
    case 0xB4:
    case 0xB5:
    case 0xB7:
    case 0xB8:
    case 0xB9:
    case 0xBA:
    case 0xBB:
    case 0xBC:
    case 0xBD:
    case 0xBF: {
      alu8((opcode - 0x80) >> 3, opcode & 0x07);
      break;
    }
    case 0x86:
    case 0x8E:
    case 0x96:
    case 0x9E:
    case 0xA6:
    case 0xAE:
    case 0xB6:
    case 0xBE: {
      alu8((opcode - 0x80) >> 3, 6); // (HL) source
      break;
    }

    case 0xC0:
      if (!z())
        ret();
      break; // RET NZ
    case 0xC1:
      pop_bc();
      break; // POP BC
    case 0xC2:
      if (!z())
        jp(fetch16());
      else
        fetch16();
      break; // JP NZ, nn16
    case 0xC3:
      jp(fetch16());
      break; // JP nn16
    case 0xC4:
      if (!z())
        call(fetch16());
      else
        fetch16();
      break; // CALL NZ, nn16
    case 0xC5:
      push_bc();
      break; // PUSH BC
    case 0xC6:
      add(fetch8());
      break; // ADD A, n8
    case 0xC7:
      rst(0x00);
      break; // RST 00H
    case 0xC8:
      if (z())
        ret();
      break; // RET Z
    case 0xC9:
      ret();
      break; // RET
    case 0xCA:
      if (z())
        jp(fetch16());
      else
        fetch16();
      break; // JP Z, nn16
    case 0xCB:
      execute_cb();
      break; // CB prefix
    case 0xCC:
      if (z())
        call(fetch16());
      else
        fetch16();
      break; // CALL Z, nn16
    case 0xCD:
      call(fetch16());
      break; // CALL nn16
    case 0xCE:
      adc(fetch8());
      break; // ADC A, n8
    case 0xCF:
      rst(0x08);
      break; // RST 08H
    case 0xD0:
      if (!c())
        ret();
      break; // RET NC
    case 0xD1:
      pop_de();
      break; // POP DE
    case 0xD2:
      if (!c())
        jp(fetch16());
      else
        fetch16();
      break; // JP NC, nn16
    case 0xD3:
      halt();
      break; // illegal
    case 0xD4:
      if (!c())
        call(fetch16());
      else
        fetch16();
      break; // CALL NC, nn16
    case 0xD5:
      push_de();
      break; // PUSH DE
    case 0xD6:
      sub(fetch8());
      break; // SUB n8
    case 0xD7:
      rst(0x10);
      break; // RST 10H
    case 0xD8:
      if (c())
        ret();
      break; // RET C
    case 0xD9:
      reti();
      break; // RETI
    case 0xDA:
      if (c())
        jp(fetch16());
      else
        fetch16();
      break; // JP C, nn16
    case 0xDB:
      halt();
      break; // illegal
    case 0xDC:
      if (c())
        call(fetch16());
      else
        fetch16();
      break; // CALL C, nn16
    case 0xDD:
      halt();
      break; // illegal
    case 0xDE:
      sbc(fetch8());
      break; // SBC A, n8
    case 0xDF:
      rst(0x18);
      break; // RST 18H
    case 0xE0:
      ldh_n_a(fetch8());
      break; // LDH (n8), A
    case 0xE1:
      pop_hl();
      break; // POP HL
    case 0xE2:
      ld_c_a();
      break; // LD (C), A
    case 0xE3:
      halt();
      break; // illegal
    case 0xE4:
      halt();
      break; // illegal
    case 0xE5:
      push_hl();
      break; // PUSH HL
    case 0xE6:
      and_a(fetch8());
      break; // AND n8
    case 0xE7:
      rst(0x20);
      break; // RST 20H
    case 0xE8:
      add_sp_r8(static_cast<int8_t>(fetch8()));
      break; // ADD SP, r8
    case 0xE9:
      jp(reg_hl());
      break; // JP (HL)
    case 0xEA:
      ld_nn_a(fetch16());
      break; // LD (nn16), A
    case 0xEB:
      halt();
      break; // illegal
    case 0xEC:
      halt();
      break; // illegal
    case 0xED:
      halt();
      break; // illegal
    case 0xEE:
      xor_a(fetch8());
      break; // XOR n8
    case 0xEF:
      rst(0x28);
      break; // RST 28H
    case 0xF0:
      ldh_a_n(fetch8());
      break; // LDH A, (n8)
    case 0xF1:
      pop_af();
      break; // POP AF
    case 0xF2:
      ld_a_c();
      break; // LD A, (C)
    case 0xF3:
      di();
      break; // DI
    case 0xF4:
      halt();
      break; // illegal
    case 0xF5:
      push_af();
      break; // PUSH AF
    case 0xF6:
      or_a(fetch8());
      break; // OR n8
    case 0xF7:
      rst(0x30);
      break; // RST 30H
    case 0xF8:
      ld_hl_sp_r8(static_cast<int8_t>(fetch8()));
      break; // LD HL, SP+r8
    case 0xF9:
      ld_sp_hl();
      break; // LD SP, HL
    case 0xFA:
      ld_a_nn(fetch16());
      break; // LD A, (nn16)
    case 0xFB:
      ei();
      break; // EI
    case 0xFC:
      halt();
      break; // illegal
    case 0xFD:
      halt();
      break; // illegal
    case 0xFE:
      cp(fetch8());
      break; // CP n8
    case 0xFF:
      rst(0x38);
      break; // RST 38H
    }
  }

  void boot() {
    reg_sp = 0xFFFE;
    reg_pc = 0x0100;
    reg_a = 0x01;
    reg_f = 0xB0;
  }

  void load_rom(const std::vector<uint8_t> &rom_buffer) {
    bus.load_rom(rom_buffer);

    reg_pc = 0x0100; // Past the ROM header...
  }

  uint8_t peek(const uint16_t addr) const { return bus.read(addr); }

  void debug_state() const {
    std::cout << std::hex << std::uppercase << "A:" << std::setw(2)
              << std::setfill('0') << static_cast<int>(reg_a)
              << " | F:" << std::setw(2) << static_cast<int>(reg_f)
              << " | B:" << std::setw(2) << static_cast<int>(reg_b)
              << " | C:" << std::setw(2) << static_cast<int>(reg_c)
              << " | D:" << std::setw(2) << static_cast<int>(reg_d)
              << " | E:" << std::setw(2) << static_cast<int>(reg_e)
              << " | H:" << std::setw(2) << static_cast<int>(reg_h)
              << " | L:" << std::setw(2) << static_cast<int>(reg_l) << '\n';

    std::cout << "BC:" << std::setw(4) << reg_bc() << " | DE:" << std::setw(4)
              << reg_de() << " | HL:" << std::setw(4) << reg_hl()
              << " | PC:" << std::setw(4) << reg_pc << " | SP:" << std::setw(4)
              << reg_sp << '\n';

    std::cout << "IR:" << std::setw(2) << static_cast<int>(reg_ir)
              << " | IE:" << std::setw(2) << static_cast<int>(reg_ie) << '\n';

    std::cout << "Flags:"
              << " | Z:" << z() << " | N:" << n() << " | H:" << h()
              << " | C:" << c() << '\n';

    std::cout << std::dec;
  }
};

// SM83 Emulator
int main(int argc, char *argv[]) {
  if (argc < 2) {
    throw std::runtime_error("usage: sm83 <rom.gb>");
  }

  std::string filename = argv[1];

  if (filename.empty()) {
    throw std::runtime_error("provide a correct filename");
  }

  std::ifstream rom_file(filename, std::ios::binary | std::ios::ate);

  if (!rom_file) {
    throw std::runtime_error("error while opening rom file");
  }

  std::streamsize rom_size = rom_file.tellg();
  rom_file.seekg(0, std::ios::beg);

  std::vector<uint8_t> rom_buffer(rom_size);

  rom_file.read(reinterpret_cast<char *>(rom_buffer.data()), rom_size);

  std::cout << "Loaded ROM file of " << rom_size << " bytes...\n";

  SM83 core{};

  core.boot();
  core.load_rom(rom_buffer);

  uint64_t total = 0;
  while (true) {
    core.execute();
    core.handle_interrupts();
    if ((++total % (1000000)) == 0) {
      core.debug_state();
    }
  }

  return 0;
}
