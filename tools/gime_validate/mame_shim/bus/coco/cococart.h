// GIME validation suite: stand-in for MAME's bus/coco/cococart.h: an empty
// cartridge slot (no cartridge ROM), as the suite runs every model.
#ifndef MAME_SHIM_COCOCART_H
#define MAME_SHIM_COCOCART_H
class cococart_slot_device {
public:
    bool started() const { return true; }
    uint8_t* get_cart_base() { return nullptr; }
    uint32_t get_cart_size() { return 0x8000; }
};
#endif
