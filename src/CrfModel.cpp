/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"

#include "CrfModel.h"

/*
Port of Wapiti's label mode (mdl_load, rdr_pattok2seq, pat_exec, tag_expsc, tag_viterbi).

  model text --load--> patterns, labels, weights of observations that have any
  row tokens --patterns--> observation strings --map--> weight blocks
  weight blocks --sum--> psi[t][y'][y] --viterbi--> best label per row

Wapiti keeps one dense weight vector over every observation (100M+ doubles for the
original GROBID models). Only observations with a listed weight can change a score,
so we keep those alone: lossless, and the original models load too.
*/

constexpr int kMaxPatternColumn = 1 << 16;

enum class ItemType : u8 {
    Literal,
    Column,
};

struct PatItem {
    ItemType type = ItemType::Literal;
    bool caps = false;
    bool absolute = false;
    int offset = 0;
    int column = 0;
    // literal text as [litStart, litStart + litLen) in CrfModel::text
    int litStart = 0;
    int litLen = 0;
};

struct Pattern {
    int firstItem = 0;
    int nItems = 0;
};

// observation kinds, from its first char: 'u' unigram, 'b' bigram, '*' both
enum ObsKind : u8 {
    kObsUni = 1,
    kObsBi = 2,
};

struct ObsEntry {
    u32 hash = 0;
    int keyStart = 0; // in CrfModel::text
    int keyLen = 0;
    u8 kind = 0;
    int uni = -1; // offset of Y weights in CrfModel::weights, -1 if all zero
    int bi = -1;  // offset of Y*Y weights
};

struct CrfModel {
    int nLabels = 0;
    StrVec labels;
    Vec<PatItem> items;
    Vec<Pattern> patterns;
    // pattern literals and observation keys
    str::Builder text;
    Vec<ObsEntry> obs;
    // open addressing over obs, -1 = empty slot
    Vec<int> table;
    Vec<double> weights;
};

static u32 HashKey(Str s) {
    return MurmurHash2(s);
}

static Str ObsKey(CrfModel* m, const ObsEntry& e) {
    return Str(m->text.els + e.keyStart, e.keyLen);
}

static int FindObs(CrfModel* m, Str key) {
    int n = len(m->table);
    if (n == 0) return -1;
    u32 h = HashKey(key);
    int mask = n - 1;
    for (int i = (int)(h & (u32)mask);; i = (i + 1) & mask) {
        int idx = m->table.els[i];
        if (idx < 0) return -1;
        ObsEntry& e = m->obs.els[idx];
        if (e.hash == h && str::Eq(ObsKey(m, e), key)) return idx;
    }
}

static void BuildObsTable(CrfModel* m) {
    int n = RoundToPowerOf2(len(m->obs) * 2 + 16);
    VecResize(m->table, n);
    for (int i = 0; i < n; i++) m->table.els[i] = -1;
    int mask = n - 1;
    for (int idx = 0; idx < len(m->obs); idx++) {
        ObsEntry& e = m->obs.els[idx];
        e.hash = HashKey(ObsKey(m, e));
        int i = (int)(e.hash & (u32)mask);
        while (m->table.els[i] >= 0) i = (i + 1) & mask;
        m->table.els[i] = idx;
    }
}

//--- model text parsing

struct Cursor {
    Str s;
    int pos = 0;
};

static bool AtEnd(Cursor& c) {
    return c.pos >= len(c.s);
}

static void SkipWs(Cursor& c) {
    while (!AtEnd(c) && str::IsWs(c.s.s[c.pos])) c.pos++;
}

static bool Expect(Cursor& c, Str lit) {
    if (!str::StartsWith(Str(c.s.s + c.pos, len(c.s) - c.pos), lit)) return false;
    c.pos += len(lit);
    return true;
}

static bool ParseU64(Cursor& c, u64* out) {
    SkipWs(c);
    int start = c.pos;
    u64 v = 0;
    while (!AtEnd(c) && str::IsDigit(c.s.s[c.pos])) {
        v = v * 10 + (u64)(c.s.s[c.pos] - '0');
        c.pos++;
    }
    *out = v;
    return c.pos > start;
}

