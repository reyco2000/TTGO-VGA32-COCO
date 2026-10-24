/*
 * GIME validation suite: comparator. Classifies each check (ours vs XRoar vs
 * MAME), resolves the class against hand-written verdicts and the known-failure
 * baseline, and writes results.json.
 */
#ifndef GIME_COMPARE_H
#define GIME_COMPARE_H

#include "gime_model.h"
#include <map>
#include <optional>
#include <stdint.h>
#include <string>
#include <vector>

enum class Cls { AGREE, OURS_ODD, REF_SPLIT, RESOLUTION, SKIPPED };
enum class Status { PASS, FAIL, KNOWN_FAIL, FIXED, STALE, DISC, UNRESOLVED, SKIP };

struct CheckRecord {
    std::string id;                 // "area.name"; "v86." prefix = info-only
    Cap cap = CAP_REGS;             // capability the value needs (set by Ctx)
    std::map<std::string, std::optional<int64_t>> v;   // keys "ours","xroar","mame"
    uint32_t tol = 0;               // ticks
    std::string note;               // e.g. the first differing pixel for renders
    bool two_way = false;           // set by classify when only ours + 1 ref
};

struct RunInfo {
    uint32_t seed = 0;
    std::string ours_commit, xroar_commit, mame_commit, mame_form;
    std::vector<std::string> ref_edits;
};

Cls classify(CheckRecord& r);

struct Verdict { std::string id, accurate; std::string confidence; };  // accurate in {ours,xroar,mame}
std::map<std::string, Verdict> parse_verdicts(const std::string& path);   // throws std::runtime_error("verdicts.md:<line>: ...")
std::map<std::string, std::string> parse_baseline(const std::string& path); // id -> reason; absent file = empty

Status resolve(const CheckRecord& r, Cls c, const std::map<std::string, Verdict>&,
               const std::map<std::string, std::string>& baseline);
std::vector<std::string> stale_baseline(const std::vector<CheckRecord>&,
                                        const std::map<std::string, std::string>&);

const char* cls_name(Cls c);
const char* status_name(Status s);        // JSON/long name
const char* status_console(Status s);     // PASS FAIL KNOWN FIXED STALE DISC UNRES SKIP

void write_results_json(const std::string& path, const std::vector<CheckRecord>&,
                        const std::vector<Cls>&, const std::vector<Status>&, const RunInfo&);
// Rewrites the baseline with the ids in `ids`, keeping existing reasons.
void write_baseline(const std::string& path, const std::vector<std::string>& ids,
                    const std::map<std::string, std::string>& old);

#endif
