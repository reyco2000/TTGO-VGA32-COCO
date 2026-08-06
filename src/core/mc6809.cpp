#pragma GCC optimize("O2")
/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : mc6809.cpp
 *  Module : Motorola MC6809 CPU emulation — full opcode set with accurate cycle counts
 * ============================================================
*/

#include "mc6809.h"

// ============================================================
// Register access macros
// ============================================================
#define GET_A()    ((uint8_t)(cpu->d >> 8))
#define GET_B()    ((uint8_t)(cpu->d & 0xFF))
#define SET_A(v)   do { cpu->d = (cpu->d & 0x00FF) | ((uint16_t)(uint8_t)(v) << 8); } while(0)
#define SET_B(v)   do { cpu->d = (cpu->d & 0xFF00) | (uint8_t)(v); } while(0)

#define CC_SET(f)    (cpu->cc |= (f))
#define CC_CLR(f)    (cpu->cc &= ~(f))
#define CC_TST(f)    (cpu->cc & (f))
// Branchless CC_PUT: ternary compiles to conditional-move on Xtensa (MOVNEZ)
#define CC_PUT(f,c)  do { cpu->cc = (cpu->cc & ~(f)) | ((c) ? (f) : 0); } while(0)

// ============================================================
// Memory access helpers
// ============================================================
static inline uint8_t mem_read(MC6809* cpu, uint16_t addr) {
    return cpu->read(addr);
}

static inline void mem_write(MC6809* cpu, uint16_t addr, uint8_t val) {
    cpu->write(addr, val);
}

static inline uint16_t mem_read16(MC6809* cpu, uint16_t addr) {
    uint8_t hi = mem_read(cpu, addr);
    uint8_t lo = mem_read(cpu, addr + 1);
    return ((uint16_t)hi << 8) | lo;
}

static inline void mem_write16(MC6809* cpu, uint16_t addr, uint16_t val) {
    mem_write(cpu, addr, (uint8_t)(val >> 8));
    mem_write(cpu, addr + 1, (uint8_t)(val & 0xFF));
}

static inline uint8_t fetch8(MC6809* cpu) {
    return mem_read(cpu, cpu->pc++);
}

static inline uint16_t fetch16(MC6809* cpu) {
    uint8_t hi = fetch8(cpu);
    uint8_t lo = fetch8(cpu);
    return ((uint16_t)hi << 8) | lo;
}

// ============================================================
// Stack helpers (hardware stack S)
// ============================================================
static inline void push8s(MC6809* cpu, uint8_t val) {
    cpu->s--;
    mem_write(cpu, cpu->s, val);
}

static inline uint8_t pull8s(MC6809* cpu) {
    uint8_t val = mem_read(cpu, cpu->s);
    cpu->s++;
    return val;
}

static inline void push16s(MC6809* cpu, uint16_t val) {
    push8s(cpu, (uint8_t)(val & 0xFF));
    push8s(cpu, (uint8_t)(val >> 8));
}

static inline uint16_t pull16s(MC6809* cpu) {
    uint16_t hi = pull8s(cpu);
    uint16_t lo = pull8s(cpu);
    return (hi << 8) | lo;
}

// Stack helpers (user stack U)
static inline void push8u(MC6809* cpu, uint8_t val) {
    cpu->u--;
    mem_write(cpu, cpu->u, val);
}

static inline uint8_t pull8u(MC6809* cpu) {
    uint8_t val = mem_read(cpu, cpu->u);
    cpu->u++;
    return val;
}

static inline void push16u(MC6809* cpu, uint16_t val) {
    push8u(cpu, (uint8_t)(val & 0xFF));
    push8u(cpu, (uint8_t)(val >> 8));
}

static inline uint16_t pull16u(MC6809* cpu) {
    uint16_t hi = pull8u(cpu);
    uint16_t lo = pull8u(cpu);
    return (hi << 8) | lo;
}

// ============================================================
// CC flag update helpers
// ============================================================

// Update N and Z for 8-bit result (compute-and-mask: single cc write)
static inline void update_nz8(MC6809* cpu, uint8_t val) {
    uint8_t f = 0;
    if (val & 0x80)  f |= MC6809_FLAG_N;
    if (val == 0)    f |= MC6809_FLAG_Z;
    cpu->cc = (cpu->cc & ~(MC6809_FLAG_N | MC6809_FLAG_Z)) | f;
}

// Update N and Z for 16-bit result (compute-and-mask: single cc write)
static inline void update_nz16(MC6809* cpu, uint16_t val) {
    uint8_t f = 0;
    if (val & 0x8000) f |= MC6809_FLAG_N;
    if (val == 0)     f |= MC6809_FLAG_Z;
    cpu->cc = (cpu->cc & ~(MC6809_FLAG_N | MC6809_FLAG_Z)) | f;
}

// 8-bit addition: H, N, Z, V, C (compute-and-mask: single cc write)
static inline uint8_t op_add8(MC6809* cpu, uint8_t a, uint8_t b, uint8_t carry) {
    uint16_t result = (uint16_t)a + (uint16_t)b + carry;
    uint8_t r8 = (uint8_t)result;

    uint8_t f = 0;
    if ((a ^ b ^ r8) & 0x10)         f |= MC6809_FLAG_H;
    if (r8 & 0x80)                   f |= MC6809_FLAG_N;
    if (r8 == 0)                     f |= MC6809_FLAG_Z;
    if ((a ^ r8) & (b ^ r8) & 0x80) f |= MC6809_FLAG_V;
    if (result & 0x100)              f |= MC6809_FLAG_C;
    cpu->cc = (cpu->cc & ~(MC6809_FLAG_H | MC6809_FLAG_N | MC6809_FLAG_Z
                         | MC6809_FLAG_V | MC6809_FLAG_C)) | f;
    return r8;
}

// 8-bit subtraction: N, Z, V, C (compute-and-mask: single cc write, H preserved)
static inline uint8_t op_sub8(MC6809* cpu, uint8_t a, uint8_t b, uint8_t carry) {
    uint16_t result = (uint16_t)a - (uint16_t)b - carry;
    uint8_t r8 = (uint8_t)result;

    uint8_t f = 0;
    if (r8 & 0x80)                   f |= MC6809_FLAG_N;
    if (r8 == 0)                     f |= MC6809_FLAG_Z;
    if ((a ^ b) & (a ^ r8) & 0x80)  f |= MC6809_FLAG_V;
    if (result & 0x100)              f |= MC6809_FLAG_C;
    cpu->cc = (cpu->cc & ~(MC6809_FLAG_N | MC6809_FLAG_Z
                         | MC6809_FLAG_V | MC6809_FLAG_C)) | f;
    return r8;
}

// 16-bit addition: N, Z, V, C (compute-and-mask: single cc write)
static inline uint16_t op_add16(MC6809* cpu, uint16_t a, uint16_t b) {
    uint32_t result = (uint32_t)a + (uint32_t)b;
    uint16_t r16 = (uint16_t)result;

    uint8_t f = 0;
    if (r16 & 0x8000)                  f |= MC6809_FLAG_N;
    if (r16 == 0)                      f |= MC6809_FLAG_Z;
    if ((a ^ r16) & (b ^ r16) & 0x8000) f |= MC6809_FLAG_V;
    if (result & 0x10000)              f |= MC6809_FLAG_C;
    cpu->cc = (cpu->cc & ~(MC6809_FLAG_N | MC6809_FLAG_Z
                         | MC6809_FLAG_V | MC6809_FLAG_C)) | f;
    return r16;
}

// 16-bit subtraction: N, Z, V, C (compute-and-mask: single cc write)
static inline uint16_t op_sub16(MC6809* cpu, uint16_t a, uint16_t b) {
    uint32_t result = (uint32_t)a - (uint32_t)b;
    uint16_t r16 = (uint16_t)result;

    uint8_t f = 0;
    if (r16 & 0x8000)                  f |= MC6809_FLAG_N;
    if (r16 == 0)                      f |= MC6809_FLAG_Z;
    if ((a ^ b) & (a ^ r16) & 0x8000) f |= MC6809_FLAG_V;
    if (result & 0x10000)              f |= MC6809_FLAG_C;
    cpu->cc = (cpu->cc & ~(MC6809_FLAG_N | MC6809_FLAG_Z
                         | MC6809_FLAG_V | MC6809_FLAG_C)) | f;
    return r16;
}

// ============================================================
// Addressing mode helpers
// ============================================================

// Direct: EA = DP:nn
static inline uint16_t addr_direct(MC6809* cpu) {
    return ((uint16_t)cpu->dp << 8) | fetch8(cpu);
}

// Extended: EA = nnnn
static inline uint16_t addr_extended(MC6809* cpu) {
    return fetch16(cpu);
}

// Get pointer to indexed register from postbyte bits 6-5
static uint16_t* idx_reg(MC6809* cpu, uint8_t postbyte) {
    switch ((postbyte >> 5) & 0x03) {
        case 0: return &cpu->x;
        case 1: return &cpu->y;
        case 2: return &cpu->u;
        case 3: return &cpu->s;
    }
    return &cpu->x; // unreachable
}

// Indexed addressing: complex postbyte decoding
// Adds extra cycles to cpu->cycles
static uint16_t addr_indexed(MC6809* cpu) {
    uint8_t postbyte = fetch8(cpu);
    uint16_t ea;
    bool indirect = false;

    if (!(postbyte & 0x80)) {
        // 5-bit signed offset, no indirect
        int8_t offset = postbyte & 0x1F;
        if (offset & 0x10) offset |= 0xE0; // sign extend
        uint16_t* reg = idx_reg(cpu, postbyte);
        ea = *reg + (int16_t)offset;
        cpu->cycles += 1;
        return ea;
    }

    uint16_t* reg = idx_reg(cpu, postbyte);
    indirect = (postbyte & 0x10) != 0;
    uint8_t mode = postbyte & 0x0F;

    switch (mode) {
        case 0x00: // ,R+  (no indirect allowed)
            ea = *reg;
            (*reg)++;
            cpu->cycles += 2;
            indirect = false; // force no indirect
            return ea;

        case 0x01: // ,R++
            ea = *reg;
            (*reg) += 2;
            cpu->cycles += indirect ? 6 : 3;
            break;

        case 0x02: // ,-R  (no indirect allowed)
            (*reg)--;
            ea = *reg;
            cpu->cycles += 2;
            indirect = false;
            return ea;

        case 0x03: // ,--R
            (*reg) -= 2;
            ea = *reg;
            cpu->cycles += indirect ? 6 : 3;
            break;

        case 0x04: // ,R (zero offset)
            ea = *reg;
            cpu->cycles += indirect ? 3 : 0;
            break;

        case 0x05: // B,R
            ea = *reg + (int16_t)(int8_t)GET_B();
            cpu->cycles += indirect ? 4 : 1;
            break;

        case 0x06: // A,R
            ea = *reg + (int16_t)(int8_t)GET_A();
            cpu->cycles += indirect ? 4 : 1;
            break;

        case 0x08: { // 8-bit offset, R
            int8_t offset = (int8_t)fetch8(cpu);
            ea = *reg + (int16_t)offset;
            cpu->cycles += indirect ? 4 : 1;
            break;
        }

        case 0x09: { // 16-bit offset, R
            int16_t offset = (int16_t)fetch16(cpu);
            ea = *reg + offset;
            cpu->cycles += indirect ? 7 : 4;
            break;
        }

        case 0x0B: // D,R
            ea = *reg + cpu->d;
            cpu->cycles += indirect ? 7 : 4;
            break;

        case 0x0C: { // 8-bit offset, PCR
            int8_t offset = (int8_t)fetch8(cpu);
            ea = cpu->pc + (int16_t)offset;
            cpu->cycles += indirect ? 4 : 1;
            break;
        }

        case 0x0D: { // 16-bit offset, PCR
            int16_t offset = (int16_t)fetch16(cpu);
            ea = cpu->pc + offset;
            cpu->cycles += indirect ? 8 : 5;
            break;
        }

        case 0x0F: // Extended indirect [nnnn]
            if (indirect) {
                ea = fetch16(cpu);
                cpu->cycles += 5;
            } else {
                // Invalid mode - treat as zero offset
                ea = *reg;
                cpu->cycles += 0;
            }
            break;

        default:
            // Undefined indexed mode - treat as ,R
            ea = *reg;
            break;
    }

    if (indirect && mode != 0x0F) {
        ea = mem_read16(cpu, ea);
    } else if (mode == 0x0F) {
        // Extended indirect already set up
        ea = mem_read16(cpu, ea);
    }

    return ea;
}

