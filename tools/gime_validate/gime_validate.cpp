/*
 * GIME validation suite runner: builds the models, runs every registered
 * scenario against them, classifies and resolves each check, prints one line
 * per check and writes results.json.
 *
 * usage: gime_validate [--seed N] [--verdicts PATH] [--baseline PATH]
 *                      [--json PATH] [--ours-commit S] [--xroar-commit S]
 *                      [--update-baseline]
 *                      [--selftest]
 */
#include "compare.h"
#include "scenario.h"
#include <inttypes.h>
#include <stdexcept>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static std::string fmt(const CheckRecord& r, const char* model) {
    auto it = r.v.find(model);
    if (it == r.v.end() || !it->second) return "-";
    char b[32];
    snprintf(b, sizeof b, "%" PRId64, *it->second);
    return b;
}

// ---- self-tests -----------------------------------------------------------

static int g_pass = 0, g_fail = 0;
#define CHECK_EQ(name, got, want) do { \
    long long g_ = (long long)(got), w_ = (long long)(want); \
    if (g_ == w_) g_pass++; \
    else { g_fail++; printf("FAIL %s: got %lld want %lld\n", name, g_, w_); } \
} while (0)

static CheckRecord mk(const char* id, std::optional<int64_t> o, std::optional<int64_t> x,
                      std::optional<int64_t> m, uint32_t tol = 0) {
    CheckRecord r;
    r.id = id;
    r.tol = tol;
    r.v["ours"] = o;
    r.v["xroar"] = x;
    r.v["mame"] = m;
    return r;
}

