/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : sv_debug.cpp
 *  Module : Supervisor debug — CPU status, GIME state, and memory dump
 * ============================================================
*/

#include "sv_debug.h"
#include "supervisor.h"
#include "sv_render.h"
#include "../core/machine.h"
#include "../core/mc6809.h"
#include "../core/mc6551.h"
#include "../hal/hal.h"
#include "../hal/hal_rs232.h"
#include "../utils/debug.h"
#include "../../config.h"     // COCO3_PHYSICAL_RAM
#include <SD.h>
#include <stdarg.h>

extern OSDCanvas* hal_video_get_canvas(void);

#if MACHINE_TYPE == 4
#include "../core/tcc1014.h"
#endif

// HID usage codes
#define HID_UP    0x52
#define HID_DOWN  0x51
#define HID_LEFT  0x50
#define HID_RIGHT 0x4F
#define HID_ENTER 0x28
#define HID_ESC   0x29

#define DUMP_BYTES_PER_LINE  16
#define DUMP_LINES           8    // 128 bytes per screen

static SV_DebugState dbg;

// ============================================================
// Status page — renders CPU + GIME state to TFT
// ============================================================

static void status_render(Supervisor_t* sv) {
    sv_render_frame("Debug: Status", "L/R=Page ENT=Serial ESC=Back");

    Machine* m = sv->machine;
    if (!m) return;

    OSDCanvas* tft = hal_video_get_canvas();
    if (!tft) return;

    tft->setTextFont(0);  // compact 6x8
    tft->setTextDatum(TL_DATUM);

    int x = SV_CONTENT_X;
    int y = SV_CONTENT_Y + 2;
    int lh = 9;
    char buf[64];

    // --- CPU Registers ---
    MC6809* cpu = &m->cpu;
    tft->setTextColor(SV_COLOR_TEXT, SV_COLOR_BG);
    tft->drawString("CPU Registers", x, y);
    y += lh + 2;

    tft->setTextColor(SV_COLOR_DIM, SV_COLOR_BG);
    snprintf(buf, sizeof(buf), "PC=%04X  S=%04X  U=%04X",
             cpu->pc, cpu->s, cpu->u);
    tft->drawString(buf, x, y); y += lh;

    snprintf(buf, sizeof(buf), " X=%04X  Y=%04X  D=%04X",
             cpu->x, cpu->y, cpu->d);
    tft->drawString(buf, x, y); y += lh;

    snprintf(buf, sizeof(buf), "DP=%02X  CC=%02X [%c%c%c%c%c%c%c%c]",
             cpu->dp, cpu->cc,
             (cpu->cc & 0x80) ? 'E' : '-',
             (cpu->cc & 0x40) ? 'F' : '-',
             (cpu->cc & 0x20) ? 'H' : '-',
             (cpu->cc & 0x10) ? 'I' : '-',
             (cpu->cc & 0x08) ? 'N' : '-',
             (cpu->cc & 0x04) ? 'Z' : '-',
             (cpu->cc & 0x02) ? 'V' : '-',
             (cpu->cc & 0x01) ? 'C' : '-');
    tft->drawString(buf, x, y); y += lh;

    snprintf(buf, sizeof(buf), "HALT=%d CWAI=%d NMI=%d IRQ=%d FIRQ=%d",
             cpu->halted ? 1 : 0,
             cpu->wait_for_interrupt ? 1 : 0,
             cpu->nmi_pending ? 1 : 0,
             cpu->irq_pending ? 1 : 0,
             cpu->firq_pending ? 1 : 0);
    tft->drawString(buf, x, y); y += lh + 4;

#if MACHINE_TYPE == 4
    // --- GIME State (CoCo 3 only — skip if runtime-switched to CoCo 2) ---
    if (g_machine_type != 4) {
        tft->setTextColor(SV_COLOR_DIM, SV_COLOR_BG);
        tft->drawString("GIME: n/a (CoCo 2 mode)", x, y);
        y += lh;
    } else {
    TCC1014* g = &m->gime;
    tft->setTextColor(SV_COLOR_TEXT, SV_COLOR_BG);
    tft->drawString("GIME State", x, y);
    y += lh + 2;

    tft->setTextColor(SV_COLOR_DIM, SV_COLOR_BG);
    snprintf(buf, sizeof(buf), "INIT0=%02X COCO=%d MMUEN=%d TR=%d",
             g->registers[0], g->COCO ? 1 : 0, g->MMUEN ? 1 : 0,
             g->TR ? 1 : 0);
    tft->drawString(buf, x, y); y += lh;

    snprintf(buf, sizeof(buf), "VMODE=%02X BP=%d LPR=%d H50=%d",
             g->registers[8], g->BP ? 1 : 0, g->LPR, g->H50 ? 1 : 0);
    tft->drawString(buf, x, y); y += lh;

    snprintf(buf, sizeof(buf), "VRES=%02X LPF=%d HRES=%d CRES=%d",
             g->registers[9], g->LPF, g->HRES, g->CRES);
    tft->drawString(buf, x, y); y += lh;

    snprintf(buf, sizeof(buf), "Y=%05X B=%05X BPR=%d res=%d",
             g->Y, g->B, g->BPR, g->resolution);
    tft->drawString(buf, x, y); y += lh;

    snprintf(buf, sizeof(buf), "IRQen=%02X FIRQen=%02X irqs=%02X firqs=%02X",
             g->registers[2], g->registers[3],
             g->irq_state, g->firq_state);
    tft->drawString(buf, x, y); y += lh;

    snprintf(buf, sizeof(buf), "TMR=%d lAA=%u row=%d scan=%d",
             g->timer_counter, g->vertical.lAA, g->row, m->scanline);
    tft->drawString(buf, x, y); y += lh;
    }
#endif

}