// ============================================================
// TFR/EXG register access
// ============================================================

static uint16_t tfr_read_reg(MC6809* cpu, uint8_t code) {
    switch (code & 0x0F) {
        case 0x0: return cpu->d;
        case 0x1: return cpu->x;
        case 0x2: return cpu->y;
        case 0x3: return cpu->u;
        case 0x4: return cpu->s;
        case 0x5: return cpu->pc;
        case 0x8: return GET_A();
        case 0x9: return GET_B();
        case 0xA: return cpu->cc;
        case 0xB: return cpu->dp;
        default:  return 0xFF;
    }
}

static void tfr_write_reg(MC6809* cpu, uint8_t code, uint16_t val) {
    switch (code & 0x0F) {
        case 0x0: cpu->d  = val; break;
        case 0x1: cpu->x  = val; break;
        case 0x2: cpu->y  = val; break;
        case 0x3: cpu->u  = val; break;
        case 0x4: cpu->s  = val; break;
        case 0x5: cpu->pc = val; break;
        case 0x8: SET_A((uint8_t)val); break;
        case 0x9: SET_B((uint8_t)val); break;
        case 0xA: cpu->cc = (uint8_t)val; break;
        case 0xB: cpu->dp = (uint8_t)val; break;
    }
}

// ============================================================
// PSHS/PULS/PSHU/PULU helpers
// ============================================================

// Returns number of bytes pushed (for cycle counting)
static int do_pshs(MC6809* cpu, uint8_t postbyte) {
    int count = 0;
    if (postbyte & 0x80) { push16s(cpu, cpu->pc); count += 2; }
    if (postbyte & 0x40) { push16s(cpu, cpu->u);  count += 2; }
    if (postbyte & 0x20) { push16s(cpu, cpu->y);  count += 2; }
    if (postbyte & 0x10) { push16s(cpu, cpu->x);  count += 2; }
    if (postbyte & 0x08) { push8s(cpu, cpu->dp);  count += 1; }
    if (postbyte & 0x04) { push8s(cpu, GET_B());  count += 1; }
    if (postbyte & 0x02) { push8s(cpu, GET_A());  count += 1; }
    if (postbyte & 0x01) { push8s(cpu, cpu->cc);  count += 1; }
    return count;
}

static int do_puls(MC6809* cpu, uint8_t postbyte) {
    int count = 0;
    if (postbyte & 0x01) { cpu->cc = pull8s(cpu);              count += 1; }
    if (postbyte & 0x02) { SET_A(pull8s(cpu));                 count += 1; }
    if (postbyte & 0x04) { SET_B(pull8s(cpu));                 count += 1; }
    if (postbyte & 0x08) { cpu->dp = pull8s(cpu);              count += 1; }
    if (postbyte & 0x10) { cpu->x  = pull16s(cpu);             count += 2; }
    if (postbyte & 0x20) { cpu->y  = pull16s(cpu);             count += 2; }
    if (postbyte & 0x40) { cpu->u  = pull16s(cpu);             count += 2; }
    if (postbyte & 0x80) { cpu->pc = pull16s(cpu);             count += 2; }
    return count;
}

static int do_pshu(MC6809* cpu, uint8_t postbyte) {
    int count = 0;
    if (postbyte & 0x80) { push16u(cpu, cpu->pc); count += 2; }
    if (postbyte & 0x40) { push16u(cpu, cpu->s);  count += 2; }
    if (postbyte & 0x20) { push16u(cpu, cpu->y);  count += 2; }
    if (postbyte & 0x10) { push16u(cpu, cpu->x);  count += 2; }
    if (postbyte & 0x08) { push8u(cpu, cpu->dp);  count += 1; }
    if (postbyte & 0x04) { push8u(cpu, GET_B());  count += 1; }
    if (postbyte & 0x02) { push8u(cpu, GET_A());  count += 1; }
    if (postbyte & 0x01) { push8u(cpu, cpu->cc);  count += 1; }
    return count;
}

static int do_pulu(MC6809* cpu, uint8_t postbyte) {
    int count = 0;
    if (postbyte & 0x01) { cpu->cc = pull8u(cpu);              count += 1; }
    if (postbyte & 0x02) { SET_A(pull8u(cpu));                 count += 1; }
    if (postbyte & 0x04) { SET_B(pull8u(cpu));                 count += 1; }
    if (postbyte & 0x08) { cpu->dp = pull8u(cpu);              count += 1; }
    if (postbyte & 0x10) { cpu->x  = pull16u(cpu);             count += 2; }
    if (postbyte & 0x20) { cpu->y  = pull16u(cpu);             count += 2; }
    if (postbyte & 0x40) { cpu->s  = pull16u(cpu);             count += 2; }
    if (postbyte & 0x80) { cpu->pc = pull16u(cpu);             count += 2; }
    return count;
}

// ============================================================
// Branch condition evaluation
// ============================================================
static bool eval_branch(MC6809* cpu, uint8_t cond) {
    // cond is the low nibble of the branch opcode (0x0 - 0xF)
    switch (cond) {
        case 0x0: return true;                                          // BRA
        case 0x1: return false;                                         // BRN
        case 0x2: return !(CC_TST(MC6809_FLAG_C) || CC_TST(MC6809_FLAG_Z));  // BHI
        case 0x3: return  (CC_TST(MC6809_FLAG_C) || CC_TST(MC6809_FLAG_Z));  // BLS
        case 0x4: return !CC_TST(MC6809_FLAG_C);                        // BCC/BHS
        case 0x5: return  CC_TST(MC6809_FLAG_C);                        // BCS/BLO
        case 0x6: return !CC_TST(MC6809_FLAG_Z);                        // BNE
        case 0x7: return  CC_TST(MC6809_FLAG_Z);                        // BEQ
        case 0x8: return !CC_TST(MC6809_FLAG_V);                        // BVC
        case 0x9: return  CC_TST(MC6809_FLAG_V);                        // BVS
        case 0xA: return !CC_TST(MC6809_FLAG_N);                        // BPL
        case 0xB: return  CC_TST(MC6809_FLAG_N);                        // BMI
        case 0xC: return !((CC_TST(MC6809_FLAG_N) != 0) ^ (CC_TST(MC6809_FLAG_V) != 0)); // BGE
        case 0xD: return  ((CC_TST(MC6809_FLAG_N) != 0) ^ (CC_TST(MC6809_FLAG_V) != 0)); // BLT
        case 0xE: return !(CC_TST(MC6809_FLAG_Z) || ((CC_TST(MC6809_FLAG_N) != 0) ^ (CC_TST(MC6809_FLAG_V) != 0))); // BGT
        case 0xF: return  (CC_TST(MC6809_FLAG_Z) || ((CC_TST(MC6809_FLAG_N) != 0) ^ (CC_TST(MC6809_FLAG_V) != 0))); // BLE
    }
    return false;
}

