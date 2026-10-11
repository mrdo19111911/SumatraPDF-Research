/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"

#include "CrfModel.h"
#include "GrobidText.h"
#include "GrobidReferences.h"

// Port of GROBID (commit c71ebff) citation parsing: engines/CitationParser.java (processingString,
// resultExtractionLayoutTokens), features/FeaturesVectorCitation.java, engines/AuthorParser.java,
// features/FeaturesVectorName.java, the FastMatcher gazetteers of lexicon/Lexicon.java and the
// DOI / arXiv / URL patterns of utilities/TextUtilities.java. Java offsets count UTF-16 chars,
// ours count UTF-8 bytes; both index the same text, so token positions agree.
//
//   raw -> strip, normalise, tokenize, split letters/digits -> one feature row per token
//       -> citation.wapiti -> clusters of same label -> title, authors, date, journal, ...
//   authors -> tokenize -> name-citation.wapiti -> forename / middlename / surname

//--- text

// Decodes the codepoint at s[i]; invalid UTF-8 yields U+FFFD over one byte.
static int CpAt(Str s, int i, int* nBytes) {
    u8 c = (u8)s.s[i];
    *nBytes = 1;
    if (c < 0x80) return c;
    int need = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (need == 0 || i + need >= len(s)) return 0xFFFD;
    int cp = c & (0x3F >> need);
    for (int k = 1; k <= need; k++) {
        u8 cc = (u8)s.s[i + k];
        if ((cc & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (cc & 0x3F);
    }
    *nBytes = need + 1;
    return cp;
}

static int CharCount(Str s) {
    return Utf8CodepointCount(s);
}

static bool IsAsciiDigit(char c) {
    return c >= '0' && c <= '9';
}

// Java regex \s: [ \t\n\x0B\f\r]
static bool IsRegexSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == 0x0B || c == '\f' || c == '\r';
}

// Character.isWhitespace
static bool IsJavaWhitespace(int cp) {
    if (cp == 0xA0 || cp == 0x2007 || cp == 0x202F) return false;
    if ((cp >= 0x09 && cp <= 0x0D) || (cp >= 0x1C && cp <= 0x20)) return true;
    return cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x205F ||
           cp == 0x3000;
}

static bool JIsUpper(int cp) {
    return cp <= 0xFFFF && IsCharUpperW((WCHAR)cp);
}

static bool JIsLower(int cp) {
    return cp <= 0xFFFF && IsCharLowerW((WCHAR)cp);
}

static bool JIsLetter(int cp) {
    return cp <= 0xFFFF && IsCharAlphaW((WCHAR)cp);
}

// String.trim(): chars <= ' ' at both ends
static Str JavaTrim(Str s) {
    int a = 0;
    int b = len(s);
    while (a < b && (u8)s.s[a] <= ' ') a++;
    while (b > a && (u8)s.s[b - 1] <= ' ') b--;
    return Str(s.s + a, b - a);
}

// StringUtils.isBlank
static bool IsBlank(Str s) {
    for (int i = 0; i < len(s);) {
        int n = 0;
        if (!IsJavaWhitespace(CpAt(s, i, &n))) return false;
        i += n;
    }
    return true;
}

// StringUtils.normalizeSpace: whitespace runs become one space, NBSP a space, ends trimmed
static TempStr NormalizeSpaceTemp(Str s) {
    str::Builder b;
    bool pendingSpace = false;
    for (int i = 0; i < len(s);) {
        int n = 0;
        int cp = CpAt(s, i, &n);
        if (IsJavaWhitespace(cp)) {
            pendingSpace = len(b) > 0;
        } else {
            if (pendingSpace) b.AppendChar(' ');
            pendingSpace = false;
            if (cp == 0xA0) {
                b.AppendChar(' ');
            } else {
                b.Append(Str(s.s + i, n));
            }
        }
        i += n;
    }
    return JavaTrim(ToStrTemp(b));
}

// TextUtilities.removeLeadingAndTrailingChars with ASCII char sets
static Str StripChars(Str s, Str leading, Str trailing) {
    int a = 0;
    int b = len(s);
    while (a < b && str::ContainsChar(leading, s.s[a])) a++;
    while (b > a && str::ContainsChar(trailing, s.s[b - 1])) b--;
    return Str(s.s + a, b - a);
}

static bool IsSpaceTok(Str tok) {
    return str::Eq(tok, StrL(" "));
}

static bool IsNewlineTok(Str tok) {
    return str::Eq(tok, StrL("\n"));
}

// TextUtilities.delimiters
static const int gDelimiters[] = {0x0A,   0x0D,   0x09,   0x0C,   0xA0,   0x200C, 0x28,  0xFF08, 0x5B, 0x20,
                                  0x2022, 0x2A,   0x2C,   0x3A,   0x3B,   0x3F,   0x2E,  0x21,   0x2F, 0x29,
                                  0xFF09, 0x2D,   0x2212, 0x2013, 0x2010, 0xAB,   0xBB,  0x201E, 0x22, 0x201C,
                                  0x201D, 0x2018, 0x2019, 0x27,   0x60,   0x24,   0x23,  0x40,   0x5D, 0x2666,
                                  0x2665, 0x2663, 0x2660, 0x3002, 0x3001, 0xFF0C, 0x30FB};

// delimiters.indexOf(token) != -1 for a token of the delimiter tokenizer: a single delimiter
static bool IsDelimiterTok(Str tok) {
    if (len(tok) == 0) return true;
    int n = 0;
    int cp = CpAt(tok, 0, &n);
    if (n != len(tok)) return false;
    for (int d : gDelimiters) {
        if (d == cp) return true;
    }
    return false;
}

static TempStr TokensTextTemp(const StrVec& toks, int start, int end) {
    str::Builder b;
    for (int i = start; i <= end && i < len(toks); i++) b.Append(toks[i]);
    return ToStrTemp(b);
}

// StringUtils.isAllLowerCase
static bool IsAllLower(Str s) {
    if (len(s) == 0) return false;
    for (int i = 0; i < len(s);) {
        int n = 0;
        if (!JIsLower(CpAt(s, i, &n))) return false;
        i += n;
    }
    return true;
}

// StringUtils.isAlpha
static bool IsAllLetters(Str s) {
    if (len(s) == 0) return false;
    for (int i = 0; i < len(s);) {
        int n = 0;
        if (!JIsLetter(CpAt(s, i, &n))) return false;
        i += n;
    }
    return true;
}

// LayoutTokensUtil.doesRequireDehypenisation for tokens without coordinates (y == -1):
// only a hyphen before a line break, followed by a lowercase word, is removed
static bool NeedsDehyphen(const StrVec& toks, int start, int end, int i) {
    int j = i + 1;
    int breakLine = 0;
    while (j <= end && (IsSpaceTok(toks[j]) || IsNewlineTok(toks[j]))) {
        if (IsNewlineTok(toks[j])) breakLine++;
        j++;
    }
    if (breakLine == 0) return false;
    if (j > end || !IsAllLower(toks[j])) return false;
    if (i < start + 1) return true;
    int z = i - 1;
    while (z > start && (IsSpaceTok(toks[z]) || IsNewlineTok(toks[z]))) z--;
    return IsAllLetters(toks[z]);
}

// LayoutTokensUtil.normalizeDehyphenizeText of tokens [start, end]
static TempStr DehyphenTextTemp(const StrVec& toks, int start, int end) {
    StrVec out;
    for (int i = start; i <= end; i++) {
        Str tok = toks[i];
        if (!str::Eq(tok, StrL("-"))) {
            out.Append(tok);
            continue;
        }
        if (NeedsDehyphen(toks, start, end, i)) {
            while (len(out) > 0 && IsSpaceTok(out[len(out) - 1])) out.RemoveAt(len(out) - 1);
            while (i + 1 <= end && (IsSpaceTok(toks[i + 1]) || IsNewlineTok(toks[i + 1]))) i++;
            continue;
        }
        out.Append(tok);
        while (i + 1 <= end && IsNewlineTok(toks[i + 1])) i++;
    }
    TempStr text = JoinTemp(&out, Str());
    return NormalizeSpaceTemp(str::ReplaceTemp(text, StrL("\n"), StrL(" ")));
}

//--- tokens

// GrobidDefaultAnalyzer REGEX "(?<=[\p{L}])(?=\d)|(?<=\d)(?=\D)": "32nd" -> 32|nd, "v2" -> v|2
static void AppendSplitDigits(Str tok, StrVec& out) {
    if (len(JavaTrim(tok)) == 0) {
        out.Append(tok);
        return;
    }
    int start = 0;
    int prev = -1;
    for (int i = 0; i < len(tok);) {
        int n = 0;
        int cp = CpAt(tok, i, &n);
        bool curDigit = cp >= '0' && cp <= '9';
        bool prevDigit = prev >= '0' && prev <= '9';
        bool cut = prev >= 0 && ((JIsLetter(prev) && curDigit) || (prevDigit && !curDigit));
        if (cut) {
            out.Append(Str(tok.s + start, i - start));
            start = i;
        }
        prev = cp;
        i += n;
    }
    out.Append(Str(tok.s + start, len(tok) - start));
}

static TempStr NormaliseTemp(Str s) {
    StrVec toks;
    GrobidTokenize(s, toks);
    return JoinTemp(&toks, Str());
}

// CitationParser.processingString + processingStringMultiple: strip "[({.,])}: " in front and
// spaces at the end, normalise, strip again, tokenize, split letters from digits
static void CitationTokens(Str raw, StrVec& toks) {
    Str lead = StrL("[({.,])}: \n");
    Str trail = StrL(" \n");
    Str s = NormaliseTemp(StripChars(raw, lead, trail));
    if (IsBlank(s)) return;
    s = StripChars(s, lead, trail);
    if (IsBlank(s)) return;
    StrVec words;
    GrobidTokenize(s, words);
    for (Str w : words) AppendSplitDigits(w, toks);
}

//--- gazetteers (FastMatcher)

// research/grobid/lex-*.txt from cmd/gen-grobid-data.ts: lowercased terms, tokens joined by a space,
// sorted by bytes, front-coded: chr(33 + bytes shared with the previous term) + the rest.
struct Gazetteer {
    char* buf = nullptr;
    Vec<Str> terms;
};

enum class Gaz {
    Journals,
    AbbrevJournals,
    Conferences,
    Publishers,
    Locations,
    Collaborations,
    PersonTitles,
    PersonSuffixes,
    Count
};

static const char* gGazFiles[] = {"lex-journals.txt",      "lex-abbrev-journals.txt", "lex-conferences.txt",
                                  "lex-publishers.txt",    "lex-locations.txt",       "lex-collaborations.txt",
                                  "lex-person-titles.txt", "lex-person-suffixes.txt"};

static Mutex gGazMutex;
static Gazetteer* gGazetteers[(int)Gaz::Count];

static Gazetteer* LoadGazetteer(Str file) {
    auto g = new Gazetteer();
    Str data = GrobidDataTemp(file);
    if (len(data) == 0) logf("GROBID: missing '%s'\n", file);

    // first pass sizes the decoded buffer, second fills it
    for (int pass = 0; pass < 2; pass++) {
        int total = 0;
        Str prev;
        for (int i = 0; i < len(data);) {
            int e = i;
            while (e < len(data) && data.s[e] != '\n') e++;
            if (e > i) {
                int shared = ClampI((int)(u8)data.s[i] - 33, 0, len(prev));
                int rest = e - i - 1;
                if (pass == 1) {
                    char* dst = g->buf + total;
                    memmove(dst, prev.s, shared);
                    memcpy(dst + shared, data.s + i + 1, rest);
                    prev = Str(dst, shared + rest);
                    VecAppend(g->terms, prev);
                } else {
                    prev = Str(data.s + i + 1, shared + rest); // only len(prev) matters here
                }
                total += shared + rest;
            }
            i = e + 1;
        }
        if (pass == 0) g->buf = (char*)malloc(total + 1);
        if (!g->buf) break;
    }
    return g;
}

static Gazetteer* GetGazetteer(Gaz which) {
    AutoUnlockMutex lock(&gGazMutex);
    Gazetteer** g = &gGazetteers[(int)which];
    if (!*g) *g = LoadGazetteer(Str(gGazFiles[(int)which]));
    return *g;
}

static int CompareBytes(Str a, Str b) {
    int n = len(a) < len(b) ? len(a) : len(b);
    int c = memcmp(a.s, b.s, n);
    if (c != 0) return c;
    return len(a) - len(b);
}

static int GazLowerBound(Gazetteer* g, Str key) {
    int lo = 0;
    int hi = len(g->terms);
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (CompareBytes(g->terms[mid], key) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

// the FastMatcher trie has a node for these tokens: some term starts with them
static bool GazHasNode(Gazetteer* g, Str key) {
    for (int i = GazLowerBound(g, key); i < len(g->terms); i++) {
        Str t = g->terms[i];
        if (!str::StartsWith(t, key)) return false;
        if (len(t) == len(key) || t.s[len(key)] == ' ') return true;
        if ((u8)t.s[len(key)] > ' ') return false;
    }
    return false;
}

static bool GazIsTerm(Gazetteer* g, Str key) {
    int i = GazLowerBound(g, key);
    return i < len(g->terms) && str::Eq(g->terms[i], key);
}

// OffsetPosition: inclusive token range, or [start, end) chars before conversion
struct GrobidPos {
    int start;
    int end;
};

struct OpenMatch {
    int start;
    int last;
    Str key;
};

// FastMatcher.matchLayoutToken(tokens, ignoreDelimiters, !caseSensitive): a match is recorded
// when the token after its last one arrives, so the list is ordered by end, not start
static void GazMatch(Gaz which, const StrVec& toks, Vec<GrobidPos>& res) {
    Gazetteer* g = GetGazetteer(which);
    if (len(g->terms) == 0) return;
    Vec<OpenMatch> open;
    Vec<OpenMatch> next;
    for (int pos = 0; pos < len(toks); pos++) {
        Str tok = toks[pos];
        if (IsSpaceTok(tok) || IsNewlineTok(tok) || IsDelimiterTok(tok)) continue;
        TempStr text = GrobidLowerTemp(tok);
        VecClear(next);
        for (OpenMatch& m : open) {
            TempStr key = fmt("%s %s", m.key, text);
            if (GazHasNode(g, key)) VecAppend(next, OpenMatch{m.start, pos, key});
            if (GazIsTerm(g, m.key)) VecAppend(res, GrobidPos{m.start, m.last});
        }
        if (GazHasNode(g, text)) VecAppend(next, OpenMatch{pos, pos, text});
        VecClear(open);
        VecAppendVec(open, next);
    }
    for (OpenMatch& m : open) {
        if (GazIsTerm(g, m.key)) VecAppend(res, GrobidPos{m.start, m.last});
    }
}

//--- identifier and URL patterns

// DOIPattern "(10\.\d{4,5}\/[\S]+[^;,.\s])", returns the match end or -1
static int MatchDoiAt(Str t, int i) {
    if (!str::StartsWith(Str(t.s + i, len(t) - i), StrL("10."))) return -1;
    int p = i + 3;
    int nDigits = 0;
    while (p + nDigits < len(t) && IsAsciiDigit(t.s[p + nDigits])) nDigits++;
    int slash = -1;
    if (nDigits >= 5 && p + 5 < len(t) && t.s[p + 5] == '/') slash = p + 5;
    if (slash < 0 && nDigits == 4 && p + 4 < len(t) && t.s[p + 4] == '/') slash = p + 4;
    if (slash < 0) return -1;
    int runStart = slash + 1;
    int runEnd = runStart;
    while (runEnd < len(t) && !IsRegexSpace(t.s[runEnd])) runEnd++;
    for (int q = runEnd - 1; q > runStart; q--) {
        char c = t.s[q];
        if (c != ';' && c != ',' && c != '.') return q + 1;
    }
    return -1;
}

static int SkipOptSpace(Str t, int p) {
    return p < len(t) && IsRegexSpace(t.s[p]) ? p + 1 : p;
}

static int CountDigits(Str t, int p) {
    int n = 0;
    while (p + n < len(t) && IsAsciiDigit(t.s[p + n])) n++;
    return n;
}

// "(v\d+)?"
static int SkipVersion(Str t, int p) {
    if (p + 1 < len(t) && t.s[p] == 'v' && IsAsciiDigit(t.s[p + 1])) return p + 1 + CountDigits(t, p + 1);
    return p;
}

static bool IsArxivOldChar(char c) {
    return c == ' ' || c == '-' || c == '.' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// arXivPattern: arXiv\s?(\.org)?\s?\:\s? then \d{4}\s?\.\s?\d{4,5}(v\d+)?
// or [ a-zA-Z\-\.]*\s?/\s?\d{7}(v\d+)?. Returns the match end or -1.
static int MatchArxivAt(Str t, int i) {
    if (!str::StartsWith(Str(t.s + i, len(t) - i), StrL("arXiv"))) return -1;
    int p = SkipOptSpace(t, i + 5);
    if (str::StartsWith(Str(t.s + p, len(t) - p), StrL(".org"))) p += 4;
    p = SkipOptSpace(t, p);
    if (p >= len(t) || t.s[p] != ':') return -1;
    p = SkipOptSpace(t, p + 1);

    int q = p;
    if (CountDigits(t, q) >= 4) {
        q = SkipOptSpace(t, q + 4);
        if (q < len(t) && t.s[q] == '.') {
            q = SkipOptSpace(t, q + 1);
            int n = CountDigits(t, q);
            if (n >= 4) return SkipVersion(t, q + (n > 5 ? 5 : n));
        }
    }

    q = p;
    while (q < len(t) && IsArxivOldChar(t.s[q])) q++;
    q = SkipOptSpace(t, q);
    if (q >= len(t) || t.s[q] != '/') return -1;
    q = SkipOptSpace(t, q + 1);
    if (CountDigits(t, q) < 7) return -1;
    return SkipVersion(t, q + 7);
}

// [-A-Z0-9+&@#/%=~_()] case-insensitive; the run class adds [?|!:.;]
static bool IsUrlEndChar(char c) {
    return IsAsciiDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           str::ContainsChar(StrL("-+&@#/%=~_()"), c);
}

static bool IsUrlChar(char c) {
    return IsUrlEndChar(c) || str::ContainsChar(StrL("?|!:.;"), c);
}

static int SkipSpaces2(Str t, int p) {
    for (int k = 0; k < 2 && p < len(t) && IsRegexSpace(t.s[p]); k++) p++;
    return p;
}

static int MatchUrlTail(Str t, int p) {
    int e = p;
    while (e < len(t) && IsUrlChar(t.s[e])) e++;
    for (int q = e - 1; q >= p; q--) {
        if (IsUrlEndChar(t.s[q])) return q + 1;
    }
    return -1;
}

// urlPattern1 "(?i)(https?|ftp)\s{0,2}:\s{0,2}//\s{0,2}RUN*END|www\s{0,2}\.\s{0,2}RUN*END"
static int MatchUrlAt(Str t, int i) {
    Str rest = Str(t.s + i, len(t) - i);
    const char* schemes[] = {"https", "http", "ftp"};
    for (const char* scheme : schemes) {
        Str sc = Str(scheme);
        if (!str::StartsWithI(rest, sc)) continue;
        int p = SkipSpaces2(t, i + len(sc));
        if (p >= len(t) || t.s[p] != ':') continue;
        p = SkipSpaces2(t, p + 1);
        if (!str::StartsWith(Str(t.s + p, len(t) - p), StrL("//"))) continue;
        int end = MatchUrlTail(t, SkipSpaces2(t, p + 2));
        if (end > 0) return end;
    }
    if (!str::StartsWithI(rest, StrL("www"))) return -1;
    int p = SkipSpaces2(t, i + 3);
    if (p >= len(t) || t.s[p] != '.') return -1;
    return MatchUrlTail(t, SkipSpaces2(t, p + 1));
}

// Matcher.find() loop: non-overlapping [start, end) char ranges
static void FindAll(Str text, int (*matchAt)(Str, int), Vec<GrobidPos>& out) {
    for (int i = 0; i < len(text);) {
        int end = matchAt(text, i);
        if (end > i) {
            VecAppend(out, GrobidPos{i, end});
            i = end;
            continue;
        }
        i++;
    }
}

// Utilities.convertStringOffsetToTokenOffset: [start, end) chars -> inclusive token range
static void ToTokenOffsets(const Vec<GrobidPos>& strPos, const StrVec& toks, Vec<GrobidPos>& out) {
    int indexText = 0;
    int indexToken = 0;
    Str token;
    for (const GrobidPos& pos : strPos) {
        while (indexToken < len(toks)) {
            token = toks[indexToken];
            if (indexText >= pos.start) {
                GrobidPos cur = {indexToken, indexToken};
                bool found = false;
                while (indexToken < len(toks)) {
                    token = toks[indexToken];
                    if (indexText + len(token) >= pos.end) {
                        cur.end = indexToken;
                        VecAppend(out, cur);
                        found = true;
                        break;
                    }
                    indexToken++;
                    indexText += len(token);
                }
                if (found) {
                    indexToken++;
                    indexText += len(token);
                    break;
                }
                cur.end = indexToken - 1;
                VecAppend(out, cur);
            }
            indexToken++;
            indexText += len(token);
        }
    }
}

static bool PosLess(const GrobidPos& a, const GrobidPos& b) {
    return a.start < b.start || (a.start == b.start && a.end < b.end);
}

static void SortPositions(Vec<GrobidPos>& v) {
    for (int i = 1; i < len(v); i++) {
        GrobidPos x = v[i];
        int j = i - 1;
        while (j >= 0 && PosLess(x, v[j])) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = x;
    }
}

// Utilities.mergePositions: union, sorted, overlapping ranges joined
static void MergePositions(Vec<GrobidPos>& a, Vec<GrobidPos>& b, Vec<GrobidPos>& out) {
    if (len(a) == 0 || len(b) == 0) {
        VecAppendVec(out, len(a) == 0 ? b : a);
        return;
    }
    Vec<GrobidPos> all;
    VecAppendVec(all, a);
    for (GrobidPos& p : b) {
        bool dup = false;
        for (GrobidPos& q : all) dup = dup || (q.start == p.start && q.end == p.end);
        if (!dup) VecAppend(all, p);
    }
    SortPositions(all);
    int prev = -1;
    for (GrobidPos& p : all) {
        if (prev < 0) {
            VecAppend(out, p);
            prev = len(out) - 1;
        } else if (p.start >= out[prev].start && p.end <= out[prev].end) {
            // contained
        } else if (out[prev].end >= p.start) {
            out[prev].end = p.end;
        } else {
            VecAppend(out, p);
            prev = len(out) - 1;
        }
    }
}

// Lexicon.tokenPositionsIdentifierPattern: DOIs and arXiv ids
static void IdentifierPositions(const StrVec& toks, Vec<GrobidPos>& out) {
    TempStr text = JoinTemp((StrVec*)&toks, Str());
    Vec<GrobidPos> chars;
    Vec<GrobidPos> doi;
    FindAll(text, MatchDoiAt, chars);
    ToTokenOffsets(chars, toks, doi);
    VecClear(chars);
    Vec<GrobidPos> arxiv;
    FindAll(text, MatchArxivAt, chars);
    ToTokenOffsets(chars, toks, arxiv);
    MergePositions(doi, arxiv, out);
}

// Lexicon.tokenPositionsUrlPattern
static void UrlPositions(const StrVec& toks, Vec<GrobidPos>& out) {
    TempStr text = JoinTemp((StrVec*)&toks, Str());
    Vec<GrobidPos> chars;
    FindAll(text, MatchUrlAt, chars);
    ToTokenOffsets(chars, toks, out);
}

// The match scan of addFeaturesCitation / addFeaturesName: walks the list once in its order,
// so a match listed after one that starts later is missed (GROBID does the same).
struct PosScan {
    Vec<GrobidPos>* pos;
    int cur = 0;
};

static bool InPositions(PosScan& sc, int n) {
    Vec<GrobidPos>& v = *sc.pos;
    if (sc.cur == len(v) - 1 && v[sc.cur].end < n) return false;
    for (int i = sc.cur; i < len(v); i++) {
        if (v[i].start <= n && v[i].end >= n) {
            sc.cur = i;
            return true;
        }
        if (v[i].start > n) {
            sc.cur = i;
            return false;
        }
    }
    return false;
}

//--- feature rows

static Str PunctType(Str text) {
    Str res = GrobidIsPunct(text) ? StrL("PUNCT") : StrL("NOPUNCT");
    if (str::Eq(text, StrL("(")) || str::Eq(text, StrL("["))) return StrL("OPENBRACKET");
    if (str::Eq(text, StrL(")")) || str::Eq(text, StrL("]"))) return StrL("ENDBRACKET");
    if (str::Eq(text, StrL("."))) return StrL("DOT");
    if (str::Eq(text, StrL(","))) return StrL("COMMA");
    if (str::Eq(text, StrL("-"))) return StrL("HYPHEN");
    if (str::Eq(text, StrL("\"")) || str::Eq(text, StrL("'")) || str::Eq(text, StrL("`"))) return StrL("QUOTE");
    return res;
}

static Str Capitalisation(Str text) {
    if (GrobidAllCapital(text)) return StrL("ALLCAP");
    if (GrobidIsUpperFirst(text)) return StrL("INITCAP");
    return StrL("NOCAPS");
}

static Str DigitStatus(Str text) {
    if (GrobidAllDigits(text)) return StrL("ALLDIGIT");
    if (GrobidHasDigit(text)) return StrL("CONTAINSDIGITS");
    return StrL("NODIGIT");
}

static void AppendBit(str::Builder& b, bool v) {
    b.Append(v ? StrL(" 1") : StrL(" 0"));
}

// printVector() columns shared by the citation and name models: token, lowercase, 4 prefixes,
// 4 suffixes, line status, capitalisation, digits, single char
static void AppendTokenColumns(str::Builder& b, Str text, Str lineStatus) {
    b.Append(text);
    b.AppendChar(' ');
    b.Append(GrobidLowerTemp(text));
    for (int k = 1; k <= 4; k++) {
        b.AppendChar(' ');
        b.Append(GrobidPrefixTemp(text, k));
    }
    for (int k = 1; k <= 4; k++) {
        b.AppendChar(' ');
        b.Append(GrobidSuffixTemp(text, k));
    }
    b.AppendChar(' ');
    b.Append(lineStatus);
    Str digit = DigitStatus(text);
    b.AppendChar(' ');
    b.Append(str::Eq(digit, StrL("ALLDIGIT")) ? StrL("NOCAPS") : Capitalisation(text));
    b.AppendChar(' ');
    b.Append(digit);
    AppendBit(b, CharCount(text) == 1);
}

// "text" of a token as the feature loops use it; empty when the token gives no row
static Str RowText(Str tok) {
    if (IsSpaceTok(tok) || IsNewlineTok(tok)) return {};
    if (len(JavaTrim(tok)) == 0) return {};
    return tok;
}

enum class CitLex {
    Journal,
    AbbrevJournal,
    Conference,
    Publisher,
    Location,
    Collaboration,
    Identifier,
    Url,
    Count
};

// FeaturesVectorCitation.addFeaturesCitation + printVector: one row per token that is not a space
static void CitationRows(const StrVec& toks, StrVec& rows, Vec<int>& rowTok) {
    Vec<GrobidPos> pos[(int)CitLex::Count];
    GazMatch(Gaz::Journals, toks, pos[(int)CitLex::Journal]);
    GazMatch(Gaz::AbbrevJournals, toks, pos[(int)CitLex::AbbrevJournal]);
    GazMatch(Gaz::Conferences, toks, pos[(int)CitLex::Conference]);
    GazMatch(Gaz::Publishers, toks, pos[(int)CitLex::Publisher]);
    GazMatch(Gaz::Locations, toks, pos[(int)CitLex::Location]);
    GazMatch(Gaz::Collaborations, toks, pos[(int)CitLex::Collaboration]);
    IdentifierPositions(toks, pos[(int)CitLex::Identifier]);
    UrlPositions(toks, pos[(int)CitLex::Url]);
    PosScan scans[(int)CitLex::Count];
    for (int k = 0; k < (int)CitLex::Count; k++) scans[k].pos = &pos[k];

    int size = len(toks);
    for (int n = 0; n < size; n++) {
        Str text = RowText(toks[n]);
        if (len(text) == 0) continue;

        bool in[(int)CitLex::Count];
        for (int k = 0; k < (int)CitLex::Count; k++) in[k] = InPositions(scans[k], n);
        if (GrobidFilterLine(text)) continue;

        Str lineStatus = StrL("LINEIN");
        if (n == 0) {
            lineStatus = StrL("LINESTART");
        } else if (size == n + 1) {
            lineStatus = StrL("LINEEND");
        }
        bool firstName = GrobidIsFirstName(text);
        bool lastName = GrobidIsLastName(text);

        str::Builder b;
        AppendTokenColumns(b, text, lineStatus);
        AppendBit(b, firstName || lastName);
        AppendBit(b, GrobidInDictionary(text));
        AppendBit(b, firstName);
        AppendBit(b, lastName);
        AppendBit(b, in[(int)CitLex::Location]);
        AppendBit(b, GrobidIsYear(text));
        AppendBit(b, GrobidIsMonth(text));
        AppendBit(b, in[(int)CitLex::Url]);
        AppendBit(b, in[(int)CitLex::Collaboration]);
        AppendBit(b, in[(int)CitLex::Journal] || in[(int)CitLex::AbbrevJournal]);
        AppendBit(b, in[(int)CitLex::Conference]);
        AppendBit(b, in[(int)CitLex::Publisher]);
        AppendBit(b, in[(int)CitLex::Identifier]);
        b.AppendChar(' ');
        b.Append(PunctType(text));
        b.Append(fmt(" %d 0", GrobidLinearScaling(n, size, 12)));
        rows.Append(ToStr(b));
        VecAppend(rowTok, n);
    }
}

// FeaturesVectorName.addFeaturesName + printVector. Its "newline" flag is reset before it is
// read, so only the first token is LINESTART.
static void NameRows(const StrVec& toks, StrVec& rows, Vec<int>& rowTok) {
    Vec<GrobidPos> titles;
    Vec<GrobidPos> suffixes;
    GazMatch(Gaz::PersonTitles, toks, titles);
    GazMatch(Gaz::PersonSuffixes, toks, suffixes);
    PosScan titleScan = {&titles};
    PosScan suffixScan = {&suffixes};

    int size = len(toks);
    for (int n = 0; n < size; n++) {
        Str text = RowText(toks[n]);
        if (len(text) == 0) continue;
        bool isTitle = InPositions(titleScan, n);
        bool isSuffix = InPositions(suffixScan, n);
        if (GrobidFilterLine(text)) continue;

        Str lineStatus = StrL("LINEIN");
        if (n == 0) {
            lineStatus = StrL("LINESTART");
        } else if (size == n + 1) {
            lineStatus = StrL("LINEEND");
        } else {
            // look ahead: a newline before the next token ends the line
            for (int i = n + 1; i < size; i++) {
                if (IsNewlineTok(toks[i])) {
                    lineStatus = StrL("LINEEND");
                    break;
                }
                if (!IsSpaceTok(toks[i])) break;
            }
        }

        str::Builder b;
        AppendTokenColumns(b, text, lineStatus);
        AppendBit(b, GrobidInDictionary(text));
        AppendBit(b, GrobidIsFirstName(text));
        AppendBit(b, GrobidIsLastName(text));
        AppendBit(b, isTitle);
        AppendBit(b, isSuffix);
        b.AppendChar(' ');
        b.Append(PunctType(text));
        b.Append(StrL(" 0"));
        rows.Append(ToStr(b));
        VecAppend(rowTok, n);
    }
}

//--- labeling

// TaggingTokenClusteror: consecutive rows with one label; "I-<x>" starts a new cluster. Each row
// owns the tokens up to its own plus the spaces after it.
struct Cluster {
    Str label; // "<title>"
    int start;
    int end;
};

static bool TagRows(Str modelName, const StrVec& rows, const Vec<int>& rowTok, const StrVec& toks,
                    Vec<Cluster>& clusters) {
    CrfModel* model = GrobidModel(modelName);
    if (!model || len(rows) == 0) return false;
    Vec<int> labels;
    if (!CrfTag(model, rows, labels) || len(labels) != len(rows)) return false;

    int prevEnd = -1;
    for (int r = 0; r < len(rows); r++) {
        Str label = CrfLabelName(model, labels[r]);
        bool begin = str::StartsWith(label, StrL("I-"));
        if (begin) label = Str(label.s + 2, len(label) - 2);
        int end = rowTok[r];
        while (end + 1 < len(toks) && (IsSpaceTok(toks[end + 1]) || IsNewlineTok(toks[end + 1]))) end++;
        int n = len(clusters);
        if (r == 0 || begin || !str::Eq(clusters[n - 1].label, label)) {
            VecAppend(clusters, Cluster{label, prevEnd + 1, end});
        } else {
            clusters[n - 1].end = end;
        }
        prevEnd = end;
    }
    return true;
}

//--- names

using Person = GrobidPerson;

static bool PersonNotNull(const Person& p) {
    return len(p.first) > 0 || len(p.middle) > 0 || len(p.last) > 0 || len(p.title) > 0;
}

// TextUtilities.capitalizeFully(s, "-.,;:/_ ")
static TempStr CapitalizeFullyTemp(Str s) {
    if (len(s) == 0) return s;
    WStr w = ToWStrTemp(s);
    bool toUpper = true;
    for (int i = 0; i < len(w); i++) {
        WCHAR c = w.s[i];
        if (c < 0x80 && str::ContainsChar(StrL("-.,;:/_ "), (char)c)) {
            toUpper = true;
            continue;
        }
        if (toUpper) {
            CharUpperBuffW(&w.s[i], 1);
        } else {
            CharLowerBuffW(&w.s[i], 1);
        }
        toUpper = false;
    }
    return ToUtf8Temp(w);
}

// Person.normalizeName: "JA" as forename is J + A
static void NormalizeName(Person& p) {
    if (len(p.middle) == 0 && CharCount(p.first) == 2) {
        int n0 = 0;
        int n1 = 0;
        int c0 = CpAt(p.first, 0, &n0);
        int c1 = CpAt(p.first, n0, &n1);
        if (JIsUpper(c0) && JIsUpper(c1)) {
            p.middle = Str(p.first.s + n0, n1);
            p.first = Str(p.first.s, n0);
        }
    }
    p.first = CapitalizeFullyTemp(p.first);
    p.middle = CapitalizeFullyTemp(p.middle);
    p.last = CapitalizeFullyTemp(p.last);
}

// a forename or surname cluster starts a new person when that field is already set,
// unless a marker just started one
static void SetNamePart(Vec<Person>& persons, Person& cur, Str Person::* field, Str content, bool& newMarker) {
    if (!newMarker && len(cur.*field) > 0) {
        if (PersonNotNull(cur)) VecAppend(persons, cur);
        cur = Person();
    }
    cur.*field = content;
    newMarker = false;
}

// "et\.? al\.?": the index where it starts, or -1
static int FindEtAl(Str s) {
    for (int i = 0; i + 2 <= len(s); i++) {
        if (s.s[i] != 'e' || s.s[i + 1] != 't') continue;
        int p = i + 2;
        if (p < len(s) && s.s[p] == '.') p++;
        if (str::StartsWith(Str(s.s + p, len(s) - p), StrL(" al"))) return i;
    }
    return -1;
}

// AuthorParser.processingCitation with the name-citation model
static void ParseNames(Str input, Vec<Person>& persons) {
    input = JavaTrim(input);
    if (len(input) == 0) return;
    int etAl = FindEtAl(input);
    if (etAl >= 0) input = str::JoinTemp(Str(input.s, etAl), StrL(" "));

    StrVec toks;
    GrobidTokenize(input, toks);
    GrobidParseNames(toks, StrL("name-citation"), persons);
}

// AuthorParser.processing: name model (name-citation / name-header) over layout tokens.
// A marker cluster ends the current person; Person.setTitle stores nothing, so a title
// cluster only consumes the marker.
void GrobidParseNames(const StrVec& toks, Str model, Vec<GrobidPerson>& persons) {
    StrVec rows;
    Vec<int> rowTok;
    NameRows(toks, rows, rowTok);
    Vec<Cluster> clusters;
    if (!TagRows(model, rows, rowTok, toks, clusters)) return;

    int first = len(persons);
    Person cur;
    bool newMarker = false;
    for (Cluster& c : clusters) {
        Str content = NormalizeSpaceTemp(TokensTextTemp(toks, c.start, c.end));
        if (IsBlank(content)) continue;
        if (str::Eq(c.label, StrL("<marker>"))) {
            newMarker = true;
            if (PersonNotNull(cur)) {
                VecAppend(persons, cur);
                cur = Person();
            }
        } else if (str::Eq(c.label, StrL("<forename>"))) {
            SetNamePart(persons, cur, &Person::first, content, newMarker);
        } else if (str::Eq(c.label, StrL("<surname>"))) {
            SetNamePart(persons, cur, &Person::last, content, newMarker);
        } else if (str::Eq(c.label, StrL("<title>"))) {
            newMarker = false;
        } else if (str::Eq(c.label, StrL("<middlename>"))) {
            bool append = !newMarker && len(cur.middle) > 0;
            cur.middle = append ? str::JoinTemp(cur.middle, StrL(" "), content) : content;
            newMarker = false;
        } else if (str::Eq(c.label, StrL("<suffix>"))) {
            cur.suffix = len(cur.suffix) > 0 ? str::JoinTemp(cur.suffix, StrL(" "), content) : content;
        }
    }
    if (PersonNotNull(cur)) VecAppend(persons, cur);
    for (int i = first; i < len(persons); i++) NormalizeName(persons[i]);
}

static void AppendInitials(str::Builder& b, Str names) {
    if (len(names) == 0) return;
    StrVec parts;
    Split(&parts, names, StrL(" "), true);
    for (Str part : parts) {
        if (len(part) == 0) continue;
        int n = 0;
        CpAt(part, 0, &n);
        if (len(b) > 0) b.AppendChar(' ');
        b.Append(Str(part.s, n));
        b.AppendChar('.');
    }
}

// "Surname, F. M.; Surname, F."
static TempStr FormatPersonsTemp(Vec<Person>& persons) {
    str::Builder out;
    for (Person& p : persons) {
        str::Builder initials;
        AppendInitials(initials, p.first);
        AppendInitials(initials, p.middle);
        str::Builder one;
        if (len(p.last) > 0) {
            one.Append(p.last);
            if (len(initials) > 0) {
                one.Append(StrL(", "));
                one.Append(ToStr(initials));
            }
        } else {
            one.Append(NormalizeSpaceTemp(str::JoinTemp(p.first, StrL(" "), p.middle)));
        }
        if (len(one) == 0) continue;
        if (len(out) > 0) out.Append(StrL("; "));
        out.Append(ToStr(one));
    }
    return ToStrTemp(out);
}

//--- fields

// TextUtilities.cleanField: punctuation trimmed at the ends, enclosing parens dropped,
// trailing stopwords removed when asked
enum class StopWords {
    Keep,
    Remove
};

static TempStr CleanFieldTemp(Str in, StopWords stop) {
    if (len(in) == 0) return {};
    Str s = str::ReplaceTemp(in, StrL(",,"), StrL(","));
    s = str::ReplaceTemp(s, StrL(", ,"), StrL(","));

    int n = len(s);
    for (int i = len(s) - 1; i > 0; i--) {
        char c = s.s[i];
        if (str::ContainsChar(StrL(", .-_/:"), c)) {
            n = i;
            continue;
        }
        if (c != ';') break;
        // keep an HTML entity like "&amp;"
        bool entity = false;
        for (int k = 3; k <= 6; k++) entity = entity || (i - k >= 0 && s.s[i - k] == '&');
        if (entity) break;
        n = i;
    }
    s = Str(s.s, n);

    n = 0;
    for (int i = 0; i < len(s); i++) {
        if (!str::ContainsChar(StrL(", .;-_:"), s.s[i])) break;
        n = i;
    }
    s = JavaTrim(Str(s.s + n, len(s) - n));

    if (str::EndsWith(s, StrL(")")) && str::StartsWith(s, StrL("("))) s = JavaTrim(Str(s.s + 1, len(s) - 2));
    Str quot = StrL("&quot;");
    if (len(s) > 12 && str::EndsWith(s, quot) && str::StartsWith(s, quot)) {
        s = JavaTrim(Str(s.s + 6, len(s) - 12));
    }

    if (stop == StopWords::Remove) {
        static const char* stopwords[] = {"the", "of", "and", "du", "de le", "de la", "des", "der", "an", "und", "for"};
        bool again = true;
        while (again) {
            again = false;
            for (const char* w : stopwords) {
                Str word = Str(w);
                if (!str::EndsWith(s, str::JoinTemp(StrL(" "), word))) continue;
                s = JavaTrim(Str(s.s, len(s) - len(word)));
                again = true;
                break;
            }
        }
    }
    return JavaTrim(s);
}

// "https?\://(dx\.)?doi\.org/" removed everywhere
static TempStr RemoveDoiOrgTemp(Str s) {
    static const char* prefixes[] = {"https://dx.doi.org/", "http://dx.doi.org/", "https://doi.org/",
                                     "http://doi.org/"};
    str::Builder b;
    for (int i = 0; i < len(s);) {
        bool skipped = false;
        for (const char* p : prefixes) {
            Str prefix = Str(p);
            if (!str::StartsWith(Str(s.s + i, len(s) - i), prefix)) continue;
            i += len(prefix);
            skipped = true;
            break;
        }
        if (skipped) continue;
        b.AppendChar(s.s[i]);
        i++;
    }
    return ToStrTemp(b);
}

// \p{M} for the scripts DOIs use
static bool IsDoiMark(int cp) {
    return (cp >= 0x300 && cp <= 0x36F) || (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x1DC0 && cp <= 0x1DFF) ||
           (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE20 && cp <= 0xFE2F);
}

// BiblioItem.cleanDOI
static TempStr CleanDoiTemp(Str doi) {
    Str s = str::ReplaceTemp(NormalizeSpaceTemp(doi), StrL(" "), StrL(""));
    s = RemoveDoiOrgTemp(s);
    if (str::StartsWithI(s, StrL("doi:")) || str::StartsWithI(s, StrL("doi/"))) s = Str(s.s + 4, len(s) - 4);
    if (str::StartsWithI(s, StrL("doi"))) s = Str(s.s + 3, len(s) - 3);
    int i = str::IndexOfI(s, StrL("doi:10."));
    if (i >= 0) s = Str(s.s + i + 4, len(s) - i - 4);
    if (str::StartsWith(s, StrL("file://")) || str::StartsWith(s, StrL("https://")) ||
        str::StartsWith(s, StrL("http://"))) {
        int ind = str::IndexOf(s, StrL("/10."));
        if (ind >= 0) s = Str(s.s + ind + 1, len(s) - ind - 1);
    }
    s = JavaTrim(s);
    int ind = str::IndexOf(s, StrL("http://"));
    if (ind > 10) s = Str(s.s, ind);

    str::Builder b;
    for (int k = 0; k < len(s);) {
        int n = 0;
        if (!IsDoiMark(CpAt(s, k, &n))) b.Append(Str(s.s + k, n));
        k += n;
    }
    s = ToStrTemp(b);

    // ( [ U+27E8 at the start, ) ] U+27E9 at the end
    Str langle = StrL("\xE2\x9F\xA8");
    Str rangle = StrL("\xE2\x9F\xA9");
    if (str::StartsWith(s, StrL("(")) || str::StartsWith(s, StrL("["))) {
        s = Str(s.s + 1, len(s) - 1);
    } else if (str::StartsWith(s, langle)) {
        s = Str(s.s + len(langle), len(s) - len(langle));
    }
    if (str::EndsWith(s, StrL(")")) || str::EndsWith(s, StrL("]"))) {
        s = Str(s.s, len(s) - 1);
    } else if (str::EndsWith(s, rangle)) {
        s = Str(s.s, len(s) - len(rangle));
    }
    return s;
}

static bool HasDoi(Str s) {
    Vec<GrobidPos> found;
    FindAll(s, MatchDoiAt, found);
    return len(found) > 0;
}

// the year of a <date> field: the first 4-digit number starting with 1 or 2 ("2013a" -> 2013)
static Str YearOf(Str date) {
    for (int i = 0; i + 4 <= len(date); i++) {
        if (date.s[i] != '1' && date.s[i] != '2') continue;
        if (i > 0 && IsAsciiDigit(date.s[i - 1])) continue;
        if (CountDigits(date, i) != 4) continue;
        return Str(date.s + i, 4);
    }
    return {};
}

struct Biblio {
    Str title;
    Str authors;
    Str date;
    Str journal;
    Str bookTitle;
    Str doi;
};

// title, booktitle, journal: the longest cluster wins, the others go to the note
static void SetLongest(Str* field, Str content) {
    if (len(*field) == 0 || CharCount(*field) < CharCount(content)) *field = content;
}

// CitationParser.resultExtractionLayoutTokens for the fields we keep
static void ExtractFields(const StrVec& toks, Vec<Cluster>& clusters, Biblio& bib) {
    for (Cluster& c : clusters) {
        Str content = DehyphenTextTemp(toks, c.start, c.end);
        if (str::Eq(c.label, StrL("<title>"))) {
            SetLongest(&bib.title, content);
        } else if (str::Eq(c.label, StrL("<booktitle>"))) {
            SetLongest(&bib.bookTitle, content);
        } else if (str::Eq(c.label, StrL("<journal>"))) {
            SetLongest(&bib.journal, content);
        } else if (str::Eq(c.label, StrL("<author>"))) {
            bib.authors = len(bib.authors) == 0 ? content : str::JoinTemp(bib.authors, StrL(" ; "), content);
        } else if (str::Eq(c.label, StrL("<date>"))) {
            bib.date = len(bib.date) == 0 ? content : str::JoinTemp(bib.date, StrL(". "), content);
        } else if (str::Eq(c.label, StrL("<pubnum>"))) {
            // setPubnum + checkIdentifier: the whole pubnum becomes the DOI
            Str pubnum = NormalizeSpaceTemp(TokensTextTemp(toks, c.start, c.end));
            if (len(pubnum) == 0 || len(bib.doi) > 0) continue;
            if (HasDoi(pubnum) || HasDoi(str::ReplaceTemp(pubnum, StrL(" "), StrL("")))) {
                bib.doi = CleanDoiTemp(pubnum);
            }
        } else if (str::Eq(c.label, StrL("<web>"))) {
            // setWeb: the DOI found in the URL
            Str web = str::ReplaceTemp(NormalizeSpaceTemp(TokensTextTemp(toks, c.start, c.end)), StrL(" "), StrL(""));
            if (len(bib.doi) > 0) continue;
            Vec<GrobidPos> found;
            FindAll(web, MatchDoiAt, found);
            if (len(found) > 0) bib.doi = CleanDoiTemp(Str(web.s + found[0].start, found[0].end - found[0].start));
        }
    }
}

//--- API

TempStr GrobidCitationFeaturesTemp(Str raw) {
    StrVec toks;
    CitationTokens(raw, toks);
    StrVec rows;
    Vec<int> rowTok;
    CitationRows(toks, rows, rowTok);
    return JoinTemp(&rows, StrL("\n"));
}

// Fills title, authors, year, venue and doi of ref from one bibliography entry.
bool GrobidParseCitation(Str raw, GrobidRef* ref) {
    StrVec toks;
    CitationTokens(raw, toks);
    if (len(toks) == 0) return false;
    StrVec rows;
    Vec<int> rowTok;
    CitationRows(toks, rows, rowTok);
    Vec<Cluster> clusters;
    if (!TagRows(StrL("citation"), rows, rowTok, toks, clusters)) return false;

    Biblio bib;
    ExtractFields(toks, clusters, bib);

    // BiblioItem.cleanTitles: a trailing " y" is a footnote marker
    Str title = CleanFieldTemp(bib.title, StopWords::Keep);
    if (str::EndsWith(title, StrL(" y"))) title = Str(title.s, len(title) - 2);
    Str venue = CleanFieldTemp(bib.journal, StopWords::Remove);
    Str bookTitle = CleanFieldTemp(bib.bookTitle, StopWords::Keep);
    // BiblioItem.toTEI: a booktitle without title or journal is the title of a book (<monogr>)
    if (len(title) == 0 && len(venue) == 0) {
        title = bookTitle;
    } else if (len(venue) == 0) {
        venue = bookTitle;
    }

    Vec<Person> persons;
    ParseNames(bib.authors, persons);
    Str authors = FormatPersonsTemp(persons);
    // the name model found nobody: keep the author field as written
    if (len(authors) == 0) authors = NormalizeSpaceTemp(bib.authors);

    str::ReplaceWithCopy(&ref->title, title);
    str::ReplaceWithCopy(&ref->authors, authors);
    str::ReplaceWithCopy(&ref->year, YearOf(bib.date));
    str::ReplaceWithCopy(&ref->venue, venue);
    str::ReplaceWithCopy(&ref->doi, bib.doi);
    return true;
}

void GrobidRefFree(GrobidRef* ref) {
    if (!ref) return;
    str::Free(ref->label);
    str::Free(ref->raw);
    str::Free(ref->title);
    str::Free(ref->authors);
    str::Free(ref->year);
    str::Free(ref->venue);
    str::Free(ref->doi);
    delete ref;
}

//--- shared with the header parser

TempStr GrobidFormatPersonsTemp(Vec<GrobidPerson>& persons) {
    return FormatPersonsTemp(persons);
}

// flags[n]: token n is inside a location gazetteer match (Lexicon.tokenPositionsLocationNames)
void GrobidLocationFlags(const StrVec& toks, Vec<bool>& flags) {
    Vec<GrobidPos> pos;
    GazMatch(Gaz::Locations, toks, pos);
    VecClear(flags);
    for (int i = 0; i < len(toks); i++) VecAppend(flags, false);
    for (GrobidPos& p : pos) {
        for (int i = p.start; i <= p.end && i < len(toks); i++) flags[i] = true;
    }
}

// flags[n]: token n is inside a match of matchAt over the joined tokens (Matcher.find() loop
// + Utilities.convertStringOffsetToTokenOffset)
void GrobidPatternFlags(const StrVec& toks, int (*matchAt)(Str text, int i), Vec<bool>& flags) {
    TempStr text = JoinTemp((StrVec*)&toks, Str());
    Vec<GrobidPos> chars;
    FindAll(text, matchAt, chars);
    Vec<GrobidPos> pos;
    ToTokenOffsets(chars, toks, pos);
    VecClear(flags);
    for (int i = 0; i < len(toks); i++) VecAppend(flags, false);
    for (GrobidPos& p : pos) {
        for (int i = p.start; i <= p.end && i < len(toks); i++) {
            if (i >= 0) flags[i] = true;
        }
    }
}

int GrobidMatchUrlAt(Str text, int i) {
    return MatchUrlAt(text, i);
}

int GrobidMatchDoiAt(Str text, int i) {
    return MatchDoiAt(text, i);
}

TempStr GrobidNormalizeSpaceTemp(Str s) {
    return NormalizeSpaceTemp(s);
}

// TextUtilities.cleanField(s, false)
TempStr GrobidCleanFieldTemp(Str s) {
    return CleanFieldTemp(s, StopWords::Keep);
}

TempStr GrobidCleanDoiTemp(Str doi) {
    return CleanDoiTemp(doi);
}
