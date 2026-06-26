/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : debug_rpc.cpp
 *  Module : Cross-core RPC bridge for the WiFi debug server
 * ============================================================
 */

#include "debug_rpc.h"

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "../core/mc6809.h"
#include "../utils/debug.h"

static Machine*         s_machine   = nullptr;
static QueueHandle_t    s_cmd_queue = nullptr;   // depth 1, holds DebugCmd*
static SemaphoreHandle_t s_done_sem = nullptr;   // binary: command complete
static SemaphoreHandle_t s_lock     = nullptr;   // serialize submitters
static volatile bool    s_paused    = false;

void debug_rpc_init(Machine* m) {
    s_machine   = m;
    s_cmd_queue = xQueueCreate(1, sizeof(DebugCmd*));
    s_done_sem  = xSemaphoreCreateBinary();
    s_lock      = xSemaphoreCreateMutex();
    s_paused    = false;
}

void debug_rpc_set_paused(bool paused) { s_paused = paused; }
bool debug_rpc_is_paused(void)         { return s_paused; }

// --- core 1 execution of a single command ---

static void exec_read_regs(DebugCmd* c) {
    MC6809* cpu = &s_machine->cpu;
    c->pc = cpu->pc; c->d = cpu->d; c->x = cpu->x; c->y = cpu->y;
    c->u = cpu->u;   c->s = cpu->s; c->dp = cpu->dp; c->cc = cpu->cc;
    c->cycles = (uint32_t)cpu->cycles;
    c->result = DBG_OK;
}

static void exec_write_regs(DebugCmd* c) {
    MC6809* cpu = &s_machine->cpu;
    if (c->reg_mask & DBG_REG_PC) cpu->pc = c->pc;
    if (c->reg_mask & DBG_REG_D)  cpu->d  = c->d;
    if (c->reg_mask & DBG_REG_X)  cpu->x  = c->x;
    if (c->reg_mask & DBG_REG_Y)  cpu->y  = c->y;
    if (c->reg_mask & DBG_REG_U)  cpu->u  = c->u;
    if (c->reg_mask & DBG_REG_S)  cpu->s  = c->s;
    if (c->reg_mask & DBG_REG_DP) cpu->dp = c->dp;
    if (c->reg_mask & DBG_REG_CC) cpu->cc = c->cc;
    c->result = DBG_OK;
}

// Returns true if the requested phys range is valid.
static bool phys_range_ok(uint32_t addr, uint32_t len) {
    if (!s_machine->ram_physical || s_machine->ram_size == 0) return false;
    return ((uint64_t)addr + len) <= s_machine->ram_size;
}

static void exec_read_mem(DebugCmd* c) {
    if (c->space == DBG_SPACE_PHYS) {
        if (!s_machine->ram_physical || s_machine->ram_size == 0) { c->result = DBG_ERR_NOSPACE; return; }
        if (!phys_range_ok(c->addr, c->len)) { c->result = DBG_ERR_RANGE; return; }
        memcpy(c->buf, s_machine->ram_physical + c->addr, c->len);
    } else {
        // CPU space is 16-bit; wrap is the hardware behaviour.
        for (uint32_t i = 0; i < c->len; i++) {
            c->buf[i] = machine_read((uint16_t)(c->addr + i));
        }
    }
    c->result = DBG_OK;
}

static void exec_write_mem(DebugCmd* c) {
    if (c->space == DBG_SPACE_PHYS) {
        if (!s_machine->ram_physical || s_machine->ram_size == 0) { c->result = DBG_ERR_NOSPACE; return; }
        if (!phys_range_ok(c->addr, c->len)) { c->result = DBG_ERR_RANGE; return; }
        memcpy(s_machine->ram_physical + c->addr, c->buf, c->len);
    } else {
        for (uint32_t i = 0; i < c->len; i++) {
            machine_write((uint16_t)(c->addr + i), c->buf[i]);
        }
    }
    c->result = DBG_OK;
}

static void exec_inject(DebugCmd* c) {
    // Always CPU space for injection (code goes through the MMU).
    for (uint32_t i = 0; i < c->len; i++) {
        machine_write((uint16_t)(c->addr + i), c->buf[i]);
    }
    if (c->set_pc) s_machine->cpu.pc = c->pc;
    if (c->resume_after) s_paused = false;
    c->result = DBG_OK;
}

static void exec_one(DebugCmd* c) {
    if (!s_machine) { c->result = DBG_ERR_NOMACHINE; return; }
    switch (c->type) {
        case DBG_CMD_READ_REGS:  exec_read_regs(c);  break;
        case DBG_CMD_WRITE_REGS: exec_write_regs(c); break;
        case DBG_CMD_READ_MEM:   exec_read_mem(c);   break;
        case DBG_CMD_WRITE_MEM:  exec_write_mem(c);  break;
        case DBG_CMD_INJECT:     exec_inject(c);     break;
        case DBG_CMD_RESET:      machine_reset(s_machine); c->result = DBG_OK; break;
        case DBG_CMD_STEP_FRAME: machine_run_frame(s_machine); c->result = DBG_OK; break;
        default:                 c->result = DBG_ERR_RANGE; break;
    }
}

void debug_rpc_poll(void) {
    if (!s_cmd_queue) return;
    DebugCmd* c = nullptr;
    if (xQueueReceive(s_cmd_queue, &c, 0) == pdTRUE) {
        exec_one(c);
        xSemaphoreGive(s_done_sem);
    }
}

bool debug_rpc_submit(DebugCmd* cmd, uint32_t timeout_ms) {
    if (!s_machine || !s_cmd_queue) { if (cmd) cmd->result = DBG_ERR_NOMACHINE; return false; }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    // Drain any stale completion signal before submitting.
    xSemaphoreTake(s_done_sem, 0);

    cmd->result = DBG_OK;
    DebugCmd* p = cmd;
    xQueueSend(s_cmd_queue, &p, portMAX_DELAY);

    bool ok = xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    if (!ok) {
        DEBUG_PRINTF("debug_rpc: command %d timed out", (int)cmd->type);
    }

    xSemaphoreGive(s_lock);
    return ok;
}