// ============================================================
// Status serial dump — invoked only on ENTER (never from a plain
// render), so navigating onto the Status page no longer spams serial.
// ============================================================

static void status_dump_to_serial(Supervisor_t* sv) {
    Machine* m = sv->machine;
    if (!m) return;
    MC6809* cpu = &m->cpu;

    Serial.println("\n=== DEBUG STATUS ===");
    Serial.printf("PC=%04X S=%04X U=%04X X=%04X Y=%04X D=%04X\n",
                  cpu->pc, cpu->s, cpu->u, cpu->x, cpu->y, cpu->d);
    Serial.printf("DP=%02X CC=%02X [%c%c%c%c%c%c%c%c]\n",
                  cpu->dp, cpu->cc,
                  (cpu->cc & 0x80) ? 'E' : '-', (cpu->cc & 0x40) ? 'F' : '-',
                  (cpu->cc & 0x20) ? 'H' : '-', (cpu->cc & 0x10) ? 'I' : '-',
                  (cpu->cc & 0x08) ? 'N' : '-', (cpu->cc & 0x04) ? 'Z' : '-',
                  (cpu->cc & 0x02) ? 'V' : '-', (cpu->cc & 0x01) ? 'C' : '-');
    Serial.printf("HALT=%d CWAI=%d NMI=%d IRQ=%d FIRQ=%d\n",
                  cpu->halted, cpu->wait_for_interrupt,
                  cpu->nmi_pending, cpu->irq_pending, cpu->firq_pending);
#if MACHINE_TYPE == 4
    if (g_machine_type == 4) {
    TCC1014* gs = &m->gime;
    Serial.printf("INIT0=%02X INIT1=%02X COCO=%d MMUEN=%d TR=%d\n",
                  gs->registers[0], gs->registers[1],
                  gs->COCO ? 1 : 0, gs->MMUEN ? 1 : 0, gs->TR ? 1 : 0);
    Serial.printf("VMODE=%02X VRES=%02X BP=%d LPF=%d HRES=%d CRES=%d LPR=%d\n",
                  gs->registers[8], gs->registers[9],
                  gs->BP ? 1 : 0, gs->LPF, gs->HRES, gs->CRES, gs->LPR);
    Serial.printf("Y=%05X B=%05X BPR=%d stride=%d res=%d\n",
                  gs->Y, gs->B, gs->BPR, gs->row_stride, gs->resolution);
    Serial.printf("IRQen=%02X FIRQen=%02X irqs=%02X firqs=%02X\n",
                  gs->registers[2], gs->registers[3],
                  gs->irq_state, gs->firq_state);
    Serial.printf("TMR=%d TINS=%d lAA=%u lTB=%u row=%d scan=%d\n",
                  gs->timer_counter, gs->TINS ? 1 : 0,
                  gs->vertical.lAA, gs->vertical.lTB,
                  gs->row, m->scanline);
    Serial.printf("MMU task%d: ", gs->TR ? 1 : 0);
    for (int i = 0; i < 8; i++)
        Serial.printf("%02X ", gs->mmu_bank[gs->TR | i]);
    Serial.println();
    Serial.printf("Palette: ");
    for (int i = 0; i < 16; i++)
        Serial.printf("%02X ", gs->palette_reg[i]);
    Serial.println();
    } else {
        Serial.println("GIME: n/a (CoCo 2 mode)");
    }
#endif
    Serial.println("====================\n");
}