// ============================================================
// Page 2 opcode execution ($10 prefix)
// ============================================================
static void execute_page2(MC6809* cpu) {
    uint8_t opcode = fetch8(cpu);

    // Long branches ($10 $2x)
    if (opcode >= 0x20 && opcode <= 0x2F) {
        int16_t offset = (int16_t)fetch16(cpu);
        bool take = eval_branch(cpu, opcode & 0x0F);
        if (take) {
            cpu->pc += offset;
            cpu->cycles += 6;
        } else {
            cpu->cycles += 5;
        }
        // LBRA ($10 $20) is actually encoded as $16, but if someone uses $10 $20...
        return;
    }

    switch (opcode) {
        case 0x3F: { // SWI2
            CC_SET(MC6809_FLAG_E);
            push16s(cpu, cpu->pc);
            push16s(cpu, cpu->u);
            push16s(cpu, cpu->y);
            push16s(cpu, cpu->x);
            push8s(cpu, cpu->dp);
            push8s(cpu, GET_B());
            push8s(cpu, GET_A());
            push8s(cpu, cpu->cc);
            cpu->pc = mem_read16(cpu, MC6809_VEC_SWI2);
            cpu->cycles += 20;
            break;
        }

        // ---- CMPD ----
        case 0x83: { // CMPD #imm16
            uint16_t val = fetch16(cpu);
            op_sub16(cpu, cpu->d, val);
            cpu->cycles += 5;
            break;
        }
        case 0x93: { // CMPD <dp
            uint16_t ea = addr_direct(cpu);
            uint16_t val = mem_read16(cpu, ea);
            op_sub16(cpu, cpu->d, val);
            cpu->cycles += 7;
            break;
        }
        case 0xA3: { // CMPD indexed
            uint16_t ea = addr_indexed(cpu);
            uint16_t val = mem_read16(cpu, ea);
            op_sub16(cpu, cpu->d, val);
            cpu->cycles += 7;
            break;
        }
        case 0xB3: { // CMPD extended
            uint16_t ea = addr_extended(cpu);
            uint16_t val = mem_read16(cpu, ea);
            op_sub16(cpu, cpu->d, val);
            cpu->cycles += 8;
            break;
        }

        // ---- CMPY ----
        case 0x8C: { // CMPY #imm16
            uint16_t val = fetch16(cpu);
            op_sub16(cpu, cpu->y, val);
            cpu->cycles += 5;
            break;
        }
        case 0x9C: { // CMPY <dp
            uint16_t ea = addr_direct(cpu);
            uint16_t val = mem_read16(cpu, ea);
            op_sub16(cpu, cpu->y, val);
            cpu->cycles += 7;
            break;
        }
        case 0xAC: { // CMPY indexed
            uint16_t ea = addr_indexed(cpu);
            uint16_t val = mem_read16(cpu, ea);
            op_sub16(cpu, cpu->y, val);
            cpu->cycles += 7;
            break;
        }
        case 0xBC: { // CMPY extended
            uint16_t ea = addr_extended(cpu);
            uint16_t val = mem_read16(cpu, ea);
            op_sub16(cpu, cpu->y, val);
            cpu->cycles += 8;
            break;
        }

        // ---- LDY ----
        case 0x8E: { // LDY #imm16
            cpu->y = fetch16(cpu);
            update_nz16(cpu, cpu->y);
            CC_CLR(MC6809_FLAG_V);
            cpu->cycles += 4;
            break;
        }
        case 0x9E: { // LDY <dp
            uint16_t ea = addr_direct(cpu);
            cpu->y = mem_read16(cpu, ea);
            update_nz16(cpu, cpu->y);
            CC_CLR(MC6809_FLAG_V);
            cpu->cycles += 6;
            break;
        }
        case 0xAE: { // LDY indexed
            uint16_t ea = addr_indexed(cpu);
            cpu->y = mem_read16(cpu, ea);
            update_nz16(cpu, cpu->y);
            CC_CLR(MC6809_FLAG_V);
            cpu->cycles += 6;
            break;
        }
        case 0xBE: { // LDY extended
            uint16_t ea = addr_extended(cpu);
            cpu->y = mem_read16(cpu, ea);
            update_nz16(cpu, cpu->y);
            CC_CLR(MC6809_FLAG_V);
            cpu->cycles += 7;
            break;
        }

        // ---- STY ----
        case 0x9F: { // STY <dp
            uint16_t ea = addr_direct(cpu);
            mem_write16(cpu, ea, cpu->y);
            update_nz16(cpu, cpu->y);
            CC_CLR(MC6809_FLAG_V);
            cpu->cycles += 6;
            break;
        }
        case 0xAF: { // STY indexed
            uint16_t ea = addr_indexed(cpu);
            mem_write16(cpu, ea, cpu->y);
            update_nz16(cpu, cpu->y);
            CC_CLR(MC6809_FLAG_V);
            cpu->cycles += 6;
            break;
        }
        case 0xBF: { // STY extended
            uint16_t ea = addr_extended(cpu);
            mem_write16(cpu, ea, cpu->y);
            update_nz16(cpu, cpu->y);
            CC_CLR(MC6809_FLAG_V);
            cpu->cycles += 7;
            break;
        }

        // ---- LDS ----
        case 0xCE: { // LDS #imm16
            cpu->s = fetch16(cpu);
            update_nz16(cpu, cpu->s);
            CC_CLR(MC6809_FLAG_V);
            cpu->cycles += 4;
            cpu->nmi_armed = true;
            break;
        }
        case 0xDE: { // LDS <dp
            uint16_t ea = addr_direct(cpu);
            cpu->s = mem_read16(cpu, ea);
            update_nz16(cpu, cpu->s);
            CC_CLR(MC6809_FLAG_V);
            cpu->cycles += 6;
            cpu->nmi_armed = true;
            break;
        }
        case 0xEE: { // LDS indexed
            uint16_t ea = addr_indexed(cpu);
            cpu->s = mem_read16(cpu, ea);
            update_nz16(cpu, cpu->s);
            CC_CLR(MC6809_FLAG_V);
            cpu->cycles += 6;
            cpu->nmi_armed = true;
            break;
        }
        case 0xFE: { // LDS extended
            uint16_t ea = addr_extended(cpu);
            cpu->s = mem_read16(cpu, ea);
            update_nz16(cpu, cpu->s);
            CC_CLR(MC6809_FLAG_V);
            cpu->cycles += 7;
            cpu->nmi_armed = true;
            break;
        }

        // ---- STS ----
        case 0xDF: { // STS <dp
            uint16_t ea = addr_direct(cpu);
            mem_write16(cpu, ea, cpu->s);
            update_nz16(cpu, cpu->s);
            CC_CLR(MC6809_FLAG_V);
            cpu->cycles += 6;
            break;
        }
        case 0xEF: { // STS indexed
            uint16_t ea = addr_indexed(cpu);
            mem_write16(cpu, ea, cpu->s);
            update_nz16(cpu, cpu->s);
            CC_CLR(MC6809_FLAG_V);
            cpu->cycles += 6;
            break;
        }
        case 0xFF: { // STS extended
            uint16_t ea = addr_extended(cpu);
            mem_write16(cpu, ea, cpu->s);
            update_nz16(cpu, cpu->s);
            CC_CLR(MC6809_FLAG_V);
            cpu->cycles += 7;
            break;
        }

        default:
            // Unimplemented page 2 opcode - treat as NOP
            cpu->cycles += 2;
            break;
    }
}

// ============================================================
// Page 3 opcode execution ($11 prefix)
// ============================================================
static void execute_page3(MC6809* cpu) {
    uint8_t opcode = fetch8(cpu);

    switch (opcode) {
        case 0x3F: { // SWI3
            CC_SET(MC6809_FLAG_E);
            push16s(cpu, cpu->pc);
            push16s(cpu, cpu->u);
            push16s(cpu, cpu->y);
            push16s(cpu, cpu->x);
            push8s(cpu, cpu->dp);
            push8s(cpu, GET_B());
            push8s(cpu, GET_A());
            push8s(cpu, cpu->cc);
            cpu->pc = mem_read16(cpu, MC6809_VEC_SWI3);
            cpu->cycles += 20;
            break;
        }

        // ---- CMPU ----
        case 0x83: { // CMPU #imm16
            uint16_t val = fetch16(cpu);
            op_sub16(cpu, cpu->u, val);
            cpu->cycles += 5;
            break;
        }
        case 0x93: { // CMPU <dp
            uint16_t ea = addr_direct(cpu);
            uint16_t val = mem_read16(cpu, ea);
            op_sub16(cpu, cpu->u, val);
            cpu->cycles += 7;
            break;
        }
        case 0xA3: { // CMPU indexed
            uint16_t ea = addr_indexed(cpu);
            uint16_t val = mem_read16(cpu, ea);
            op_sub16(cpu, cpu->u, val);
            cpu->cycles += 7;
            break;
        }
        case 0xB3: { // CMPU extended
            uint16_t ea = addr_extended(cpu);
            uint16_t val = mem_read16(cpu, ea);
            op_sub16(cpu, cpu->u, val);
            cpu->cycles += 8;
            break;
        }

        // ---- CMPS ----
        case 0x8C: { // CMPS #imm16
            uint16_t val = fetch16(cpu);
            op_sub16(cpu, cpu->s, val);
            cpu->cycles += 5;
            break;
        }
        case 0x9C: { // CMPS <dp
            uint16_t ea = addr_direct(cpu);
            uint16_t val = mem_read16(cpu, ea);
            op_sub16(cpu, cpu->s, val);
            cpu->cycles += 7;
            break;
        }
        case 0xAC: { // CMPS indexed
            uint16_t ea = addr_indexed(cpu);
            uint16_t val = mem_read16(cpu, ea);
            op_sub16(cpu, cpu->s, val);
            cpu->cycles += 7;
            break;
        }
        case 0xBC: { // CMPS extended
            uint16_t ea = addr_extended(cpu);
            uint16_t val = mem_read16(cpu, ea);
            op_sub16(cpu, cpu->s, val);
            cpu->cycles += 8;
            break;
        }

        default:
            // Unimplemented page 3 opcode - treat as NOP
            cpu->cycles += 2;
            break;
    }
}