static int selftest() {
    using std::nullopt;
    { CheckRecord r = mk("a.x", 5, 5, 5);
      CHECK_EQ("classify_all_equal_is_agree", classify(r) == Cls::AGREE, 1); }
    { CheckRecord r = mk("a.x", 912, 40, 40, 912);
      CHECK_EQ("classify_ours_within_tolerance_is_resolution", classify(r) == Cls::RESOLUTION, 1); }
    { CheckRecord r = mk("a.x", -1, 817, nullopt, 912);   // -1: event never seen
      Cls c = classify(r);
      CHECK_EQ("classify_no_event_is_not_within_tolerance", (c == Cls::REF_SPLIT) && r.two_way, 1); }
    { CheckRecord r = mk("a.x", -1, -1, nullopt, 912);
      CHECK_EQ("classify_no_event_both_is_agree", classify(r) == Cls::AGREE, 1); }
    { CheckRecord r = mk("a.x", 5, 7, 7);
      CHECK_EQ("classify_ours_odd", classify(r) == Cls::OURS_ODD, 1); }
    { CheckRecord r = mk("a.x", 5, 7, 9);
      CHECK_EQ("classify_ref_split", classify(r) == Cls::REF_SPLIT, 1); }
    { CheckRecord r = mk("a.x", 5, 7, nullopt);
      Cls c = classify(r);
      CHECK_EQ("classify_two_way_split", (c == Cls::REF_SPLIT) && r.two_way, 1); }
    { CheckRecord r = mk("a.x", 5, nullopt, nullopt);
      CHECK_EQ("classify_no_ref_is_skipped", classify(r) == Cls::SKIPPED, 1); }
    { CheckRecord r = mk("v86.x", 1, 7, 7);
      CHECK_EQ("classify_v86_ignores_ours", classify(r) == Cls::AGREE, 1); }
    std::map<std::string, Verdict> nov;
    { CheckRecord r = mk("a.x", 5, 7, 7);
      CHECK_EQ("resolve_known_fail",
               resolve(r, Cls::OURS_ODD, nov, {{"a.x", "why"}}) == Status::KNOWN_FAIL, 1); }
    { CheckRecord r = mk("a.x", 5, 5, 5);
      CHECK_EQ("resolve_fixed",
               resolve(r, Cls::AGREE, nov, {{"a.x", "why"}}) == Status::FIXED, 1); }
    { std::vector<CheckRecord> recs = {mk("a.x", 5, 5, 5)};
      std::vector<std::string> st = stale_baseline(recs, {{"a.x", ""}, {"a.gone", ""}});
      CHECK_EQ("baseline_stale_id_fails", st.size() == 1 && st[0] == "a.gone", 1); }
    std::map<std::string, Verdict> vx = {{"a.x", {"a.x", "xroar", "high"}}};
    { CheckRecord r = mk("a.x", 7, 7, 9);
      CHECK_EQ("resolve_split_with_verdict_ours_matches",
               resolve(r, classify(r), vx, {}) == Status::DISC, 1); }
    { CheckRecord r = mk("a.x", 9, 7, 9);
      CHECK_EQ("resolve_split_with_verdict_ours_differs",
               resolve(r, classify(r), vx, {}) == Status::FAIL, 1); }
    { CheckRecord r = mk("a.x", 7, 7, 9);
      CHECK_EQ("resolve_split_without_verdict",
               resolve(r, classify(r), nov, {}) == Status::UNRESOLVED, 1); }
    {   // --update-baseline keeps a SKIPPED baseline id (and its reason), drops
        // a now-passing one, adds a new FAIL, and drops nothing else
        std::vector<CheckRecord> recs = {mk("a.skip", 5, nullopt, nullopt), mk("a.pass", 5, 5, 5),
                                         mk("a.fail", 5, 7, 7), mk("a.newskip", 5, nullopt, nullopt)};
        std::vector<Status> st = {Status::SKIP, Status::FIXED, Status::FAIL, Status::SKIP};
        std::map<std::string, std::string> old = {{"a.skip", "needs xroar"}, {"a.pass", "was bad"}};
        std::vector<std::string> ids = baseline_ids(recs, st, old);
        CHECK_EQ("update_baseline_keeps_skipped_ids",
                 ids == std::vector<std::string>({"a.skip", "a.fail"}), 1);
        std::string path = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") +
                           "/gime_validate_selftest_baseline.txt";
        bool wrote = write_baseline(path, ids, old);
        std::map<std::string, std::string> back = parse_baseline(path);
        remove(path.c_str());
        CHECK_EQ("update_baseline_keeps_skipped_reason",
                 wrote && back.size() == 2 && back["a.skip"] == "needs xroar", 1);
        fflush(stderr);                      // silence the expected diagnostic
        int saved_err = dup(2), devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, 2);
        bool unwritable_ok = write_baseline("/nonexistent-dir/known_failures.txt", ids, old);
        fflush(stderr);
        if (saved_err >= 0) { dup2(saved_err, 2); close(saved_err); }
        if (devnull >= 0) close(devnull);
        CHECK_EQ("write_baseline_unwritable_fails", unwritable_ok, 0);
    }
    {   // malformed verdict file: unknown model must throw and name the line
        std::string path = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") +
                           "/gime_validate_selftest_verdicts.md";
        FILE* f = fopen(path.c_str(), "w");
        if (f) { fputs("# header\n\n## a.x\naccurate: vcc\nconfidence: high\n", f); fclose(f); }
        bool threw = false, named = false;
        try { parse_verdicts(path); }
        catch (const std::runtime_error& e) {
            threw = true;
            named = strstr(e.what(), "verdicts.md:") != nullptr;
        }
        remove(path.c_str());
        CHECK_EQ("verdict_parse_rejects_unknown_model", threw && named, 1);
    }
    printf("selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

// ---- main -----------------------------------------------------------------

int main(int argc, char** argv) {
    uint32_t seed = 0x6809;
    std::string verdicts_path, baseline_path, json_path;
    RunInfo ri;
    bool update_baseline = false;
    for (int i = 1; i < argc; i++) {
        bool more = i + 1 < argc;
        if (!strcmp(argv[i], "--seed") && more) seed = (uint32_t)strtoul(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--verdicts") && more) verdicts_path = argv[++i];
        else if (!strcmp(argv[i], "--baseline") && more) baseline_path = argv[++i];
        else if (!strcmp(argv[i], "--json") && more) json_path = argv[++i];
        else if (!strcmp(argv[i], "--ours-commit") && more) ri.ours_commit = argv[++i];
        else if (!strcmp(argv[i], "--xroar-commit") && more) ri.xroar_commit = argv[++i];
        else if (!strcmp(argv[i], "--update-baseline")) update_baseline = true;
        else if (!strcmp(argv[i], "--selftest")) return selftest();
        else { fprintf(stderr, "unknown argument: %s\n", argv[i]); return 2; }
    }
    ri.seed = seed;

    std::map<std::string, Verdict> verdicts;
    std::map<std::string, std::string> baseline;
    try {
        if (!verdicts_path.empty()) verdicts = parse_verdicts(verdicts_path);
    } catch (const std::exception& e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 2;
    }
    if (!baseline_path.empty()) baseline = parse_baseline(baseline_path);
    if (update_baseline && baseline_path.empty()) {
        fprintf(stderr, "--update-baseline needs --baseline PATH\n");
        return 2;
    }
    printf("gime_validate seed=0x%X variant=1987\n", seed);

    Ctx ctx;
    std::unique_ptr<GimeModel> ours = make_ours();
    ctx.models.push_back(ours.get());
#ifdef HAVE_XROAR
    std::unique_ptr<GimeModel> xroar = make_xroar(ctx.variant);
    ctx.models.push_back(xroar.get());
#endif

    std::vector<Cls> cls;
    std::vector<Status> sts;
    unsigned cnt[8] = {0};
    std::map<std::string, unsigned> per_area;
    for (const Scenario& s : scenarios()) {
        size_t first = ctx.records().size();
        ctx.scenario = s.id;
        s.fn(ctx);
        for (size_t i = first; i < ctx.records().size(); i++) {
            CheckRecord& r = ctx.records()[i];
            Cls c = classify(r);
            Status st = resolve(r, c, verdicts, baseline);
            cls.push_back(c);
            sts.push_back(st);
            cnt[(int)st]++;
            per_area[r.id.substr(0, r.id.find('.'))]++;
            printf("%-6s %-40s ours=%-8s xroar=%-8s mame=%-8s%s%s\n", status_console(st),
                   r.id.c_str(), fmt(r, "ours").c_str(), fmt(r, "xroar").c_str(),
                   fmt(r, "mame").c_str(), r.note.empty() ? "" : "  ", r.note.c_str());
        }
    }
    std::vector<std::string> stale = stale_baseline(ctx.records(), baseline);
    for (auto& id : stale) printf("STALE  %s\n", id.c_str());
    cnt[(int)Status::STALE] += (unsigned)stale.size();

    for (auto& kv : per_area) printf("area %-10s checks=%u\n", kv.first.c_str(), kv.second);
    printf("TOTAL pass=%u fail=%u known=%u fixed=%u stale=%u disc=%u unresolved=%u skip=%u\n",
           cnt[(int)Status::PASS], cnt[(int)Status::FAIL], cnt[(int)Status::KNOWN_FAIL],
           cnt[(int)Status::FIXED], cnt[(int)Status::STALE], cnt[(int)Status::DISC],
           cnt[(int)Status::UNRESOLVED], cnt[(int)Status::SKIP]);

    if (!json_path.empty() && !write_results_json(json_path, ctx.records(), cls, sts, ri))
        return 2;

    if (update_baseline) {
        std::vector<std::string> ids = baseline_ids(ctx.records(), sts, baseline);
        if (!write_baseline(baseline_path, ids, baseline)) return 2;
        printf("baseline rewritten: %zu entries\n", ids.size());
        return 0;
    }
    return (cnt[(int)Status::FAIL] || cnt[(int)Status::FIXED] || cnt[(int)Status::STALE]) ? 1 : 0;
}