// ============================================================
// Dump page — hex dump to TFT + serial
// ============================================================

static void dump_to_serial(uint16_t addr) {
    Serial.printf("\n=== HEX DUMP $%04X-$%04X ===\n", addr, addr + DUMP_BYTES_PER_LINE * DUMP_LINES - 1);
    for (int row = 0; row < DUMP_LINES; row++) {
        uint16_t a = addr + row * DUMP_BYTES_PER_LINE;
        Serial.printf("%04X: ", a);
        char ascii[DUMP_BYTES_PER_LINE + 1];
        for (int col = 0; col < DUMP_BYTES_PER_LINE; col++) {
            uint8_t b = machine_read(a + col);
            Serial.printf("%02X ", b);
            ascii[col] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
        }
        ascii[DUMP_BYTES_PER_LINE] = 0;
        Serial.printf(" |%s|\n", ascii);
    }
    Serial.println("============================\n");
}

static void dump_render(Supervisor_t* sv) {
    sv_render_frame("Debug: Hex Dump", "L/R=Page U/D=Scroll 0-F=Addr ENT=Serial");

    Machine* m = sv->machine;
    if (!m) return;

    OSDCanvas* tft = hal_video_get_canvas();
    if (!tft) return;

    tft->setTextFont(0);  // compact 6x8 — hex dump needs the density
    tft->setTextDatum(TL_DATUM);

    int x = SV_CONTENT_X;
    int y = SV_CONTENT_Y + 2;
    int lh = 9;
    char buf[80];

    // Address field
    tft->setTextColor(dbg.active_field == SV_DBG_FIELD_ADDR ?
                      SV_COLOR_HL_TEXT : SV_COLOR_TEXT, SV_COLOR_BG);
    snprintf(buf, sizeof(buf), "Addr: $%04X  (type 4 hex digits)", dbg.dump_addr);
    tft->drawString(buf, x, y); y += lh + 4;

    // Hex dump header
    tft->setTextColor(SV_COLOR_TEXT, SV_COLOR_BG);
    tft->drawString("ADDR +0+1+2+3+4+5+6+7 +8+9+A+B+C+D+E+F", x, y);
    y += lh;

    // Dump 8 rows × 16 bytes = 128 bytes
    tft->setTextColor(SV_COLOR_DIM, SV_COLOR_BG);
    for (int row = 0; row < DUMP_LINES; row++) {
        uint16_t a = dbg.dump_addr + row * DUMP_BYTES_PER_LINE;
        int bx = x;

        // Address
        snprintf(buf, sizeof(buf), "%04X:", a);
        tft->drawString(buf, bx, y);
        bx += 6 * 5;  // 5 chars × 6px

        // Hex bytes (compact: 2 chars + space per byte)
        for (int col = 0; col < DUMP_BYTES_PER_LINE; col++) {
            uint8_t b = machine_read(a + col);
            snprintf(buf, sizeof(buf), "%02X", b);
            tft->drawString(buf, bx, y);
            bx += 6 * 2;  // 2 chars
            if (col == 7) bx += 3;  // gap between groups
        }

        y += lh;
    }

    y += 2;
    // Dump action — ENTER always dumps the current window (see key handler).
    tft->setTextColor(SV_COLOR_HL_TEXT, SV_COLOR_BG);
    tft->drawString("[ENTER] Dump to Serial", x, y);
}

