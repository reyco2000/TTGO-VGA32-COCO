/*
 * GIME validation suite runner: builds the models, runs every registered
 * scenario against them and prints one line per check.
 *
 * usage: gime_validate [--seed N]
 */
#include "scenario.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Placeholder until Task 2's comparator: SKIP unless two models observed it.
static const char* status_of(const CheckRecord& r) {
    int n = 0;
    for (auto& kv : r.values) if (kv.second) n++;
    return n < 2 ? "SKIP" : "?";
}

static std::string fmt(const CheckRecord& r, const char* model) {
    auto it = r.values.find(model);
    if (it == r.values.end() || !it->second) return "-";
    char b[32];
    snprintf(b, sizeof b, "%" PRId64, *it->second);
    return b;
}

int main(int argc, char** argv) {
    uint32_t seed = 0x6809;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint32_t)strtoul(argv[++i], nullptr, 0);
        else { fprintf(stderr, "unknown argument: %s\n", argv[i]); return 2; }
    }
    printf("gime_validate seed=0x%X variant=1987\n", seed);

    std::unique_ptr<GimeModel> ours = make_ours();
    Ctx ctx;
    ctx.models.push_back(ours.get());

    unsigned pass = 0, fail = 0, known = 0, disc = 0, unresolved = 0, skip = 0;
    std::map<std::string, unsigned> per_area;
    for (const Scenario& s : scenarios()) {
        size_t first = ctx.records().size();
        s.fn(ctx);
        for (size_t i = first; i < ctx.records().size(); i++) {
            const CheckRecord& r = ctx.records()[i];
            const char* st = status_of(r);
            if (!strcmp(st, "SKIP")) skip++;
            per_area[r.id.substr(0, r.id.find('.'))]++;
            printf("%-6s %-40s ours=%-8s xroar=%-8s mame=%-8s\n", st, r.id.c_str(),
                   fmt(r, "ours").c_str(), fmt(r, "xroar").c_str(), fmt(r, "mame").c_str());
        }
    }
    for (auto& kv : per_area) printf("area %-10s checks=%u\n", kv.first.c_str(), kv.second);
    printf("TOTAL pass=%u fail=%u known=%u disc=%u unresolved=%u skip=%u\n",
           pass, fail, known, disc, unresolved, skip);
    return fail ? 1 : 0;
}