// ============================================================
// Main opcode execution (page 1)
// ============================================================
static void execute_one(MC6809* cpu) {
    uint8_t opcode = fetch8(cpu);

    switch (opcode) {

    // ============================================================
    // $00-$0F: Direct page read-modify-write
    // ============================================================
    case 0x00: { // NEG <dp
        uint16_t ea = addr_direct(cpu);
        uint8_t val = mem_read(cpu, ea);
        uint8_t r = op_sub8(cpu, 0, val, 0);
        mem_write(cpu, ea, r);
        cpu->cycles += 6;
        break;
    }
    case 0x03: { // COM <dp
        uint16_t ea = addr_direct(cpu);
        uint8_t val = ~mem_read(cpu, ea);
        mem_write(cpu, ea, val);
        update_nz8(cpu, val);
        CC_CLR(MC6809_FLAG_V);
        CC_SET(MC6809_FLAG_C);
        cpu->cycles += 6;
        break;
    }
    case 0x04: { // LSR <dp
        uint16_t ea = addr_direct(cpu);
        uint8_t val = mem_read(cpu, ea);
        CC_PUT(MC6809_FLAG_C, val & 0x01);
        val >>= 1;
        mem_write(cpu, ea, val);
        CC_CLR(MC6809_FLAG_N);
        CC_PUT(MC6809_FLAG_Z, val == 0);
        cpu->cycles += 6;
        break;
    }
    case 0x06: { // ROR <dp
        uint16_t ea = addr_direct(cpu);
        uint8_t val = mem_read(cpu, ea);
        uint8_t c = CC_TST(MC6809_FLAG_C) ? 0x80 : 0;
        CC_PUT(MC6809_FLAG_C, val & 0x01);
        val = (val >> 1) | c;
        mem_write(cpu, ea, val);
        update_nz8(cpu, val);
        cpu->cycles += 6;
        break;
    }
    case 0x07: { // ASR <dp
        uint16_t ea = addr_direct(cpu);
        uint8_t val = mem_read(cpu, ea);
        CC_PUT(MC6809_FLAG_C, val & 0x01);
        val = (val >> 1) | (val & 0x80);
        mem_write(cpu, ea, val);
        update_nz8(cpu, val);
        cpu->cycles += 6;
        break;
    }
    case 0x08: { // ASL/LSL <dp
        uint16_t ea = addr_direct(cpu);
        uint8_t val = mem_read(cpu, ea);
        CC_PUT(MC6809_FLAG_C, val & 0x80);
        CC_PUT(MC6809_FLAG_V, (val ^ (val << 1)) & 0x80);
        val <<= 1;
        mem_write(cpu, ea, val);
        update_nz8(cpu, val);
        cpu->cycles += 6;
        break;
    }
    case 0x09: { // ROL <dp
        uint16_t ea = addr_direct(cpu);
        uint8_t val = mem_read(cpu, ea);
        uint8_t c = CC_TST(MC6809_FLAG_C) ? 1 : 0;
        CC_PUT(MC6809_FLAG_C, val & 0x80);
        CC_PUT(MC6809_FLAG_V, (val ^ (val << 1)) & 0x80);
        val = (val << 1) | c;
        mem_write(cpu, ea, val);
        update_nz8(cpu, val);
        cpu->cycles += 6;
        break;
    }
    case 0x0A: { // DEC <dp
        uint16_t ea = addr_direct(cpu);
        uint8_t val = mem_read(cpu, ea);
        CC_PUT(MC6809_FLAG_V, val == 0x80);
        val--;
        mem_write(cpu, ea, val);
        update_nz8(cpu, val);
        cpu->cycles += 6;
        break;
    }
    case 0x0C: { // INC <dp
        uint16_t ea = addr_direct(cpu);
        uint8_t val = mem_read(cpu, ea);
        CC_PUT(MC6809_FLAG_V, val == 0x7F);
        val++;
        mem_write(cpu, ea, val);
        update_nz8(cpu, val);
        cpu->cycles += 6;
        break;
    }
    case 0x0D: { // TST <dp
        uint16_t ea = addr_direct(cpu);
        uint8_t val = mem_read(cpu, ea);
        update_nz8(cpu, val);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 6;
        break;
    }
    case 0x0E: { // JMP <dp
        cpu->pc = addr_direct(cpu);
        cpu->cycles += 3;
        break;
    }
    case 0x0F: { // CLR <dp
        uint16_t ea = addr_direct(cpu);
        mem_write(cpu, ea, 0);
        CC_CLR(MC6809_FLAG_N | MC6809_FLAG_V | MC6809_FLAG_C);
        CC_SET(MC6809_FLAG_Z);
        cpu->cycles += 6;
        break;
    }

    // ============================================================
    // $10, $11: Page 2/3 prefixes
    // ============================================================
    case 0x10:
        execute_page2(cpu);
        break;
    case 0x11:
        execute_page3(cpu);
        break;

    // ============================================================
    // $12-$1F: Misc inherent
    // ============================================================
    case 0x12: // NOP
        cpu->cycles += 2;
        break;

    case 0x13: // SYNC
        cpu->wait_for_interrupt = true;
        cpu->cycles += 4;
        break;

    case 0x16: { // LBRA
        int16_t offset = (int16_t)fetch16(cpu);
        cpu->pc += offset;
        cpu->cycles += 5;
        break;
    }

    case 0x17: { // LBSR
        int16_t offset = (int16_t)fetch16(cpu);
        push16s(cpu, cpu->pc);
        cpu->pc += offset;
        cpu->cycles += 9;
        break;
    }

    case 0x19: { // DAA
        uint8_t a = GET_A();
        uint8_t correction = 0;
        if ((a & 0x0F) > 0x09 || CC_TST(MC6809_FLAG_H))
            correction |= 0x06;
        if ((a & 0xF0) > 0x90 || CC_TST(MC6809_FLAG_C) ||
            ((a & 0xF0) > 0x80 && (a & 0x0F) > 0x09))
            correction |= 0x60;
        uint16_t result = (uint16_t)a + correction;
        SET_A((uint8_t)result);
        update_nz8(cpu, GET_A());
        CC_CLR(MC6809_FLAG_V);
        if (result & 0x100) CC_SET(MC6809_FLAG_C);
        cpu->cycles += 2;
        break;
    }

    case 0x1A: { // ORCC #imm
        cpu->cc |= fetch8(cpu);
        cpu->cycles += 3;
        break;
    }

    case 0x1C: { // ANDCC #imm
        cpu->cc &= fetch8(cpu);
        cpu->cycles += 3;
        break;
    }

    case 0x1D: { // SEX (sign-extend B into A)
        uint8_t b = GET_B();
        SET_A((b & 0x80) ? 0xFF : 0x00);
        update_nz16(cpu, cpu->d);
        // Note: SEX updates N and Z based on 16-bit D result
        // but some sources say Z is based on B only. Using D.
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 2;
        break;
    }

    case 0x1E: { // EXG
        uint8_t postbyte = fetch8(cpu);
        uint8_t src = (postbyte >> 4) & 0x0F;
        uint8_t dst = postbyte & 0x0F;
        uint16_t sv = tfr_read_reg(cpu, src);
        uint16_t dv = tfr_read_reg(cpu, dst);
        tfr_write_reg(cpu, src, dv);
        tfr_write_reg(cpu, dst, sv);
        cpu->cycles += 8;
        break;
    }

    case 0x1F: { // TFR
        uint8_t postbyte = fetch8(cpu);
        uint8_t src = (postbyte >> 4) & 0x0F;
        uint8_t dst = postbyte & 0x0F;
        uint16_t val = tfr_read_reg(cpu, src);
        tfr_write_reg(cpu, dst, val);
        cpu->cycles += 6;
        break;
    }

    // ============================================================
    // $20-$2F: Short branches
    // ============================================================
    case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x24: case 0x25: case 0x26: case 0x27:
    case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x2C: case 0x2D: case 0x2E: case 0x2F: {
        int8_t offset = (int8_t)fetch8(cpu);
        if (eval_branch(cpu, opcode & 0x0F)) {
            cpu->pc += offset;
        }
        cpu->cycles += 3;
        break;
    }

    // ============================================================
    // $30-$33: LEA
    // ============================================================
    case 0x30: { // LEAX indexed
        cpu->x = addr_indexed(cpu);
        CC_PUT(MC6809_FLAG_Z, cpu->x == 0);
        cpu->cycles += 4;
        break;
    }
    case 0x31: { // LEAY indexed
        cpu->y = addr_indexed(cpu);
        CC_PUT(MC6809_FLAG_Z, cpu->y == 0);
        cpu->cycles += 4;
        break;
    }
    case 0x32: { // LEAS indexed
        cpu->s = addr_indexed(cpu);
        cpu->cycles += 4;
        cpu->nmi_armed = true;
        break;
    }
    case 0x33: { // LEAU indexed
        cpu->u = addr_indexed(cpu);
        cpu->cycles += 4;
        break;
    }

    // ============================================================
    // $34-$37: Push/Pull
    // ============================================================
    case 0x34: { // PSHS
        uint8_t postbyte = fetch8(cpu);
        int n = do_pshs(cpu, postbyte);
        cpu->cycles += 5 + n;
        break;
    }
    case 0x35: { // PULS
        uint8_t postbyte = fetch8(cpu);
        int n = do_puls(cpu, postbyte);
        cpu->cycles += 5 + n;
        break;
    }
    case 0x36: { // PSHU
        uint8_t postbyte = fetch8(cpu);
        int n = do_pshu(cpu, postbyte);
        cpu->cycles += 5 + n;
        break;
    }
    case 0x37: { // PULU
        uint8_t postbyte = fetch8(cpu);
        int n = do_pulu(cpu, postbyte);
        cpu->cycles += 5 + n;
        break;
    }

    // ============================================================
    // $39-$3F: Misc
    // ============================================================
    case 0x39: { // RTS
        cpu->pc = pull16s(cpu);
        cpu->cycles += 5;
        break;
    }

    case 0x3A: { // ABX
        cpu->x += GET_B();
        cpu->cycles += 3;
        break;
    }

    case 0x3B: { // RTI
        cpu->cc = pull8s(cpu);
        if (CC_TST(MC6809_FLAG_E)) {
            SET_A(pull8s(cpu));
            SET_B(pull8s(cpu));
            cpu->dp = pull8s(cpu);
            cpu->x = pull16s(cpu);
            cpu->y = pull16s(cpu);
            cpu->u = pull16s(cpu);
            cpu->pc = pull16s(cpu);
            cpu->cycles += 15;
        } else {
            cpu->pc = pull16s(cpu);
            cpu->cycles += 6;
        }
        break;
    }

    case 0x3C: { // CWAI
        uint8_t imm = fetch8(cpu);
        cpu->cc &= imm;
        CC_SET(MC6809_FLAG_E);
        // Push entire state (pre-push for interrupt handler)
        push16s(cpu, cpu->pc);
        push16s(cpu, cpu->u);
        push16s(cpu, cpu->y);
        push16s(cpu, cpu->x);
        push8s(cpu, cpu->dp);
        push8s(cpu, GET_B());
        push8s(cpu, GET_A());
        push8s(cpu, cpu->cc);
        cpu->wait_for_interrupt = true;
        cpu->cwai_state = true;   // State already pushed — skip push in interrupt handler
        cpu->cycles += 20;
        break;
    }

    case 0x3D: { // MUL
        uint16_t result = (uint16_t)GET_A() * (uint16_t)GET_B();
        cpu->d = result;
        CC_PUT(MC6809_FLAG_Z, result == 0);
        CC_PUT(MC6809_FLAG_C, result & 0x80); // C = bit 7 of result
        cpu->cycles += 11;
        break;
    }

    case 0x3F: { // SWI
        CC_SET(MC6809_FLAG_E);
        push16s(cpu, cpu->pc);
        push16s(cpu, cpu->u);
        push16s(cpu, cpu->y);
        push16s(cpu, cpu->x);
        push8s(cpu, cpu->dp);
        push8s(cpu, GET_B());
        push8s(cpu, GET_A());
        push8s(cpu, cpu->cc);
        CC_SET(MC6809_FLAG_I | MC6809_FLAG_F);
        cpu->pc = mem_read16(cpu, MC6809_VEC_SWI);
        cpu->cycles += 19;
        break;
    }

    // ============================================================
    // $40-$4F: Accumulator A inherent operations
    // ============================================================
    case 0x40: { // NEGA
        uint8_t a = GET_A();
        uint8_t r = op_sub8(cpu, 0, a, 0);
        SET_A(r);
        cpu->cycles += 2;
        break;
    }
    case 0x43: { // COMA
        uint8_t r = ~GET_A();
        SET_A(r);
        update_nz8(cpu, r);
        CC_CLR(MC6809_FLAG_V);
        CC_SET(MC6809_FLAG_C);
        cpu->cycles += 2;
        break;
    }
    case 0x44: { // LSRA
        uint8_t a = GET_A();
        CC_PUT(MC6809_FLAG_C, a & 0x01);
        a >>= 1;
        SET_A(a);
        CC_CLR(MC6809_FLAG_N);
        CC_PUT(MC6809_FLAG_Z, a == 0);
        cpu->cycles += 2;
        break;
    }
    case 0x46: { // RORA
        uint8_t a = GET_A();
        uint8_t c = CC_TST(MC6809_FLAG_C) ? 0x80 : 0;
        CC_PUT(MC6809_FLAG_C, a & 0x01);
        a = (a >> 1) | c;
        SET_A(a);
        update_nz8(cpu, a);
        cpu->cycles += 2;
        break;
    }
    case 0x47: { // ASRA
        uint8_t a = GET_A();
        CC_PUT(MC6809_FLAG_C, a & 0x01);
        a = (a >> 1) | (a & 0x80);
        SET_A(a);
        update_nz8(cpu, a);
        cpu->cycles += 2;
        break;
    }
    case 0x48: { // ASLA/LSLA
        uint8_t a = GET_A();
        CC_PUT(MC6809_FLAG_C, a & 0x80);
        CC_PUT(MC6809_FLAG_V, (a ^ (a << 1)) & 0x80);
        a <<= 1;
        SET_A(a);
        update_nz8(cpu, a);
        cpu->cycles += 2;
        break;
    }
    case 0x49: { // ROLA
        uint8_t a = GET_A();
        uint8_t c = CC_TST(MC6809_FLAG_C) ? 1 : 0;
        CC_PUT(MC6809_FLAG_C, a & 0x80);
        CC_PUT(MC6809_FLAG_V, (a ^ (a << 1)) & 0x80);
        a = (a << 1) | c;
        SET_A(a);
        update_nz8(cpu, a);
        cpu->cycles += 2;
        break;
    }
    case 0x4A: { // DECA
        uint8_t a = GET_A();
        CC_PUT(MC6809_FLAG_V, a == 0x80);
        a--;
        SET_A(a);
        update_nz8(cpu, a);
        cpu->cycles += 2;
        break;
    }
    case 0x4C: { // INCA
        uint8_t a = GET_A();
        CC_PUT(MC6809_FLAG_V, a == 0x7F);
        a++;
        SET_A(a);
        update_nz8(cpu, a);
        cpu->cycles += 2;
        break;
    }
    case 0x4D: { // TSTA
        update_nz8(cpu, GET_A());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 2;
        break;
    }
    case 0x4F: { // CLRA
        SET_A(0);
        CC_CLR(MC6809_FLAG_N | MC6809_FLAG_V | MC6809_FLAG_C);
        CC_SET(MC6809_FLAG_Z);
        cpu->cycles += 2;
        break;
    }

    // ============================================================
    // $50-$5F: Accumulator B inherent operations
    // ============================================================
    case 0x50: { // NEGB
        uint8_t b = GET_B();
        uint8_t r = op_sub8(cpu, 0, b, 0);
        SET_B(r);
        cpu->cycles += 2;
        break;
    }
    case 0x53: { // COMB
        uint8_t r = ~GET_B();
        SET_B(r);
        update_nz8(cpu, r);
        CC_CLR(MC6809_FLAG_V);
        CC_SET(MC6809_FLAG_C);
        cpu->cycles += 2;
        break;
    }
    case 0x54: { // LSRB
        uint8_t b = GET_B();
        CC_PUT(MC6809_FLAG_C, b & 0x01);
        b >>= 1;
        SET_B(b);
        CC_CLR(MC6809_FLAG_N);
        CC_PUT(MC6809_FLAG_Z, b == 0);
        cpu->cycles += 2;
        break;
    }
    case 0x56: { // RORB
        uint8_t b = GET_B();
        uint8_t c = CC_TST(MC6809_FLAG_C) ? 0x80 : 0;
        CC_PUT(MC6809_FLAG_C, b & 0x01);
        b = (b >> 1) | c;
        SET_B(b);
        update_nz8(cpu, b);
        cpu->cycles += 2;
        break;
    }
    case 0x57: { // ASRB
        uint8_t b = GET_B();
        CC_PUT(MC6809_FLAG_C, b & 0x01);
        b = (b >> 1) | (b & 0x80);
        SET_B(b);
        update_nz8(cpu, b);
        cpu->cycles += 2;
        break;
    }
    case 0x58: { // ASLB/LSLB
        uint8_t b = GET_B();
        CC_PUT(MC6809_FLAG_C, b & 0x80);
        CC_PUT(MC6809_FLAG_V, (b ^ (b << 1)) & 0x80);
        b <<= 1;
        SET_B(b);
        update_nz8(cpu, b);
        cpu->cycles += 2;
        break;
    }
    case 0x59: { // ROLB
        uint8_t b = GET_B();
        uint8_t c = CC_TST(MC6809_FLAG_C) ? 1 : 0;
        CC_PUT(MC6809_FLAG_C, b & 0x80);
        CC_PUT(MC6809_FLAG_V, (b ^ (b << 1)) & 0x80);
        b = (b << 1) | c;
        SET_B(b);
        update_nz8(cpu, b);
        cpu->cycles += 2;
        break;
    }
    case 0x5A: { // DECB
        uint8_t b = GET_B();
        CC_PUT(MC6809_FLAG_V, b == 0x80);
        b--;
        SET_B(b);
        update_nz8(cpu, b);
        cpu->cycles += 2;
        break;
    }
    case 0x5C: { // INCB
        uint8_t b = GET_B();
        CC_PUT(MC6809_FLAG_V, b == 0x7F);
        b++;
        SET_B(b);
        update_nz8(cpu, b);
        cpu->cycles += 2;
        break;
    }
    case 0x5D: { // TSTB
        update_nz8(cpu, GET_B());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 2;
        break;
    }
    case 0x5F: { // CLRB
        SET_B(0);
        CC_CLR(MC6809_FLAG_N | MC6809_FLAG_V | MC6809_FLAG_C);
        CC_SET(MC6809_FLAG_Z);
        cpu->cycles += 2;
        break;
    }

    // ============================================================
    // $60-$6F: Indexed read-modify-write
    // ============================================================
    case 0x60: { // NEG indexed
        uint16_t ea = addr_indexed(cpu);
        uint8_t val = mem_read(cpu, ea);
        uint8_t r = op_sub8(cpu, 0, val, 0);
        mem_write(cpu, ea, r);
        cpu->cycles += 6;
        break;
    }
    case 0x63: { // COM indexed
        uint16_t ea = addr_indexed(cpu);
        uint8_t val = ~mem_read(cpu, ea);
        mem_write(cpu, ea, val);
        update_nz8(cpu, val);
        CC_CLR(MC6809_FLAG_V);
        CC_SET(MC6809_FLAG_C);
        cpu->cycles += 6;
        break;
    }
    case 0x64: { // LSR indexed
        uint16_t ea = addr_indexed(cpu);
        uint8_t val = mem_read(cpu, ea);
        CC_PUT(MC6809_FLAG_C, val & 0x01);
        val >>= 1;
        mem_write(cpu, ea, val);
        CC_CLR(MC6809_FLAG_N);
        CC_PUT(MC6809_FLAG_Z, val == 0);
        cpu->cycles += 6;
        break;
    }
    case 0x66: { // ROR indexed
        uint16_t ea = addr_indexed(cpu);
        uint8_t val = mem_read(cpu, ea);
        uint8_t c = CC_TST(MC6809_FLAG_C) ? 0x80 : 0;
        CC_PUT(MC6809_FLAG_C, val & 0x01);
        val = (val >> 1) | c;
        mem_write(cpu, ea, val);
        update_nz8(cpu, val);
        cpu->cycles += 6;
        break;
    }
    case 0x67: { // ASR indexed
        uint16_t ea = addr_indexed(cpu);
        uint8_t val = mem_read(cpu, ea);
        CC_PUT(MC6809_FLAG_C, val & 0x01);
        val = (val >> 1) | (val & 0x80);
        mem_write(cpu, ea, val);
        update_nz8(cpu, val);
        cpu->cycles += 6;
        break;
    }
    case 0x68: { // ASL indexed
        uint16_t ea = addr_indexed(cpu);
        uint8_t val = mem_read(cpu, ea);
        CC_PUT(MC6809_FLAG_C, val & 0x80);
        CC_PUT(MC6809_FLAG_V, (val ^ (val << 1)) & 0x80);
        val <<= 1;
        mem_write(cpu, ea, val);
        update_nz8(cpu, val);
        cpu->cycles += 6;
        break;
    }
    case 0x69: { // ROL indexed
        uint16_t ea = addr_indexed(cpu);
        uint8_t val = mem_read(cpu, ea);
        uint8_t c = CC_TST(MC6809_FLAG_C) ? 1 : 0;
        CC_PUT(MC6809_FLAG_C, val & 0x80);
        CC_PUT(MC6809_FLAG_V, (val ^ (val << 1)) & 0x80);
        val = (val << 1) | c;
        mem_write(cpu, ea, val);
        update_nz8(cpu, val);
        cpu->cycles += 6;
        break;
    }
    case 0x6A: { // DEC indexed
        uint16_t ea = addr_indexed(cpu);
        uint8_t val = mem_read(cpu, ea);
        CC_PUT(MC6809_FLAG_V, val == 0x80);
        val--;
        mem_write(cpu, ea, val);
        update_nz8(cpu, val);
        cpu->cycles += 6;
        break;
    }
    case 0x6C: { // INC indexed
        uint16_t ea = addr_indexed(cpu);
        uint8_t val = mem_read(cpu, ea);
        CC_PUT(MC6809_FLAG_V, val == 0x7F);
        val++;
        mem_write(cpu, ea, val);
        update_nz8(cpu, val);
        cpu->cycles += 6;
        break;
    }
    case 0x6D: { // TST indexed
        uint16_t ea = addr_indexed(cpu);
        uint8_t val = mem_read(cpu, ea);
        update_nz8(cpu, val);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 6;
        break;
    }
    case 0x6E: { // JMP indexed
        cpu->pc = addr_indexed(cpu);
        cpu->cycles += 3;
        break;
    }
    case 0x6F: { // CLR indexed
        uint16_t ea = addr_indexed(cpu);
        mem_write(cpu, ea, 0);
        CC_CLR(MC6809_FLAG_N | MC6809_FLAG_V | MC6809_FLAG_C);
        CC_SET(MC6809_FLAG_Z);
        cpu->cycles += 6;
        break;
    }

    // ============================================================
    // $70-$7F: Extended read-modify-write
    // ============================================================
    case 0x70: { // NEG extended
        uint16_t ea = addr_extended(cpu);
        uint8_t val = mem_read(cpu, ea);
        uint8_t r = op_sub8(cpu, 0, val, 0);
        mem_write(cpu, ea, r);
        cpu->cycles += 7;
        break;
    }
    case 0x73: { // COM extended
        uint16_t ea = addr_extended(cpu);
        uint8_t val = ~mem_read(cpu, ea);
        mem_write(cpu, ea, val);
        update_nz8(cpu, val);
        CC_CLR(MC6809_FLAG_V);
        CC_SET(MC6809_FLAG_C);
        cpu->cycles += 7;
        break;
    }
    case 0x74: { // LSR extended
        uint16_t ea = addr_extended(cpu);
        uint8_t val = mem_read(cpu, ea);
        CC_PUT(MC6809_FLAG_C, val & 0x01);
        val >>= 1;
        mem_write(cpu, ea, val);
        CC_CLR(MC6809_FLAG_N);
        CC_PUT(MC6809_FLAG_Z, val == 0);
        cpu->cycles += 7;
        break;
    }
    case 0x76: { // ROR extended
        uint16_t ea = addr_extended(cpu);
        uint8_t val = mem_read(cpu, ea);
        uint8_t c = CC_TST(MC6809_FLAG_C) ? 0x80 : 0;
        CC_PUT(MC6809_FLAG_C, val & 0x01);
        val = (val >> 1) | c;
        mem_write(cpu, ea, val);
        update_nz8(cpu, val);
        cpu->cycles += 7;
        break;
    }
    case 0x77: { // ASR extended
        uint16_t ea = addr_extended(cpu);
        uint8_t val = mem_read(cpu, ea);
        CC_PUT(MC6809_FLAG_C, val & 0x01);
        val = (val >> 1) | (val & 0x80);
        mem_write(cpu, ea, val);
        update_nz8(cpu, val);
        cpu->cycles += 7;
        break;
    }
    case 0x78: { // ASL extended
        uint16_t ea = addr_extended(cpu);
        uint8_t val = mem_read(cpu, ea);
        CC_PUT(MC6809_FLAG_C, val & 0x80);
        CC_PUT(MC6809_FLAG_V, (val ^ (val << 1)) & 0x80);
        val <<= 1;
        mem_write(cpu, ea, val);
        update_nz8(cpu, val);
        cpu->cycles += 7;
        break;
    }
    case 0x79: { // ROL extended
        uint16_t ea = addr_extended(cpu);
        uint8_t val = mem_read(cpu, ea);
        uint8_t c = CC_TST(MC6809_FLAG_C) ? 1 : 0;
        CC_PUT(MC6809_FLAG_C, val & 0x80);
        CC_PUT(MC6809_FLAG_V, (val ^ (val << 1)) & 0x80);
        val = (val << 1) | c;
        mem_write(cpu, ea, val);
        update_nz8(cpu, val);
        cpu->cycles += 7;
        break;
    }
    case 0x7A: { // DEC extended
        uint16_t ea = addr_extended(cpu);
        uint8_t val = mem_read(cpu, ea);
        CC_PUT(MC6809_FLAG_V, val == 0x80);
        val--;
        mem_write(cpu, ea, val);
        update_nz8(cpu, val);
        cpu->cycles += 7;
        break;
    }
    case 0x7C: { // INC extended
        uint16_t ea = addr_extended(cpu);
        uint8_t val = mem_read(cpu, ea);
        CC_PUT(MC6809_FLAG_V, val == 0x7F);
        val++;
        mem_write(cpu, ea, val);
        update_nz8(cpu, val);
        cpu->cycles += 7;
        break;
    }
    case 0x7D: { // TST extended
        uint16_t ea = addr_extended(cpu);
        uint8_t val = mem_read(cpu, ea);
        update_nz8(cpu, val);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 7;
        break;
    }
    case 0x7E: { // JMP extended
        cpu->pc = addr_extended(cpu);
        cpu->cycles += 4;
        break;
    }
    case 0x7F: { // CLR extended
        uint16_t ea = addr_extended(cpu);
        mem_write(cpu, ea, 0);
        CC_CLR(MC6809_FLAG_N | MC6809_FLAG_V | MC6809_FLAG_C);
        CC_SET(MC6809_FLAG_Z);
        cpu->cycles += 7;
        break;
    }

    // ============================================================
    // $80-$8F: A-register immediate
    // ============================================================
    case 0x80: { // SUBA #imm
        uint8_t val = fetch8(cpu);
        SET_A(op_sub8(cpu, GET_A(), val, 0));
        cpu->cycles += 2;
        break;
    }
    case 0x81: { // CMPA #imm
        uint8_t val = fetch8(cpu);
        op_sub8(cpu, GET_A(), val, 0);
        cpu->cycles += 2;
        break;
    }
    case 0x82: { // SBCA #imm
        uint8_t val = fetch8(cpu);
        uint8_t c = CC_TST(MC6809_FLAG_C) ? 1 : 0;
        SET_A(op_sub8(cpu, GET_A(), val, c));
        cpu->cycles += 2;
        break;
    }
    case 0x83: { // SUBD #imm16
        uint16_t val = fetch16(cpu);
        cpu->d = op_sub16(cpu, cpu->d, val);
        cpu->cycles += 4;
        break;
    }
    case 0x84: { // ANDA #imm
        SET_A(GET_A() & fetch8(cpu));
        update_nz8(cpu, GET_A());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 2;
        break;
    }
    case 0x85: { // BITA #imm
        uint8_t r = GET_A() & fetch8(cpu);
        update_nz8(cpu, r);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 2;
        break;
    }
    case 0x86: { // LDA #imm
        SET_A(fetch8(cpu));
        update_nz8(cpu, GET_A());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 2;
        break;
    }
    case 0x88: { // EORA #imm
        SET_A(GET_A() ^ fetch8(cpu));
        update_nz8(cpu, GET_A());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 2;
        break;
    }
    case 0x89: { // ADCA #imm
        uint8_t val = fetch8(cpu);
        uint8_t c = CC_TST(MC6809_FLAG_C) ? 1 : 0;
        SET_A(op_add8(cpu, GET_A(), val, c));
        cpu->cycles += 2;
        break;
    }
    case 0x8A: { // ORA #imm
        SET_A(GET_A() | fetch8(cpu));
        update_nz8(cpu, GET_A());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 2;
        break;
    }
    case 0x8B: { // ADDA #imm
        uint8_t val = fetch8(cpu);
        SET_A(op_add8(cpu, GET_A(), val, 0));
        cpu->cycles += 2;
        break;
    }
    case 0x8C: { // CMPX #imm16
        uint16_t val = fetch16(cpu);
        op_sub16(cpu, cpu->x, val);
        cpu->cycles += 4;
        break;
    }
    case 0x8D: { // BSR
        int8_t offset = (int8_t)fetch8(cpu);
        push16s(cpu, cpu->pc);
        cpu->pc += offset;
        cpu->cycles += 7;
        break;
    }
    case 0x8E: { // LDX #imm16
        cpu->x = fetch16(cpu);
        update_nz16(cpu, cpu->x);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 3;
        break;
    }

    // ============================================================
    // $90-$9F: A-register direct
    // ============================================================
    case 0x90: { // SUBA <dp
        uint16_t ea = addr_direct(cpu);
        SET_A(op_sub8(cpu, GET_A(), mem_read(cpu, ea), 0));
        cpu->cycles += 4;
        break;
    }
    case 0x91: { // CMPA <dp
        uint16_t ea = addr_direct(cpu);
        op_sub8(cpu, GET_A(), mem_read(cpu, ea), 0);
        cpu->cycles += 4;
        break;
    }
    case 0x92: { // SBCA <dp
        uint16_t ea = addr_direct(cpu);
        uint8_t c = CC_TST(MC6809_FLAG_C) ? 1 : 0;
        SET_A(op_sub8(cpu, GET_A(), mem_read(cpu, ea), c));
        cpu->cycles += 4;
        break;
    }
    case 0x93: { // SUBD <dp
        uint16_t ea = addr_direct(cpu);
        cpu->d = op_sub16(cpu, cpu->d, mem_read16(cpu, ea));
        cpu->cycles += 6;
        break;
    }
    case 0x94: { // ANDA <dp
        uint16_t ea = addr_direct(cpu);
        SET_A(GET_A() & mem_read(cpu, ea));
        update_nz8(cpu, GET_A());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 4;
        break;
    }
    case 0x95: { // BITA <dp
        uint16_t ea = addr_direct(cpu);
        uint8_t r = GET_A() & mem_read(cpu, ea);
        update_nz8(cpu, r);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 4;
        break;
    }
    case 0x96: { // LDA <dp
        uint16_t ea = addr_direct(cpu);
        SET_A(mem_read(cpu, ea));
        update_nz8(cpu, GET_A());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 4;
        break;
    }
    case 0x97: { // STA <dp
        uint16_t ea = addr_direct(cpu);
        mem_write(cpu, ea, GET_A());
        update_nz8(cpu, GET_A());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 4;
        break;
    }
    case 0x98: { // EORA <dp
        uint16_t ea = addr_direct(cpu);
        SET_A(GET_A() ^ mem_read(cpu, ea));
        update_nz8(cpu, GET_A());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 4;
        break;
    }
    case 0x99: { // ADCA <dp
        uint16_t ea = addr_direct(cpu);
        uint8_t c = CC_TST(MC6809_FLAG_C) ? 1 : 0;
        SET_A(op_add8(cpu, GET_A(), mem_read(cpu, ea), c));
        cpu->cycles += 4;
        break;
    }
    case 0x9A: { // ORA <dp
        uint16_t ea = addr_direct(cpu);
        SET_A(GET_A() | mem_read(cpu, ea));
        update_nz8(cpu, GET_A());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 4;
        break;
    }
    case 0x9B: { // ADDA <dp
        uint16_t ea = addr_direct(cpu);
        SET_A(op_add8(cpu, GET_A(), mem_read(cpu, ea), 0));
        cpu->cycles += 4;
        break;
    }
    case 0x9C: { // CMPX <dp
        uint16_t ea = addr_direct(cpu);
        op_sub16(cpu, cpu->x, mem_read16(cpu, ea));
        cpu->cycles += 6;
        break;
    }
    case 0x9D: { // JSR <dp
        uint16_t ea = addr_direct(cpu);
        push16s(cpu, cpu->pc);
        cpu->pc = ea;
        cpu->cycles += 7;
        break;
    }
    case 0x9E: { // LDX <dp
        uint16_t ea = addr_direct(cpu);
        cpu->x = mem_read16(cpu, ea);
        update_nz16(cpu, cpu->x);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 5;
        break;
    }
    case 0x9F: { // STX <dp
        uint16_t ea = addr_direct(cpu);
        mem_write16(cpu, ea, cpu->x);
        update_nz16(cpu, cpu->x);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 5;
        break;
    }

    // ============================================================
    // $A0-$AF: A-register indexed
    // ============================================================
    case 0xA0: { // SUBA indexed
        uint16_t ea = addr_indexed(cpu);
        SET_A(op_sub8(cpu, GET_A(), mem_read(cpu, ea), 0));
        cpu->cycles += 4;
        break;
    }
    case 0xA1: { // CMPA indexed
        uint16_t ea = addr_indexed(cpu);
        op_sub8(cpu, GET_A(), mem_read(cpu, ea), 0);
        cpu->cycles += 4;
        break;
    }
    case 0xA2: { // SBCA indexed
        uint16_t ea = addr_indexed(cpu);
        uint8_t c = CC_TST(MC6809_FLAG_C) ? 1 : 0;
        SET_A(op_sub8(cpu, GET_A(), mem_read(cpu, ea), c));
        cpu->cycles += 4;
        break;
    }
    case 0xA3: { // SUBD indexed
        uint16_t ea = addr_indexed(cpu);
        cpu->d = op_sub16(cpu, cpu->d, mem_read16(cpu, ea));
        cpu->cycles += 6;
        break;
    }
    case 0xA4: { // ANDA indexed
        uint16_t ea = addr_indexed(cpu);
        SET_A(GET_A() & mem_read(cpu, ea));
        update_nz8(cpu, GET_A());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 4;
        break;
    }
    case 0xA5: { // BITA indexed
        uint16_t ea = addr_indexed(cpu);
        uint8_t r = GET_A() & mem_read(cpu, ea);
        update_nz8(cpu, r);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 4;
        break;
    }
    case 0xA6: { // LDA indexed
        uint16_t ea = addr_indexed(cpu);
        SET_A(mem_read(cpu, ea));
        update_nz8(cpu, GET_A());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 4;
        break;
    }
    case 0xA7: { // STA indexed
        uint16_t ea = addr_indexed(cpu);
        mem_write(cpu, ea, GET_A());
        update_nz8(cpu, GET_A());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 4;
        break;
    }
    case 0xA8: { // EORA indexed
        uint16_t ea = addr_indexed(cpu);
        SET_A(GET_A() ^ mem_read(cpu, ea));
        update_nz8(cpu, GET_A());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 4;
        break;
    }
    case 0xA9: { // ADCA indexed
        uint16_t ea = addr_indexed(cpu);
        uint8_t c = CC_TST(MC6809_FLAG_C) ? 1 : 0;
        SET_A(op_add8(cpu, GET_A(), mem_read(cpu, ea), c));
        cpu->cycles += 4;
        break;
    }
    case 0xAA: { // ORA indexed
        uint16_t ea = addr_indexed(cpu);
        SET_A(GET_A() | mem_read(cpu, ea));
        update_nz8(cpu, GET_A());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 4;
        break;
    }
    case 0xAB: { // ADDA indexed
        uint16_t ea = addr_indexed(cpu);
        SET_A(op_add8(cpu, GET_A(), mem_read(cpu, ea), 0));
        cpu->cycles += 4;
        break;
    }
    case 0xAC: { // CMPX indexed
        uint16_t ea = addr_indexed(cpu);
        op_sub16(cpu, cpu->x, mem_read16(cpu, ea));
        cpu->cycles += 6;
        break;
    }
    case 0xAD: { // JSR indexed
        uint16_t ea = addr_indexed(cpu);
        push16s(cpu, cpu->pc);
        cpu->pc = ea;
        cpu->cycles += 7;
        break;
    }
    case 0xAE: { // LDX indexed
        uint16_t ea = addr_indexed(cpu);
        cpu->x = mem_read16(cpu, ea);
        update_nz16(cpu, cpu->x);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 5;
        break;
    }
    case 0xAF: { // STX indexed
        uint16_t ea = addr_indexed(cpu);
        mem_write16(cpu, ea, cpu->x);
        update_nz16(cpu, cpu->x);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 5;
        break;
    }

    // ============================================================
    // $B0-$BF: A-register extended
    // ============================================================
    case 0xB0: { // SUBA extended
        uint16_t ea = addr_extended(cpu);
        SET_A(op_sub8(cpu, GET_A(), mem_read(cpu, ea), 0));
        cpu->cycles += 5;
        break;
    }
    case 0xB1: { // CMPA extended
        uint16_t ea = addr_extended(cpu);
        op_sub8(cpu, GET_A(), mem_read(cpu, ea), 0);
        cpu->cycles += 5;
        break;
    }
    case 0xB2: { // SBCA extended
        uint16_t ea = addr_extended(cpu);
        uint8_t c = CC_TST(MC6809_FLAG_C) ? 1 : 0;
        SET_A(op_sub8(cpu, GET_A(), mem_read(cpu, ea), c));
        cpu->cycles += 5;
        break;
    }
    case 0xB3: { // SUBD extended
        uint16_t ea = addr_extended(cpu);
        cpu->d = op_sub16(cpu, cpu->d, mem_read16(cpu, ea));
        cpu->cycles += 7;
        break;
    }
    case 0xB4: { // ANDA extended
        uint16_t ea = addr_extended(cpu);
        SET_A(GET_A() & mem_read(cpu, ea));
        update_nz8(cpu, GET_A());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 5;
        break;
    }
    case 0xB5: { // BITA extended
        uint16_t ea = addr_extended(cpu);
        uint8_t r = GET_A() & mem_read(cpu, ea);
        update_nz8(cpu, r);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 5;
        break;
    }
    case 0xB6: { // LDA extended
        uint16_t ea = addr_extended(cpu);
        SET_A(mem_read(cpu, ea));
        update_nz8(cpu, GET_A());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 5;
        break;
    }
    case 0xB7: { // STA extended
        uint16_t ea = addr_extended(cpu);
        mem_write(cpu, ea, GET_A());
        update_nz8(cpu, GET_A());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 5;
        break;
    }
    case 0xB8: { // EORA extended
        uint16_t ea = addr_extended(cpu);
        SET_A(GET_A() ^ mem_read(cpu, ea));
        update_nz8(cpu, GET_A());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 5;
        break;
    }
    case 0xB9: { // ADCA extended
        uint16_t ea = addr_extended(cpu);
        uint8_t c = CC_TST(MC6809_FLAG_C) ? 1 : 0;
        SET_A(op_add8(cpu, GET_A(), mem_read(cpu, ea), c));
        cpu->cycles += 5;
        break;
    }
    case 0xBA: { // ORA extended
        uint16_t ea = addr_extended(cpu);
        SET_A(GET_A() | mem_read(cpu, ea));
        update_nz8(cpu, GET_A());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 5;
        break;
    }
    case 0xBB: { // ADDA extended
        uint16_t ea = addr_extended(cpu);
        SET_A(op_add8(cpu, GET_A(), mem_read(cpu, ea), 0));
        cpu->cycles += 5;
        break;
    }
    case 0xBC: { // CMPX extended
        uint16_t ea = addr_extended(cpu);
        op_sub16(cpu, cpu->x, mem_read16(cpu, ea));
        cpu->cycles += 7;
        break;
    }
    case 0xBD: { // JSR extended
        uint16_t ea = addr_extended(cpu);
        push16s(cpu, cpu->pc);
        cpu->pc = ea;
        cpu->cycles += 8;
        break;
    }
    case 0xBE: { // LDX extended
        uint16_t ea = addr_extended(cpu);
        cpu->x = mem_read16(cpu, ea);
        update_nz16(cpu, cpu->x);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 6;
        break;
    }
    case 0xBF: { // STX extended
        uint16_t ea = addr_extended(cpu);
        mem_write16(cpu, ea, cpu->x);
        update_nz16(cpu, cpu->x);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 6;
        break;
    }

    // ============================================================
    // $C0-$CF: B-register immediate / 16-bit immediate
    // ============================================================
    case 0xC0: { // SUBB #imm
        uint8_t val = fetch8(cpu);
        SET_B(op_sub8(cpu, GET_B(), val, 0));
        cpu->cycles += 2;
        break;
    }
    case 0xC1: { // CMPB #imm
        uint8_t val = fetch8(cpu);
        op_sub8(cpu, GET_B(), val, 0);
        cpu->cycles += 2;
        break;
    }
    case 0xC2: { // SBCB #imm
        uint8_t val = fetch8(cpu);
        uint8_t c = CC_TST(MC6809_FLAG_C) ? 1 : 0;
        SET_B(op_sub8(cpu, GET_B(), val, c));
        cpu->cycles += 2;
        break;
    }
    case 0xC3: { // ADDD #imm16
        uint16_t val = fetch16(cpu);
        cpu->d = op_add16(cpu, cpu->d, val);
        cpu->cycles += 4;
        break;
    }
    case 0xC4: { // ANDB #imm
        SET_B(GET_B() & fetch8(cpu));
        update_nz8(cpu, GET_B());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 2;
        break;
    }
    case 0xC5: { // BITB #imm
        uint8_t r = GET_B() & fetch8(cpu);
        update_nz8(cpu, r);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 2;
        break;
    }
    case 0xC6: { // LDB #imm
        SET_B(fetch8(cpu));
        update_nz8(cpu, GET_B());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 2;
        break;
    }
    case 0xC8: { // EORB #imm
        SET_B(GET_B() ^ fetch8(cpu));
        update_nz8(cpu, GET_B());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 2;
        break;
    }
    case 0xC9: { // ADCB #imm
        uint8_t val = fetch8(cpu);
        uint8_t c = CC_TST(MC6809_FLAG_C) ? 1 : 0;
        SET_B(op_add8(cpu, GET_B(), val, c));
        cpu->cycles += 2;
        break;
    }
    case 0xCA: { // ORB #imm
        SET_B(GET_B() | fetch8(cpu));
        update_nz8(cpu, GET_B());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 2;
        break;
    }
    case 0xCB: { // ADDB #imm
        uint8_t val = fetch8(cpu);
        SET_B(op_add8(cpu, GET_B(), val, 0));
        cpu->cycles += 2;
        break;
    }
    case 0xCC: { // LDD #imm16
        cpu->d = fetch16(cpu);
        update_nz16(cpu, cpu->d);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 3;
        break;
    }
    case 0xCE: { // LDU #imm16
        cpu->u = fetch16(cpu);
        update_nz16(cpu, cpu->u);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 3;
        break;
    }

    // ============================================================
    // $D0-$DF: B-register direct / 16-bit direct
    // ============================================================
    case 0xD0: { // SUBB <dp
        uint16_t ea = addr_direct(cpu);
        SET_B(op_sub8(cpu, GET_B(), mem_read(cpu, ea), 0));
        cpu->cycles += 4;
        break;
    }
    case 0xD1: { // CMPB <dp
        uint16_t ea = addr_direct(cpu);
        op_sub8(cpu, GET_B(), mem_read(cpu, ea), 0);
        cpu->cycles += 4;
        break;
    }
    case 0xD2: { // SBCB <dp
        uint16_t ea = addr_direct(cpu);
        uint8_t c = CC_TST(MC6809_FLAG_C) ? 1 : 0;
        SET_B(op_sub8(cpu, GET_B(), mem_read(cpu, ea), c));
        cpu->cycles += 4;
        break;
    }
    case 0xD3: { // ADDD <dp
        uint16_t ea = addr_direct(cpu);
        cpu->d = op_add16(cpu, cpu->d, mem_read16(cpu, ea));
        cpu->cycles += 6;
        break;
    }
    case 0xD4: { // ANDB <dp
        uint16_t ea = addr_direct(cpu);
        SET_B(GET_B() & mem_read(cpu, ea));
        update_nz8(cpu, GET_B());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 4;
        break;
    }
    case 0xD5: { // BITB <dp
        uint16_t ea = addr_direct(cpu);
        uint8_t r = GET_B() & mem_read(cpu, ea);
        update_nz8(cpu, r);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 4;
        break;
    }
    case 0xD6: { // LDB <dp
        uint16_t ea = addr_direct(cpu);
        SET_B(mem_read(cpu, ea));
        update_nz8(cpu, GET_B());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 4;
        break;
    }
    case 0xD7: { // STB <dp
        uint16_t ea = addr_direct(cpu);
        mem_write(cpu, ea, GET_B());
        update_nz8(cpu, GET_B());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 4;
        break;
    }
    case 0xD8: { // EORB <dp
        uint16_t ea = addr_direct(cpu);
        SET_B(GET_B() ^ mem_read(cpu, ea));
        update_nz8(cpu, GET_B());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 4;
        break;
    }
    case 0xD9: { // ADCB <dp
        uint16_t ea = addr_direct(cpu);
        uint8_t c = CC_TST(MC6809_FLAG_C) ? 1 : 0;
        SET_B(op_add8(cpu, GET_B(), mem_read(cpu, ea), c));
        cpu->cycles += 4;
        break;
    }
    case 0xDA: { // ORB <dp
        uint16_t ea = addr_direct(cpu);
        SET_B(GET_B() | mem_read(cpu, ea));
        update_nz8(cpu, GET_B());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 4;
        break;
    }
    case 0xDB: { // ADDB <dp
        uint16_t ea = addr_direct(cpu);
        SET_B(op_add8(cpu, GET_B(), mem_read(cpu, ea), 0));
        cpu->cycles += 4;
        break;
    }
    case 0xDC: { // LDD <dp
        uint16_t ea = addr_direct(cpu);
        cpu->d = mem_read16(cpu, ea);
        update_nz16(cpu, cpu->d);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 5;
        break;
    }
    case 0xDD: { // STD <dp
        uint16_t ea = addr_direct(cpu);
        mem_write16(cpu, ea, cpu->d);
        update_nz16(cpu, cpu->d);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 5;
        break;
    }
    case 0xDE: { // LDU <dp
        uint16_t ea = addr_direct(cpu);
        cpu->u = mem_read16(cpu, ea);
        update_nz16(cpu, cpu->u);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 5;
        break;
    }
    case 0xDF: { // STU <dp
        uint16_t ea = addr_direct(cpu);
        mem_write16(cpu, ea, cpu->u);
        update_nz16(cpu, cpu->u);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 5;
        break;
    }

    // ============================================================
    // $E0-$EF: B-register indexed / 16-bit indexed
    // ============================================================
    case 0xE0: { // SUBB indexed
        uint16_t ea = addr_indexed(cpu);
        SET_B(op_sub8(cpu, GET_B(), mem_read(cpu, ea), 0));
        cpu->cycles += 4;
        break;
    }
    case 0xE1: { // CMPB indexed
        uint16_t ea = addr_indexed(cpu);
        op_sub8(cpu, GET_B(), mem_read(cpu, ea), 0);
        cpu->cycles += 4;
        break;
    }
    case 0xE2: { // SBCB indexed
        uint16_t ea = addr_indexed(cpu);
        uint8_t c = CC_TST(MC6809_FLAG_C) ? 1 : 0;
        SET_B(op_sub8(cpu, GET_B(), mem_read(cpu, ea), c));
        cpu->cycles += 4;
        break;
    }
    case 0xE3: { // ADDD indexed
        uint16_t ea = addr_indexed(cpu);
        cpu->d = op_add16(cpu, cpu->d, mem_read16(cpu, ea));
        cpu->cycles += 6;
        break;
    }
    case 0xE4: { // ANDB indexed
        uint16_t ea = addr_indexed(cpu);
        SET_B(GET_B() & mem_read(cpu, ea));
        update_nz8(cpu, GET_B());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 4;
        break;
    }
    case 0xE5: { // BITB indexed
        uint16_t ea = addr_indexed(cpu);
        uint8_t r = GET_B() & mem_read(cpu, ea);
        update_nz8(cpu, r);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 4;
        break;
    }
    case 0xE6: { // LDB indexed
        uint16_t ea = addr_indexed(cpu);
        SET_B(mem_read(cpu, ea));
        update_nz8(cpu, GET_B());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 4;
        break;
    }
    case 0xE7: { // STB indexed
        uint16_t ea = addr_indexed(cpu);
        mem_write(cpu, ea, GET_B());
        update_nz8(cpu, GET_B());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 4;
        break;
    }
    case 0xE8: { // EORB indexed
        uint16_t ea = addr_indexed(cpu);
        SET_B(GET_B() ^ mem_read(cpu, ea));
        update_nz8(cpu, GET_B());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 4;
        break;
    }
    case 0xE9: { // ADCB indexed
        uint16_t ea = addr_indexed(cpu);
        uint8_t c = CC_TST(MC6809_FLAG_C) ? 1 : 0;
        SET_B(op_add8(cpu, GET_B(), mem_read(cpu, ea), c));
        cpu->cycles += 4;
        break;
    }
    case 0xEA: { // ORB indexed
        uint16_t ea = addr_indexed(cpu);
        SET_B(GET_B() | mem_read(cpu, ea));
        update_nz8(cpu, GET_B());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 4;
        break;
    }
    case 0xEB: { // ADDB indexed
        uint16_t ea = addr_indexed(cpu);
        SET_B(op_add8(cpu, GET_B(), mem_read(cpu, ea), 0));
        cpu->cycles += 4;
        break;
    }
    case 0xEC: { // LDD indexed
        uint16_t ea = addr_indexed(cpu);
        cpu->d = mem_read16(cpu, ea);
        update_nz16(cpu, cpu->d);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 5;
        break;
    }
    case 0xED: { // STD indexed
        uint16_t ea = addr_indexed(cpu);
        mem_write16(cpu, ea, cpu->d);
        update_nz16(cpu, cpu->d);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 5;
        break;
    }
    case 0xEE: { // LDU indexed
        uint16_t ea = addr_indexed(cpu);
        cpu->u = mem_read16(cpu, ea);
        update_nz16(cpu, cpu->u);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 5;
        break;
    }
    case 0xEF: { // STU indexed
        uint16_t ea = addr_indexed(cpu);
        mem_write16(cpu, ea, cpu->u);
        update_nz16(cpu, cpu->u);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 5;
        break;
    }

    // ============================================================
    // $F0-$FF: B-register extended / 16-bit extended
    // ============================================================
    case 0xF0: { // SUBB extended
        uint16_t ea = addr_extended(cpu);
        SET_B(op_sub8(cpu, GET_B(), mem_read(cpu, ea), 0));
        cpu->cycles += 5;
        break;
    }
    case 0xF1: { // CMPB extended
        uint16_t ea = addr_extended(cpu);
        op_sub8(cpu, GET_B(), mem_read(cpu, ea), 0);
        cpu->cycles += 5;
        break;
    }
    case 0xF2: { // SBCB extended
        uint16_t ea = addr_extended(cpu);
        uint8_t c = CC_TST(MC6809_FLAG_C) ? 1 : 0;
        SET_B(op_sub8(cpu, GET_B(), mem_read(cpu, ea), c));
        cpu->cycles += 5;
        break;
    }
    case 0xF3: { // ADDD extended
        uint16_t ea = addr_extended(cpu);
        cpu->d = op_add16(cpu, cpu->d, mem_read16(cpu, ea));
        cpu->cycles += 7;
        break;
    }
    case 0xF4: { // ANDB extended
        uint16_t ea = addr_extended(cpu);
        SET_B(GET_B() & mem_read(cpu, ea));
        update_nz8(cpu, GET_B());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 5;
        break;
    }
    case 0xF5: { // BITB extended
        uint16_t ea = addr_extended(cpu);
        uint8_t r = GET_B() & mem_read(cpu, ea);
        update_nz8(cpu, r);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 5;
        break;
    }
    case 0xF6: { // LDB extended
        uint16_t ea = addr_extended(cpu);
        SET_B(mem_read(cpu, ea));
        update_nz8(cpu, GET_B());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 5;
        break;
    }
    case 0xF7: { // STB extended
        uint16_t ea = addr_extended(cpu);
        mem_write(cpu, ea, GET_B());
        update_nz8(cpu, GET_B());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 5;
        break;
    }
    case 0xF8: { // EORB extended
        uint16_t ea = addr_extended(cpu);
        SET_B(GET_B() ^ mem_read(cpu, ea));
        update_nz8(cpu, GET_B());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 5;
        break;
    }
    case 0xF9: { // ADCB extended
        uint16_t ea = addr_extended(cpu);
        uint8_t c = CC_TST(MC6809_FLAG_C) ? 1 : 0;
        SET_B(op_add8(cpu, GET_B(), mem_read(cpu, ea), c));
        cpu->cycles += 5;
        break;
    }
    case 0xFA: { // ORB extended
        uint16_t ea = addr_extended(cpu);
        SET_B(GET_B() | mem_read(cpu, ea));
        update_nz8(cpu, GET_B());
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 5;
        break;
    }
    case 0xFB: { // ADDB extended
        uint16_t ea = addr_extended(cpu);
        SET_B(op_add8(cpu, GET_B(), mem_read(cpu, ea), 0));
        cpu->cycles += 5;
        break;
    }
    case 0xFC: { // LDD extended
        uint16_t ea = addr_extended(cpu);
        cpu->d = mem_read16(cpu, ea);
        update_nz16(cpu, cpu->d);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 6;
        break;
    }
    case 0xFD: { // STD extended
        uint16_t ea = addr_extended(cpu);
        mem_write16(cpu, ea, cpu->d);
        update_nz16(cpu, cpu->d);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 6;
        break;
    }
    case 0xFE: { // LDU extended
        uint16_t ea = addr_extended(cpu);
        cpu->u = mem_read16(cpu, ea);
        update_nz16(cpu, cpu->u);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 6;
        break;
    }
    case 0xFF: { // STU extended
        uint16_t ea = addr_extended(cpu);
        mem_write16(cpu, ea, cpu->u);
        update_nz16(cpu, cpu->u);
        CC_CLR(MC6809_FLAG_V);
        cpu->cycles += 6;
        break;
    }

    // ============================================================
    // Default: unimplemented opcode
    // ============================================================
    default:
        // Treat as NOP (2 cycles)
        cpu->cycles += 2;
        break;

    } // end switch
}

