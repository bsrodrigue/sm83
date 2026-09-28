// tests.cpp - Unit tests for the SM83 core (sm83.cpp)
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

#define private public
#include "sm83.hpp"
#undef private

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failures;                                                          \
      std::cout << "FAIL " << __FILE__ << ":" << __LINE__ << "  " << #cond   \
                << '\n';                                                     \
    }                                                                        \
  } while (0)

void load_program(SM83 &c, uint16_t addr, const std::vector<uint8_t> &bytes) {
  for (size_t i = 0; i < bytes.size(); ++i) {
    c.bus.write((addr + i) & 0xFFFF, bytes[i]);
  }
  c.reg_pc = addr;
}

void test_arithmetic() {
  SM83 c{};
  
  // Test via decoder to see full cycle count
  load_program(c, 0, {0x3E, 0x05, 0x80}); // LD A,5 (8) ; ADD A,B (4)
  c.reg_b = 0x03;
  c.total_cycles = 0;
  c.execute(); // LD A,5
  CHECK(c.reg_a == 0x05 && c.total_cycles == 8);
  c.execute(); // ADD A,B
  CHECK(c.reg_a == 0x08 && c.total_cycles == 12);
}

void test_subroutines() {
  SM83 c{};
  c.reg_sp = 0xFFFE;
  load_program(c, 0x1000, {0xCD, 0x20, 0x00}); // CALL 0x0020 (24 cycles)
  c.total_cycles = 0;
  c.execute();
  CHECK(c.reg_pc == 0x0020);
  CHECK(c.reg_sp == 0xFFFC);
  CHECK(c.total_cycles == 24);
  
  c.bus.write(0x0020, 0xC9); // RET (16 cycles)
  c.execute();
  CHECK(c.reg_pc == 0x1003);
  CHECK(c.total_cycles == 40);
}

void test_cb_timing() {
  SM83 c{};
  c.set_reg_hl(0x8000);
  c.bus.write(0x8000, 0x40);
  load_program(c, 0, {0xCB, 0x06}); // RLC (HL) (16 cycles)
  c.total_cycles = 0;
  c.execute();
  CHECK(c.bus.read(0x8000) == 0x80 && c.total_cycles == 16);
}

void test_jr_timing() {
  SM83 c{};
  // JR NZ, r8: Taken (12 cycles), Not Taken (8 cycles)
  load_program(c, 0, {0x20, 0x02, 0x00, 0x00, 0x20, 0x02}); 
  
  c.reg_a = 1; c.sub(1); // Z=1
  c.reg_pc = 0;
  c.total_cycles = 0;
  c.execute(); // JR NZ, 2 (Not taken)
  CHECK(c.reg_pc == 2 && c.total_cycles == 8);
  
  c.reg_a = 1; c.sub(0); // Z=0
  c.reg_pc = 4;
  c.total_cycles = 0;
  c.execute(); // JR NZ, 2 (Taken: PC = 6 + 2 = 8)
  CHECK(c.reg_pc == 8 && c.total_cycles == 12);
}

void test_interrupt_registers() {
  SM83 c{};
  // IF and IE are standard memory addresses $FF0F and $FFFF
  c.write8(0xFFFF, 0x01); // Enable V-Blank
  CHECK(c.read8(0xFFFF) == 0x01);
  CHECK(c.total_cycles == 8); // Two 4-cycle ops
  
  c.write8(0xFF0F, 0x04); // Request Timer
  CHECK(c.read8(0xFF0F) == 0x04);
}

void test_bus_memory_map() {
  Bus bus{};

  // 1. Cartridge ROM area ($0000 - $7FFF)
  bus.write(0x0000, 0x12);
  bus.write(0x7FFF, 0x34);
  CHECK(bus.read(0x0000) == 0x12);
  CHECK(bus.read(0x7FFF) == 0x34);

  // 2. Video RAM ($8000 - $9FFF)
  bus.write(0x8000, 0xAA);
  bus.write(0x9FFF, 0x55);
  CHECK(bus.read(0x8000) == 0xAA);
  CHECK(bus.read(0x9FFF) == 0x55);

  // 3. External RAM ($A000 - $BFFF)
  bus.write(0xA000, 0xBE);
  bus.write(0xBFFF, 0xEF);
  CHECK(bus.read(0xA000) == 0xBE);
  CHECK(bus.read(0xBFFF) == 0xEF);

  // 4. Work RAM ($C000 - $DFFF)
  bus.write(0xC000, 0x42);
  bus.write(0xDFFF, 0x99);
  CHECK(bus.read(0xC000) == 0x42);
  CHECK(bus.read(0xDFFF) == 0x99);

  // 5. Echo RAM mirroring ($E000 - $FDFF mirrors $C000 - $DDFF)
  // Writing to WRAM should be visible in Echo RAM
  bus.write(0xC050, 0x77);
  CHECK(bus.read(0xE050) == 0x77);

  // Writing to Echo RAM should be visible in WRAM
  bus.write(0xE100, 0x88);
  CHECK(bus.read(0xC100) == 0x88);

  // 6. Object Attribute Memory ($FE00 - $FE9F)
  bus.write(0xFE00, 0x11);
  bus.write(0xFE9F, 0x22);
  CHECK(bus.read(0xFE00) == 0x11);
  CHECK(bus.read(0xFE9F) == 0x22);

  // 7. Unusable memory ($FEA0 - $FEFF)
  bus.write(0xFEA0, 0x33);
  CHECK(bus.read(0xFEA0) == 0xFF);

  // 8. I/O Registers ($FF00 - $FF7F)
  bus.write(0xFF00, 0x0F);
  bus.write(0xFF40, 0x91); // LCDC
  CHECK(bus.read(0xFF00) == 0x0F);
  CHECK(bus.read(0xFF40) == 0x91);

  // 9. High RAM / Zero Page ($FF80 - $FFFE)
  bus.write(0xFF80, 0xAB);
  bus.write(0xFFFE, 0xCD);
  CHECK(bus.read(0xFF80) == 0xAB);
  CHECK(bus.read(0xFFFE) == 0xCD);

  // 10. Interrupt Enable ($FFFF)
  bus.write(0xFFFF, 0x1F);
  CHECK(bus.read(0xFFFF) == 0x1F);
}

