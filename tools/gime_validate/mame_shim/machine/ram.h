// GIME validation suite: stand-in for MAME's machine/ram.h: the RAM device is
// a pointer and a size (gime.cpp:613 memory_pointer, :1028).
#ifndef MAME_SHIM_RAM_H
#define MAME_SHIM_RAM_H
class ram_device {
public:
    ram_device(uint8_t* p, uint32_t size) : m_p(p), m_size(size) {}
    bool started() const { return true; }
    uint8_t* pointer() const { return m_p; }
    uint32_t size() const { return m_size; }
private:
    uint8_t* m_p;
    uint32_t m_size;
};
#endif
