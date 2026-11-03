// Host-side equivalence test for the OPT-G4 per-mode GIME scanline renderers.
//
// Builds tcc1014.cpp natively (no Arduino) with the legacy monolithic renderer
// compiled in, then drives both renderers from the same randomized GIME / SAM /
// PIA1B state and VRAM contents and requires bit-identical line_buffer output.
//
//   tools/gime_render_test/run.sh            # 200k random lines, all modes
//
// Any mismatch prints the full mode state and the first differing pixel.

#define TCC1014_RENDER_TEST 1
#include "../../src/core/tcc1014.cpp"

#include <stdio.h>
#include <stdlib.h>

void tcc1014_render_scanline_legacy(TCC1014* gime);

static const uint32_t RAM_SIZE = 512 * 1024;
static uint8_t ram[RAM_SIZE];

// Stand-ins for the VGA HAL's two colour paths (hal_video.cpp), with the raw
// byte modelled as packed RGB222 (FabGL adds constant sync bits on top):
//   legacy: RGB565 (byte-swapped) --64K LUT--> rgb565_to_rgb222
//   OPT-G6: GIME 6-bit colour     --64 LUT-->  gime_idx_to_rgb222
static uint8_t raw_from_rgb565_swapped(uint16_t v) {
    uint16_t c = (uint16_t)((v >> 8) | (v << 8));
    return ((c >> 14) & 3) | (((c >> 9) & 3) << 2) | (((c >> 3) & 3) << 4);
}
static uint8_t raw_lut[64];
static void build_raw_lut(void) {
    for (int i = 0; i < 64; i++) {
        uint8_t r = ((i >> 4) & 2) | ((i >> 2) & 1);
        uint8_t g = ((i >> 3) & 2) | ((i >> 1) & 1);
        uint8_t b = ((i >> 2) & 2) | (i & 1);
        raw_lut[i] = r | (g << 2) | (b << 4);
    }
}

static uint32_t rng_state = 0x12345678;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void randomize(TCC1014* g) {
    tcc1014_init(g);
    tcc1014_set_ram(g, ram, RAM_SIZE);
    for (int i = 0; i < 16; i++) tcc1014_write_palette(g, i, rnd() & 0x3F);

    // PIA1B shadow: enable DDR then write PDR (GnA/GM2/GM1/GM0/CSS)
    tcc1014_snoop_pia1b(g, 0xFF23, 0x04);
    tcc1014_snoop_pia1b(g, 0xFF22, rnd() & 0xF8);
    tcc1014_set_sam_register(g, rnd() & 0xFFFF);

    tcc1014_write_register(g, 0, (rnd() & 1) ? 0x80 : 0x00);  // COCO
    tcc1014_write_register(g, 8, rnd() & 0xBF);               // VMODE
    tcc1014_write_register(g, 9, rnd() & 0x7F);               // VRES
    tcc1014_write_register(g, 0x0C, rnd() & 0x0F);            // VSC

    g->vertical.active_area = true;
    g->blink = rnd() & 1;
    g->inverted_text = rnd() & 1;
    // Mostly ordinary addresses; 1 in 8 near the top of RAM to force wrap.
    g->B = (rnd() & 7) ? (rnd() & (RAM_SIZE - 1))
                       : (RAM_SIZE - 1 - (rnd() & 0xFF));
    // VDG-compat rows stay within a 12-line character cell (larger rows read
    // past the end of the font in both renderers — undefined, not compared).
    g->row = g->COCO ? (rnd() % 12) : (rnd() & 0x0F);
}

