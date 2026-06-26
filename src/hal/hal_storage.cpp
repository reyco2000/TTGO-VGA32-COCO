/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : hal_storage.cpp
 *  Module : Storage HAL — SD card access via dedicated SPI bus
 * ============================================================
*/

/*
 * hal_storage.cpp - SD card storage implementation
 *
 * Pins from config.h: PIN_SD_CS, PIN_SD_MOSI, PIN_SD_MISO, PIN_SD_SCLK
 */

#include "hal.h"
#include "../utils/debug.h"
#include <SD.h>
#include <SPI.h>

// Use a DEDICATED SPI bus for SD card (SPI3/HSPI) to avoid conflicts with
// the FabGL VGA controller on the default SPI peripheral.
static SPIClass sd_spi(HSPI);

static bool storage_ready = false;

bool hal_storage_init(void) {
    DEBUG_PRINT("Storage: Initializing SD card...");
    DEBUG_PRINTF("  SPI pins: SCLK=%d MOSI=%d MISO=%d CS=%d",
                 PIN_SD_SCLK, PIN_SD_MOSI, PIN_SD_MISO, PIN_SD_CS);

    // Configure CS pin as output and deselect
    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);

    // Small delay to let card power up
    delay(100);

    // Initialize DEDICATED SPI bus for SD card (HSPI / SPI3)
    sd_spi.begin(PIN_SD_SCLK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);

    DEBUG_PRINTF("  Free heap before SD.begin: %d", ESP.getFreeHeap());
    DEBUG_PRINT("  Using dedicated HSPI bus for SD card");

    // Try mounting at lower speed first, then increase if it works
    // Some SD cards need a slow start
    bool mounted = false;

    // Attempt 1: 4 MHz (safe for most cards)
    DEBUG_PRINT("  Trying SD.begin at 4 MHz...");
    if (SD.begin(PIN_SD_CS, sd_spi, 4000000)) {
        mounted = true;
        DEBUG_PRINT("  Mounted at 4 MHz on HSPI");
    }

    // Attempt 2: Try with explicit SPI re-init
    if (!mounted) {
        DEBUG_PRINT("  Attempt 1 failed. Retrying with SPI re-init...");
        sd_spi.end();
        delay(100);
        sd_spi.begin(PIN_SD_SCLK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
        delay(100);

        if (SD.begin(PIN_SD_CS, sd_spi, 1000000)) {
            mounted = true;
            DEBUG_PRINT("  Mounted at 1 MHz on HSPI (slow mode)");
        }
    }

    // Attempt 3: Fall back to default SPI bus (last resort)
    if (!mounted) {
        DEBUG_PRINT("  Attempt 2 failed. Falling back to default SPI bus...");
        sd_spi.end();
        delay(100);
        SPI.begin(PIN_SD_SCLK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
        delay(100);

        if (SD.begin(PIN_SD_CS, SPI, 4000000)) {
            mounted = true;
            DEBUG_PRINT("  WARNING: Mounted on shared SPI bus (fallback)");
        }
    }

    if (!mounted) {
        DEBUG_PRINT("  ERROR: All SD mount attempts failed!");
        DEBUG_PRINT("  Checklist:");
        DEBUG_PRINT("    - Is an SD card inserted?");
        DEBUG_PRINT("    - Is it formatted as FAT32?");
        DEBUG_PRINTF("    - Are SPI pins correct? SCLK=%d MOSI=%d MISO=%d CS=%d",
                     PIN_SD_SCLK, PIN_SD_MOSI, PIN_SD_MISO, PIN_SD_CS);
        storage_ready = false;
        return false;
    }

    // Check card type
    uint8_t cardType = SD.cardType();
    const char* typeStr = "Unknown";
    switch (cardType) {
        case CARD_NONE:
            DEBUG_PRINT("  ERROR: No SD card detected after mount!");
            storage_ready = false;
            return false;
        case CARD_MMC:  typeStr = "MMC";  break;
        case CARD_SD:   typeStr = "SD";   break;
        case CARD_SDHC: typeStr = "SDHC"; break;
    }

    storage_ready = true;

    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    uint64_t totalBytes = SD.totalBytes() / (1024 * 1024);
    uint64_t usedBytes = SD.usedBytes() / (1024 * 1024);

    DEBUG_PRINTF("  Card type: %s", typeStr);
    DEBUG_PRINTF("  Card size: %llu MB", cardSize);
    DEBUG_PRINTF("  Total: %llu MB, Used: %llu MB", totalBytes, usedBytes);
    DEBUG_PRINTF("  Free heap after SD init: %d", ESP.getFreeHeap());

    // List root directory for debugging
    DEBUG_PRINT("  Root directory listing:");
    File root = SD.open("/");
    if (root && root.isDirectory()) {
        File entry;
        int count = 0;
        while ((entry = root.openNextFile()) && count < 20) {
            DEBUG_PRINTF("    %s%s  %d bytes",
                         entry.isDirectory() ? "[DIR] " : "      ",
                         entry.name(),
                         (int)entry.size());
            entry.close();
            count++;
        }
        root.close();
        if (count == 0) {
            DEBUG_PRINT("    (empty)");
        }
    } else {
        DEBUG_PRINT("    ERROR: Could not open root directory");
    }

    // Check if ROM directory exists
    char romPath[32];
    snprintf(romPath, sizeof(romPath), "%s", ROM_BASE_PATH);
    if (SD.exists(romPath)) {
        DEBUG_PRINTF("  ROM directory '%s' found:", romPath);
        File romDir = SD.open(romPath);
        if (romDir && romDir.isDirectory()) {
            File entry;
            while ((entry = romDir.openNextFile())) {
                DEBUG_PRINTF("    %s  %d bytes", entry.name(), (int)entry.size());
                entry.close();
            }
            romDir.close();
        }
    } else {
        DEBUG_PRINTF("  WARNING: ROM directory '%s' not found!", romPath);
        DEBUG_PRINT("  Create it and place ROM files there.");
    }

    return true;
}

bool hal_storage_is_ready(void) {
    return storage_ready;
}

bool hal_storage_load_file(const char* path, uint8_t* buffer, size_t size) {
    if (!storage_ready) {
        DEBUG_PRINTF("  Storage: load_file('%s') - storage not ready", path);
        return false;
    }

    DEBUG_PRINTF("  Storage: loading '%s' (max %d bytes)...", path, (int)size);

    if (!SD.exists(path)) {
        DEBUG_PRINTF("  Storage: file '%s' not found", path);
        return false;
    }

    File file = SD.open(path, FILE_READ);
    if (!file) {
        DEBUG_PRINTF("  Storage: failed to open '%s'", path);
        return false;
    }

    size_t fileSize = file.size();
    size_t toRead = (fileSize < size) ? fileSize : size;

    DEBUG_PRINTF("  Storage: file size = %d, reading %d bytes", (int)fileSize, (int)toRead);

    size_t bytesRead = file.read(buffer, toRead);
    file.close();

    if (bytesRead != toRead) {
        DEBUG_PRINTF("  Storage: read error - expected %d, got %d", (int)toRead, (int)bytesRead);
        return false;
    }

    DEBUG_PRINTF("  Storage: loaded %d bytes from '%s'", (int)bytesRead, path);
    return true;
}

bool hal_storage_file_exists(const char* path) {
    if (!storage_ready) return false;
    bool exists = SD.exists(path);
    DEBUG_PRINTF("  Storage: file_exists('%s') = %s", path, exists ? "yes" : "no");
    return exists;
}

