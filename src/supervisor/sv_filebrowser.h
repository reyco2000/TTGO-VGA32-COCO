/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : sv_filebrowser.h
 *  Module : File browser interface — disk image selection from SD card
 * ============================================================
*/

/*
 * sv_filebrowser.h - SD card file browser for disk image selection
 */

#ifndef SV_FILEBROWSER_H
#define SV_FILEBROWSER_H

#include <stdint.h>
#include <stdbool.h>

// Forward declaration
struct Supervisor_t;

#define SV_FB_VISIBLE_ITEMS    6
#define SV_FB_MAX_ENTRIES      128

struct SV_FileEntry {
    char     name[32];
    uint32_t size;
    bool     is_dir;
    bool     is_supported;
};

void sv_filebrowser_init(Supervisor_t* sv);
void sv_filebrowser_open(Supervisor_t* sv, const char* path, uint8_t target_drive);
void sv_filebrowser_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed);
void sv_filebrowser_render(Supervisor_t* sv);
const char* sv_filebrowser_get_selected_path(Supervisor_t* sv);

int  sv_fb_scan_directory(const char* path, SV_FileEntry* entries, int max_entries);
void sv_fb_sort_entries(SV_FileEntry* entries, int count);
bool sv_fb_is_disk_image(const char* filename);

#endif // SV_FILEBROWSER_H