// netstring "<len>:<bytes>,\n" as written by Wapiti's ns_writestr
static bool ParseNetStr(Cursor& c, Str* out) {
    u64 n = 0;
    if (!ParseU64(c, &n) || !Expect(c, StrL(":"))) return false;
    if (n > (u64)(len(c.s) - c.pos - 1)) return false;
    *out = Str(c.s.s + c.pos, (int)n);
    c.pos += (int)n;
    if (!Expect(c, StrL(","))) return false;
    if (!AtEnd(c)) c.pos++; // the '\n'
    return true;
}

static Str RestOfLine(Cursor& c) {
    int start = c.pos;
    while (!AtEnd(c) && c.s.s[c.pos] != '\n') c.pos++;
    Str res(c.s.s + start, c.pos - start);
    if (!AtEnd(c)) c.pos++;
    return res;
}

// C99 "%a" output such as -0x1.b106a6e28d32dp-9 (what Wapiti's "%la" writes)
static bool ParseHexDouble(Str s, double* out) {
    int i = 0;
    int n = len(s);
    bool neg = false;
    if (i < n && (s.s[i] == '-' || s.s[i] == '+')) neg = s.s[i++] == '-';
    if (i + 2 > n || s.s[i] != '0' || (s.s[i + 1] != 'x' && s.s[i + 1] != 'X')) return false;
    i += 2;
    u64 mant = 0;
    int exp = 0;
    bool afterDot = false;
    int nDigits = 0;
    for (; i < n && s.s[i] != 'p' && s.s[i] != 'P'; i++) {
        if (s.s[i] == '.') {
            afterDot = true;
            continue;
        }
        int d = str::HexDigitVal(s.s[i]);
        if (d < 0) return false;
        // a double has 53 significant bits; 15 hex digits never lose any of them
        if (nDigits < 15) {
            mant = mant * 16 + (u64)d;
            if (afterDot) exp -= 4;
        } else if (!afterDot) {
            exp += 4;
        }
        nDigits++;
    }
    if (i >= n) return false;
    i++;
    if (i < n && s.s[i] == '+') i++;
    int e = ParseInt(Str(s.s + i, n - i));
    double v = ldexp((double)mant, e + exp);
    *out = neg ? -v : v;
    return true;
}

static bool ParseDouble(Str s, double* out) {
    str::TrimWsBoth(s);
    if (ParseHexDouble(s, out)) return true;
    char* end = nullptr;
    *out = strtod(CStrTemp(s), &end);
    return end && *end == 0;
}

// pat_comp for the subset GROBID uses: literal text, %x[off,col], %X[off,col] and [@off,col]
static bool CompilePattern(CrfModel* m, Str src) {
    Pattern pat;
    pat.firstItem = len(m->items);
    int pos = 0;
    int n = len(src);
    while (pos < n) {
        PatItem it;
        if (src.s[pos] != '%') {
            int start = pos;
            while (pos < n && src.s[pos] != '%') pos++;
            it.type = ItemType::Literal;
            it.litStart = len(m->text);
            it.litLen = pos - start;
            m->text.Append(Str(src.s + start, pos - start));
            VecAppend(m->items, it);
            continue;
        }
        if (pos + 2 >= n || (src.s[pos + 1] != 'x' && src.s[pos + 1] != 'X') || src.s[pos + 2] != '[') {
            logf("CrfModelLoad: unsupported pattern '%s'\n", src);
            return false;
        }
        it.type = ItemType::Column;
        it.caps = src.s[pos + 1] == 'X';
        pos += 3;
        if (pos < n && src.s[pos] == '@') {
            it.absolute = true;
            pos++;
        }
        bool neg = false;
        if (pos < n && (src.s[pos] == '-' || src.s[pos] == '+')) neg = src.s[pos++] == '-';
        Cursor c{src, pos};
        u64 off = 0, col = 0;
        if (!ParseU64(c, &off) || !Expect(c, StrL(",")) || !ParseU64(c, &col) || !Expect(c, StrL("]"))) {
            logf("CrfModelLoad: invalid pattern '%s'\n", src);
            return false;
        }
        if (col > kMaxPatternColumn || off > kMaxPatternColumn) return false;
        it.offset = neg ? -(int)off : (int)off;
        it.column = (int)col;
        pos = c.pos;
        VecAppend(m->items, it);
    }
    pat.nItems = len(m->items) - pat.firstItem;
    VecAppend(m->patterns, pat);
    return true;
}

