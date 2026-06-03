/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : mc6809_opcodes.h
 *  Module : MC6809 opcode definitions and cycle count reference tables (PROGMEM-ready)
 * ============================================================
*/

#ifndef MC6809_OPCODES_H
#define MC6809_OPCODES_H

#include <Arduino.h>

// ============================================================
// Page 1 cycle counts (256 entries)
// 0 = illegal/undefined opcode
// ============================================================
const uint8_t PROGMEM mc6809_cycles_page1[256] = {
//  x0  x1  x2  x3  x4  x5  x6  x7  x8  x9  xA  xB  xC  xD  xE  xF
     6,  0,  0,  6,  6,  0,  6,  6,  6,  6,  6,  0,  6,  6,  3,  6, // 0x
     0,  0,  2,  4,  0,  0,  5,  9,  0,  2,  3,  0,  3,  2,  8,  6, // 1x
     3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3, // 2x
     4,  4,  4,  4,  5,  5,  5,  5,  0,  5,  3,  6, 20, 11,  0, 19, // 3x
     2,  0,  0,  2,  2,  0,  2,  2,  2,  2,  2,  0,  2,  2,  0,  2, // 4x
     2,  0,  0,  2,  2,  0,  2,  2,  2,  2,  2,  0,  2,  2,  0,  2, // 5x
     6,  0,  0,  6,  6,  0,  6,  6,  6,  6,  6,  0,  6,  6,  3,  6, // 6x
     7,  0,  0,  7,  7,  0,  7,  7,  7,  7,  7,  0,  7,  7,  4,  7, // 7x
     2,  2,  2,  4,  2,  2,  2,  0,  2,  2,  2,  2,  4,  7,  3,  0, // 8x
     4,  4,  4,  6,  4,  4,  4,  4,  4,  4,  4,  4,  6,  7,  5,  5, // 9x
     4,  4,  4,  6,  4,  4,  4,  4,  4,  4,  4,  4,  6,  7,  5,  5, // Ax
     5,  5,  5,  7,  5,  5,  5,  5,  5,  5,  5,  5,  7,  8,  6,  6, // Bx
     2,  2,  2,  4,  2,  2,  2,  0,  2,  2,  2,  2,  3,  0,  3,  0, // Cx
     4,  4,  4,  6,  4,  4,  4,  4,  4,  4,  4,  4,  5,  5,  5,  5, // Dx
     4,  4,  4,  6,  4,  4,  4,  4,  4,  4,  4,  4,  5,  5,  5,  5, // Ex
     5,  5,  5,  7,  5,  5,  5,  5,  5,  5,  5,  5,  6,  6,  6,  6, // Fx
};

// ============================================================
// Opcode mnemonics for debug/trace (optional, not used in tests)
// ============================================================

// MC6809 NOP opcode value
#define MC6809_OP_NOP   0x12

// SWI opcode value (used as halt/breakpoint in tests)
#define MC6809_OP_SWI   0x3F

// Common opcode values
#define MC6809_OP_LDA_IMM   0x86
#define MC6809_OP_LDB_IMM   0xC6
#define MC6809_OP_LDX_IMM   0x8E
#define MC6809_OP_LDD_IMM   0xCC
#define MC6809_OP_TFR        0x1F
#define MC6809_OP_EXG        0x1E
#define MC6809_OP_BRA        0x20
#define MC6809_OP_BNE        0x26
#define MC6809_OP_BSR        0x8D
#define MC6809_OP_RTS        0x39
#define MC6809_OP_PSHS       0x34
#define MC6809_OP_PULS       0x35
#define MC6809_OP_DECA       0x4A
#define MC6809_OP_CLRA       0x4F
#define MC6809_OP_CLRB       0x5F
#define MC6809_OP_ADDA_IMM   0x8B

// Page prefixes
#define MC6809_OP_PAGE2      0x10
#define MC6809_OP_PAGE3      0x11

// TFR/EXG register codes
#define MC6809_REG_D    0x0
#define MC6809_REG_X    0x1
#define MC6809_REG_Y    0x2
#define MC6809_REG_U    0x3
#define MC6809_REG_S    0x4
#define MC6809_REG_PC   0x5
#define MC6809_REG_A    0x8
#define MC6809_REG_B    0x9
#define MC6809_REG_CC   0xA
#define MC6809_REG_DP   0xB

#endif // MC6809_OPCODES_H
