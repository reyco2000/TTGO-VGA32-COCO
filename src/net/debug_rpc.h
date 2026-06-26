/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : debug_rpc.h
 *  Module : Cross-core RPC bridge for the WiFi debug server
 * ============================================================
 *
 * The emulator runs single-threaded on core 1 (Arduino loop()). The WiFi
 * debug server runs on a core-0 task. To touch emulator state (CPU regs,
 * memory, reset) safely, core 0 NEVER accesses it directly: it fills a
 * DebugCmd, submits it, and blocks on a completion semaphore. Core 1 drains
 * the single-slot queue between frames via debug_rpc_poll(), executes the
 * command against the active Machine, and signals completion.
 *
 * Pause/resume is a simple flag read by loop(): while paused, loop() stops
 * calling machine_run_frame() and just spins on debug_rpc_poll() so commands
 * are still serviced.
 */

#ifndef NET_DEBUG_RPC_H
#define NET_DEBUG_RPC_H

#include <stdint.h>
#include <stdbool.h>
#include "../core/machine.h"

// --- Register selection mask (DBG_CMD_WRITE_REGS) ---
#define DBG_REG_PC  (1u << 0)
#define DBG_REG_D   (1u << 1)
#define DBG_REG_X   (1u << 2)
#define DBG_REG_Y   (1u << 3)
#define DBG_REG_U   (1u << 4)
#define DBG_REG_S   (1u << 5)
#define DBG_REG_DP  (1u << 6)
#define DBG_REG_CC  (1u << 7)

// --- Memory address space ---
#define DBG_SPACE_CPU  0   // MMU-correct, via machine_read/write
#define DBG_SPACE_PHYS 1   // raw ram_physical[] (CoCo 3 512KB)

// --- Command types that must run on core 1 ---
typedef enum {
    DBG_CMD_READ_REGS,
    DBG_CMD_WRITE_REGS,
    DBG_CMD_READ_MEM,
    DBG_CMD_WRITE_MEM,
    DBG_CMD_INJECT,
    DBG_CMD_RESET,
    DBG_CMD_STEP_FRAME,   // run exactly one machine_run_frame() (for screenshots)
} DebugCmdType;

// --- Result codes ---
#define DBG_OK            0
#define DBG_ERR_NOMACHINE (-1)
#define DBG_ERR_RANGE     (-2)
#define DBG_ERR_NOSPACE   (-3)   // phys space requested but RAM not present

// Single command slot. For memory ops, `buf` is caller-owned (core 0) and
// stays valid until debug_rpc_submit() returns.
typedef struct {
    DebugCmdType type;

    // Registers (read output / write input)
    uint16_t pc, d, x, y, u, s;
    uint8_t  dp, cc;
    uint32_t cycles;     // read-only: total CPU cycles
    uint16_t reg_mask;   // DBG_CMD_WRITE_REGS: which fields to apply

    // Memory / inject
    uint32_t addr;
    uint32_t len;
    uint8_t* buf;
    uint8_t  space;      // DBG_SPACE_CPU / DBG_SPACE_PHYS

    // Inject extras
    bool set_pc;
    bool resume_after;

    // Output
    int8_t result;
} DebugCmd;

// Bind the active machine and create FreeRTOS primitives. Call once from setup().
void debug_rpc_init(Machine* m);

// Core 0: submit a command and block until core 1 executes it (or timeout).
// Returns true if completed, false on timeout. On success, cmd->result and any
// read fields/buffers are populated.
bool debug_rpc_submit(DebugCmd* cmd, uint32_t timeout_ms);

// Core 1: drain at most one pending command. Call between frames in loop().
void debug_rpc_poll(void);

// Pause flag (set/read from either core).
void debug_rpc_set_paused(bool paused);
bool debug_rpc_is_paused(void);

#endif // NET_DEBUG_RPC_H
