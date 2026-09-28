  ### Recommended Implementation Roadmap

  #### 1. Hardware Timers & Divider ($FF04–$FF07)

  • DIV ($FF04): Increments every 256 T-cycles (16,384 Hz). Writing any value resets it to 0.
  • TIMA ($FF05), TMA ($FF06), TAC ($FF07): Timer counter configured by TAC clock select and enable bits; requests Timer Interrupt (IF bit 2) when TIMA overflows and reloads from TMA.
  • Hook to cycle ticks: Drive these directly inside sm83.cpp:410-413.

  #### 2. Essential I/O Registers & DMA ($FF00, $FF46)

  • OAM DMA ($FF46): Writing byte xx triggers copying 160 bytes from $xx00–$xx9F to $FE00–$FE9F. This is required for sprites to populate.
  • Joypad Register ($FF00): Action / Direction matrix selection bits so game input loops do not stall.

  #### 3. PPU Timing State Machine & Scanline Counter (LY $FF44, STAT $FF41)

  • Even before pixel drawing is hooked up to a window/screen, implement the PPU mode cycle progression:
      • Mode 2 (OAM Search): 80 T-cycles
      • Mode 3 (Pixel Transfer): 172–289 T-cycles
      • Mode 0 (H-Blank): 204–87 T-cycles
      • Mode 1 (V-Blank): Lines 144–153 (4560 T-cycles total)
  • Increment LY ($FF44) every 456 T-cycles (reset at line 154).
  • Request V-Blank Interrupt (IF bit 0) when LY == 144.
  • Update STAT mode bits and trigger STAT Interrupt (IF bit 1) on LY=LYC or mode transitions.

                                                                                                                                                                                                 #### 4. Graphics & LCD Rendering (PPU)

  • Once LY and V-Blank allow the game loop to run and populate VRAM/OAM:
      • Implement background/window tile line renderer (LCDC $FF40, SCX/SCY $FF42/$FF43, BGP $FF47).
      • Implement sprite scanline renderer (OBP0/OBP1 $FF48/$FF49).
      • Output the frame buffer (160×144) to a lightweight frontend (e.g. SDL2, Raylib, or Minifb).