// ============================================================
// Key handler
// ============================================================

void sv_debug_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed) {
    if (!pressed) return;

    switch (hid_usage) {
    case HID_LEFT:
        // Switch page
        if (dbg.page > 0) {
            dbg.page = (SV_DebugPage)(dbg.page - 1);
        } else {
            dbg.page = (SV_DebugPage)(SV_DBG_PAGE_COUNT - 1);
        }
        sv->needs_redraw = true;
        break;

    case HID_RIGHT:
        dbg.page = (SV_DebugPage)((dbg.page + 1) % SV_DBG_PAGE_COUNT);
        sv->needs_redraw = true;
        break;

    case HID_ESC:
        sv->state = SV_DEBUG_MENU;
        sv->menu_cursor = (int8_t)dbg.page;
        sv->menu_item_count = SV_DBG_PAGE_COUNT;
        sv->needs_redraw = true;
        break;

    default:
        if (dbg.page != SV_DBG_PAGE_DUMP) {
            // Status / RS-232 pages: read-only. ENTER refreshes the screen and,
            // on the Status page, dumps to serial. Navigation alone never dumps.
            if (hid_usage == HID_ENTER) {
                if (dbg.page == SV_DBG_PAGE_STATUS) status_dump_to_serial(sv);
                sv->needs_redraw = true;
            }
        } else {
            // Dump page
            switch (hid_usage) {
            case HID_UP:
                if (dbg.active_field == SV_DBG_FIELD_ADDR) {
                    // Scroll up by one page
                    dbg.dump_addr -= DUMP_BYTES_PER_LINE * DUMP_LINES;
                } else {
                    dbg.active_field = (SV_DebugField)(dbg.active_field - 1);
                }
                sv->needs_redraw = true;
                break;

            case HID_DOWN:
                if (dbg.active_field == SV_DBG_FIELD_ADDR) {
                    // Scroll down by one page
                    dbg.dump_addr += DUMP_BYTES_PER_LINE * DUMP_LINES;
                } else if (dbg.active_field < SV_DBG_FIELD_COUNT - 1) {
                    dbg.active_field = (SV_DebugField)(dbg.active_field + 1);
                }
                sv->needs_redraw = true;
                break;

            case HID_ENTER:
                // ENTER always dumps the current 128-byte window to serial, as
                // the footer documents (ENT=Serial). The address is set with the
                // 0-F hex keys and U/D scrolls it, so there is no field
                // navigation: U/D never leaves SV_DBG_FIELD_ADDR, which made the
                // old DUMP_BTN-gated check unreachable and ENTER a no-op.
                dump_to_serial(dbg.dump_addr);
                sv->needs_redraw = true;
                break;

            default: {
                // Hex digit input for address field
                if (dbg.active_field != SV_DBG_FIELD_ADDR) break;

                int nibble_val = -1;
                if (hid_usage >= 0x04 && hid_usage <= 0x09)
                    nibble_val = 0x0A + (hid_usage - 0x04);
                else if (hid_usage == 0x27)
                    nibble_val = 0;
                else if (hid_usage >= 0x1E && hid_usage <= 0x26)
                    nibble_val = 1 + (hid_usage - 0x1E);

                if (nibble_val >= 0 && nibble_val <= 0x0F) {
                    // Rolling hex entry: shift the address left one nibble and
                    // append the new digit at the low position. Typing four
                    // digits sets a full $XXXX address; extra digits roll off
                    // the top. (The old fixed cursor stuck at digit 3, so only
                    // the last nibble could be changed.)
                    dbg.dump_addr = (uint16_t)((dbg.dump_addr << 4) | (unsigned)nibble_val);
                    sv->needs_redraw = true;
                }
                break;
            }
            }
        }
        break;
    }
}

// ============================================================
// Public API
// ============================================================

void sv_debug_init(Supervisor_t* sv) {
    dbg.page         = SV_DBG_PAGE_STATUS;
    dbg.active_field = SV_DBG_FIELD_ADDR;
    dbg.dump_addr    = 0x0000;
    dbg.hex_digit    = 0;
    dbg.dumping      = false;
}

