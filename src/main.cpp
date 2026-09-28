#include "sm83.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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