// ============================================================
// Public API implementation
// ============================================================

void mc6809_init(MC6809* cpu) {
    memset(cpu, 0, sizeof(MC6809));
    cpu->cc = MC6809_FLAG_I | MC6809_FLAG_F;
    cpu->read = NULL;
    cpu->write = NULL;
}

void mc6809_reset(MC6809* cpu) {
    cpu->dp = 0x00;
    cpu->cc = MC6809_FLAG_I | MC6809_FLAG_F;
    cpu->halted = false;
    cpu->wait_for_interrupt = false;
    cpu->cwai_state = false;
    cpu->nmi_armed = false;
    cpu->nmi_line = false;
    cpu->nmi_pending = false;
    cpu->firq_pending = false;
    cpu->irq_pending = false;
    cpu->cycles = 0;

    if (cpu->read) {
        cpu->pc = mem_read16(cpu, MC6809_VEC_RESET);
    } else {
        cpu->pc = 0x0000;
    }
}

// Interrupt service routine — check and dispatch pending interrupts
// IMPORTANT: If cwai_state is true, the state has been pre-pushed by CWAI.
// In that case, skip the push and just vector to the handler.
static void check_interrupts(MC6809* cpu) {
    // NMI: highest priority, requires nmi_armed (set by first LDS)
    // Edge-triggered: nmi_pending latched on 0→1 transition, cleared after service
    if (cpu->nmi_pending && cpu->nmi_armed) {
        cpu->nmi_pending = false;
        cpu->nmi_line = false;  // Consume edge — require new transition for next NMI
        if (!cpu->cwai_state) {
            CC_SET(MC6809_FLAG_E);
            push16s(cpu, cpu->pc);
            push16s(cpu, cpu->u);
            push16s(cpu, cpu->y);
            push16s(cpu, cpu->x);
            push8s(cpu, cpu->dp);
            push8s(cpu, GET_B());
            push8s(cpu, GET_A());
            push8s(cpu, cpu->cc);
            cpu->cycles += 19;
        } else {
            cpu->cycles += 7;  // CWAI already counted push cycles
        }
        CC_SET(MC6809_FLAG_I | MC6809_FLAG_F);
        cpu->pc = mem_read16(cpu, MC6809_VEC_NMI);
        cpu->wait_for_interrupt = false;
        cpu->cwai_state = false;
        return;
    }

    // FIRQ: masked by F flag
    if (cpu->firq_pending && !CC_TST(MC6809_FLAG_F)) {
        if (!cpu->cwai_state) {
            CC_CLR(MC6809_FLAG_E);  // Only CC and PC saved
            push16s(cpu, cpu->pc);
            push8s(cpu, cpu->cc);
            cpu->cycles += 10;
        } else {
            // CWAI pushed full state with E=1; FIRQ normally saves partial,
            // but CWAI always saves entire state — keep E=1 on stack
            cpu->cycles += 7;
        }
        CC_SET(MC6809_FLAG_I | MC6809_FLAG_F);
        cpu->pc = mem_read16(cpu, MC6809_VEC_FIRQ);
        cpu->wait_for_interrupt = false;
        cpu->cwai_state = false;
        return;
    }

    // IRQ: masked by I flag
    if (cpu->irq_pending && !CC_TST(MC6809_FLAG_I)) {
        if (!cpu->cwai_state) {
            CC_SET(MC6809_FLAG_E);
            push16s(cpu, cpu->pc);
            push16s(cpu, cpu->u);
            push16s(cpu, cpu->y);
            push16s(cpu, cpu->x);
            push8s(cpu, cpu->dp);
            push8s(cpu, GET_B());
            push8s(cpu, GET_A());
            push8s(cpu, cpu->cc);
            cpu->cycles += 19;
        } else {
            cpu->cycles += 7;  // CWAI already counted push cycles
        }
        CC_SET(MC6809_FLAG_I);
        cpu->pc = mem_read16(cpu, MC6809_VEC_IRQ);
        cpu->wait_for_interrupt = false;
        cpu->cwai_state = false;
        return;
    }
}

int mc6809_run(MC6809* cpu, int budget) {
    cpu->cycles = 0;

    if (!cpu->read || !cpu->write) {
        return 0;
    }

    while ((int)cpu->cycles < budget) {
        if (cpu->halted) {
            cpu->cycles = budget;
            break;
        }

        // Check for pending interrupts
        check_interrupts(cpu);

        if (cpu->wait_for_interrupt) {
            // CWAI/SYNC: burn remaining cycles waiting
            cpu->cycles = budget;
            break;
        }

        execute_one(cpu);
    }

    return (int)cpu->cycles;
}

void mc6809_nmi(MC6809* cpu, bool active) {
    if (active && !cpu->nmi_line) {
        // Edge-triggered: latch on inactive→active transition only
        cpu->nmi_pending = true;
        cpu->wait_for_interrupt = false;
    }
    cpu->nmi_line = active;
}

void mc6809_firq(MC6809* cpu, bool active) {
    cpu->firq_pending = active;
    if (active) cpu->wait_for_interrupt = false;
}

void mc6809_irq(MC6809* cpu, bool active) {
    cpu->irq_pending = active;
    if (active) cpu->wait_for_interrupt = false;
}