static u8 KindOf(Str obs) {
    if (len(obs) == 0) return 0;
    switch (obs.s[0]) {
        case 'u':
            return kObsUni;
        case 'b':
            return kObsBi;
        case '*':
            return kObsUni | kObsBi;
    }
    return 0;
}

// Returns the weight slot for feature f of observation o, allocating its block on first use.
static double* WeightSlot(CrfModel* m, int obsIdx, u64 d) {
    ObsEntry& e = m->obs.els[obsIdx];
    int Y = m->nLabels;
    if (e.kind & kObsUni) {
        if (d < (u64)Y) {
            if (e.uni < 0) {
                e.uni = len(m->weights);
                VecAppendBlanks(m->weights, Y);
            }
            return m->weights.els + e.uni + d;
        }
        d -= Y;
    }
    if (!(e.kind & kObsBi) || d >= (u64)Y * Y) return nullptr;
    if (e.bi < 0) {
        e.bi = len(m->weights);
        VecAppendBlanks(m->weights, Y * Y);
    }
    return m->weights.els + e.bi + d;
}

static bool LoadModel(CrfModel* m, Str text) {
    Cursor c{text, 0};
    u64 type = 0, nact = 0, npats = 0, ntoks = 0, autouni = 0;
    if (!Expect(c, StrL("#mdl#")) || !ParseU64(c, &type) || !Expect(c, StrL("#")) || !ParseU64(c, &nact)) {
        return false;
    }
    // label mode with tag_expsc scores, as GROBID uses (type 1 would be a MEMM)
    if (type != 2) return false;
    SkipWs(c);
    if (!Expect(c, StrL("#rdr#")) || !ParseU64(c, &npats) || !Expect(c, StrL("/")) || !ParseU64(c, &ntoks)) {
        return false;
    }
    if (Expect(c, StrL("/")) && (!ParseU64(c, &autouni) || autouni != 0)) return false;
    SkipWs(c);
    for (u64 i = 0; i < npats; i++) {
        Str src;
        if (!ParseNetStr(c, &src) || !CompilePattern(m, src)) return false;
    }

    u64 nLabels = 0;
    if (!Expect(c, StrL("#qrk#")) || !ParseU64(c, &nLabels) || nLabels == 0 || nLabels > 4096) return false;
    SkipWs(c);
    m->nLabels = (int)nLabels;
    for (u64 i = 0; i < nLabels; i++) {
        Str lbl;
        if (!ParseNetStr(c, &lbl)) return false;
        m->labels.Append(lbl);
    }

    // mdl_sync: per observation Y unigram and/or Y*Y bigram features, in file order
    u64 nObs = 0;
    if (!Expect(c, StrL("#qrk#")) || !ParseU64(c, &nObs) || nObs > (u64)INT_MAX / 2) return false;
    SkipWs(c);
    Vec<u64> featStart;
    Vec<int> keyPos;
    Vec<int> keyLen;
    Vec<int> slot; // observation -> index in m->obs, -1 until it gets a weight
    VecResize(featStart, (int)nObs + 1);
    VecResize(keyPos, (int)nObs);
    VecResize(keyLen, (int)nObs);
    VecResize(slot, (int)nObs);
    u64 Y = nLabels;
    u64 F = 0;
    for (int o = 0; o < (int)nObs; o++) {
        Str key;
        if (!ParseNetStr(c, &key)) return false;
        featStart.els[o] = F;
        keyPos.els[o] = (int)(key.s - text.s);
        keyLen.els[o] = len(key);
        slot.els[o] = -1;
        u8 kind = KindOf(key);
        if (kind & kObsUni) F += Y;
        if (kind & kObsBi) F += Y * Y;
    }
    featStart.els[nObs] = F;

    for (u64 i = 0; i < nact; i++) {
        SkipWs(c);
        Str line = RestOfLine(c);
        Str fstr, vstr;
        if (!str::CutChar(line, '=', &fstr, &vstr)) return false;
        Cursor fc{fstr, 0};
        u64 f = 0;
        double v = 0;
        if (!ParseU64(fc, &f) || f >= F || !ParseDouble(vstr, &v)) return false;

        // last observation whose features start at or before f
        int lo = 0, hi = (int)nObs - 1;
        while (lo < hi) {
            int mid = lo + (hi - lo + 1) / 2;
            if (featStart.els[mid] <= f) {
                lo = mid;
            } else {
                hi = mid - 1;
            }
        }
        if (slot.els[lo] < 0) {
            Str key(text.s + keyPos.els[lo], keyLen.els[lo]);
            ObsEntry e;
            e.keyStart = len(m->text);
            e.keyLen = len(key);
            e.kind = KindOf(key);
            m->text.Append(key);
            slot.els[lo] = len(m->obs);
            VecAppend(m->obs, e);
        }
        double* w = WeightSlot(m, slot.els[lo], f - featStart.els[lo]);
        if (!w) return false;
        *w = v;
    }
    BuildObsTable(m);
    return true;
}

