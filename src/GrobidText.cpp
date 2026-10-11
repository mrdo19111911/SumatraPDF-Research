/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/File.h"

#include "CrfModel.h"
#include "EmbeddedResources.h"
#include "SumatraConfig.h"
#include "GrobidText.h"

// Ports of GROBID (commit c71ebff) helpers: features/FeatureFactory.java,
// utilities/TextUtilities.java, utilities/UnicodeUtil.java, lexicon/Lexicon.java (English)
// and analyzers/GrobidDefaultAnalyzer.java. Java works on UTF-16 chars; we decode UTF-8
// to codepoints and give the Java answer for BMP codepoints.

//--- codepoints

// Decodes the codepoint at s[i]; invalid UTF-8 yields U+FFFD over one byte.
static int CpAt(Str s, int i, int* nBytes) {
    u8 c = (u8)s.s[i];
    int n = len(s) - i;
    *nBytes = 1;
    if (c < 0x80) return c;
    int need = 0;
    int cp = 0;
    if (c >= 0xC2 && c <= 0xDF) {
        need = 1;
        cp = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
        need = 2;
        cp = c & 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        need = 3;
        cp = c & 0x07;
    } else {
        return 0xFFFD;
    }
    if (need >= n) return 0xFFFD;
    for (int k = 1; k <= need; k++) {
        u8 cc = (u8)s.s[i + k];
        if ((cc & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (cc & 0x3F);
    }
    *nBytes = need + 1;
    return cp;
}

static void AppendCp(str::Builder& b, int cp) {
    char buf[8];
    int off = 0;
    str::Utf8Encode(buf, off, cp);
    b.Append(Str(buf, off));
}

// Character.isUpperCase / isLowerCase on a UTF-16 char: a surrogate is neither
static bool JIsUpper(int cp) {
    return cp <= 0xFFFF && IsCharUpperW((WCHAR)cp);
}

static bool JIsLower(int cp) {
    return cp <= 0xFFFF && IsCharLowerW((WCHAR)cp);
}

// first codepoint of each run of 10 decimal digits (Unicode Nd) in the BMP
static const int gNdStarts[] = {0x0030, 0x0660, 0x06F0, 0x07C0, 0x0966, 0x09E6, 0x0A66, 0x0AE6, 0x0B66, 0x0BE6,
                                0x0C66, 0x0CE6, 0x0D66, 0x0DE6, 0x0E50, 0x0ED0, 0x0F20, 0x1040, 0x1090, 0x17E0,
                                0x1810, 0x1946, 0x19D0, 0x1A80, 0x1A90, 0x1B50, 0x1BB0, 0x1C40, 0x1C50, 0xA620,
                                0xA8D0, 0xA900, 0xA9D0, 0xA9F0, 0xAA50, 0xABF0, 0xFF10};

// Character.isDigit: Nd only, so superscripts like U+00B2 are not digits
static bool JIsDigit(int cp) {
    for (int start : gNdStarts) {
        if (cp >= start && cp < start + 10) return true;
    }
    return false;
}

static bool IsAsciiDigit(char c) {
    return c >= '0' && c <= '9';
}

static bool IsAsciiAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool CpIn(int cp, const int* set, int n) {
    for (int i = 0; i < n; i++) {
        if (set[i] == cp) return true;
    }
    return false;
}

// TextUtilities.fullPunctuations
static const int gFullPunct[] = {0x28,   0xFF08, 0x5B,   0x20,   0x2022, 0x2A,   0x2C,   0x3A,   0x3B,  0x3F, 0x2E,
                                 0x21,   0x2F,   0x29,   0xFF09, 0x2D,   0x2212, 0x2013, 0x2010, 0xAB,  0xBB, 0x201E,
                                 0x22,   0x201C, 0x201D, 0x2018, 0x2019, 0x27,   0x60,   0x24,   0x23,  0x40, 0x5D,
                                 0x2666, 0x2665, 0x2663, 0x2660, 0xA0,   0x3002, 0x3001, 0xFF0C, 0x30FB};

static bool IsFullPunct(int cp) {
    return CpIn(cp, gFullPunct, dimofi(gFullPunct));
}

// TextUtilities.delimiters: LF, CR, tab, form feed, U+00A0, U+200C and fullPunctuations
static bool IsDelimiter(int cp) {
    static const int extra[] = {'\n', '\r', '\t', '\f', 0xA0, 0x200C};
    return CpIn(cp, extra, dimofi(extra)) || IsFullPunct(cp);
}

//--- UnicodeUtil.normaliseText

static const int gHorizontalWs[] = {0x09,   0x20,   0xA0,   0x1680, 0x180E, 0x2000, 0x2001,
                                    0x2002, 0x2003, 0x2004, 0x2005, 0x2006, 0x2007, 0x2008,
                                    0x2009, 0x200A, 0x2028, 0x2029, 0x202F, 0x205F, 0x3000};
static const int gNewLines[] = {0x0C, 0x0A, 0x0D, 0x0B, 0x85, 0x2029, 0x2028};
// \p{Pd} plus U+2212
static const int gDashes[] = {0x2D,   0x58A,  0x5BE,  0x1400, 0x1806, 0x2010, 0x2011, 0x2012,  0x2013,
                              0x2014, 0x2015, 0x2E17, 0x2E1A, 0x2E3A, 0x2E3B, 0x2E40, 0x2E5D,  0x301C,
                              0x3030, 0x30A0, 0xFE31, 0xFE32, 0xFE58, 0xFE63, 0xFF0D, 0x10EAD, 0x2212};
static const int gLowLines[] = {0x5F, 0x203F, 0x2040, 0x2054, 0xFE4D, 0xFE4E, 0xFE4F, 0xFF3F, 0xFE33, 0xFE34};
static const int gVerticalLines[] = {0x7C, 0x1C0, 0x5C0, 0x2223, 0x2758};
static const int gBullets[] = {0x2022, 0x2023, 0x25E6, 0x2043, 0x204C, 0x204D, 0x2219,
                               0x25D8, 0x29BE, 0x29BF, 0x23FA, 0x25CF, 0x26AB, 0x2B24};
static const int gOpenParens[] = {0x28, 0xFF08, 0x27EE, 0x2985, 0x2768, 0x276A, 0x27EC};
static const int gCloseParens[] = {0x29, 0xFF09, 0x27EF, 0x2986, 0x2769, 0x276B, 0x27ED};
constexpr int kBullet = 0x2022;
constexpr int kMiddleDot = 0xB7;

static bool IsCjk(int cp) {
    // Han, Hiragana, Katakana and Hangul scripts
    return (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x2E80 && cp <= 0x2FDF) || (cp >= 0x3005 && cp <= 0x3007) ||
           (cp >= 0x3021 && cp <= 0x3029) || (cp >= 0x302E && cp <= 0x302F) || (cp >= 0x3038 && cp <= 0x303B) ||
           (cp >= 0x3041 && cp <= 0x3096) || (cp >= 0x309D && cp <= 0x30FF && cp != 0x30FB && cp != 0x30A0) ||
           (cp >= 0x3131 && cp <= 0x318E) || (cp >= 0x31F0 && cp <= 0x31FF) || (cp >= 0x3200 && cp <= 0x327E) ||
           (cp >= 0x32D0 && cp <= 0x3357) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0xA960 && cp <= 0xA97F) || (cp >= 0xAC00 && cp <= 0xD7FF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFF66 && cp <= 0xFFDC) || (cp >= 0x20000 && cp <= 0x3FFFF);
}

// letter, number or mark, minus CJK scripts: a middle dot between two of these stays (Catalan "l.l")
static bool IsMiddleDotWordChar(int cp) {
    if (cp < 0 || cp > 0xFFFF || IsCjk(cp)) return false;
    WCHAR c = (WCHAR)cp;
    WORD t1 = 0, t3 = 0;
    GetStringTypeW(CT_CTYPE1, &c, 1, &t1);
    GetStringTypeW(CT_CTYPE3, &c, 1, &t3);
    return (t1 & (C1_ALPHA | C1_DIGIT)) || (t3 & (C3_NONSPACING | C3_DIACRITIC | C3_VOWELMARK));
}

static void DecodeCps(Str s, Vec<int>& cps) {
    for (int i = 0; i < len(s);) {
        int n = 0;
        VecAppend(cps, CpAt(s, i, &n));
        i += n;
    }
}

// UnicodeUtil.normaliseText, applied in its order
static void NormaliseCps(Vec<int>& cps) {
    // horizontal spaces -> ' ', then "\r\n" -> "\n", then new lines -> '\n'
    for (int& cp : cps) {
        if (CpIn(cp, gHorizontalWs, dimofi(gHorizontalWs))) cp = ' ';
    }
    for (int i = 0; i + 1 < len(cps); i++) {
        if (cps[i] == '\r' && cps[i + 1] == '\n') VecRemoveAt(cps, i);
    }
    for (int& cp : cps) {
        if (CpIn(cp, gNewLines, dimofi(gNewLines))) {
            cp = '\n';
        } else if (CpIn(cp, gDashes, dimofi(gDashes))) {
            cp = '-';
        } else if (CpIn(cp, gLowLines, dimofi(gLowLines))) {
            cp = '_';
        } else if (CpIn(cp, gVerticalLines, dimofi(gVerticalLines))) {
            cp = '|';
        } else if (CpIn(cp, gBullets, dimofi(gBullets))) {
            cp = kBullet;
        }
    }
    // a middle dot is a bullet unless a word char is on both sides
    int n = len(cps);
    Vec<int> dots;
    for (int i = 0; i < n; i++) {
        if (cps[i] != kMiddleDot) continue;
        bool inWord = i > 0 && i + 1 < n && IsMiddleDotWordChar(cps[i - 1]) && IsMiddleDotWordChar(cps[i + 1]);
        if (!inWord) VecAppend(dots, i);
    }
    for (int i : dots) cps[i] = kBullet;
    for (int& cp : cps) {
        if (CpIn(cp, gOpenParens, dimofi(gOpenParens))) {
            cp = '(';
        } else if (CpIn(cp, gCloseParens, dimofi(gCloseParens))) {
            cp = ')';
        }
    }
}

static TempStr GrobidNormaliseTemp(Str s) {
    Vec<int> cps;
    DecodeCps(s, cps);
    NormaliseCps(cps);
    str::Builder b;
    for (int cp : cps) AppendCp(b, cp);
    return ToStrTemp(b);
}

// GrobidDefaultAnalyzer.tokenize: normaliseText, then StringTokenizer(text, delimiters, true),
// i.e. runs of non-delimiters and each delimiter on its own. "Smith, J." -> Smith|,| |J|.
void GrobidTokenize(Str text, StrVec& out) {
    Vec<int> cps;
    DecodeCps(text, cps);
    NormaliseCps(cps);
    str::Builder tok;
    for (int cp : cps) {
        if (!IsDelimiter(cp)) {
            AppendCp(tok, cp);
            continue;
        }
        if (len(tok) > 0) out.Append(ToStr(tok));
        tok.Reset();
        AppendCp(tok, cp);
        out.Append(ToStr(tok));
        tok.Reset();
    }
    if (len(tok) > 0) out.Append(ToStr(tok));
}

//--- FeatureFactory

int GrobidLinearScaling(double pos, double total, int nbBins) {
    if (pos >= total) return nbBins;
    if (pos <= 0) return 0;
    double rel = pos / total;
    double rel2 = rel * nbBins;
    return (int)rel2;
}

// FeatureFactory.linearScaling(int, int, int) computes in float, which rounds differently
// from the double overload: 29/100*100 is 29 here but 28 there.
int GrobidLinearScaling(int pos, int total, int nbBins) {
    if (pos >= total) return nbBins;
    if (pos <= 0) return 0;
    float rel = (float)pos / (float)total;
    float rel2 = rel * (float)nbBins;
    return (int)rel2;
}

// test_first_capital
bool GrobidIsUpperFirst(Str s) {
    if (len(s) == 0) return false;
    int n = 0;
    return JIsUpper(CpAt(s, 0, &n));
}

// test_all_capital: no lowercase char ("2019" and "," count as all capital)
bool GrobidAllCapital(Str s) {
    if (len(s) == 0) return false;
    for (int i = 0; i < len(s);) {
        int n = 0;
        if (JIsLower(CpAt(s, i, &n))) return false;
        i += n;
    }
    return true;
}

// test_digit: any Character.isDigit char
bool GrobidHasDigit(Str s) {
    for (int i = 0; i < len(s);) {
        int n = 0;
        if (JIsDigit(CpAt(s, i, &n))) return true;
        i += n;
    }
    return false;
}

// Java's '$' also matches before one line terminator ending the input.
static Str StripFinalLineTerm(Str s) {
    if (str::EndsWith(s, StrL("\r\n"))) return Str(s.s, len(s) - 2);
    if (str::EndsWith(s, StrL("\n")) || str::EndsWith(s, StrL("\r"))) return Str(s.s, len(s) - 1);
    if (str::EndsWith(s, StrL("\xC2\x85"))) return Str(s.s, len(s) - 2);
    if (str::EndsWith(s, StrL("\xE2\x80\xA8")) || str::EndsWith(s, StrL("\xE2\x80\xA9"))) {
        return Str(s.s, len(s) - 3);
    }
    return s;
}

static bool AllAsciiDigits(Str s) {
    if (len(s) == 0) return false;
    for (int i = 0; i < len(s); i++) {
        if (!IsAsciiDigit(s.s[i])) return false;
    }
    return true;
}

// FeatureFactory.isDigit "^\d+$" with find() (Java's \d is ASCII), the ALLDIGIT feature
bool GrobidAllDigits(Str s) {
    return AllAsciiDigits(s) || AllAsciiDigits(StripFinalLineTerm(s));
}

// FeatureFactory.year "[1,2][0-9][0-9][0-9]" with find(): the class includes ',', so ",999" matches
bool GrobidIsYear(Str s) {
    for (int i = 0; i + 4 <= len(s); i++) {
        char c = s.s[i];
        if (c != '1' && c != ',' && c != '2') continue;
        if (IsAsciiDigit(s.s[i + 1]) && IsAsciiDigit(s.s[i + 2]) && IsAsciiDigit(s.s[i + 3])) return true;
    }
    return false;
}

// [a-zA-Z0-9_'^&amp;/+-]: the "&amp;" is literal, so ';' and '&' are local-part chars
static bool IsEmailLocalChar(char c) {
    return IsAsciiAlpha(c) || IsAsciiDigit(c) || str::ContainsChar(StrL("_'^&;/+-"), c);
}

// L+(\.L+)*
static bool IsDotSeparated(Str s, bool (*isChar)(char)) {
    if (len(s) == 0) return false;
    bool prevDot = true;
    for (int i = 0; i < len(s); i++) {
        if (s.s[i] == '.') {
            if (prevDot) return false;
            prevDot = true;
            continue;
        }
        if (!isChar(s.s[i])) return false;
        prevDot = false;
    }
    return !prevDot;
}

// 25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?
static bool IsIpOctet(Str s) {
    int n = len(s);
    if (n < 1 || n > 3 || !AllAsciiDigits(s)) return false;
    if (n < 3) return true;
    char a = s.s[0], b = s.s[1], c = s.s[2];
    return a == '0' || a == '1' || (a == '2' && b <= '4') || (a == '2' && b == '5' && c <= '5');
}

// (\[?IP\.){3}IP\]?
static bool IsEmailIpDomain(Str d) {
    StrVec parts;
    Split(&parts, d, StrL("."));
    if (len(parts) != 4) return false;
    for (int i = 0; i < 4; i++) {
        Str p = parts[i];
        if (i < 3 && len(p) > 0 && p.s[0] == '[') p = Str(p.s + 1, len(p) - 1);
        if (i == 3 && len(p) > 0 && p.s[len(p) - 1] == ']') p = Str(p.s, len(p) - 1);
        if (!IsIpOctet(p)) return false;
    }
    return true;
}

static bool IsDomainLabelChar(char c) {
    return IsAsciiAlpha(c) || IsAsciiDigit(c) || c == '-';
}

// ([a-zA-Z0-9-]+\.)+[a-zA-Z]{2,}\.?
static bool IsEmailNameDomain(Str d) {
    if (str::EndsWith(d, StrL("."))) d = Str(d.s, len(d) - 1);
    int dot = str::LastIndexOfChar(d, '.');
    if (dot < 0) return false;
    Str tld(d.s + dot + 1, len(d) - dot - 1);
    if (len(tld) < 2) return false;
    for (int i = 0; i < len(tld); i++) {
        if (!IsAsciiAlpha(tld.s[i])) return false;
    }
    return IsDotSeparated(Str(d.s, dot), IsDomainLabelChar);
}

static bool IsEmailExact(Str s) {
    int at = str::IndexOfChar(s, '@');
    if (at < 0 || !IsDotSeparated(Str(s.s, at), IsEmailLocalChar)) return false;
    Str d(s.s + at + 1, len(s) - at - 1);
    return IsEmailIpDomain(d) || IsEmailNameDomain(d);
}

// FeatureFactory.email (anchored ^...$) with find()
bool GrobidIsEmail(Str s) {
    Str stripped = StripFinalLineTerm(s);
    return IsEmailExact(s) || (len(stripped) != len(s) && IsEmailExact(stripped));
}

// FeatureFactory.http "http(s)?" with find()
bool GrobidIsHttp(Str s) {
    return str::Contains(s, StrL("http"));
}

static bool AllPunctChars(Str s) {
    if (len(s) == 0) return false;
    for (int i = 0; i < len(s); i++) {
        if (!str::ContainsChar(StrL(",:;?."), s.s[i])) return false;
    }
    return true;
}

// FeatureFactory.isPunct "^[\,\:;\?\.]+$" with find()
bool GrobidIsPunct(Str s) {
    return AllPunctChars(s) || AllPunctChars(StripFinalLineTerm(s));
}

// String.toLowerCase()
TempStr GrobidLowerTemp(Str s) {
    bool ascii = true;
    for (int i = 0; i < len(s) && ascii; i++) ascii = (u8)s.s[i] < 0x80;
    if (ascii) {
        TempStr res = str::DupTemp(s);
        str::ToLowerInPlace(res);
        return res;
    }
    WStr w = ToWStrTemp(s);
    CharLowerBuffW(w.s, (DWORD)len(w));
    return ToUtf8Temp(w);
}

// test_month
bool GrobidIsMonth(Str s) {
    static const char* months[] = {"january",   "february", "march",    "april",    "may", "june", "july", "august",
                                   "september", "october",  "november", "december", "jan", "feb",  "mar",  "apr",
                                   "jun",       "jul",      "aug",      "sep",      "oct", "nov",  "dec"};
    TempStr lower = GrobidLowerTemp(s);
    for (const char* m : months) {
        if (str::Eq(lower, Str(m))) return true;
    }
    return false;
}

// TextUtilities.filterLine
bool GrobidFilterLine(Str s) {
    return len(s) == 0 || str::Contains(s, StrL("@IMAGE")) || str::Contains(s, StrL("@PAGE"));
}

// TextUtilities.prefix, counting chars, not bytes
TempStr GrobidPrefixTemp(Str s, int n) {
    int i = 0;
    for (int k = 0; k < n && i < len(s); k++) {
        int nb = 0;
        CpAt(s, i, &nb);
        i += nb;
    }
    return str::DupTemp(Str(s.s, i));
}

// TextUtilities.suffix
TempStr GrobidSuffixTemp(Str s, int n) {
    int nChars = Utf8CodepointCount(s);
    if (nChars <= n) return str::DupTemp(s);
    int i = 0;
    for (int k = 0; k < nChars - n; k++) {
        int nb = 0;
        CpAt(s, i, &nb);
        i += nb;
    }
    return str::DupTemp(Str(s.s + i, len(s) - i));
}

// TextUtilities.punctuationProfile: the punctuation chars of a line, spaces skipped
TempStr GrobidPunctProfileTemp(Str line) {
    str::Builder b;
    for (int i = 0; i < len(line);) {
        int n = 0;
        int cp = CpAt(line, i, &n);
        if (cp != ' ' && IsFullPunct(cp)) b.Append(Str(line.s + i, n));
        i += n;
    }
    return ToStrTemp(b);
}

// FeatureFactory.getPattern: ASCII letters only, lowercased
TempStr GrobidPatternTemp(Str line) {
    str::Builder b;
    for (int i = 0; i < len(line); i++) {
        char c = line.s[i];
        if (!IsAsciiAlpha(c)) continue;
        b.AppendChar(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
    }
    return ToStrTemp(b);
}

//--- data files

// research/grobid/<name> from the embedded archive; debug builds fall back to the repository.
Str GrobidDataTemp(Str name) {
    int size = 0;
    u8* data = GetEmbeddedFileData(fmt("research\\grobid\\%s", name), &size);
    if (data) {
        TempStr res = str::DupTemp(Str((char*)data, size));
        free(data);
        return res;
    }
    if (!gIsDebugBuild) return {};
    // debug builds live two levels below the repository (out/dbg64)
    TempStr repo = path::GetDirTemp(path::GetDirTemp(GetSelfExeDirTemp()));
    TempStr path = path::JoinTemp(repo, StrL("research\\grobid"), name);
    return file::ReadFileWithArena(path, GetTempArena());
}

// Read-only string set over lines of a data file that stays loaded.
struct WordSet {
    Str data;
    Vec<Str> words;
    Vec<u32> hashes;
    Vec<int> table;
};

static void WordSetInsert(WordSet& set, Str w) {
    VecAppend(set.words, w);
    VecAppend(set.hashes, MurmurHash2(w));
}

static void WordSetBuild(WordSet& set) {
    int n = RoundToPowerOf2(len(set.words) * 2 + 16);
    VecResize(set.table, n);
    for (int i = 0; i < n; i++) set.table[i] = -1;
    for (int idx = 0; idx < len(set.words); idx++) {
        int i = (int)(set.hashes[idx] & (u32)(n - 1));
        while (set.table[i] >= 0) i = (i + 1) & (n - 1);
        set.table[i] = idx;
    }
}

static bool WordSetHas(WordSet& set, Str w) {
    int n = len(set.table);
    if (n == 0 || len(w) == 0) return false;
    u32 h = MurmurHash2(w);
    for (int i = (int)(h & (u32)(n - 1));; i = (i + 1) & (n - 1)) {
        int idx = set.table[i];
        if (idx < 0) return false;
        if (set.hashes[idx] == h && str::Eq(set.words[idx], w)) return true;
    }
}

static Mutex gLexiconMutex;
static WordSet* gDictEn = nullptr;
static WordSet* gFirstNames = nullptr;
static WordSet* gLastNames = nullptr;

// one entry per non-blank line (Lexicon.addDictionary; names are lowercased by gen-grobid-data.ts)
static WordSet* LoadWordSet(Str file) {
    auto set = new WordSet();
    set->data = str::Dup(GrobidDataTemp(file));
    if (len(set->data) == 0) logf("GROBID: missing research\\grobid\\%s\n", file);
    Str rest = set->data;
    Str line;
    while (str::NextLine(rest, line, rest)) {
        str::TrimSuffix(line, StrL("\r"));
        if (str::IsEmptyOrWhiteSpace(line)) continue;
        WordSetInsert(*set, line);
    }
    WordSetBuild(*set);
    return set;
}

static WordSet* Lexicon(WordSet** set, Str file) {
    AutoUnlockMutex lock(&gLexiconMutex);
    if (!*set) *set = LoadWordSet(file);
    return *set;
}

// Lexicon.inDictionary(s, EN): one trailing . , : ; dropped; "a-b" / "a b" need both halves
static bool InDictionaryEn(Str s) {
    WordSet* dict = Lexicon(&gDictEn, StrL("words-en.txt"));
    if (str::EndsWith(s, StrL(".")) || str::EndsWith(s, StrL(",")) || str::EndsWith(s, StrL(":")) ||
        str::EndsWith(s, StrL(";"))) {
        s = Str(s.s, len(s) - 1);
    }
    int i1 = str::IndexOfChar(s, '-');
    int i2 = str::IndexOfChar(s, ' ');
    int i = i1 >= 0 ? i1 : i2;
    if (i < 0) return WordSetHas(*dict, s);
    return WordSetHas(*dict, Str(s.s, i)) && WordSetHas(*dict, Str(s.s + i + 1, len(s) - i - 1));
}

// String.trim(): chars <= ' ' at both ends
static Str JavaTrim(Str s) {
    int a = 0, b = len(s);
    while (a < b && (u8)s.s[a] <= ' ') a++;
    while (b > a && (u8)s.s[b - 1] <= ' ') b--;
    return Str(s.s + a, b - a);
}

// FeatureFactory.test_common
bool GrobidInDictionary(Str s) {
    if (len(s) == 0) return false;
    return InDictionaryEn(GrobidLowerTemp(JavaTrim(s)));
}

// FeatureFactory.test_first_names
bool GrobidIsFirstName(Str s) {
    return WordSetHas(*Lexicon(&gFirstNames, StrL("names-first.txt")), GrobidLowerTemp(s));
}

// FeatureFactory.test_last_names
bool GrobidIsLastName(Str s) {
    return WordSetHas(*Lexicon(&gLastNames, StrL("names-last.txt")), GrobidLowerTemp(s));
}

//--- models

struct CachedModel {
    CachedModel* next;
    Str name;
    CrfModel* model;
};

static Mutex gModelsMutex;
static CachedModel* gModels = nullptr;

// research/grobid/<name>.wapiti, loaded once and never freed. nullptr if missing or invalid.
CrfModel* GrobidModel(Str name) {
    AutoUnlockMutex lock(&gModelsMutex);
    for (CachedModel* c = gModels; c; c = c->next) {
        if (str::Eq(c->name, name)) return c->model;
    }
    auto c = new CachedModel();
    c->name = str::Dup(name);
    c->model = CrfModelLoad(GrobidDataTemp(fmt("%s.wapiti", name)));
    if (!c->model) logf("GROBID: cannot load model '%s'\n", name);
    ListInsertFront(&gModels, c);
    return c->model;
}

//--- test hooks

// grobid-crf: model is a name in research/grobid or a path to a .wapiti file; the features
// file has one row per token and a blank line between sequences. Returns one label per row,
// a blank line between sequences, "#fail" for a sequence the model rejects.
TempStr GrobidTestCrfTemp(Str model, Str featuresPath) {
    bool isPath = str::EndsWithI(model, StrL(".wapiti"));
    CrfModel* m = nullptr;
    if (isPath) {
        Str text = file::ReadFile(model);
        m = CrfModelLoad(text);
        str::Free(text);
    } else {
        m = GrobidModel(model);
    }
    if (!m) return fmt("cannot load model %s", model);
    Str features = file::ReadFile(featuresPath);
    if (!features.s) {
        if (isPath) CrfModelFree(m);
        return fmt("cannot read %s", featuresPath);
    }

    str::Builder out;
    StrVec rows;
    Vec<int> labels;
    auto flush = [&]() {
        if (len(rows) == 0) return;
        if (len(out) > 0) out.Append(StrL("\n\n"));
        if (!CrfTag(m, rows, labels)) {
            out.Append(StrL("#fail"));
        } else {
            for (int i = 0; i < len(labels); i++) {
                if (i > 0) out.AppendChar('\n');
                out.Append(CrfLabelName(m, labels[i]));
            }
        }
        rows.Reset();
    };
    Str rest = features;
    Str line;
    while (str::NextLine(rest, line, rest)) {
        str::TrimSuffix(line, StrL("\r"));
        if (str::IsEmptyOrWhiteSpace(line)) {
            flush();
            continue;
        }
        rows.Append(line);
    }
    flush();
    str::Free(features);
    if (isPath) CrfModelFree(m);
    return ToStrTemp(out);
}

static Str BoolStr(bool b) {
    return b ? StrL("true") : StrL("false");
}

// grobid-text: runs one helper on arg. Prefix/Suffix take "n,s"; LinearScaling and
// LinearScalingInt take "pos,total,bins"; Tokenize joins tokens with '|'.
TempStr GrobidTestTextTemp(Str fn, Str arg) {
    struct BoolFn {
        const char* name;
        bool (*fn)(Str);
    };
    static const BoolFn boolFns[] = {
        {"IsUpperFirst", GrobidIsUpperFirst},
        {"AllCapital", GrobidAllCapital},
        {"HasDigit", GrobidHasDigit},
        {"AllDigits", GrobidAllDigits},
        {"IsYear", GrobidIsYear},
        {"IsEmail", GrobidIsEmail},
        {"IsHttp", GrobidIsHttp},
        {"IsPunct", GrobidIsPunct},
        {"IsMonth", GrobidIsMonth},
        {"InDictionary", GrobidInDictionary},
        {"IsFirstName", GrobidIsFirstName},
        {"IsLastName", GrobidIsLastName},
        {"FilterLine", GrobidFilterLine},
    };
    for (const BoolFn& f : boolFns) {
        if (str::Eq(fn, Str(f.name))) return str::DupTemp(BoolStr(f.fn(arg)));
    }
    if (str::Eq(fn, StrL("Lower"))) return GrobidLowerTemp(arg);
    if (str::Eq(fn, StrL("Pattern"))) return GrobidPatternTemp(arg);
    if (str::Eq(fn, StrL("PunctProfile"))) return GrobidPunctProfileTemp(arg);
    if (str::Eq(fn, StrL("Normalise"))) return GrobidNormaliseTemp(arg);
    if (str::Eq(fn, StrL("Data"))) return fmt("%d", len(GrobidDataTemp(arg)));
    if (str::Eq(fn, StrL("Tokenize"))) {
        StrVec toks;
        GrobidTokenize(arg, toks);
        return JoinTemp(&toks, StrL("|"));
    }
    if (str::Eq(fn, StrL("Prefix")) || str::Eq(fn, StrL("Suffix"))) {
        Str num, s;
        if (!str::CutChar(arg, ',', &num, &s)) return str::DupTemp(StrL("bad arg"));
        int n = ParseInt(num);
        return str::Eq(fn, StrL("Prefix")) ? GrobidPrefixTemp(s, n) : GrobidSuffixTemp(s, n);
    }
    if (str::Eq(fn, StrL("LinearScaling")) || str::Eq(fn, StrL("LinearScalingInt"))) {
        StrVec parts;
        if (Split(&parts, arg, StrL(",")) != 3) return str::DupTemp(StrL("bad arg"));
        int bins = ParseInt(parts[2]);
        if (str::Eq(fn, StrL("LinearScalingInt"))) {
            return fmt("%d", GrobidLinearScaling(ParseInt(parts[0]), ParseInt(parts[1]), bins));
        }
        double pos = atof(CStrTemp(parts[0]));
        double total = atof(CStrTemp(parts[1]));
        return fmt("%d", GrobidLinearScaling(pos, total, bins));
    }
    return fmt("unknown grobid-text helper %s", fn);
}