void sv_debug_set_page(SV_DebugPage page) {
    if (page < SV_DBG_PAGE_COUNT) dbg.page = page;
}

// ============================================================
// RS-232 Pak page — ACIA registers + transport counters
// ============================================================

static void rs232_render(Supervisor_t* sv) {
    sv_render_frame("Debug: RS-232 Pak", "L/R=Page  ESC=Back");
    (void)sv;

    OSDCanvas* tft = hal_video_get_canvas();
    if (!tft) return;

    tft->setTextFont(0);  // compact 6x8
    tft->setTextDatum(TL_DATUM);

    int x = SV_CONTENT_X;
    int y = SV_CONTENT_Y + 2;
    int lh = 9;
    char buf[64];

    MC6551Debug d;
    mc6551_get_debug(&d);

    tft->setTextColor(SV_COLOR_TEXT, SV_COLOR_BG);
    snprintf(buf, sizeof(buf), "RS-232 PAK   [%s]",
             rs232_pak_enabled() ? "enabled" : "disabled");
    tft->drawString(buf, x, y); y += lh + 2;

    tft->setTextColor(SV_COLOR_DIM, SV_COLOR_BG);
    snprintf(buf, sizeof(buf), "CTRL $%02X  STAT $%02X  CMD $%02X",
             d.control, d.status, d.command);
    tft->drawString(buf, x, y); y += lh;

    snprintf(buf, sizeof(buf), "Baud %u   Echo %s",
             (unsigned)d.baud, d.echo ? "on" : "off");
    tft->drawString(buf, x, y); y += lh;

    snprintf(buf, sizeof(buf), "TX cnt %lu  TDRE %d  TX-IRQ %s",
             (unsigned long)d.tx_count, d.tdre ? 1 : 0, d.tx_irq_en ? "on" : "off");
    tft->drawString(buf, x, y); y += lh;

    snprintf(buf, sizeof(buf), "RX cnt %lu  RDRF %d  RX-IRQ %s",
             (unsigned long)d.rx_count, d.rdrf ? 1 : 0, d.rx_irq_en ? "on" : "off");
    tft->drawString(buf, x, y); y += lh;

    snprintf(buf, sizeof(buf), "Overrun %lu  Ring %u/%u",
             (unsigned long)d.overrun_count,
             (unsigned)hal_rs232_ring_fill(), (unsigned)hal_rs232_ring_capacity());
    tft->drawString(buf, x, y); y += lh;

    snprintf(buf, sizeof(buf), "FIRQ asserts: %lu", (unsigned long)d.firq_count);
    tft->drawString(buf, x, y); y += lh;
}

void sv_debug_render(Supervisor_t* sv) {
    switch (dbg.page) {
    case SV_DBG_PAGE_STATUS:
        status_render(sv);
        break;
    case SV_DBG_PAGE_DUMP:
        dump_render(sv);
        break;
    case SV_DBG_PAGE_RS232:
        rs232_render(sv);
        break;
    default:
        break;
    }
}

// ============================================================
// "Dump RAM to SD" screen — full-memory dump to hex-text files
//
// Writes two files under /DUMPS/ on the SD card:
//   <NAME>-CPU.txt  64KB CPU address space as the 6809 sees it now
//                   (RAM via the current MMU map + mapped ROM + I/O).
//   <NAME>-RAM.txt  full physical RAM: 512KB on CoCo 3, 64KB on CoCo 2.
// Both use the same "AAAAA: bb bb ... |ascii|" layout as the serial dump.
// ============================================================

#define HID_BACKSPACE 0x2A

// 4KB batch buffer so a 512KB dump becomes ~256 bulk SD writes, not 32K
// per-line writes. Allocated on the (internal, DMA-capable) heap only for the
// duration of a dump and freed afterwards — keeps it out of permanent .bss so
// the WiFi/WebServer static pools fit in internal DRAM.
#define DUMP_BUF_SIZE 4096
static char* s_dump_buf = nullptr;

