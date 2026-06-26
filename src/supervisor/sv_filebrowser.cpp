/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : sv_filebrowser.cpp
 *  Module : SD card file browser — FAT32 directory browser for .DSK/.VDK disk image selection
 * ============================================================
*/

/*
 * sv_filebrowser.cpp - SD card file browser for disk images
 *
 * Browses FAT32 SD card directories.
 * Supports .DSK (JVC) and .VDK files. DMK shown with warning.
 *
 * IMPORTANT: SD card reads must happen OUTSIDE tft.startWrite()/endWrite()
 * because SD and TFT share the SPI bus.
 */

#include "sv_filebrowser.h"
#include "supervisor.h"
#include "sv_disk.h"
#include "sv_render.h"
#include "../utils/debug.h"
#include "../../config.h"
#include <SD.h>
#include <string.h>

// HID usage codes
#define HID_UP      0x52
#define HID_DOWN    0x51
#define HID_ENTER   0x28
#define HID_ESC     0x29
#define HID_BS      0x2A
#define HID_PGUP    0x4B
#define HID_PGDN    0x4E
#define HID_HOME    0x4A
#define HID_END     0x4D
#define HID_LEFT    0x50
#define HID_RIGHT   0x4F
#define HID_U       0x18
#define HID_F       0x09
#define HID_F1      0x3A

// File list starts below the two-row drive strip.
#define SV_FB_LIST_ROW0  2

// Selected file path buffer
static char selected_path[256];
static bool has_selection = false;

bool sv_fb_is_disk_image(const char* filename) {
    const char* ext = strrchr(filename, '.');
    if (!ext) return false;
    return (strcasecmp(ext, ".dsk") == 0 ||
            strcasecmp(ext, ".vdk") == 0 ||
            strcasecmp(ext, ".dmk") == 0);
}

static bool is_supported_format(const char* filename) {
    const char* ext = strrchr(filename, '.');
    if (!ext) return false;
    return (strcasecmp(ext, ".dsk") == 0 ||
            strcasecmp(ext, ".vdk") == 0);
    // DMK is recognized but not fully supported
}

static int strcasecmp_wrapper(const char* a, const char* b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return *a - *b;
}

int sv_fb_scan_directory(const char* path, SV_FileEntry* entries, int max_entries) {
    File dir = SD.open(path);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return 0;
    }

    int count = 0;

    // Add ".." unless at root
    if (strcmp(path, "/") != 0) {
        strncpy(entries[count].name, "..", sizeof(entries[count].name));
        entries[count].size = 0;
        entries[count].is_dir = true;
        entries[count].is_supported = false;
        count++;
    }

    File entry = dir.openNextFile();
    while (entry && count < max_entries) {
        const char* fullpath = entry.name();
        // ESP32 SD returns full path — extract just the filename
        const char* name = strrchr(fullpath, '/');
        name = name ? name + 1 : fullpath;
        // Skip hidden files
        if (name[0] == '.') {
            entry = dir.openNextFile();
            continue;
        }

        strncpy(entries[count].name, name, sizeof(entries[count].name) - 1);
        entries[count].name[sizeof(entries[count].name) - 1] = '\0';
        entries[count].is_dir = entry.isDirectory();
        entries[count].size = entry.isDirectory() ? 0 : entry.size();
        entries[count].is_supported = entries[count].is_dir ? false : is_supported_format(name);

        count++;
        entry = dir.openNextFile();
    }
    dir.close();

    return count;
}

void sv_fb_sort_entries(SV_FileEntry* entries, int count) {
    // Simple insertion sort (max 128 entries)
    for (int i = 1; i < count; i++) {
        SV_FileEntry temp = entries[i];
        int j = i - 1;

        while (j >= 0) {
            bool swap = false;

            // ".." always first
            if (strcmp(temp.name, "..") == 0) {
                swap = true;
            } else if (strcmp(entries[j].name, "..") == 0) {
                swap = false;
            }
            // Directories before files
            else if (temp.is_dir && !entries[j].is_dir) {
                swap = true;
            } else if (!temp.is_dir && entries[j].is_dir) {
                swap = false;
            }
            // Within same type: alphabetical (case-insensitive)
            else if (temp.is_dir == entries[j].is_dir) {
                // Supported files before unsupported
                if (!temp.is_dir && temp.is_supported != entries[j].is_supported) {
                    swap = temp.is_supported && !entries[j].is_supported;
                } else {
                    swap = strcasecmp_wrapper(temp.name, entries[j].name) < 0;
                }
            }

            if (swap) {
                entries[j + 1] = entries[j];
                j--;
            } else {
                break;
            }
        }
        entries[j + 1] = temp;
    }
}

