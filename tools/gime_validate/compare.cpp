#include "compare.h"
#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <stdio.h>

// -1 is the "event never seen" value of event checks (Ctx::event_tick): it
// equals only itself, whatever the tolerance.
static bool eq(int64_t a, int64_t b, uint32_t tol) {
    if ((a == -1) != (b == -1)) return false;
    int64_t d = a > b ? a - b : b - a;
    return d <= (int64_t)tol;
}

static std::optional<int64_t> val(const CheckRecord& r, const char* k) {
    auto it = r.v.find(k);
    return it == r.v.end() ? std::nullopt : it->second;
}

static bool is_v86(const CheckRecord& r) { return r.id.compare(0, 4, "v86.") == 0; }

Cls classify(CheckRecord& r) {
    r.two_way = false;
    std::optional<int64_t> o = val(r, "ours"), x = val(r, "xroar"), m = val(r, "mame");
    std::vector<int64_t> refs;
    if (x) refs.push_back(*x);
    if (m) refs.push_back(*m);

    if (is_v86(r)) {                        // references only, information only
        if (refs.size() < 2) return Cls::SKIPPED;
        return eq(refs[0], refs[1], r.tol) ? Cls::AGREE : Cls::REF_SPLIT;
    }
    if (!o || refs.empty()) return Cls::SKIPPED;
    if (refs.size() == 2 && !eq(refs[0], refs[1], r.tol)) return Cls::REF_SPLIT;

    bool all_eq = true, exact = (*o == refs[0]);
    for (int64_t rv : refs) {
        if (!eq(*o, rv, r.tol)) all_eq = false;
        if (*o != rv) exact = false;
    }
    if (refs.size() == 1 && !all_eq) {
        r.two_way = true;
        return Cls::REF_SPLIT;
    }
    if (!all_eq) return Cls::OURS_ODD;
    return exact ? Cls::AGREE : Cls::RESOLUTION;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::map<std::string, Verdict> parse_verdicts(const std::string& path) {
    std::map<std::string, Verdict> out;
    std::ifstream f(path);
    if (!f) return out;                      // absent = none loaded
    auto err = [](int line, const std::string& msg) {
        return std::runtime_error("verdicts.md:" + std::to_string(line) + ": " + msg);
    };
    std::string s;
    int ln = 0, entry_line = 0;
    bool fence = false, in_entry = false;
    Verdict cur;
    bool have_acc = false;
    auto finish = [&]() {
        if (!in_entry) return;
        if (!have_acc) throw err(entry_line, "entry '" + cur.id + "' has no 'accurate:' line");
        if (cur.confidence.empty()) cur.confidence = "medium";
        out[cur.id] = cur;
        in_entry = false;
    };
    while (std::getline(f, s)) {
        ln++;
        if (s.compare(0, 3, "```") == 0) { fence = !fence; continue; }
        if (fence) continue;
        if (s.compare(0, 3, "## ") == 0) {
            finish();
            cur = Verdict();
            cur.id = trim(s.substr(3));
            if (cur.id.empty() || cur.id.find(' ') != std::string::npos ||
                cur.id.find('.') == std::string::npos)
                throw err(ln, "bad check id '" + cur.id + "'");
            if (cur.id.find('*') != std::string::npos && cur.id.find('*') != cur.id.size() - 1)
                throw err(ln, "'*' is only allowed as the last character of '" + cur.id + "'");
            if (out.count(cur.id)) throw err(ln, "duplicate verdict for '" + cur.id + "'");
            in_entry = true;
            have_acc = false;
            entry_line = ln;
            continue;
        }
        if (!in_entry) continue;
        if (s.compare(0, 9, "accurate:") == 0) {
            if (have_acc) throw err(ln, "duplicate 'accurate:'");
            std::string v = trim(s.substr(9));
            if (v == "ours")
                throw err(ln, "'accurate: ours' compares ours with itself; name the reference "
                              "that has the hardware-true value (xroar or mame)");
            if (v != "xroar" && v != "mame")
                throw err(ln, "unknown model '" + v + "' (want xroar or mame)");
            cur.accurate = v;
            have_acc = true;
        } else if (s.compare(0, 11, "confidence:") == 0) {
            std::string v = trim(s.substr(11));
            if (v != "high" && v != "medium" && v != "low")
                throw err(ln, "bad confidence '" + v + "' (want high, medium or low)");
            cur.confidence = v;
        }
    }
    finish();
    return out;
}

std::map<std::string, std::string> parse_baseline(const std::string& path) {
    std::map<std::string, std::string> out;
    std::ifstream f(path);
    if (!f) return out;
    std::string s;
    while (std::getline(f, s)) {
        size_t h = s.find('#');
        std::string id = trim(h == std::string::npos ? s : s.substr(0, h));
        if (id.empty()) continue;
        out[id] = h == std::string::npos ? "" : trim(s.substr(h + 1));
    }
    return out;
}

bool is_random_id(const std::string& id) { return id.compare(0, 7, "random.") == 0; }

const Verdict* find_verdict(const std::map<std::string, Verdict>& verdicts, const std::string& id) {
    auto it = verdicts.find(id);
    if (it != verdicts.end()) return &it->second;
    const Verdict* best = nullptr;
    size_t best_len = 0;
    for (auto& kv : verdicts) {
        const std::string& k = kv.first;
        if (k.empty() || k.back() != '*') continue;
        size_t n = k.size() - 1;
        if (n >= best_len && id.compare(0, n, k, 0, n) == 0 && id.size() >= n) {
            best = &kv.second;
            best_len = n;
        }
    }
    return best;
}

bool fails_run(const CheckRecord& r, Status s, bool random_gated) {
    if (s != Status::FAIL && s != Status::FIXED) return false;
    return random_gated || !is_random_id(r.id);
}

Status resolve(const CheckRecord& r, Cls c, const std::map<std::string, Verdict>& verdicts,
               const std::map<std::string, std::string>& baseline, bool random_gated) {
    bool in_base = baseline.count(r.id) != 0 && (random_gated || !is_random_id(r.id));
    switch (c) {
    case Cls::SKIPPED: return Status::SKIP;
    case Cls::AGREE:
    case Cls::RESOLUTION: return in_base ? Status::FIXED : Status::PASS;
    case Cls::OURS_ODD: return in_base ? Status::KNOWN_FAIL : Status::FAIL;
    case Cls::REF_SPLIT: {
        if (is_v86(r)) return Status::DISC;
        const Verdict* v = find_verdict(verdicts, r.id);
        if (!v) return Status::UNRESOLVED;
        auto o = val(r, "ours"), a = val(r, v->accurate.c_str());
        if (!o || !a) return Status::UNRESOLVED;       // accurate model not observed
        if (eq(*o, *a, r.tol)) return in_base ? Status::FIXED : Status::DISC;
        return in_base ? Status::KNOWN_FAIL : Status::FAIL;
    }
    }
    return Status::UNRESOLVED;
}

std::vector<std::string> stale_baseline(const std::vector<CheckRecord>& recs,
                                        const std::map<std::string, std::string>& baseline,
                                        bool random_gated) {
    std::vector<std::string> out;
    for (auto& kv : baseline) {
        if (!random_gated && is_random_id(kv.first)) continue;
        bool found = false;
        for (auto& r : recs) if (r.id == kv.first) { found = true; break; }
        if (!found) out.push_back(kv.first);
    }
    return out;
}

const char* cls_name(Cls c) {
    switch (c) {
    case Cls::AGREE: return "AGREE";
    case Cls::OURS_ODD: return "OURS_ODD";
    case Cls::REF_SPLIT: return "REF_SPLIT";
    case Cls::RESOLUTION: return "RESOLUTION";
    case Cls::SKIPPED: return "SKIPPED";
    }
    return "?";
}

const char* status_name(Status s) {
    switch (s) {
    case Status::PASS: return "PASS";
    case Status::FAIL: return "FAIL";
    case Status::KNOWN_FAIL: return "KNOWN_FAIL";
    case Status::FIXED: return "FIXED";
    case Status::STALE: return "STALE";
    case Status::DISC: return "DISC";
    case Status::UNRESOLVED: return "UNRESOLVED";
    case Status::SKIP: return "SKIP";
    }
    return "?";
}

const char* status_console(Status s) {
    switch (s) {
    case Status::KNOWN_FAIL: return "KNOWN";
    case Status::UNRESOLVED: return "UNRES";
    default: return status_name(s);
    }
}

const char* console_label(Cls c, Status s) {
    if (c == Cls::RESOLUTION && s == Status::PASS) return "RES";
    return status_console(s);
}

static std::string jesc(const std::string& s) {
    std::string o;
    for (char ch : s) {
        if (ch == '"' || ch == '\\') { o += '\\'; o += ch; }
        else if (ch == '\n') o += "\\n";
        else if (ch == '\t') o += "\\t";
        else if ((unsigned char)ch < 0x20) o += ' ';
        else o += ch;
    }
    return o;
}

bool write_results_json(const std::string& path, const std::vector<CheckRecord>& recs,
                        const std::vector<Cls>& cls, const std::vector<Status>& st,
                        const RunInfo& ri) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", path.c_str()); return false; }
    fprintf(f, "{\n  \"run\": {\"seed\": %u, \"ours_commit\": \"%s\", \"xroar_commit\": \"%s\", "
               "\"mame_commit\": \"%s\", \"mame_form\": \"%s\", \"random_gated\": %s, "
               "\"ref_edits\": [",
            ri.seed, jesc(ri.ours_commit).c_str(), jesc(ri.xroar_commit).c_str(),
            jesc(ri.mame_commit).c_str(), jesc(ri.mame_form).c_str(),
            ri.random_gated ? "true" : "false");
    for (size_t i = 0; i < ri.ref_edits.size(); i++)
        fprintf(f, "%s\"%s\"", i ? ", " : "", jesc(ri.ref_edits[i]).c_str());
    fprintf(f, "]},\n  \"stale\": [");
    for (size_t i = 0; i < ri.stale.size(); i++)
        fprintf(f, "%s\"%s\"", i ? ", " : "", jesc(ri.stale[i]).c_str());
    fprintf(f, "],\n  \"checks\": [\n");
    for (size_t i = 0; i < recs.size(); i++) {
        const CheckRecord& r = recs[i];
        fprintf(f, "    {\"id\": \"%s\", \"values\": {", jesc(r.id).c_str());
        bool first = true;
        for (auto& kv : r.v) {
            fprintf(f, "%s\"%s\": ", first ? "" : ", ", jesc(kv.first).c_str());
            if (kv.second) fprintf(f, "%lld", (long long)*kv.second);
            else fprintf(f, "null");
            first = false;
        }
        fprintf(f, "}, \"tol\": %u, \"two_way\": %s, \"class\": \"%s\", \"status\": \"%s\", "
                   "\"note\": \"%s\", \"verdict_id\": ",
                r.tol, r.two_way ? "true" : "false", cls_name(cls[i]), status_name(st[i]),
                jesc(r.note).c_str());
        if (r.verdict_id.empty()) fprintf(f, "null");
        else fprintf(f, "\"%s\"", jesc(r.verdict_id).c_str());
        if (!r.baseline_reason.empty() || st[i] == Status::KNOWN_FAIL)
            fprintf(f, ", \"baseline_reason\": \"%s\"", jesc(r.baseline_reason).c_str());
        fprintf(f, "}%s\n", i + 1 < recs.size() ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    if (fclose(f) != 0) { fprintf(stderr, "cannot write %s\n", path.c_str()); return false; }
    return true;
}

std::vector<std::string> baseline_ids(const std::vector<CheckRecord>& recs,
                                      const std::vector<Status>& st,
                                      const std::map<std::string, std::string>& old,
                                      bool random_gated) {
    std::vector<std::string> ids;
    for (size_t i = 0; i < recs.size() && i < st.size(); i++) {
        if (!random_gated && is_random_id(recs[i].id)) {
            if (old.count(recs[i].id)) ids.push_back(recs[i].id);    // keep, never add
            continue;
        }
        if (st[i] == Status::FAIL || st[i] == Status::KNOWN_FAIL ||
            (st[i] == Status::SKIP && old.count(recs[i].id)))
            ids.push_back(recs[i].id);
    }
    return ids;
}

bool write_baseline(const std::string& path, const std::vector<std::string>& ids,
                    const std::map<std::string, std::string>& old) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", path.c_str()); return false; }
    fprintf(f, "# Known failures: checks where our GIME differs from the references and the\n"
               "# difference is a real bug or accepted simplification. Format:\n"
               "#   <check id>  # reason\n"
               "# A listed check that starts passing is reported FIXED and an id that no\n"
               "# longer exists is reported STALE; both fail the run until removed here.\n"
               "# Regenerate with: tools/gime_validate/run.sh --update-baseline\n");
    for (const std::string& id : ids) {
        auto it = old.find(id);
        fprintf(f, "%s  # %s\n", id.c_str(),
                it != old.end() && !it->second.empty() ? it->second.c_str() : "TODO reason");
    }
    if (fclose(f) != 0) { fprintf(stderr, "cannot write %s\n", path.c_str()); return false; }
    return true;
}