// Format one 16-byte hex line into out[]. five=true uses a 5-digit address
// column (physical RAM up to $7FFFF); false uses 4 digits (64KB CPU space).
// Returns the number of characters written (max ~74, so out must be >= 80).
static int fmt_hex_line(char* out, uint32_t addr, const uint8_t* bytes,
                        int n, bool five) {
    int p = 0;
    p += snprintf(out + p, 9, five ? "%05X: " : "%04X: ", addr);
    char ascii[17];
    for (int i = 0; i < 16; i++) {
        if (i < n) {
            uint8_t b = bytes[i];
            p += snprintf(out + p, 4, "%02X ", b);
            ascii[i] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
        } else {
            p += snprintf(out + p, 4, "   ");
            ascii[i] = ' ';
        }
    }
    ascii[16] = 0;
    p += snprintf(out + p, 20, "|%s|\n", ascii);
    return p;
}

// Flush helper: append a formatted line to the batch buffer, flushing to the
// file when the buffer would overflow. Returns false on a write error.
static bool dump_emit(File& f, int& blen, const char* line, int ll) {
    if (blen + ll > DUMP_BUF_SIZE) {
        if (f.write((const uint8_t*)s_dump_buf, blen) != (size_t)blen) return false;
        blen = 0;
    }
    memcpy(s_dump_buf + blen, line, ll);
    blen += ll;
    return true;
}

// Write a flat memory buffer as hex text. Returns bytes written, or -1 on error.
static long write_hex_buffer(const char* path, const uint8_t* mem,
                             uint32_t size, bool five) {
    File f = SD.open(path, FILE_WRITE);
    if (!f) return -1;
    int blen = 0;
    char line[80];
    for (uint32_t a = 0; a < size; a += 16) {
        int n = (size - a >= 16) ? 16 : (int)(size - a);
        int ll = fmt_hex_line(line, a, mem + a, n, five);
        if (!dump_emit(f, blen, line, ll)) { f.close(); return -1; }
    }
    if (blen && f.write((const uint8_t*)s_dump_buf, blen) != (size_t)blen) {
        f.close();
        return -1;
    }
    long total = (long)f.size();
    f.close();
    return total;
}

// Write the 64KB CPU address space as hex text, fetched via machine_read so
// the dump reflects the live MMU map + mapped ROM/I-O. Returns bytes or -1.
static long write_hex_cpu(const char* path) {
    File f = SD.open(path, FILE_WRITE);
    if (!f) return -1;
    int blen = 0;
    char line[80];
    uint8_t bytes[16];
    for (uint32_t a = 0; a <= 0xFFFF; a += 16) {
        for (int i = 0; i < 16; i++)
            bytes[i] = machine_read((uint16_t)(a + i));
        int ll = fmt_hex_line(line, a, bytes, 16, false);
        if (!dump_emit(f, blen, line, ll)) { f.close(); return -1; }
    }
    if (blen && f.write((const uint8_t*)s_dump_buf, blen) != (size_t)blen) {
        f.close();
        return -1;
    }
    long total = (long)f.size();
    f.close();
    return total;
}

static void dump_addline(const char* fmt, ...) {
    if (dbg.dump_result_lines >= 4) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dbg.dump_result[dbg.dump_result_lines],
              sizeof(dbg.dump_result[0]), fmt, ap);
    va_end(ap);
    dbg.dump_result_lines++;
}