void sv_filebrowser_init(Supervisor_t* sv) {
    sv->file_cursor = 0;
    sv->file_scroll_offset = 0;
    sv->file_count = 0;
    has_selection = false;
}

void sv_filebrowser_open(Supervisor_t* sv, const char* path, uint8_t target_drive) {
    sv->target_drive = target_drive;
    strncpy(sv->current_path, path, sizeof(sv->current_path) - 1);
    sv->current_path[sizeof(sv->current_path) - 1] = '\0';

    if (!sv->file_entries) {
        sv->file_entries = (SV_FileEntry*)malloc(SV_FB_MAX_ENTRIES * sizeof(SV_FileEntry));
        if (!sv->file_entries) {
            DEBUG_PRINT("FileBrowser: failed to allocate file entries");
            sv->file_count = 0;
            return;
        }
    }

    // Scan directory (SD reads happen here, before any TFT rendering)
    sv->file_count = sv_fb_scan_directory(path, sv->file_entries, SV_FB_MAX_ENTRIES);
    sv_fb_sort_entries(sv->file_entries, sv->file_count);

    sv->file_cursor = 0;
    sv->file_scroll_offset = 0;
    sv->needs_redraw = true;
    has_selection = false;

    DEBUG_PRINTF("FileBrowser: %s -> %d entries", path, sv->file_count);
}

static void navigate_to_parent(Supervisor_t* sv) {
    char* last_slash = strrchr(sv->current_path, '/');
    if (last_slash && last_slash != sv->current_path) {
        *last_slash = '\0';
    } else {
        strcpy(sv->current_path, "/");
    }
    sv_filebrowser_open(sv, sv->current_path, sv->target_drive);
}

static void enter_directory(Supervisor_t* sv, const char* dirname) {
    if (strcmp(dirname, "..") == 0) {
        navigate_to_parent(sv);
        return;
    }

    char new_path[256];
    if (strcmp(sv->current_path, "/") == 0) {
        snprintf(new_path, sizeof(new_path), "/%s", dirname);
    } else {
        snprintf(new_path, sizeof(new_path), "%s/%s", sv->current_path, dirname);
    }
    sv_filebrowser_open(sv, new_path, sv->target_drive);
}

static void select_file(Supervisor_t* sv, const char* filename) {
    if (strcmp(sv->current_path, "/") == 0) {
        snprintf(selected_path, sizeof(selected_path), "/%s", filename);
    } else {
        snprintf(selected_path, sizeof(selected_path), "%s/%s", sv->current_path, filename);
    }
    has_selection = true;

    // Mount the disk
    if (sv->machine) {
        bool ok = sv_disk_mount(&sv->machine->fdc, sv->target_drive, selected_path);
        if (ok) {
            DEBUG_PRINTF("FileBrowser: Mounted %s to drive %d", selected_path, sv->target_drive);
            // Save state for quick-mount
            supervisor_save_state();
        } else {
            DEBUG_PRINTF("FileBrowser: Failed to mount %s", selected_path);
        }
    }

    // Stay on the Disk Manager screen so the drive strip reflects the new
    // mount and the user can mount additional drives.
    sv->needs_redraw = true;
}

