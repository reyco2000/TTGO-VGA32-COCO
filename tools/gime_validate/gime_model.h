/*
 * GIME validation suite: the interface every model (ours, XRoar, MAME)
 * implements. Time is in 14.31818 MHz pixel-clock ticks; one scanline is
 * TCC1014_tSL = 912 ticks.
 */
#ifndef GIME_MODEL_H
#define GIME_MODEL_H

#include <stdint.h>
#include <memory>
#include <vector>

enum class Variant { G1986, G1987 };
enum Cap : unsigned { CAP_REGS=1, CAP_DECODE=2, CAP_IRQ=4, CAP_TIMER_EV=8,
                      CAP_SYNC_EV=16, CAP_RENDER=32 };
enum class EvType { TIMER, HS_FALL, FS_FALL, FS_RISE, IRQ_RISE, FIRQ_RISE };
struct Event { EvType type; uint64_t tick; };
struct Decode { unsigned S; uint32_t Z; bool RAS; };   // XRoar semantics

class GimeModel {
public:
  virtual ~GimeModel() {}
  virtual const char* name() const = 0;          // "ours","xroar","mame"
  virtual unsigned caps() const = 0;
  virtual uint32_t resolution(Cap c) const = 0;  // ticks; 1 = exact
  virtual void reset(Variant v, uint8_t* ram, uint32_t ram_size) = 0;
  virtual void write(uint16_t addr, uint8_t val) = 0;  // CPU write at now()
  virtual uint8_t read(uint16_t addr) = 0;             // CPU read, with side effects
  virtual Decode decode(uint16_t addr) = 0;            // decode only, no side effects
  virtual void advance(uint64_t ticks) = 0;
  virtual uint64_t now() const = 0;
  virtual bool irq() const = 0;
  virtual bool firq() const = 0;
  virtual void set_il(unsigned line, bool level) = 0;  // IL0..IL2
  virtual std::vector<Event> drain_events() = 0;
  // Colours (6-bit) of the most recent finished line's active area; false
  // if this model has no rendered active area for that line.
  virtual bool last_line(std::vector<uint8_t>& colours, int& first_active_px) = 0;
};

std::unique_ptr<GimeModel> make_ours();

#endif