// Perform the actual blocking dump and fill dbg.dump_result[] for display.
static void perform_dump(Supervisor_t* sv) {
    dbg.dump_result_lines = 0;

    Machine* m = sv->machine;
    if (!m) {
        dump_addline("ERROR: no machine");
        dump_addline("Press any key to return");
        return;
    }
    if (!hal_storage_is_ready()) {
        dump_addline("ERROR: SD card not ready");
        dump_addline("Press any key to return");
        return;
    }

    SD.mkdir("/DUMPS");

    // Allocate the batch buffer on the internal (DMA-capable) heap for the
    // duration of the dump; freed before returning.
    s_dump_buf = (char*)malloc(DUMP_BUF_SIZE);
    if (!s_dump_buf) {
        dump_addline("ERROR: out of memory");
        dump_addline("Press any key to return");
        return;
    }

    char path[40];
    long cpu_bytes, ram_bytes;

    Serial.printf("\n[DUMP] Writing CPU space to /DUMPS/%s-CPU.txt ...\n",
                  dbg.dump_name);
    snprintf(path, sizeof(path), "/DUMPS/%s-CPU.txt", dbg.dump_name);
    cpu_bytes = write_hex_cpu(path);

    uint32_t ram_size = (g_machine_type == 4) ? COCO3_PHYSICAL_RAM : 0x10000;
    Serial.printf("[DUMP] Writing %u KB physical RAM to /DUMPS/%s-RAM.txt ...\n",
                  (unsigned)(ram_size / 1024), dbg.dump_name);
    snprintf(path, sizeof(path), "/DUMPS/%s-RAM.txt", dbg.dump_name);
    ram_bytes = write_hex_buffer(path, m->ram_physical, ram_size,
                                 ram_size > 0x10000);

    Serial.printf("[DUMP] Done. CPU=%ld RAM=%ld bytes\n", cpu_bytes, ram_bytes);

    free(s_dump_buf);
    s_dump_buf = nullptr;

    if (cpu_bytes < 0 || ram_bytes < 0) {
        dump_addline("ERROR writing to SD card");
        if (cpu_bytes < 0) dump_addline("  CPU file failed");
        if (ram_bytes < 0) dump_addline("  RAM file failed");
        dump_addline("Press any key to return");
        return;
    }

    dump_addline("Saved to /DUMPS/ :");
    dump_addline("  %s-CPU.txt (%ld KB)", dbg.dump_name, (cpu_bytes + 1023) / 1024);
    dump_addline("  %s-RAM.txt (%ld KB)", dbg.dump_name, (ram_bytes + 1023) / 1024);
    dump_addline("Press any key to return");
}

void sv_debug_begin_dump(Supervisor_t* sv) {
    dbg.dump_phase = SV_DUMP_INPUT;
    strncpy(dbg.dump_name, "COCODUMP", sizeof(dbg.dump_name));
    dbg.dump_name[SV_DUMP_NAME_MAX] = 0;
    dbg.dump_name_len = (uint8_t)strlen(dbg.dump_name);
    dbg.dump_result_lines = 0;
    sv->state = SV_DEBUG_DUMP_NAME;
    sv->needs_redraw = true;
}

void sv_debug_dump_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed) {
    if (!pressed) return;

    if (dbg.dump_phase == SV_DUMP_SAVING) return;  // ignore keys mid-write

    if (dbg.dump_phase == SV_DUMP_RESULT) {
        // Any key returns to the Debug submenu.
        dbg.dump_phase = SV_DUMP_INPUT;
        sv->state = SV_DEBUG_MENU;
        sv->menu_cursor = SV_DBG_PAGE_COUNT;  // keep cursor on the dump row
        sv->menu_item_count = SV_DBG_PAGE_COUNT + 1;
        sv->needs_redraw = true;
        return;
    }

    // SV_DUMP_INPUT — filename entry
    switch (hid_usage) {
    case HID_ESC:
        sv->state = SV_DEBUG_MENU;
        sv->menu_cursor = SV_DBG_PAGE_COUNT;
        sv->menu_item_count = SV_DBG_PAGE_COUNT + 1;
        sv->needs_redraw = true;
        break;

    case HID_ENTER:
        if (dbg.dump_name_len > 0) {
            dbg.dump_phase = SV_DUMP_SAVING;
            sv->needs_redraw = true;  // render draws "Saving..." then writes
        }
        break;

    case HID_BACKSPACE:
        if (dbg.dump_name_len > 0) {
            dbg.dump_name[--dbg.dump_name_len] = 0;
            sv->needs_redraw = true;
        }
        break;

    default: {
        // The supervisor HID route delivers uppercase letters (0x04-0x1D) and
        // digits (0x1E-0x27); symbols are dropped upstream, which keeps names
        // FAT-friendly. Append until the 8-char cap.
        if (dbg.dump_name_len >= SV_DUMP_NAME_MAX) break;
        char c = 0;
        if (hid_usage >= 0x04 && hid_usage <= 0x1D)
            c = 'A' + (hid_usage - 0x04);
        else if (hid_usage >= 0x1E && hid_usage <= 0x26)
            c = '1' + (hid_usage - 0x1E);
        else if (hid_usage == 0x27)
            c = '0';
        if (c) {
            dbg.dump_name[dbg.dump_name_len++] = c;
            dbg.dump_name[dbg.dump_name_len] = 0;
            sv->needs_redraw = true;
        }
        break;
    }
    }
}