void test_timer() {
  Bus bus{};

  // 1. DIV starts at 0 and increments every 256 cycles
  CHECK(bus.read(0xFF04) == 0x00);
  bus.tick(252);
  CHECK(bus.read(0xFF04) == 0x00);
  bus.tick(4);
  CHECK(bus.read(0xFF04) == 0x01);

  // 2. Writing any value to DIV resets it and its accumulator
  bus.tick(200);
  bus.write(0xFF04, 0xAB);
  CHECK(bus.read(0xFF04) == 0x00);
  bus.tick(255);
  CHECK(bus.read(0xFF04) == 0x00);
  bus.tick(1);
  CHECK(bus.read(0xFF04) == 0x01);

  // 3. TIMA does not count when TAC bit 2 is disabled
  bus.write(0xFF05, 0x10); // TIMA = 0x10
  bus.write(0xFF07, 0x01); // Rate: 16 cycles, but Enable bit 2 = 0
  bus.tick(64);
  CHECK(bus.read(0xFF05) == 0x10);

  // 4. Enable TIMA at 16 cycles/tick (TAC = 0x05)
  bus.write(0xFF07, 0x05); // Bit 2 (enable) | 01 (16 cycles)
  bus.tick(16);
  CHECK(bus.read(0xFF05) == 0x11);
  bus.tick(32);
  CHECK(bus.read(0xFF05) == 0x13);

  // 5. TIMA overflow: reloads TMA and requests Timer interrupt (IF bit 2 = 0x04)
  bus.write(0xFF06, 0x80); // TMA modulo reload = 0x80
  bus.write(0xFF05, 0xFE); // TIMA = 0xFE
  bus.write(0xFF0F, 0x00); // Clear interrupt flags

  bus.tick(16); // TIMA: 0xFE -> 0xFF
  CHECK(bus.read(0xFF05) == 0xFF);
  CHECK((bus.read(0xFF0F) & 0x04) == 0);

  bus.tick(16); // TIMA: 0xFF -> overflow -> reloads TMA (0x80) + requests interrupt
  CHECK(bus.read(0xFF05) == 0x80);
  CHECK((bus.read(0xFF0F) & 0x04) == 0x04);
}

void test_halt() {
  SM83 c{};
  load_program(c, 0x1000, {0x76, 0x3E, 0x42}); // HALT ; LD A, 0x42

  c.write8(0xFFFF, 0x04); // Enable Timer interrupt in IE
  c.write8(0xFF0F, 0x00); // Clear all IF flags
  c.reg_ime = false;      // Disable master interrupts
  c.reg_pc = 0x1000;
  c.total_cycles = 0;

  // 1. Execute HALT instruction
  c.execute();
  CHECK(c.is_on_halt == true);
  CHECK(c.reg_pc == 0x1001); // HALT opcode was fetched

  // 2. Subsequent execution while halted: PC stays fixed, 4 cycles elapse
  c.execute();
  CHECK(c.is_on_halt == true);
  CHECK(c.reg_pc == 0x1001);
  CHECK(c.total_cycles == 8); // 4 cycles for fetch8(HALT) + 4 idle cycles

  // 3. Request Timer interrupt in IF (0x04)
  c.bus.interrupts.request_interrupt(0x04);

  // 4. Interrupt handler checks wakeup condition
  c.handle_interrupts();
  CHECK(c.is_on_halt == false); // HALT state cancelled!
  CHECK(c.reg_pc == 0x1001);   // IME was false, so PC did not jump to vector

  // 5. Next execute runs the instruction following HALT (LD A, 0x42)
  c.execute();
  CHECK(c.reg_a == 0x42);
  CHECK(c.reg_pc == 0x1003);
}

} // namespace

int main() {
  test_arithmetic();
  test_subroutines();
  test_cb_timing();
  test_jr_timing();
  test_interrupt_registers();
  test_bus_memory_map();
  test_timer();
  test_halt();

  std::cout << "SM83 timing tests: " << g_checks << " checks, " << g_failures
            << " failures\n";
  return g_failures == 0 ? 0 : 1;
}
