/*
 * GIME validation suite: C++ API over MAME's gime.cpp (gime_ntsc_device),
 * built in the stand-in form (mame_gime.cpp against mame_shim/). Two builds
 * exist side by side, one per GIME_TYPE_1987 value (gime.cpp:99), each in its
 * own namespace. Time is in 14.31818 MHz ticks (two 28.636363 MHz master
 * clocks) and only moves through advance().
 */
#ifndef MAME_GIME_H
#define MAME_GIME_H

#include "gime_model.h"
#include <memory>
#include <stdint.h>
#include <vector>

class MameGime {
public:
    virtual ~MameGime() {}
    virtual void write(uint16_t addr, uint8_t val) = 0;   // CPU write at now()
    virtual uint8_t read(uint16_t addr) = 0;              // CPU read, with side effects
    // S/RAS as the coco3.cpp address map selects; Z is valid only for RAM.
    virtual void decode(uint16_t addr, unsigned* S, uint32_t* Z, bool* RAS, bool* z_valid) = 0;
    virtual void advance(uint64_t ticks) = 0;
    virtual uint64_t now() const = 0;
    virtual bool irq() const = 0;                         // the irq_wr callback's level
    virtual bool firq() const = 0;
    virtual void set_il(unsigned line, bool level) = 0;
    virtual std::vector<Event> drain_events() = 0;
    // 6-bit colours of the active area of the most recent finished line.
    virtual bool last_line(std::vector<uint8_t>& colours) = 0;
    // Its border colour: the bitmap pixel left of the active area; -1 on a
    // wide line, which has no border pixels.
    virtual int last_line_border() const = 0;
};

namespace mame86 { std::unique_ptr<MameGime> create(uint8_t* ram, uint32_t ram_size); }
namespace mame87 { std::unique_ptr<MameGime> create(uint8_t* ram, uint32_t ram_size); }

#endif
