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
    std::string verdict_id;         // verdicts.md entry applied (exact id or "prefix*"); set by the runner
    std::string baseline_reason;    // known_failures.txt reason, if the id is listed; set by the runner
};

// The default seed and run count: only a run with both gates on the baseline
// for random.* checks (their values depend on the seed, their ids on the count).
constexpr uint32_t DEFAULT_SEED = 0x6809;
constexpr unsigned DEFAULT_RANDOM = 200;

struct RunInfo {
    uint32_t seed = 0;
    std::string ours_commit, xroar_commit, mame_commit, mame_form;
    std::vector<std::string> ref_edits;
    bool random_gated = true;           // false: non-default --seed/--random
    std::vector<std::string> stale;     // baseline ids with no check this run
};

Cls classify(CheckRecord& r);

struct Verdict { std::string id, accurate; std::string confidence; };  // id may end in "*" (prefix); accurate in {xroar,mame}
std::map<std::string, Verdict> parse_verdicts(const std::string& path);   // throws std::runtime_error("verdicts.md:<line>: ...")
std::map<std::string, std::string> parse_baseline(const std::string& path); // id -> reason; absent file = empty

bool is_random_id(const std::string& id);   // "random." prefix
// The verdict for a check id: an exact entry beats any prefix entry ("a.b*",
// the star only ever last); among prefixes the longest wins. nullptr if none.
const Verdict* find_verdict(const std::map<std::string, Verdict>&, const std::string& id);

// random_gated = false (non-default seed or run count): random.* checks are
// classified against no baseline (so never FIXED), and fails_run() ignores
// their FAIL. stale_baseline() skips random.* ids and baseline_ids() keeps only
// the old random.* entries, adding none.
Status resolve(const CheckRecord& r, Cls c, const std::map<std::string, Verdict>&,
               const std::map<std::string, std::string>& baseline, bool random_gated = true);
std::vector<std::string> stale_baseline(const std::vector<CheckRecord>&,
                                        const std::map<std::string, std::string>&,
                                        bool random_gated = true);
// Does this status make the run exit 1? (FAIL or FIXED; STALE is handled apart.)
bool fails_run(const CheckRecord& r, Status s, bool random_gated = true);

const char* cls_name(Cls c);
const char* status_name(Status s);        // JSON/long name
const char* status_console(Status s);     // PASS FAIL KNOWN FIXED STALE DISC UNRES SKIP
// The console label of a resolved check: status_console(s), except RES for a
// PASS that is RESOLUTION-limited (ours differs within its resolution).
const char* console_label(Cls c, Status s);

// Both writers return false (after printing why) if the file can't be written.
bool write_results_json(const std::string& path, const std::vector<CheckRecord>&,
                        const std::vector<Cls>&, const std::vector<Status>&, const RunInfo&);
// The ids --update-baseline keeps: every FAIL or KNOWN_FAIL check, plus every
// existing baseline id whose check was SKIPPED this run (its model wasn't
// built), so a run without a reference source doesn't drop entries.
std::vector<std::string> baseline_ids(const std::vector<CheckRecord>&, const std::vector<Status>&,
                                      const std::map<std::string, std::string>& old,
                                      bool random_gated = true);
// Rewrites the baseline with the ids in `ids`, keeping existing reasons.
bool write_baseline(const std::string& path, const std::vector<std::string>& ids,
                    const std::map<std::string, std::string>& old);

#endif
