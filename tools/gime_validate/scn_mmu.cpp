#include "scenario.h"
#include <stdio.h>

// MMU / address-decode scenarios (1987 GIME). RAM is whatever Ctx gives.
static void check_decode(Ctx& ctx, const std::string& id, uint16_t addr, bool main_is_z) {
    ctx.check_opt(id + (main_is_z ? "" : "_Z"),
                  [addr](GimeModel& m) -> std::optional<int64_t> {
                      Decode d = m.decode(addr);
                      if (!d.z_valid) return std::nullopt;
                      return (int64_t)d.Z;
                  }, CAP_DECODE);
    ctx.check(id + (main_is_z ? "_S" : ""),
              [addr](GimeModel& m) { return (int64_t)m.decode(addr).S; }, CAP_DECODE);
}

// Fill all 16 MMU registers with slot*7+task (mod 64), MMUEN set.
static void fill_mmu(Ctx& ctx) {
    ctx.write(0xFF90, 0x40);                          // MMUEN
    for (unsigned i = 0; i < 16; i++)
        ctx.write((uint16_t)(0xFFA0 + i), (uint8_t)(((i % 8) * 7 + i / 8) & 63));
}

SCENARIO(mmu, decode_Z) {
    for (unsigned task = 0; task < 2; task++)
        for (unsigned slot = 0; slot < 8; slot++) {
            char id[40];
            snprintf(id, sizeof id, "mmu.decode_Z_%u_%u", task, slot);
            ctx.reset();
            fill_mmu(ctx);
            ctx.write(0xFF91, (uint8_t)task);         // TR
            check_decode(ctx, id, (uint16_t)(slot << 13 | 0x123), true);
        }
}

SCENARIO(mmu, mmuen_off_Z) {
    for (unsigned slot = 0; slot < 8; slot++) {
        char id[40];
        snprintf(id, sizeof id, "mmu.mmuen_off_Z_%u", slot);
        ctx.reset();
        fill_mmu(ctx);
        ctx.write(0xFF90, 0x00);                      // MMUEN off: fixed $38|slot
        check_decode(ctx, id, (uint16_t)(slot << 13 | 0x123), true);
    }
}

SCENARIO(mmu, mc3_fe00) {
    for (unsigned mc3 = 0; mc3 < 2; mc3++)
        for (unsigned a : { 0xFE00u, 0xFE80u, 0xFEFFu, 0xFDFFu }) {
            char id[40];
            snprintf(id, sizeof id, "mmu.mc3_fe00_%s_%04X", mc3 ? "set" : "clr", a);
            ctx.reset();
            fill_mmu(ctx);
            ctx.write(0xFF90, (uint8_t)(0x40 | (mc3 ? 0x08 : 0)));
            check_decode(ctx, id, (uint16_t)a, true);
        }
}

SCENARIO(mmu, rom_S) {
    for (unsigned mc = 0; mc < 4; mc++)
        for (unsigned ty = 0; ty < 2; ty++)
            for (unsigned a : { 0x8000u, 0xA000u, 0xC000u, 0xE000u }) {
                char id[48];
                snprintf(id, sizeof id, "mmu.rom_S_mc%u_%04X%s", mc, a, ty ? "_ty1" : "");
                ctx.reset();
                ctx.write(0xFF90, (uint8_t)mc);                   // MC1/MC0
                ctx.write(ty ? 0xFFDF : 0xFFDE, 0);               // SAM TY
                check_decode(ctx, id, (uint16_t)(a + 0x123), false);
            }
}

SCENARIO(mmu, bank_mask) {
    for (unsigned v : { 0xFFu, 0x7Fu, 0x40u, 0x80u, 0x3Fu }) {
        char id[40];
        snprintf(id, sizeof id, "mmu.bank_mask_%02X", v);
        ctx.reset();
        ctx.write(0xFF90, 0x40);
        ctx.write(0xFFA0, (uint8_t)v);
        ctx.check_read(std::string(id) + "_read", 0xFFA0, 0x3F);
        check_decode(ctx, id, 0x0123, true);
    }
}
