/*
 * GIME validation suite runner: builds the models, runs every registered
 * scenario against them, classifies and resolves each check, prints one line
 * per check and writes results.json.
 *
 * usage: gime_validate [--seed N] [--verdicts PATH] [--baseline PATH]
 *                      [--json PATH] [--ours-commit S] [--xroar-commit S]
 *                      [--mame-commit S]
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

// For expressions with top-level commas (braced initialisers).
#define CHECK_TRUE(name, ...) do { bool b_ = (__VA_ARGS__); CHECK_EQ(name, b_, 1); } while (0)

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
    {   // R12: random.* gate on the baseline only for the default seed and count
        CheckRecord r = mk("random.regs.3.read_FF90", 5, 7, 7);
        Status s = resolve(r, classify(r), nov, {}, false);
        CheckRecord q = mk("a.x", 5, 7, 7);
        CHECK_TRUE("random_nondefault_seed_fail_does_not_gate",
                 s == Status::FAIL && !fails_run(r, s, false) && fails_run(r, s, true) &&
                 fails_run(q, Status::FAIL, false));
        CheckRecord g = mk("random.regs.3.read_FF90", 5, 5, 5);
        CHECK_TRUE("random_nondefault_seed_no_fixed",
                 resolve(g, classify(g), nov, {{g.id, "why"}}, false) == Status::PASS &&
                 resolve(g, classify(g), nov, {{g.id, "why"}}, true) == Status::FIXED);
        std::vector<CheckRecord> recs = {mk("a.x", 5, 5, 5)};
        std::map<std::string, std::string> base = {{"random.regs.9.x", ""}, {"a.gone", ""}};
        CHECK_TRUE("random_nondefault_seed_no_stale",
                 stale_baseline(recs, base, false) == std::vector<std::string>({"a.gone"}) &&
                 stale_baseline(recs, base, true).size() == 2);
        std::vector<CheckRecord> rr = {mk("random.regs.0.x", 5, 7, 7), mk("random.regs.1.x", 5, 7, 7),
                                       mk("a.fail", 5, 7, 7)};
        std::vector<Status> rs = {Status::FAIL, Status::KNOWN_FAIL, Status::FAIL};
        CHECK_TRUE("random_nondefault_seed_update_baseline_keeps_old_random_only",
                 baseline_ids(rr, rs, {{"random.regs.1.x", "r"}}, false) ==
                     std::vector<std::string>({"random.regs.1.x", "a.fail"}));
    }
    {   // R13: prefix verdicts
        std::map<std::string, Verdict> vp = {{"a.*", {"a.*", "xroar", "high"}}};
        CheckRecord r = mk("a.x", 7, 7, 9);
        CHECK_TRUE("verdict_prefix_matches",
                 resolve(r, classify(r), vp, {}) == Status::DISC &&
                 find_verdict(vp, "a.x") && find_verdict(vp, "a.x")->id == "a.*" &&
                 !find_verdict(vp, "b.x"));
        std::map<std::string, Verdict> ve = {{"a.*", {"a.*", "xroar", "high"}},
                                             {"a.x", {"a.x", "mame", "low"}}};
        CheckRecord e = mk("a.x", 9, 7, 9);     // xroar's verdict would FAIL, mame's is DISC
        CHECK_TRUE("verdict_exact_beats_prefix",
                 resolve(e, classify(e), ve, {}) == Status::DISC &&
                 find_verdict(ve, "a.x")->id == "a.x");
        std::map<std::string, Verdict> vl = {{"a.*", {"a.*", "xroar", "high"}},
                                             {"a.b*", {"a.b*", "mame", "high"}}};
        CheckRecord l = mk("a.bc", 9, 7, 9);
        CHECK_TRUE("verdict_longest_prefix_wins",
                 resolve(l, classify(l), vl, {}) == Status::DISC &&
                 find_verdict(vl, "a.bc")->id == "a.b*" && find_verdict(vl, "a.zz")->id == "a.*");
        std::string path = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") +
                           "/gime_validate_selftest_prefix.md";
        auto parse_text = [&](const char* text, bool* threw) {
            FILE* f = fopen(path.c_str(), "w");
            if (f) { fputs(text, f); fclose(f); }
            std::map<std::string, Verdict> m;
            *threw = false;
            try { m = parse_verdicts(path); } catch (const std::runtime_error&) { *threw = true; }
            remove(path.c_str());
            return m;
        };
        bool t1, t2, t3;
        std::map<std::string, Verdict> ok = parse_text("## a.b*\naccurate: ours\n", &t1);
        parse_text("## a.*b\naccurate: ours\n", &t2);
        parse_text("## a.b**\naccurate: ours\n", &t3);
        CHECK_TRUE("verdict_parse_star_only_final",
                 !t1 && ok.count("a.b*") && t2 && t3);
    }
    printf("selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

// ---- main -----------------------------------------------------------------

int main(int argc, char** argv) {
    uint32_t seed = DEFAULT_SEED;
    unsigned nrandom = DEFAULT_RANDOM;
    std::string verdicts_path, baseline_path, json_path;
    RunInfo ri;
    bool update_baseline = false;
    for (int i = 1; i < argc; i++) {
        bool more = i + 1 < argc;
        if (!strcmp(argv[i], "--seed") && more) seed = (uint32_t)strtoul(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--random") && more) nrandom = (unsigned)strtoul(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--verdicts") && more) verdicts_path = argv[++i];
        else if (!strcmp(argv[i], "--baseline") && more) baseline_path = argv[++i];
        else if (!strcmp(argv[i], "--json") && more) json_path = argv[++i];
        else if (!strcmp(argv[i], "--ours-commit") && more) ri.ours_commit = argv[++i];
        else if (!strcmp(argv[i], "--xroar-commit") && more) ri.xroar_commit = argv[++i];
        else if (!strcmp(argv[i], "--mame-commit") && more) ri.mame_commit = argv[++i];
        else if (!strcmp(argv[i], "--update-baseline")) update_baseline = true;
        else if (!strcmp(argv[i], "--selftest")) return selftest();
        else { fprintf(stderr, "unknown argument: %s\n", argv[i]); return 2; }
    }
    ri.seed = seed;
    // Random values depend on the seed and the ids on the count, so only the
    // default run is checked against the baseline (R12).
    ri.random_gated = (seed == DEFAULT_SEED && nrandom == DEFAULT_RANDOM);

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
#ifdef HAVE_MAME
    ri.mame_form = "standin";       // model_mame.cpp: why not the extract form
    ri.ref_edits.push_back("mame gime.cpp:99 GIME_TYPE_1987 0->1 (1987 build)");
#endif
    printf("gime_validate seed=0x%X random=%u variant=1987 (+v86 reference-only copies)\n", seed, nrandom);
    if (!ri.random_gated)
        printf("note: non-default --seed/--random: random.* checks are classified and printed but do not gate\n");
    printf("mame form: %s\n", ri.mame_form.empty() ? "none (not built)" : ri.mame_form.c_str());

    Ctx ctx;
    std::unique_ptr<GimeModel> ours = make_ours();
    ctx.models.push_back(ours.get());
#ifdef HAVE_XROAR
    std::unique_ptr<GimeModel> xroar = make_xroar(ctx.variant);
    ctx.models.push_back(xroar.get());
#endif
#ifdef HAVE_MAME
    std::unique_ptr<GimeModel> mame = make_mame(ctx.variant);
    ctx.models.push_back(mame.get());
#endif

    // Scripted scenarios and the randomized runs on the 1987 models.
    for (const Scenario& s : scenarios()) { ctx.scenario = s.id; s.fn(ctx); }
    run_random(ctx, seed, nrandom);
    std::vector<CheckRecord> all = ctx.records();

    // Every scripted scenario again on the 1986 references. Ours has no 1986
    // variant and classify() ignores it for "v86." ids, but it stays in the
    // run: the tolerances (sync/timer resolution) come from the models
    // present, and must be the same as in the 1987 run. Information only.
    {
        Ctx c86;
        c86.variant = Variant::G1986;
        c86.prefix = "v86.";
        std::unique_ptr<GimeModel> x86, m86;
        std::unique_ptr<GimeModel> o86 = make_ours();
        c86.models.push_back(o86.get());
#ifdef HAVE_XROAR
        x86 = make_xroar(c86.variant);
        c86.models.push_back(x86.get());
#endif
#ifdef HAVE_MAME
        m86 = make_mame(c86.variant);
        c86.models.push_back(m86.get());
#endif
        if (c86.models.size() == 3) {
            for (const Scenario& s : scenarios()) { c86.scenario = "v86." + s.id; s.fn(c86); }
            all.insert(all.end(), c86.records().begin(), c86.records().end());
        }
    }

    std::vector<Cls> cls;
    std::vector<Status> sts;
    unsigned cnt[8] = {0}, gating_fail = 0, ungated_fail = 0;
    std::map<std::string, unsigned> per_area;
    for (CheckRecord& r : all) {
        Cls c = classify(r);
        Status st = resolve(r, c, verdicts, baseline, ri.random_gated);
        if (c == Cls::REF_SPLIT)
            if (const Verdict* v = find_verdict(verdicts, r.id)) r.verdict_id = v->id;
        auto bit = baseline.find(r.id);
        if (bit != baseline.end() && (ri.random_gated || !is_random_id(r.id)))
            r.baseline_reason = bit->second;
        if (fails_run(r, st, ri.random_gated)) gating_fail++;
        else if (st == Status::FAIL || st == Status::FIXED) ungated_fail++;
        cls.push_back(c);
        sts.push_back(st);
        cnt[(int)st]++;
        per_area[r.id.substr(0, r.id.find('.'))]++;
        printf("%-6s %-40s ours=%-8s xroar=%-8s mame=%-8s%s%s\n", status_console(st),
               r.id.c_str(), fmt(r, "ours").c_str(), fmt(r, "xroar").c_str(),
               fmt(r, "mame").c_str(), r.note.empty() ? "" : "  ", r.note.c_str());
    }
    std::vector<std::string> stale = stale_baseline(all, baseline, ri.random_gated);
    ri.stale = stale;
    for (auto& id : stale) printf("STALE  %s\n", id.c_str());
    cnt[(int)Status::STALE] += (unsigned)stale.size();

    for (auto& kv : per_area) printf("area %-10s checks=%u\n", kv.first.c_str(), kv.second);
    printf("TOTAL pass=%u fail=%u known=%u fixed=%u stale=%u disc=%u unresolved=%u skip=%u\n",
           cnt[(int)Status::PASS], cnt[(int)Status::FAIL], cnt[(int)Status::KNOWN_FAIL],
           cnt[(int)Status::FIXED], cnt[(int)Status::STALE], cnt[(int)Status::DISC],
           cnt[(int)Status::UNRESOLVED], cnt[(int)Status::SKIP]);

    if (ungated_fail) printf("non-gating random failures: %u\n", ungated_fail);
    if (!json_path.empty() && !write_results_json(json_path, all, cls, sts, ri))
        return 2;

    if (update_baseline) {
        std::vector<std::string> ids = baseline_ids(all, sts, baseline, ri.random_gated);
        if (!write_baseline(baseline_path, ids, baseline)) return 2;
        printf("baseline rewritten: %zu entries\n", ids.size());
        return 0;
    }
    return (gating_fail || cnt[(int)Status::STALE]) ? 1 : 0;
}
