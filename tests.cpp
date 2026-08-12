// tests.cpp - Unit tests for the SM83 core (sm83.cpp)
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

#define private public
#define main sm83_program_main
#include "sm83.cpp"
#undef main
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
    c.memory[(addr + i) & 0xFFFF] = bytes[i];
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
  
  c.memory[0x0020] = 0xC9; // RET (16 cycles)
  c.execute();
  CHECK(c.reg_pc == 0x1003);
  CHECK(c.total_cycles == 40);
}

void test_cb_timing() {
  SM83 c{};
  c.set_reg_hl(0x8000);
  c.memory[0x8000] = 0x40;
  load_program(c, 0, {0xCB, 0x06}); // RLC (HL) (16 cycles)
  c.total_cycles = 0;
  c.execute();
  CHECK(c.memory[0x8000] == 0x80 && c.total_cycles == 16);
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

} // namespace

int main() {
  test_arithmetic();
  test_subroutines();
  test_cb_timing();
  test_jr_timing();
  test_interrupt_registers();

  std::cout << "SM83 timing tests: " << g_checks << " checks, " << g_failures
            << " failures\n";
  return g_failures == 0 ? 0 : 1;
}