void sv_filebrowser_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed) {
    if (!pressed) return;
    if (sv->file_count == 0 &&
        hid_usage != HID_ESC && hid_usage != HID_BS &&
        hid_usage != HID_LEFT && hid_usage != HID_RIGHT &&
        hid_usage != HID_U && hid_usage != HID_F && hid_usage != HID_F1) {
        return;
    }

    switch (hid_usage) {
        case HID_UP:
            if (sv->file_cursor > 0) {
                sv->file_cursor--;
                if (sv->file_cursor < sv->file_scroll_offset) {
                    sv->file_scroll_offset = sv->file_cursor;
                }
                sv->needs_redraw = true;
            }
            break;

        case HID_DOWN:
            if (sv->file_cursor < sv->file_count - 1) {
                sv->file_cursor++;
                if (sv->file_cursor >= sv->file_scroll_offset + SV_FB_VISIBLE_ITEMS) {
                    sv->file_scroll_offset = sv->file_cursor - SV_FB_VISIBLE_ITEMS + 1;
                }
                sv->needs_redraw = true;
            }
            break;

        case HID_PGUP:
            sv->file_cursor -= SV_FB_VISIBLE_ITEMS;
            if (sv->file_cursor < 0) sv->file_cursor = 0;
            sv->file_scroll_offset = sv->file_cursor;
            sv->needs_redraw = true;
            break;

        case HID_PGDN:
            sv->file_cursor += SV_FB_VISIBLE_ITEMS;
            if (sv->file_cursor >= sv->file_count) sv->file_cursor = sv->file_count - 1;
            sv->file_scroll_offset = sv->file_cursor - SV_FB_VISIBLE_ITEMS + 1;
            if (sv->file_scroll_offset < 0) sv->file_scroll_offset = 0;
            sv->needs_redraw = true;
            break;

        case HID_HOME:
            sv->file_cursor = 0;
            sv->file_scroll_offset = 0;
            sv->needs_redraw = true;
            break;

        case HID_END:
            sv->file_cursor = sv->file_count - 1;
            sv->file_scroll_offset = sv->file_count - SV_FB_VISIBLE_ITEMS;
            if (sv->file_scroll_offset < 0) sv->file_scroll_offset = 0;
            sv->needs_redraw = true;
            break;

        case HID_ENTER: {
            SV_FileEntry* e = &sv->file_entries[sv->file_cursor];
            if (e->is_dir) {
                enter_directory(sv, e->name);
            } else if (e->is_supported) {
                select_file(sv, e->name);
            }
            break;
        }

        case HID_BS:
            navigate_to_parent(sv);
            break;

        case HID_LEFT:
            if (sv->target_drive > 0) {
                sv->target_drive--;
                sv->needs_redraw = true;
            }
            break;

        case HID_RIGHT:
            if (sv->target_drive < SV_DISK_MAX_DRIVES - 1) {
                sv->target_drive++;
                sv->needs_redraw = true;
            }
            break;

        case HID_U:
            if (sv->machine && sv_disk_is_mounted(&sv->machine->fdc, sv->target_drive)) {
                sv_disk_eject(&sv->machine->fdc, sv->target_drive);
                supervisor_save_state();
                sv->needs_redraw = true;
            }
            break;

        case HID_F:
            if (sv->machine && sv_disk_is_mounted(&sv->machine->fdc, sv->target_drive)) {
                sv_disk_flush(&sv->machine->fdc, sv->target_drive);
                sv->needs_redraw = true;
            }
            break;

        case HID_F1:
            supervisor_toggle();
            break;

        case HID_ESC:
            sv->state = SV_MAIN_MENU;
            sv->menu_cursor = 0;  // land on the "Disk Manager" item
            sv->needs_redraw = true;
            break;
    }
}

void sv_filebrowser_render(Supervisor_t* sv) {
    // --- Drive status strip (render rows 0-1) ---
    char        cell_buf[SV_DISK_MAX_DRIVES][24];
    const char* cells[SV_DISK_MAX_DRIVES];
    bool        mounted[SV_DISK_MAX_DRIVES];

    for (int d = 0; d < SV_DISK_MAX_DRIVES; d++) {
        mounted[d] = sv->machine && sv_disk_is_mounted(&sv->machine->fdc, d);
        if (mounted[d]) {
            const char* path  = sv_disk_get_path(&sv->machine->fdc, d);
            if (!path) path = "?";
            const char* fname = strrchr(path, '/');
            fname = fname ? fname + 1 : path;
            char shortname[11];  // up to 10 chars + NUL
            snprintf(shortname, sizeof(shortname), "%s", fname);
            snprintf(cell_buf[d], sizeof(cell_buf[d]), "D%d: %s", d, shortname);
        } else {
            snprintf(cell_buf[d], sizeof(cell_buf[d]), "D%d: (empty)", d);
        }
        cells[d] = cell_buf[d];
    }

    sv_render_frame("Disk Manager", "<>Drv ENT Mnt U Eject F Flsh ESC");
    sv_render_drive_strip(cells, mounted, sv->target_drive);

    if (sv->file_count == 0) return;

    int visible = SV_FB_VISIBLE_ITEMS;
    if (visible > sv->file_count) visible = sv->file_count;

    for (int i = 0; i < visible; i++) {
        int idx = sv->file_scroll_offset + i;
        if (idx >= sv->file_count) break;

        SV_FileEntry* e = &sv->file_entries[idx];
        sv_render_file_entry(i + SV_FB_LIST_ROW0, e->name, e->size,
                             e->is_dir, e->is_supported || e->is_dir,
                             idx == sv->file_cursor);
    }

    sv_render_scrollbar(sv->file_scroll_offset, SV_FB_VISIBLE_ITEMS,
                        sv->file_count, SV_FB_LIST_ROW0);
}

const char* sv_filebrowser_get_selected_path(Supervisor_t* sv) {
    (void)sv;
    return has_selection ? selected_path : nullptr;
}