// Loads a Wapiti model saved in text form. Returns nullptr if it is not a model we can run.
CrfModel* CrfModelLoad(Str wapitiText) {
    auto m = new CrfModel();
    if (!LoadModel(m, wapitiText)) {
        logf("CrfModelLoad: invalid or unsupported model\n");
        delete m;
        return nullptr;
    }
    return m;
}

void CrfModelFree(CrfModel* m) {
    delete m;
}

int CrfLabelCount(CrfModel* m) {
    return m ? m->nLabels : 0;
}

Str CrfLabelName(CrfModel* m, int label) {
    if (!m || label < 0 || label >= m->nLabels) return {};
    return m->labels.At(label);
}

//--- tagging

static bool IsCWs(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

// rdr_raw2seq: skip leading whitespace, tokens separated by whitespace runs
static void SplitRow(Str row, Vec<Str>& toks) {
    int i = 0;
    int n = len(row);
    while (i < n) {
        while (i < n && IsCWs(row.s[i])) i++;
        if (i >= n) break;
        int start = i;
        while (i < n && !IsCWs(row.s[i])) i++;
        VecAppend(toks, Str(row.s + start, i - start));
    }
}

// pat_exec: builds the observation string of pattern p at position t. False on a missing column.
static bool ExecPattern(CrfModel* m, const Pattern& p, Vec<Str>& toks, Vec<int>& rowStart, int t, str::Builder& out) {
    static const char* before[] = {"_x-1", "_x-2", "_x-3", "_x-4", "_x-#"};
    static const char* after[] = {"_x+1", "_x+2", "_x+3", "_x+4", "_x+#"};
    int T = len(rowStart) - 1;
    out.Reset();
    for (int k = 0; k < p.nItems; k++) {
        const PatItem& it = m->items.els[p.firstItem + k];
        if (it.type == ItemType::Literal) {
            out.Append(Str(m->text.els + it.litStart, it.litLen));
            continue;
        }
        int pos = it.offset;
        if (!it.absolute) {
            pos += t;
        } else if (it.offset < 0) {
            pos += T;
        } else {
            pos--;
        }
        Str v;
        if (pos < 0) {
            v = Str(before[-pos - 1 < 4 ? -pos - 1 : 4], 4);
        } else if (pos >= T) {
            v = Str(after[pos - T < 4 ? pos - T : 4], 4);
        } else {
            int nCols = rowStart.els[pos + 1] - rowStart.els[pos];
            if (it.column >= nCols) return false;
            v = toks.els[rowStart.els[pos] + it.column];
        }
        int start = len(out);
        out.Append(v);
        if (it.caps) {
            for (int i = start; i < len(out); i++) {
                char ch = out.els[i];
                if (ch >= 'A' && ch <= 'Z') out.els[i] = ch + ('a' - 'A');
            }
        }
    }
    return true;
}

// Tags one sequence (one row per token, space-separated columns) with plain Viterbi.
// False when a pattern needs a column a row lacks (Wapiti rejects the sequence).
bool CrfTag(CrfModel* m, const StrVec& rows, Vec<int>& labelsOut) {
    VecReset(labelsOut);
    if (!m) return false;
    int T = len(rows);
    if (T == 0) return true;
    int Y = m->nLabels;

    Vec<Str> toks;
    Vec<int> rowStart;
    for (int t = 0; t < T; t++) {
        VecAppend(rowStart, len(toks));
        SplitRow(rows.At(t), toks);
    }
    VecAppend(rowStart, len(toks));

    // active observations per position, in pattern order (the order Wapiti sums them in)
    Vec<int> uniObs, biObs;
    Vec<int> uniStart, biStart;
    str::Builder obsStr;
    for (int t = 0; t < T; t++) {
        VecAppend(uniStart, len(uniObs));
        VecAppend(biStart, len(biObs));
        for (const Pattern& p : m->patterns) {
            if (!ExecPattern(m, p, toks, rowStart, t, obsStr)) {
                logf("CrfTag: row %d lacks a column a pattern needs\n", t);
                return false;
            }
            int idx = FindObs(m, ToStr(obsStr));
            if (idx < 0) continue;
            const ObsEntry& e = m->obs.els[idx];
            if (e.kind & kObsUni) VecAppend(uniObs, e.uni);
            if (e.kind & kObsBi) VecAppend(biObs, e.bi);
        }
    }
    VecAppend(uniStart, len(uniObs));
    VecAppend(biStart, len(biObs));

    // tag_expsc + tag_viterbi, one position at a time: psi[y'][y] = uni[y] + bi[y'][y]
    const double* w = m->weights.els;
    Vec<double> uni, bi, cur, old;
    Vec<int> back;
    VecResize(uni, Y);
    VecResize(bi, Y * Y);
    VecResize(cur, Y);
    VecResize(old, Y);
    VecResize(back, T * Y);
    for (int t = 0; t < T; t++) {
        for (int y = 0; y < Y; y++) {
            double sum = 0.0;
            for (int n = uniStart.els[t]; n < uniStart.els[t + 1]; n++) {
                int off = uniObs.els[n];
                if (off >= 0) sum += w[off + y];
            }
            uni.els[y] = sum;
        }
        if (t == 0) {
            for (int y = 0; y < Y; y++) cur.els[y] = uni.els[y];
            continue;
        }
        for (int d = 0; d < Y * Y; d++) {
            double sum = 0.0;
            for (int n = biStart.els[t]; n < biStart.els[t + 1]; n++) {
                int off = biObs.els[n];
                if (off >= 0) sum += w[off + d];
            }
            bi.els[d] = sum;
        }
        for (int y = 0; y < Y; y++) old.els[y] = cur.els[y];
        for (int y = 0; y < Y; y++) {
            double bst = -HUGE_VAL;
            int idx = 0;
            for (int yp = 0; yp < Y; yp++) {
                double psi = uni.els[y] + bi.els[yp * Y + y];
                double val = old.els[yp] + psi;
                if (val > bst) {
                    bst = val;
                    idx = yp;
                }
            }
            back.els[t * Y + y] = idx;
            cur.els[y] = bst;
        }
    }

    int bst = 0;
    for (int y = 1; y < Y; y++) {
        if (cur.els[y] > cur.els[bst]) bst = y;
    }
    VecResize(labelsOut, T);
    for (int t = T; t > 0; t--) {
        labelsOut.els[t - 1] = bst;
        bst = t != 1 ? back.els[(t - 1) * Y + bst] : 0;
    }
    return true;
}