void sv_debug_dump_render(Supervisor_t* sv) {
    OSDCanvas* tft = hal_video_get_canvas();

    if (dbg.dump_phase == SV_DUMP_SAVING) {
        // Draw the wait banner FIRST (FabGL scans the framebuffer out
        // continuously, so it is on screen immediately), then block on the
        // write, then flip to the result screen on the next redraw.
        sv_render_frame("Dump RAM to SD", "Saving...");
        if (tft) {
            tft->setTextFont(0);
            tft->setTextDatum(TL_DATUM);
            tft->setTextColor(SV_COLOR_TEXT, SV_COLOR_BG);
            int x = SV_CONTENT_X, y = SV_CONTENT_Y + 8;
            tft->drawString("Saving to SD card, please wait...", x, y); y += 14;
            tft->setTextColor(SV_COLOR_DIM, SV_COLOR_BG);
            tft->drawString("Writing full RAM as hex (~512KB).", x, y); y += 10;
            tft->drawString("This can take ~30s. Do not remove card.", x, y);
        }
        perform_dump(sv);
        dbg.dump_phase = SV_DUMP_RESULT;
        sv->needs_redraw = true;
        return;
    }

    if (dbg.dump_phase == SV_DUMP_RESULT) {
        sv_render_frame("Dump RAM to SD", "Press any key to return");
        if (!tft) return;
        tft->setTextFont(0);
        tft->setTextDatum(TL_DATUM);
        int x = SV_CONTENT_X, y = SV_CONTENT_Y + 8;
        bool err = (dbg.dump_result_lines > 0 &&
                    strncmp(dbg.dump_result[0], "ERROR", 5) == 0);
        for (int i = 0; i < dbg.dump_result_lines; i++) {
            tft->setTextColor((i == 0 && err) ? SV_COLOR_WARN : SV_COLOR_TEXT,
                              SV_COLOR_BG);
            tft->drawString(dbg.dump_result[i], x, y);
            y += 12;
        }
        return;
    }

    // SV_DUMP_INPUT
    sv_render_frame("Dump RAM to SD", "A-Z 0-9  BkSp  ENTER Save  ESC Cancel");
    if (!tft) return;
    tft->setTextFont(0);
    tft->setTextDatum(TL_DATUM);
    int x = SV_CONTENT_X, y = SV_CONTENT_Y + 4;

    tft->setTextColor(SV_COLOR_TEXT, SV_COLOR_BG);
    tft->drawString("Enter filename (max 8 chars):", x, y);
    y += 16;

    char field[24];
    snprintf(field, sizeof(field), "Name: %s%s",
             dbg.dump_name, dbg.dump_name_len < SV_DUMP_NAME_MAX ? "_" : "");
    tft->setTextColor(SV_COLOR_HL_TEXT, SV_COLOR_BG);
    tft->drawString(field, x, y);
    y += 18;

    tft->setTextColor(SV_COLOR_DIM, SV_COLOR_BG);
    tft->drawString("Writes two hex-text files to /DUMPS/:", x, y); y += 11;
    char l[44];
    snprintf(l, sizeof(l), "  %s-CPU.txt  64KB CPU address space",
             dbg.dump_name_len ? dbg.dump_name : "NAME");
    tft->drawString(l, x, y); y += 10;
    snprintf(l, sizeof(l), "  %s-RAM.txt  %uKB physical RAM",
             dbg.dump_name_len ? dbg.dump_name : "NAME",
             (unsigned)(((g_machine_type == 4) ? COCO3_PHYSICAL_RAM : 0x10000) / 1024));
    tft->drawString(l, x, y); y += 13;
    tft->drawString("Machine is paused during the dump.", x, y);
}
