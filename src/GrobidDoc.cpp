/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"

#include <wctype.h>

#include "base/GuessFileType.h"
#include "gui/UIModels.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "PageLayout.h"
#include "GrobidText.h"
#include "GrobidDoc.h"

// mupdf page layout -> GROBID Document, as pdfalto's ALTO -> PDFALTOSaxHandler:
//
//   PageLayoutBlock -> TextBlock   (Block: startToken .. endToken, then "\n")
//   PageLayoutLine  -> TextLine    ("\n" at its end)
//   PageLayoutWord  -> String      (GrobidTokenize sub-tokens, then " " unless "-")
//   bitmaps, figure-sized vector groups -> Illustration

//--- Java text helpers (UnicodeUtil.normaliseText, TextUtilities.clean)

static int NextCodepoint(Str s, int& i) {
    u8 c = (u8)s.s[i++];
    if (c < 0x80) {
        return c;
    }
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    int cp = c & (0x3F >> n);
    for (int k = 0; k < n && i < len(s); k++) {
        cp = (cp << 6) | ((u8)s.s[i++] & 0x3F);
    }
    return cp;
}

static void AppendCodepoint(str::Builder& b, int cp) {
    char buf[4];
    int n = 0;
    if (cp < 0x80) {
        buf[n++] = (char)cp;
    } else if (cp < 0x800) {
        buf[n++] = (char)(0xC0 | (cp >> 6));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        buf[n++] = (char)(0xE0 | (cp >> 12));
        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        buf[n++] = (char)(0xF0 | (cp >> 18));
        buf[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    }
    b.Append(Str(buf, n));
}

// Java String.length(): UTF-16 code units
static int Utf16Len(Str s) {
    int n = 0;
    for (int i = 0; i < len(s);) {
        n += NextCodepoint(s, i) >= 0x10000 ? 2 : 1;
    }
    return n;
}

// Java String.trim(): drops chars <= U+0020 at both ends
static Str JavaTrim(Str s) {
    int start = 0;
    int end = len(s);
    while (start < end && (u8)s.s[start] <= ' ') {
        start++;
    }
    while (end > start && (u8)s.s[end - 1] <= ' ') {
        end--;
    }
    return Str(s.s + start, end - start);
}

static bool IsMyWhitespace(int c) {
    return c == 0x09 || c == 0x20 || c == 0xA0 || c == 0x1680 || c == 0x180E || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

static bool IsNewLineChar(int c) {
    return c == 0x0C || c == 0x0A || c == 0x0D || c == 0x0B || c == 0x85;
}

// \p{Pd} plus U+2212 MINUS SIGN
static bool IsDash(int c) {
    switch (c) {
        case 0x2D:
        case 0x58A:
        case 0x5BE:
        case 0x1400:
        case 0x1806:
        case 0x2E17:
        case 0x2E1A:
        case 0x2E3A:
        case 0x2E3B:
        case 0x2E40:
        case 0x2E5D:
        case 0x301C:
        case 0x3030:
        case 0x30A0:
        case 0xFE31:
        case 0xFE32:
        case 0xFE58:
        case 0xFE63:
        case 0xFF0D:
        case 0x10EAD:
        case 0x2212:
            return true;
    }
    return c >= 0x2010 && c <= 0x2015;
}

static bool IsLowLine(int c) {
    return c == 0x5F || c == 0x203F || c == 0x2040 || c == 0x2054 || (c >= 0xFE4D && c <= 0xFE4F) || c == 0xFF3F ||
           c == 0xFE33 || c == 0xFE34;
}

static bool IsVerticalLine(int c) {
    return c == 0x7C || c == 0x1C0 || c == 0x5C0 || c == 0x2223 || c == 0x2758;
}

static bool IsBulletChar(int c) {
    switch (c) {
        case 0x2022:
        case 0x2023:
        case 0x25E6:
        case 0x2043:
        case 0x204C:
        case 0x204D:
        case 0x2219:
        case 0x25D8:
        case 0x29BE:
        case 0x29BF:
        case 0x23FA:
        case 0x25CF:
        case 0x26AB:
        case 0x2B24:
            return true;
    }
    return false;
}

static bool IsOpenParen(int c) {
    return c == 0x28 || c == 0xFF08 || c == 0x27EE || c == 0x2985 || c == 0x2768 || c == 0x276A || c == 0x27EC;
}

static bool IsCloseParen(int c) {
    return c == 0x29 || c == 0xFF09 || c == 0x27EF || c == 0x2986 || c == 0x2769 || c == 0x276B || c == 0x27ED;
}

// MIDDLE_DOT_WORD_CHAR: letter, digit or mark, except CJK
static bool IsMiddleDotWordChar(int c) {
    if (c < 0) {
        return false;
    }
    bool cjk = (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0x1100 && c <= 0x11FF) ||
               (c >= 0xF900 && c <= 0xFAFF) || (c >= 0x20000 && c <= 0x3FFFF);
    if (cjk) {
        return false;
    }
    if (c >= 0x300 && c <= 0x36F) {
        return true;
    }
    if (c > 0xFFFF) {
        return true;
    }
    return iswalpha((wint_t)c) || iswdigit((wint_t)c);
}

static void Codepoints(Str s, Vec<int>& cps) {
    for (int i = 0; i < len(s);) {
        VecAppend(cps, NextCodepoint(s, i));
    }
}

// UnicodeUtil.normaliseText
static TempStr NormaliseTextTemp(Str s) {
    Vec<int> cps;
    Codepoints(s, cps);
    str::Builder out;
    int n = len(cps);
    for (int i = 0; i < n; i++) {
        int c = cps[i];
        if (IsMyWhitespace(c)) {
            c = ' ';
        } else if (c == '\r' && i + 1 < n && cps[i + 1] == '\n') {
            continue;
        } else if (IsNewLineChar(c)) {
            c = '\n';
        } else if (IsDash(c)) {
            c = '-';
        } else if (IsLowLine(c)) {
            c = '_';
        } else if (IsVerticalLine(c)) {
            c = '|';
        } else if (IsBulletChar(c)) {
            c = 0x2022;
        } else if (c == 0xB7) {
            bool inWord = i > 0 && IsMiddleDotWordChar(cps[i - 1]) && i + 1 < n && IsMiddleDotWordChar(cps[i + 1]);
            if (!inWord) {
                c = 0x2022;
            }
        } else if (IsOpenParen(c)) {
            c = '(';
        } else if (IsCloseParen(c)) {
            c = ')';
        }
        AppendCodepoint(out, c);
    }
    return ToStrTemp(out);
}

// TextUtilities.clean: ligatures, quotes, bullets, author markers
static TempStr CleanTemp(Str s) {
    str::Builder out;
    for (int i = 0; i < len(s);) {
        int c = NextCodepoint(s, i);
        switch (c) {
            case 0xFB00:
                out.Append(StrL("ff"));
                continue;
            case 0xFB01:
                out.Append(StrL("fi"));
                continue;
            case 0xFB02:
                out.Append(StrL("fl"));
                continue;
            case 0xFB03:
                out.Append(StrL("ffi"));
                continue;
            case 0xFB04:
                out.Append(StrL("ffl"));
                continue;
            case 0xFB06:
                out.Append(StrL("st"));
                continue;
            case 0xFB05:
                out.Append(StrL("ft"));
                continue;
            case 0x201C:
            case 0x201D:
            case 0x201E:
            case 0x201F:
                out.Append(StrL("\""));
                continue;
            case 0x2019:
            case 0x2018:
                out.Append(StrL("'"));
                continue;
            case 0x2022:
            case 0x2023:
            case 0x2043:
            case 0x204C:
            case 0x204D:
            case 0x2219:
            case 0x25C9:
            case 0x25D8:
            case 0x25E6:
            case 0x2619:
            case 0x2765:
            case 0x2767:
            case 0x29BE:
            case 0x29BF:
                AppendCodepoint(out, 0x2022);
                continue;
            case 0x2217:
                out.Append(StrL(" * "));
                continue;
            case 0x2020:
            case 0x2021:
            case 0xA7:
            case 0xB6:
            case 0x204B:
            case 0x1C2:
                out.Append(StrL(" "));
                AppendCodepoint(out, c);
                continue;
        }
        AppendCodepoint(out, c);
    }
    return ToStrTemp(out);
}

//--- pdfalto word styles

// TextRawWord: bold/italic from the subset-less lowercased font name, else FontDescriptor flags
static void WordStyle(PageLayoutWord& w, bool& bold, bool& italic) {
    TempStr name = str::DupTemp(w.font);
    str::ToLowerInPlace(name);
    bool hasSubsetTag = len(name) > 7 && name.s[6] == '+';
    for (int i = 0; hasSubsetTag && i < 6; i++) {
        hasSubsetTag = isalpha((u8)name.s[i]) != 0;
    }
    if (hasSubsetTag) {
        name = Str(name.s + 7, len(name) - 7);
    }
    bold = str::Contains(name, StrL("bold")) || str::Contains(name, StrL("heavy")) ||
           str::Contains(name, StrL("black")) || str::Contains(name, StrL("_bd"));
    italic =
        str::Contains(name, StrL("italic")) || str::Contains(name, StrL("oblique")) || str::Contains(name, StrL("_it"));
    if (w.pdfFontFlags >= 0) {
        bold = bold || (w.pdfFontFlags & kPdfFontFlagForceBold) != 0;
        italic = italic || (w.pdfFontFlags & kPdfFontFlagItalic) != 0;
    } else {
        bold = bold || w.bold;
        italic = italic || w.italic;
    }
}

enum class ScriptPos {
    None,
    Super,
    Sub,
};

// pdfalto's per-line superscript/subscript test: smaller than the line's largest
// font so far and off the running baseline
static void LineScripts(PageTextLayout& page, PageLayoutLine& line, Vec<ScriptPos>& out) {
    VecReset(out);
    double lineFontSize = 0;
    double base = 0;
    double yMin = 0;
    double yMax = 0;
    for (int i = 0; i < line.nWords; i++) {
        PageLayoutWord& w = page.words[line.firstWord + i];
        if (w.fontSize > lineFontSize) {
            lineFontSize = w.fontSize;
        }
        ScriptPos pos = ScriptPos::None;
        double wMax = w.y + w.h;
        if (base != 0 && i > 0 && w.baseline < base && wMax > yMin && w.fontSize < lineFontSize) {
            pos = ScriptPos::Super;
        } else if (i > 0 && w.baseline > base && w.y < yMax && w.fontSize < lineFontSize) {
            pos = ScriptPos::Sub;
        } else {
            base = w.baseline;
            yMin = w.y;
            yMax = wMax;
        }
        VecAppend(out, pos);
    }
}

//--- Document building

struct DocBuilder {
    GrobidDoc* doc = nullptr;
    int page = 0;
    // PDFALTOSaxHandler's currentX/Y/Width/Height: keep the previous value for a 0
    double curX = 0;
    double curY = 0;
    double curW = 0;
    double curH = 0;
    Str lastFontSrc;
    Str lastFontLower;
};

static void AddSeparator(DocBuilder& b, Str text) {
    GrobidToken t;
    t.text = text;
    t.page = b.page;
    VecAppend(b.doc->tokens, t);
}

static void UpdateCurrent(double& cur, double v) {
    if (v != cur && v != 0.0) {
        cur = fabs(v);
    }
}

static Str FontLower(DocBuilder& b, Str font) {
    if (len(font) == 0) {
        return StrL("default");
    }
    if (font.s != b.lastFontSrc.s || len(font) != len(b.lastFontSrc)) {
        b.lastFontSrc = font;
        b.lastFontLower = str::Dup(b.doc->arena, GrobidLowerTemp(font));
    }
    return b.lastFontLower;
}

// one ALTO String: sub-tokens with widths split by length, then " "
static void AddWord(DocBuilder& b, PageLayoutWord& w, ScriptPos script, GrobidBlock& block) {
    UpdateCurrent(b.curX, w.x);
    UpdateCurrent(b.curY, w.y);
    UpdateCurrent(b.curW, w.w);
    UpdateCurrent(b.curH, w.h);

    TempStr tok0 = CleanTemp(JavaTrim(NormaliseTextTemp(JavaTrim(w.text))));
    if (len(tok0) > 0) {
        StrVec subs;
        GrobidTokenize(tok0, subs);
        double total = 0;
        for (int i = 0; i < len(subs); i++) {
            total += Utf16Len(subs.At(i));
        }
        bool bold, italic;
        WordStyle(w, bold, italic);
        Str font = FontLower(b, w.font);
        double prevSubWidth = 0;
        for (int i = 0; i < len(subs); i++) {
            Str sub = subs.At(i);
            double subW = b.curW * (Utf16Len(sub) / total);
            double subX = b.curX + prevSubWidth;
            prevSubWidth += subW;
            if (len(sub) == 0) {
                continue;
            }
            GrobidToken t;
            t.text = str::Dup(b.doc->arena, sub);
            t.font = font;
            t.fontSize = w.fontSize;
            t.bold = bold;
            t.italic = italic;
            t.superscript = script == ScriptPos::Super;
            t.x = (float)subX;
            t.y = (float)b.curY;
            t.w = (float)subW;
            t.h = (float)b.curH;
            t.page = b.page;
            VecAppend(b.doc->tokens, t);
        }
        int n = len(b.doc->tokens);
        if (n > 0 && !str::EndsWith(b.doc->tokens[n - 1].text, StrL("-"))) {
            AddSeparator(b, StrL(" "));
        }
    }
    block.endToken = len(b.doc->tokens);
}

// BoundingBoxCalculator.calculateOneBox over tokens with a width
static void BlockBox(GrobidDoc* doc, GrobidBlock& block) {
    bool any = false;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (int i = block.firstToken; i < block.firstToken + block.nTokens; i++) {
        GrobidToken& t = doc->tokens[i];
        if (t.page == -1 || t.w <= 0) {
            continue;
        }
        if (!any) {
            x0 = t.x;
            y0 = t.y;
            x1 = t.x + t.w;
            y1 = t.y + t.h;
            any = true;
            continue;
        }
        x0 = fmin(x0, (double)t.x);
        y0 = fmin(y0, (double)t.y);
        x1 = fmax(x1, (double)(t.x + t.w));
        y1 = fmax(y1, (double)(t.y + t.h));
    }
    if (!any) {
        return;
    }
    block.x = (float)x0;
    block.y = (float)y0;
    block.w = (float)(x1 - x0);
    block.h = (float)(y1 - y0);
}

static void AddTextBlock(DocBuilder& b, PageTextLayout& page, PageLayoutBlock& lb, GrobidPage& gp) {
    GrobidDoc* doc = b.doc;
    GrobidBlock block;
    block.startToken = len(doc->tokens);
    block.firstToken = block.startToken;
    block.page = b.page;

    Vec<ScriptPos> scripts;
    for (int l = lb.firstLine; l < lb.firstLine + lb.nLines; l++) {
        PageLayoutLine& line = page.lines[l];
        LineScripts(page, line, scripts);
        for (int i = 0; i < line.nWords; i++) {
            AddWord(b, page.words[line.firstWord + i], scripts[i], block);
        }
        AddSeparator(b, StrL("\n"));
    }
    AddSeparator(b, StrL("\n"));
    block.nTokens = len(doc->tokens) - block.firstToken;

    // addBlock: kept unless no String moved endToken; Block.getEndToken() for an unset end
    if (block.startToken == block.endToken) {
        return;
    }
    if (block.endToken == -1) {
        block.endToken = block.startToken + block.nTokens;
    }
    BlockBox(doc, block);
    VecAppend(doc->blocks, block);
    gp.nBlocks++;
}

// pdfalto clampIllustrationBox: keep the position non-negative
static void AddImage(GrobidDoc* doc, int page, bool vector, PageLayoutImage& im) {
    GrobidImage g;
    g.page = page;
    g.vector = vector;
    g.x = im.x;
    g.y = im.y;
    g.w = im.w;
    g.h = im.h;
    if (g.x < 0) {
        g.w += g.x;
        g.x = 0;
    }
    if (g.y < 0) {
        g.h += g.y;
        g.y = 0;
    }
    g.w = fmaxf(g.w, 0.f);
    g.h = fmaxf(g.h, 0.f);
    VecAppend(doc->images, g);
}

// thinner paths are rules, table lines or underlines
constexpr float kMinVectorSide = 2;
// larger fills are page backgrounds
constexpr float kMaxVectorPageFraction = 0.8f;
// paths closer than this belong to one graphic
constexpr float kVectorJoinGap = 10;
// smaller groups are decorations, not figures
constexpr float kMinGraphicSide = 20;

struct VecBox {
    float x0, y0, x1, y1;
};

static bool BoxesTouch(VecBox& a, VecBox& b) {
    return a.x0 <= b.x1 + kVectorJoinGap && b.x0 <= a.x1 + kVectorJoinGap && a.y0 <= b.y1 + kVectorJoinGap &&
           b.y0 <= a.y1 + kVectorJoinGap;
}

static void JoinBox(VecBox& a, VecBox& b) {
    a.x0 = fminf(a.x0, b.x0);
    a.y0 = fminf(a.y0, b.y0);
    a.x1 = fmaxf(a.x1, b.x1);
    a.y1 = fmaxf(a.y1, b.y1);
}

// Figure-sized groups of vector paths, like GROBID's VectorGraphicBoxCalculator boxes.
// pdfalto's single per-page union box would also cover rules and backgrounds and
// flag most blocks as next to a graphic.
static void AddVectorGraphics(GrobidDoc* doc, int page, PageTextLayout& layout) {
    float pageArea = layout.width * layout.height;
    Vec<VecBox> groups;
    for (PageLayoutVector& v : layout.vectors) {
        if (v.decoration || v.w < kMinVectorSide || v.h < kMinVectorSide) {
            continue;
        }
        if (v.w * v.h > kMaxVectorPageFraction * pageArea) {
            continue;
        }
        VecBox box{v.x, v.y, v.x + v.w, v.y + v.h};
        bool joined = false;
        for (VecBox& g : groups) {
            if (BoxesTouch(g, box)) {
                JoinBox(g, box);
                joined = true;
                break;
            }
        }
        if (!joined) {
            VecAppend(groups, box);
        }
    }

    // a grown group can now touch others
    for (bool changed = true; changed;) {
        changed = false;
        for (int i = 0; i < len(groups); i++) {
            for (int j = len(groups) - 1; j > i; j--) {
                if (!BoxesTouch(groups[i], groups[j])) {
                    continue;
                }
                JoinBox(groups[i], groups[j]);
                VecRemoveAt(groups, j);
                changed = true;
            }
        }
    }

    for (VecBox& g : groups) {
        PageLayoutImage im{g.x0, g.y0, g.x1 - g.x0, g.y1 - g.y0};
        if (im.w < kMinGraphicSide || im.h < kMinGraphicSide) {
            continue;
        }
        AddImage(doc, page, true, im);
    }
}

static void AddPage(DocBuilder& b, PageTextLayout& page) {
    GrobidPage gp;
    gp.firstBlock = len(b.doc->blocks);
    gp.width = page.width;
    gp.height = page.height;
    for (PageLayoutBlock& lb : page.blocks) {
        AddTextBlock(b, page, lb, gp);
    }
    for (PageLayoutImage& im : page.images) {
        AddImage(b.doc, b.page, false, im);
    }
    AddVectorGraphics(b.doc, b.page, page);
    VecAppend(b.doc->pages, gp);
}

struct CoordCounter {
    Vec<int> values;
    void Add(int v) { VecAppend(values, v); }
    bool IsEmpty() const { return len(values) == 0; }
};

// Document.getCoordItem: with the frequency cut commented out it is the min / max key
static int CoordItem(CoordCounter& c, bool getMin) {
    int res = c.values[0];
    for (int v : c.values) {
        if (getMin ? v < res : v > res) {
            res = v;
        }
    }
    return res;
}

static void SetMainArea(GrobidPage& p, double x, double y, double w, double h) {
    p.mainX = (float)x;
    p.mainY = (float)y;
    p.mainW = (float)w;
    p.mainH = (float)h;
}

// Document.calculatePageMainAreas
static void CalcMainAreas(GrobidDoc* doc) {
    CoordCounter leftEven, rightEven, leftOdd, rightOdd, top, bottom;
    for (GrobidBlock& b : doc->blocks) {
        // small blocks are page numbers, running heads and the like
        double x = b.x, y = b.y, w = b.w, h = b.h;
        if (x == 0 || h < 20 || w < 20 || h * w < 3000) {
            continue;
        }
        if (b.page % 2 == 0) {
            leftEven.Add((int)x);
            rightEven.Add((int)(x + w));
        } else {
            leftOdd.Add((int)x);
            rightOdd.Add((int)(x + w));
        }
        top.Add((int)y);
        bottom.Add((int)(y + h));
    }

    if (leftEven.IsEmpty() || leftOdd.IsEmpty()) {
        for (GrobidPage& p : doc->pages) {
            SetMainArea(p, 0, 0, p.width, p.height);
        }
        return;
    }
    int evenX = 0;
    int evenW = 0;
    if (len(doc->pages) > 1) {
        evenX = CoordItem(leftEven, true);
        evenW = CoordItem(rightEven, false) - evenX + 1;
    }
    int oddX = CoordItem(leftOdd, true);
    int oddW = CoordItem(rightOdd, false) - oddX + 1;
    int pageY = CoordItem(top, true);
    int pageH = CoordItem(bottom, false) - pageY + 1;
    for (int i = 0; i < len(doc->pages); i++) {
        bool even = (i + 1) % 2 == 0;
        SetMainArea(doc->pages[i], even ? evenX : oddX, pageY, even ? evenW : oddW, pageH);
    }
}

// Document.produceStatistics: length in tokens and block spacing
static void ProduceStatistics(GrobidDoc* doc) {
    // documentLenghtChar starts at -1 in Document.java
    doc->documentLengthChar = -1;
    for (GrobidBlock& b : doc->blocks) {
        doc->documentLengthChar += b.nTokens;
    }

    doc->maxBlockSpacing = 0.0;
    doc->minBlockSpacing = 10000.0;
    double previousBlockBottom = 0.0;
    for (GrobidPage& p : doc->pages) {
        int pageLength = 0;
        for (int i = 0; i < p.nBlocks; i++) {
            GrobidBlock& b = doc->blocks[p.firstBlock + i];
            if (i != 0 && previousBlockBottom > 0.0) {
                double spacing = b.y - previousBlockBottom;
                if (spacing > 0.0 && spacing < p.height) {
                    if (spacing > doc->maxBlockSpacing) {
                        doc->maxBlockSpacing = spacing;
                    } else if (spacing < doc->minBlockSpacing) {
                        doc->minBlockSpacing = spacing;
                    }
                }
            }
            previousBlockBottom = (double)b.y + b.h;
            pageLength += b.nTokens;
        }
        p.lengthChar = pageLength;
    }
}

// Null for a non-mupdf engine or a document without text blocks (GROBID's NO_BLOCKS).
GrobidDoc* GrobidDocFromEngine(EngineBase* engine) {
    if (!engine) {
        return nullptr;
    }
    auto doc = new GrobidDoc();
    doc->arena = ArenaNew();
    DocBuilder b;
    b.doc = doc;
    int nPages = engine->PageCount();
    for (int p = 1; p <= nPages; p++) {
        PageTextLayout page;
        page.arena = doc->arena;
        if (!ExtractPageLayout(engine, p, page) && p == 1) {
            GrobidDocFree(doc);
            return nullptr;
        }
        b.page = p;
        AddPage(b, page);
    }
    if (len(doc->blocks) == 0) {
        GrobidDocFree(doc);
        return nullptr;
    }
    CalcMainAreas(doc);
    ProduceStatistics(doc);
    return doc;
}

GrobidDoc* GrobidDocFromPdf(Str path) {
    EngineBase* engine = CreateEngineMupdfFromFile(path, FileType::PDF, 72);
    if (!engine) {
        return nullptr;
    }
    GrobidDoc* doc = GrobidDocFromEngine(engine);
    engine->Release();
    return doc;
}

void GrobidDocFree(GrobidDoc* doc) {
    if (!doc) {
        return;
    }
    if (doc->arena) {
        ArenaDelete(doc->arena);
    }
    delete doc;
}

// Block.getText(): its tokens concatenated
TempStr GrobidBlockTextTemp(GrobidDoc* doc, int block) {
    if (!doc || block < 0 || block >= len(doc->blocks)) {
        return {};
    }
    GrobidBlock& b = doc->blocks[block];
    str::Builder out;
    for (int i = b.firstToken; i < b.firstToken + b.nTokens; i++) {
        out.Append(doc->tokens[i].text);
    }
    return ToStrTemp(out);
}

static void AppendEscaped(str::Builder& out, Str s) {
    for (int i = 0; i < len(s); i++) {
        char c = s.s[i];
        if (c == '\n') {
            out.Append(StrL("\\n"));
        } else if (c == '\t') {
            out.Append(StrL("\\t"));
        } else if (c == '\\') {
            out.Append(StrL("\\\\"));
        } else {
            out.AppendChar(c);
        }
    }
}

// Tab separated records, one per line (tests/ad-hoc-grobid-layout.ts parses them):
//   doc   key=value ...
//   page  n w h firstBlock nBlocks lengthChar mainX mainY mainW mainH
//   block i page start end first n x y w h
//   image page bitmap|vector x y w h
//   tok   i page text font size flags(B I S or -) x y w h
TempStr GrobidLayoutDumpTemp(GrobidDoc* doc) {
    if (!doc) {
        return {};
    }
    str::Builder out;
    out.Append(fmt("doc\tpages=%d\ttokens=%d\tblocks=%d\timages=%d\tlengthChar=%d\tmaxSpacing=%.3f\tminSpacing=%.3f\n",
                   len(doc->pages), len(doc->tokens), len(doc->blocks), len(doc->images), doc->documentLengthChar,
                   doc->maxBlockSpacing, doc->minBlockSpacing));
    for (int i = 0; i < len(doc->pages); i++) {
        GrobidPage& p = doc->pages[i];
        out.Append(fmt("page\t%d\t%.2f\t%.2f\t%d\t%d\t%d\t%.2f\t%.2f\t%.2f\t%.2f\n", i + 1, p.width, p.height,
                       p.firstBlock, p.nBlocks, p.lengthChar, p.mainX, p.mainY, p.mainW, p.mainH));
    }
    for (int i = 0; i < len(doc->blocks); i++) {
        GrobidBlock& b = doc->blocks[i];
        out.Append(fmt("block\t%d\t%d\t%d\t%d\t%d\t%d\t%.2f\t%.2f\t%.2f\t%.2f\n", i, b.page, b.startToken, b.endToken,
                       b.firstToken, b.nTokens, b.x, b.y, b.w, b.h));
    }
    for (GrobidImage& im : doc->images) {
        out.Append(fmt("image\t%d\t%s\t%.2f\t%.2f\t%.2f\t%.2f\n", im.page, im.vector ? StrL("vector") : StrL("bitmap"),
                       im.x, im.y, im.w, im.h));
    }
    for (int i = 0; i < len(doc->tokens); i++) {
        GrobidToken& t = doc->tokens[i];
        out.Append(fmt("tok\t%d\t%d\t", i, t.page));
        AppendEscaped(out, t.text);
        out.Append(StrL("\t"));
        AppendEscaped(out, t.font);
        str::Builder flags;
        if (t.bold) flags.AppendChar('B');
        if (t.italic) flags.AppendChar('I');
        if (t.superscript) flags.AppendChar('S');
        if (len(flags) == 0) flags.AppendChar('-');
        out.Append(fmt("\t%.2f\t%s\t%.2f\t%.2f\t%.2f\t%.2f\n", t.fontSize, ToStr(flags), t.x, t.y, t.w, t.h));
    }
    return ToStrTemp(out);
}