int main(int argc, char** argv) {
    long trials = (argc > 1) ? atol(argv[1]) : 200000;
    static TCC1014 g;
    build_raw_lut();
    tcc1014_set_raw_lut(raw_lut, raw_lut);      // RGB monitor: the same table
    static uint16_t ref[640];
    unsigned ref_width;
    long seen[2][2][4][4] = {};  // [COCO][BP][CRES][resolution]
    long perturbed[10][2] = {};  // [kind][picture changed]
    long false_dirty = 0;

    for (long t = 0; t < trials; t++) {
        if ((t & 1023) == 0)
            for (uint32_t i = 0; i < RAM_SIZE; i++) ram[i] = rnd();
        randomize(&g);

        const TCC1014 snapshot = g;
        memset(g.line_buffer, 0xA5, sizeof(g.line_buffer));
        tcc1014_render_scanline_legacy(&g);
        memcpy(ref, g.line_buffer, sizeof(ref));
        ref_width = g.line_width;

        g = snapshot;
        g.raw_output = false;
        memset(g.line_buffer, 0x5A, sizeof(g.line_buffer));
        tcc1014_render_scanline(&g, 0);

        seen[g.COCO][g.BP][g.CRES][g.resolution & 3]++;
        bool bad = g.line_width != ref_width;
        unsigned x = 0;
        for (; !bad && x < ref_width; x++) bad = g.line_buffer[x] != ref[x];
        if (bad) {
            printf("MISMATCH trial %ld: COCO=%d BP=%d CRES=%u HRES=%u res=%u BPR=%u "
                   "GnA=%d GM1=%d GM0=%d CSS=%d pdr=%02X row=%u rowmask=%u B=%05X\n",
                   t, g.COCO, g.BP, g.CRES, g.HRES, g.resolution, g.BPR,
                   g.VDG.GnA, g.VDG.GM1, g.VDG.GM0, g.VDG.CSS, g.PIA1B_shadow.pdr,
                   g.row, g.rowmask, g.B);
            printf("  width legacy=%u new=%u, first diff at x=%u: legacy=%04X new=%04X\n",
                   ref_width, g.line_width, x ? x - 1 : 0,
                   ref[x ? x - 1 : 0], g.line_buffer[x ? x - 1 : 0]);
            return 1;
        }

        // OPT-G6 raw path must equal the legacy RGB565 path through the HAL LUT
        g = snapshot;
        g.raw_output = true;
        memset(g.line_raw, 0x77, sizeof(g.line_raw));
        tcc1014_invalidate_lines();
        tcc1014_render_scanline(&g, 0);
        bad = !g.raw_output || g.line_clean || g.line_width != ref_width;
        for (x = 0; !bad && x < ref_width; x++)
            bad = g.line_raw[x] != raw_from_rgb565_swapped(ref[x]);
        if (bad) {
            printf("RAW MISMATCH trial %ld: COCO=%d BP=%d CRES=%u HRES=%u res=%u "
                   "width legacy=%u new=%u x=%u legacy=%02X new=%02X\n",
                   t, g.COCO, g.BP, g.CRES, g.HRES, g.resolution, ref_width,
                   g.line_width, x ? x - 1 : 0,
                   raw_from_rgb565_swapped(ref[x ? x - 1 : 0]), g.line_raw[x ? x - 1 : 0]);
            return 1;
        }

        // OPT-G5 dirty-line skip. Same state again must be skipped...
        const unsigned L = rnd() % 225;
        g = snapshot; g.raw_output = true;
        tcc1014_invalidate_lines();
        tcc1014_render_scanline(&g, L);
        if (g.line_clean) { printf("SKIP: clean right after invalidate, trial %ld\n", t); return 1; }
        g = snapshot; g.raw_output = true;
        tcc1014_render_scanline(&g, L);
        if (!g.line_clean) { printf("SKIP: identical state not skipped, trial %ld\n", t); return 1; }
        g = snapshot; g.raw_output = false;      // screenshot frame: never skipped
        tcc1014_render_scanline(&g, L);
        if (g.line_clean) { printf("SKIP: RGB565 (capture) line skipped, trial %ld\n", t); return 1; }

        // ...and a skip is only allowed when the legacy output is unchanged:
        // perturb one thing, and if the picture changes the line must be dirty.
        TCC1014 p = snapshot;
        const unsigned kind = rnd() % 10;
        switch (kind) {
        case 0: case 1: case 2:
            ram[(p.B + rnd() % 160) & (RAM_SIZE - 1)] ^= 1 << (rnd() & 7); break;
        case 3: tcc1014_write_palette(&p, rnd() & 15, rnd() & 0x3F); break;
        case 4: tcc1014_write_register(&p, 8, rnd() & 0xBF); break;
        case 5: tcc1014_write_register(&p, 9, rnd() & 0x7F); break;
        case 6: tcc1014_snoop_pia1b(&p, 0xFF22, rnd() & 0xF8); break;
        case 7: tcc1014_set_sam_register(&p, rnd() & 0xFFFF); break;
        case 8: p.row = p.COCO ? (rnd() % 12) : (rnd() & 0x0F); break;
        default: if (rnd() & 1) p.blink = !p.blink; else p.inverted_text = !p.inverted_text; break;
        }
        TCC1014 q = p;
        memset(q.line_buffer, 0xA5, sizeof(q.line_buffer));
        tcc1014_render_scanline_legacy(&q);
        const bool changed = q.line_width != ref_width ||
                             memcmp(q.line_buffer, ref, ref_width * sizeof(uint16_t)) != 0;
        q = p; q.raw_output = true;
        tcc1014_render_scanline(&q, L);
        perturbed[kind][changed]++;
        if (changed && q.line_clean) {
            printf("SKIP UNSOUND trial %ld: perturbation kind %u changed the picture "
                   "but the line was skipped (COCO=%d BP=%d CRES=%u HRES=%u)\n",
                   t, kind, p.COCO, p.BP, p.CRES, p.HRES);
            return 1;
        }
        if (!changed && !q.line_clean) false_dirty++;
    }

    printf("skip soundness: perturbation kind -> [picture same, picture changed]\n");
    for (int k = 0; k < 10; k++)
        if (perturbed[k][0] + perturbed[k][1])
            printf("  kind %d: %ld same, %ld changed (all changed lines re-rendered)\n",
                   k, perturbed[k][0], perturbed[k][1]);
    printf("  lines re-rendered although the picture was the same: %ld\n", false_dirty);
    printf("OK: %ld lines identical (RGB565 and raw paths)\n", trials);
    printf("coverage [COCO BP CRES res] = lines:\n");
    for (int c = 0; c < 2; c++) for (int b = 0; b < 2; b++)
        for (int cr = 0; cr < 4; cr++) for (int r = 0; r < 4; r++)
            if (seen[c][b][cr][r])
                printf("  %d %d %d %d = %ld\n", c, b, cr, r, seen[c][b][cr][r]);

    // PMODE 4 artifacts: with the setting on, RG6 lines (either CSS) must be
    // what gime_artifact.h makes of the line's bytes, palette pair and border
    // (that header is checked against MAME by tools/gime_artifact_test), every
    // other line must still match the legacy renderer, and changing the mode
    // must defeat the skip.
    long art_lines = 0, other_lines = 0;
    for (long t = 0; t < trials; t++) {
        const uint8_t mode = (t & 2) ? GIME_ARTIFACT_REVERSE : GIME_ARTIFACT_STANDARD;
        tcc1014_set_artifacts(mode);
        randomize(&g);
        if (t & 1) {   // steer half the trials towards PMODE 4, CSS either way
            tcc1014_write_register(&g, 0, 0x80);
            tcc1014_snoop_pia1b(&g, 0xFF22, (rnd() & 8) | 0xF0);
            if (rnd() & 1)   // now and then a border that counts as set
                g.border_colour = g.palette_reg[g.VDG.CSS ? TCC1014_RGCSS1_1 : TCC1014_RGCSS0_1];
        }
        const TCC1014 snapshot = g;
        if (!artifact_line(&g)) {
            memset(g.line_buffer, 0xA5, sizeof(g.line_buffer));
            tcc1014_render_scanline_legacy(&g);
            memcpy(ref, g.line_buffer, sizeof(ref));
            ref_width = g.line_width;
            g = snapshot;
            g.raw_output = false;
            tcc1014_render_scanline(&g, 0);
            if (g.line_width != ref_width ||
                memcmp(g.line_buffer, ref, ref_width * sizeof(uint16_t)) != 0) {
                printf("ARTIFACT MISMATCH trial %ld: non-PMODE 4 line changed "
                       "(COCO=%d pdr=%02X res=%u)\n", t, g.COCO, g.PIA1B_shadow.pdr,
                       g.resolution);
                return 1;
            }
            other_lines++;
            continue;
        }
        const unsigned n = g.BPR;
        uint8_t bytes[160];
        memcpy(bytes, fetch_line_bytes(&g, n), n);
        const uint8_t c0 = g.palette_reg[g.VDG.CSS ? TCC1014_RGCSS1_0 : TCC1014_RGCSS0_0] & 0x3F;
        const uint8_t c1 = g.palette_reg[g.VDG.CSS ? TCC1014_RGCSS1_1 : TCC1014_RGCSS0_1] & 0x3F;
        uint8_t cols[16], want[160 * 16];
        gime_artifact_mix(mode, GIME_MONITOR_RGB, c0, c1, cols);
        gime_artifact_line<uint8_t>(bytes, n, cols, (g.border_colour & 0x3F) == c1, want);
        for (int pass = 0; pass < 2; pass++) {
            g = snapshot;
            g.raw_output = pass;
            tcc1014_invalidate_lines();
            tcc1014_render_scanline(&g, 0);
            bool bad = g.line_width != n * 16;
            unsigned x = 0;
            for (; !bad && x < g.line_width; x++) {
                const uint8_t code = want[x];
                bad = pass ? g.line_raw[x] != raw_lut[code]
                           : g.line_buffer[x] != display_rgb565_swapped(code);
            }
            if (bad) {
                printf("ARTIFACT MISMATCH trial %ld (%s path) at x=%u, width=%u\n",
                       t, pass ? "raw" : "RGB565", x ? x - 1 : 0, g.line_width);
                return 1;
            }
        }
        art_lines++;
    }
    // Changing the mode must re-render a line the skip would otherwise keep:
    // Standard to Reverse, then to Off.
    tcc1014_set_artifacts(GIME_ARTIFACT_STANDARD);
    do { randomize(&g); tcc1014_write_register(&g, 0, 0x80);
         tcc1014_snoop_pia1b(&g, 0xFF22, 0xF8); } while (!artifact_line(&g));
    {
        const TCC1014 snapshot = g;
        tcc1014_invalidate_lines();
        g.raw_output = true; tcc1014_render_scanline(&g, 3);
        const uint8_t next[2] = { GIME_ARTIFACT_REVERSE, GIME_ARTIFACT_OFF };
        for (int i = 0; i < 2; i++) {
            tcc1014_set_artifacts(next[i]);
            g = snapshot; g.raw_output = true; tcc1014_render_scanline(&g, 3);
            if (g.line_clean) {
                printf("ARTIFACT MODE CHANGE skipped a line that changed\n");
                return 1;
            }
        }
    }
    printf("OK: artifacts on: %ld PMODE 4 lines as expected, %ld other lines "
           "identical to legacy; changing the mode re-renders\n", art_lines, other_lines);
    return 0;
}
