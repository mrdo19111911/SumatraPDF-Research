/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"

#include "CrfModel.h"
#include "GrobidText.h"
#include "GrobidDoc.h"
#include "GrobidSegmentation.h"
#include "GrobidReferences.h"
#include "GrobidHeader.h"

// Port of GROBID's (commit c71ebff) HeaderParser: getSectionHeaderFeatured with
// FeaturesVectorHeader.printVector, resultExtraction over TaggingTokenClusteror clusters and
// the processingHeaderSection post-processing. Authors go through AuthorParser with the
// name-header model. Affiliations, dates and emails stay raw: the affiliation-address and
// date models are not ported.
//
//   "<header>" pieces -> one row per token -> header.wapiti -> clusters
//       -> title, author (-> name-header.wapiti), abstract, keyword, date, email, pubnum, ...

constexpr int kMaxDoiPages = 3; // Document.getDOIMatches looks at the first pages only

static bool IsStr(Str s, const char* lit) {
    return str::Eq(s, Str(lit));
}

// Java String.length(): UTF-16 code units
static int Utf16Len(Str s) {
    int n = 0;
    for (int i = 0; i < len(s); i++) {
        u8 c = (u8)s.s[i];
        if ((c & 0xC0) == 0x80) continue;
        n += (c >= 0xF0) ? 2 : 1;
    }
    return n;
}

// Java String.trim(): strips chars <= ' ' at both ends
static Str JavaTrim(Str s) {
    int start = 0;
    int end = len(s);
    while (start < end && (u8)s.s[start] <= ' ') start++;
    while (end > start && (u8)s.s[end - 1] <= ' ') end--;
    return Str(s.s + start, end - start);
}

// s.substring(n) counting Java chars (BMP codepoints)
static Str SkipChars(Str s, int n) {
    int i = 0;
    for (int k = 0; k < n && i < len(s); k++) {
        i++;
        while (i < len(s) && ((u8)s.s[i] & 0xC0) == 0x80) i++;
    }
    return Str(s.s + i, len(s) - i);
}

static TempStr RemoveCharsTemp(Str s, Str chars) {
    str::Builder b;
    for (int i = 0; i < len(s); i++) {
        if (!str::ContainsChar(chars, s.s[i])) b.AppendChar(s.s[i]);
    }
    return ToStrTemp(b);
}

// StringUtils.isAllLowerCase / isAlpha over UTF-16 chars
static bool AllChars(Str s, bool alpha) {
    if (len(s) == 0) return false;
    WStr w = ToWStrTemp(s);
    for (int i = 0; i < len(w); i++) {
        bool ok = alpha ? IsCharAlphaW(w.s[i]) : IsCharLowerW(w.s[i]);
        if (!ok) return false;
    }
    return true;
}

//--- feature rows

// FeaturesVectorHeader
struct HeadFeatures {
    Str string;
    const char* blockStatus = nullptr;
    const char* lineStatus = nullptr;
    const char* alignmentStatus = nullptr;
    const char* fontStatus = nullptr;
    const char* fontSize = nullptr;
    const char* capitalisation = nullptr;
    const char* digit = nullptr;
    const char* punctType = nullptr;
    bool bold = false;
    bool italic = false;
    bool singleChar = false;
    bool properName = false;
    bool commonName = false;
    bool year = false;
    bool month = false;
    bool locationName = false;
    bool email = false;
    bool http = false;
    bool largestFont = false;
    bool smallestFont = false;
    bool largerThanAverageFont = false;
};

static void AppendCol(str::Builder& b, Str s) {
    b.AppendChar(' ');
    b.Append(s);
}

static void AppendCol(str::Builder& b, const char* s) {
    AppendCol(b, s ? Str(s) : StrL("null"));
}

static void AppendFlag(str::Builder& b, bool v) {
    AppendCol(b, v ? StrL("1") : StrL("0"));
}

// printVector() without the trailing newline: 31 features + label "0"
static TempStr PrintVectorTemp(const HeadFeatures& f) {
    Str s = f.string;
    str::Builder b;
    b.Append(s);
    AppendCol(b, GrobidLowerTemp(s));
    for (int i = 1; i <= 4; i++) AppendCol(b, GrobidPrefixTemp(s, i));
    for (int i = 1; i <= 4; i++) AppendCol(b, GrobidSuffixTemp(s, i));
    AppendCol(b, f.blockStatus);
    AppendCol(b, f.lineStatus);
    AppendCol(b, f.alignmentStatus);
    AppendCol(b, f.fontStatus);
    AppendCol(b, f.fontSize);
    AppendFlag(b, f.bold);
    AppendFlag(b, f.italic);
    bool allDigit = f.digit && str::Eq(Str(f.digit), StrL("ALLDIGIT"));
    AppendCol(b, allDigit ? "NOCAPS" : f.capitalisation);
    AppendCol(b, f.digit);
    AppendFlag(b, f.singleChar);
    AppendFlag(b, f.properName);
    AppendFlag(b, f.commonName);
    AppendFlag(b, f.year);
    AppendFlag(b, f.month);
    AppendFlag(b, f.locationName);
    AppendFlag(b, f.email);
    AppendFlag(b, f.http);
    AppendCol(b, f.punctType);
    AppendFlag(b, f.largestFont);
    AppendFlag(b, f.smallestFont);
    AppendFlag(b, f.largerThanAverageFont);
    AppendCol(b, StrL("0")); // label
    return str::DupTemp(ToStr(b));
}

static bool IsWordChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// Java regex \s
static bool IsRegexSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == 0x0B || c == '\f' || c == '\r';
}

static bool IsEmailSep(char c) {
    return c == '.' || c == '-' || c == '_' || c == ',';
}

static int WordRun(Str t, int p) {
    int n = 0;
    while (p + n < len(t) && IsWordChar(t.s[p + n])) n++;
    return n;
}

// ends of an optional "(\.|-|_|,)\w+"
static void OptGroupEnds(Str t, int p, Vec<int>& ends) {
    VecAppend(ends, p);
    if (p >= len(t) || !IsEmailSep(t.s[p])) return;
    int w = WordRun(t, p + 1);
    for (int k = 1; k <= w; k++) VecAppend(ends, p + 1 + k);
}

// t[i, at) is "\w+((\.|-|_|,)\w+)?\s?((\.|-|_|,)\w+)?\s?"
static bool EmailLocal(Str t, int i, int at) {
    int w = WordRun(t, i);
    for (int k = w; k >= 1; k--) {
        Vec<int> ends1;
        OptGroupEnds(t, i + k, ends1);
        for (int e1 : ends1) {
            for (int s1 = 0; s1 <= 1; s1++) {
                int p = e1 + s1;
                if (s1 == 1 && (e1 >= len(t) || !IsRegexSpace(t.s[e1]))) continue;
                Vec<int> ends2;
                OptGroupEnds(t, p, ends2);
                for (int e2 : ends2) {
                    if (e2 == at) return true;
                    if (e2 + 1 == at && IsRegexSpace(t.s[e2])) return true;
                }
            }
        }
    }
    return false;
}

// "\s?\w+(\s?(\.|-)\s?\w+)+" after the '@': the match end or -1
static int EmailDomainEnd(Str t, int p) {
    auto optSpaceThen = [&](int q, bool (*want)(char)) {
        if (q < len(t) && IsRegexSpace(t.s[q]) && q + 1 < len(t) && want(t.s[q + 1])) return q + 1;
        return q;
    };
    auto isDot = [](char c) { return c == '.' || c == '-'; };
    p = optSpaceThen(p, IsWordChar);
    int w = WordRun(t, p);
    if (w == 0) return -1;
    p += w;
    int iterations = 0;
    while (true) {
        int q = optSpaceThen(p, isDot);
        if (q >= len(t) || !isDot(t.s[q])) break;
        q = optSpaceThen(q + 1, IsWordChar);
        int w2 = WordRun(t, q);
        if (w2 == 0) break;
        p = q + w2;
        iterations++;
    }
    return iterations > 0 ? p : -1;
}

// TextUtilities.emailPattern "\w+((\.|-|_|,)\w+)?\s?((\.|-|_|,)\w+)?\s?@\s?\w+(\s?(\.|-)\s?\w+)+"
// matched at i: the match end or -1
static int MatchEmailAt(Str t, int i) {
    if (!IsWordChar(t.s[i])) return -1;
    int at = i;
    while (at < len(t) && t.s[at] != '@') at++;
    if (at >= len(t)) return -1;
    if (!EmailLocal(t, i, at)) return -1;
    return EmailDomainEnd(t, at + 1);
}

// font statistics over the blocks of the header pieces, separators (font size 0) included
struct FontStats {
    double largest = 0.0;
    double smallest = 100000.0;
    double average = 0.0;
};

// DocumentPointer.getBlockPtr() of a piece end: the block holding its last token
static int EndBlock(GrobidDoc* doc, const GrobidPiece& p) {
    int nBlocks = len(doc->blocks);
    for (int b = p.block; b < nBlocks; b++) {
        GrobidBlock& block = doc->blocks[b];
        if (p.endToken < block.firstToken + block.nTokens) return b;
    }
    return nBlocks - 1;
}

static void CollectHeaderPieces(const Vec<GrobidPiece>& pieces, Vec<GrobidPiece>& out) {
    for (const GrobidPiece& p : pieces) {
        if (str::Eq(p.label, StrL("<header>"))) VecAppend(out, p);
    }
}

static FontStats HeaderFontStats(GrobidDoc* doc, const Vec<GrobidPiece>& pieces) {
    FontStats fs;
    double accumulated = 0.0;
    int nbTokens = 0;
    for (const GrobidPiece& p : pieces) {
        int last = EndBlock(doc, p);
        for (int b = p.block; b <= last; b++) {
            GrobidBlock& block = doc->blocks[b];
            for (int i = block.firstToken; i < block.firstToken + block.nTokens; i++) {
                double size = doc->tokens[i].fontSize;
                if (size > fs.largest) fs.largest = size;
                if (size < fs.smallest) fs.smallest = size;
                accumulated += size;
                nbTokens++;
            }
        }
    }
    fs.average = accumulated / nbTokens;
    return fs;
}

// rows for header.wapiti, their first column, and the tokens the rows came from
// (headerTokenizations) as doc token indexes
struct HeadRows {
    StrVec rows;
    StrVec rowText;
    Vec<int> toks;
};

static void AddRow(HeadRows& out, const HeadFeatures& f) {
    out.rows.Append(PrintVectorTemp(f));
    out.rowText.Append(f.string);
}

// getSectionHeaderFeatured. False when there is no header piece.
static bool HeaderRows(GrobidDoc* doc, const Vec<GrobidPiece>& allPieces, HeadRows& out) {
    Vec<GrobidPiece> pieces;
    CollectHeaderPieces(allPieces, pieces);
    if (len(pieces) == 0 || len(doc->blocks) == 0) return false;

    FontStats fs = HeaderFontStats(doc, pieces);
    Str currentFont;
    int currentFontSize = -1;
    HeadFeatures prev;
    bool havePrev = false;
    double lineStartX = NAN;
    bool indented = false;

    for (const GrobidPiece& piece : pieces) {
        int lastBlock = EndBlock(doc, piece);
        for (int blockIndex = piece.block; blockIndex <= lastBlock; blockIndex++) {
            GrobidBlock& block = doc->blocks[blockIndex];
            bool newline = false;
            bool previousNewline = true;
            bool endblock = false;
            if (havePrev) prev.blockStatus = "BLOCKEND";
            int nTokens = block.nTokens;
            if (nTokens == 0) continue;

            int startIndex = 0;
            int n = 0;
            if (blockIndex == piece.block) {
                n = piece.startToken - block.startToken;
                startIndex = n;
            }
            int endPos = piece.endToken - block.startToken; // dp2.getTokenDocPos() - block.getStartToken()

            StrVec blockToks;
            for (int i = 0; i < nTokens; i++) blockToks.Append(doc->tokens[block.firstToken + i].text);
            Vec<bool> locations;
            Vec<bool> emails;
            Vec<bool> urls;
            GrobidLocationFlags(blockToks, locations);
            GrobidPatternFlags(blockToks, MatchEmailAt, emails);
            GrobidPatternFlags(blockToks, GrobidMatchUrlAt, urls);

            for (; n < nTokens; n++) {
                if (blockIndex == lastBlock && n > endPos) break;
                int tokIdx = block.firstToken + n;
                GrobidToken& token = doc->tokens[tokIdx];
                VecAppend(out.toks, tokIdx);

                Str text = RemoveCharsTemp(token.text, StrL(" "));
                if (len(text) == 0) continue;
                if (IsStr(text, "\n") || IsStr(text, "\r")) {
                    previousNewline = true;
                    newline = false;
                    continue;
                }

                // indentation: line start moved more than one character width
                if (previousNewline) {
                    newline = true;
                    previousNewline = false;
                    if (havePrev) {
                        double previousLineStartX = lineStartX;
                        lineStartX = token.x;
                        double characterWidth = (double)token.w / Utf16Len(token.text);
                        if (!isnan(previousLineStartX)) {
                            if (lineStartX - previousLineStartX > characterWidth) {
                                indented = true;
                            } else if (previousLineStartX - lineStartX > characterWidth) {
                                indented = false;
                            }
                        }
                    }
                } else {
                    newline = false;
                }

                text = RemoveCharsTemp(text, StrL(" \n"));
                if (GrobidFilterLine(text)) continue;

                HeadFeatures f;
                f.string = text;
                if (newline) f.lineStatus = "LINESTART";

                if (GrobidIsPunct(text)) f.punctType = "PUNCT";
                if (IsStr(text, "(") || IsStr(text, "[")) {
                    f.punctType = "OPENBRACKET";
                } else if (IsStr(text, ")") || IsStr(text, "]")) {
                    f.punctType = "ENDBRACKET";
                } else if (IsStr(text, ".")) {
                    f.punctType = "DOT";
                } else if (IsStr(text, ",")) {
                    f.punctType = "COMMA";
                } else if (IsStr(text, "-")) {
                    f.punctType = "HYPHEN";
                } else if (IsStr(text, "\"") || IsStr(text, "'") || IsStr(text, "`")) {
                    f.punctType = "QUOTE";
                }

                if (n == startIndex) {
                    f.lineStatus = "LINESTART";
                    f.blockStatus = "BLOCKSTART";
                } else if (n == nTokens - 1 || n + 1 > endPos) {
                    f.lineStatus = "LINEEND";
                    previousNewline = true;
                    f.blockStatus = "BLOCKEND";
                    endblock = true;
                } else {
                    // look ahead to see if we are at the end of a line within the block
                    bool endline = false;
                    bool endloop = false;
                    for (int ii = 1; n + ii < nTokens && !endloop; ii++) {
                        Str toto = blockToks[n + ii];
                        if (IsStr(toto, "\n") || IsStr(text, "\r")) {
                            endline = true;
                            endloop = true;
                        } else if (len(JavaTrim(toto)) != 0 && !IsStr(text, "\xC2\xA0") &&
                                   !str::Contains(toto, StrL("@IMAGE")) && !str::Contains(toto, StrL("@PAGE")) &&
                                   !str::Contains(text, StrL(".pbm")) && !str::Contains(text, StrL(".ppm")) &&
                                   !str::Contains(text, StrL(".png")) && !str::Contains(text, StrL(".svg")) &&
                                   !str::Contains(text, StrL(".jpg"))) {
                            endloop = true;
                        }
                        if (n + ii == nTokens - 1) {
                            endblock = true;
                            endline = true;
                        }
                    }

                    if (!endline && !newline) {
                        f.lineStatus = "LINEIN";
                    } else if (!newline) {
                        f.lineStatus = "LINEEND";
                        previousNewline = true;
                    }
                    if (!endblock && !f.blockStatus) {
                        f.blockStatus = "BLOCKIN";
                    } else if (!f.blockStatus) {
                        f.blockStatus = "BLOCKEND";
                    }
                }

                f.alignmentStatus = indented ? "LINEINDENT" : "ALIGNEDLEFT";
                if (Utf16Len(text) == 1) f.singleChar = true;
                if (GrobidIsUpperFirst(text)) f.capitalisation = "INITCAP";
                if (GrobidAllCapital(text)) f.capitalisation = "ALLCAP";
                if (GrobidHasDigit(text)) f.digit = "CONTAINSDIGITS";
                if (GrobidAllDigits(text)) f.digit = "ALLDIGIT";
                if (GrobidInDictionary(text)) f.commonName = true;
                if (GrobidIsFirstName(text) || GrobidIsLastName(text)) f.properName = true;
                if (GrobidIsMonth(text)) f.month = true;
                if (GrobidIsYear(text)) f.year = true;
                f.locationName = locations[n];
                f.email = emails[n];
                f.http = urls[n];

                if (!currentFont.s || !str::Eq(currentFont, token.font)) {
                    currentFont = token.font;
                    f.fontStatus = "NEWFONT";
                } else {
                    f.fontStatus = "SAMEFONT";
                }

                int newFontSize = (int)token.fontSize;
                if (currentFontSize == -1) {
                    currentFontSize = newFontSize;
                    f.fontSize = "HIGHERFONT";
                } else if (currentFontSize == newFontSize) {
                    f.fontSize = "SAMEFONTSIZE";
                } else if (currentFontSize < newFontSize) {
                    f.fontSize = "HIGHERFONT";
                    currentFontSize = newFontSize;
                } else {
                    f.fontSize = "LOWERFONT";
                    currentFontSize = newFontSize;
                }

                double size = token.fontSize;
                if (size == fs.largest) f.largestFont = true;
                if (size == fs.smallest) f.smallestFont = true;
                if (size > fs.average) f.largerThanAverageFont = true;
                f.bold = token.bold;
                f.italic = token.italic;

                if (!f.capitalisation) f.capitalisation = "NOCAPS";
                if (!f.digit) f.digit = "NODIGIT";
                if (!f.punctType) f.punctType = "NOPUNCT";

                if (havePrev) AddRow(out, prev);
                prev = f;
                havePrev = true;
            }

            if (havePrev) {
                prev.blockStatus = "BLOCKEND";
                prev.lineStatus = "LINEEND";
                AddRow(out, prev);
                havePrev = false;
            }
        }
    }
    return true;
}

// Header feature rows (printVector output), one per line.
TempStr GrobidHeaderFeaturesTemp(GrobidDoc* doc, const Vec<GrobidPiece>& pieces) {
    HeadRows hr;
    if (!doc || !HeaderRows(doc, pieces, hr)) return {};
    str::Builder out;
    for (int i = 0; i < len(hr.rows); i++) {
        out.Append(hr.rows[i]);
        out.AppendChar('\n');
    }
    return ToStrTemp(out);
}

//--- clusters

// TaggingTokenCluster: tokens [start, end) of HeadRows.toks with one plain label
struct HeadCluster {
    Str label;
    int start = 0;
    int end = 0;
};

// TaggingTokenSynchronizer + TaggingTokenClusteror. Each row takes the tokens up to its own
// plus the spaces and newlines after it. False when tokens and rows get out of sync.
static bool ClusterRows(GrobidDoc* doc, CrfModel* model, HeadRows& hr, const Vec<int>& labels,
                        Vec<HeadCluster>& clusters) {
    int ptr = 0;
    int nToks = len(hr.toks);
    for (int r = 0; r < len(hr.rows); r++) {
        int start = ptr;
        bool stop = false;
        while (!stop && ptr < nToks) {
            Str t = doc->tokens[hr.toks[ptr]].text;
            if (IsStr(t, "\n") || IsStr(t, " ")) {
                // trailing newline / space
            } else if (str::Eq(RemoveCharsTemp(t, StrL(" \n")), hr.rowText[r])) {
                stop = true;
            } else if (len(t) != 0) {
                logf("GrobidParseHeader: tokens out of sync at row %d '%s'\n", r, hr.rowText[r]);
                return false;
            }
            ptr++;
        }
        while (ptr < nToks) {
            Str t = doc->tokens[hr.toks[ptr]].text;
            if (!IsStr(t, "\n") && !IsStr(t, " ")) break;
            ptr++;
        }

        Str label = CrfLabelName(model, labels[r]);
        bool begin = str::StartsWith(label, StrL("I-"));
        if (begin) label = Str(label.s + 2, len(label) - 2);
        int n = len(clusters);
        if (r == 0 || begin || !str::Eq(clusters[n - 1].label, label)) {
            HeadCluster c;
            c.label = label;
            c.start = start;
            c.end = ptr;
            VecAppend(clusters, c);
        } else {
            clusters[n - 1].end = ptr;
        }
    }
    return true;
}

// LayoutTokensUtil.toText
static TempStr ClusterTextTemp(GrobidDoc* doc, HeadRows& hr, const HeadCluster& c) {
    str::Builder b;
    for (int i = c.start; i < c.end; i++) b.Append(doc->tokens[hr.toks[i]].text);
    return ToStrTemp(b);
}

// LayoutTokensUtil.doesRequireDehypenisation over doc tokens toks[i]
static bool NeedsDehyphen(GrobidDoc* doc, const Vec<int>& toks, int i) {
    auto tok = [&](int k) -> GrobidToken& { return doc->tokens[toks[k]]; };
    auto isSep = [&](int k) { return IsStr(tok(k).text, " ") || IsStr(tok(k).text, "\n"); };
    int size = len(toks);
    int j = i + 1;
    int breakLine = 0;
    double coordinateY = tok(i).y;
    while (j < size && isSep(j)) {
        if (IsStr(tok(j).text, "\n")) breakLine++;
        j++;
    }
    if (breakLine == 0 && j < size && tok(j).y == coordinateY) return false;
    if (j >= size || !AllChars(tok(j).text, false)) return false;
    if (i < 1) return true;
    if (tok(j).y > coordinateY) return true;
    int z = i - 1;
    while (z > 0 && isSep(z)) z--;
    if (!AllChars(tok(z).text, true)) return false;
    if (tok(z).y < coordinateY) return true;
    return coordinateY == -1 && breakLine > 0;
}

// LayoutTokensUtil.normalizeDehyphenizeText
static TempStr DehyphenTextTemp(GrobidDoc* doc, HeadRows& hr, const HeadCluster& c) {
    Vec<int> toks;
    for (int i = c.start; i < c.end; i++) VecAppend(toks, hr.toks[i]);
    int size = len(toks);
    StrVec out;
    for (int i = 0; i < size; i++) {
        GrobidToken& t = doc->tokens[toks[i]];
        if (!IsStr(t.text, "-") || t.superscript) {
            out.Append(t.text);
            continue;
        }
        if (NeedsDehyphen(doc, toks, i)) {
            while (len(out) > 0 && IsStr(out[len(out) - 1], " ")) out.RemoveAt(len(out) - 1);
            while (i + 1 < size &&
                   (IsStr(doc->tokens[toks[i + 1]].text, " ") || IsStr(doc->tokens[toks[i + 1]].text, "\n"))) {
                i++;
            }
            continue;
        }
        out.Append(t.text);
        while (i + 1 < size && IsStr(doc->tokens[toks[i + 1]].text, "\n")) i++;
    }
    TempStr text = JoinTemp(&out, Str());
    return GrobidNormalizeSpaceTemp(str::ReplaceTemp(text, StrL("\n"), StrL(" ")));
}

//--- field cleaning

// TextUtilities.dehyphenize(String) of text without line breaks: tokenizing normalises it,
// and no hyphen qualifies without a line break
static TempStr NormaliseTemp(Str s) {
    StrVec toks;
    GrobidTokenize(s, toks);
    return JoinTemp(&toks, Str());
}

static bool StartsWithLower(Str lower, const char* prefix) {
    return str::StartsWith(lower, Str(prefix));
}

// BiblioItem.cleanKeywords
static TempStr CleanKeywordsTemp(Str s) {
    if (len(s) == 0) return s;
    Str res = GrobidNormalizeSpaceTemp(s);
    TempStr low = GrobidLowerTemp(res);
    if (StartsWithLower(low, "keywords")) {
        res = SkipChars(res, 8);
    } else if (StartsWithLower(low, "key words") || StartsWithLower(low, "mots cl\xC3\xA9s") ||
               StartsWithLower(low, "mots cles")) {
        res = SkipChars(res, 9);
    } else if (StartsWithLower(low, "mots clefs")) {
        res = SkipChars(res, 10);
    }
    res = JavaTrim(res);
    if (str::StartsWith(res, StrL(":")) || str::StartsWith(res, StrL("\xE2\x80\x94")) ||
        str::StartsWith(res, StrL("-"))) {
        res = SkipChars(res, 1);
    }
    if (str::EndsWith(res, StrL("."))) res = Str(res.s, len(res) - 1);
    return str::DupTemp(JavaTrim(res));
}

// BiblioItem.cleanAbstract
static TempStr CleanAbstractTemp(Str s) {
    if (len(s) == 0) return s;
    Str res = GrobidNormalizeSpaceTemp(s);
    TempStr res0 = GrobidLowerTemp(res);
    static const char* prefixes[] = {"abstract", "summary", "r\xC3\xA9sum\xC3\xA9", "abr\xC3\xA9g\xC3\xA9",
                                     "a b s t r a c t"};
    for (const char* prefix : prefixes) {
        if (!str::StartsWith(res0, Str(prefix))) continue;
        int n = Utf16Len(Str(prefix));
        res = n < Utf16Len(res) ? SkipChars(res, n) : Str();
        break;
    }
    if (str::StartsWith(res, StrL(".")) || str::StartsWith(res, StrL(":")) || str::StartsWith(res, StrL(")"))) {
        res = JavaTrim(SkipChars(res, 1));
    }
    TempStr out = str::ReplaceTemp(res, StrL("( "), StrL("("));
    out = str::ReplaceTemp(out, StrL(" )"), StrL(")"));
    return str::ReplaceTemp(out, StrL("  "), StrL(" "));
}

// TextUtilities.getLastToken
static Str LastToken(Str section) {
    int sp = -1;
    for (int i = 0; i < len(section); i++) {
        if (section.s[i] == ' ') sp = i;
    }
    return sp < 0 ? section : Str(section.s + sp + 1, len(section) - sp - 1);
}

// StringTokenizer: runs of chars not in delims
static void SplitAny(Str s, Str delims, StrVec& out) {
    int start = -1;
    for (int i = 0; i <= len(s); i++) {
        bool delim = i == len(s) || str::ContainsChar(delims, s.s[i]);
        if (!delim && start < 0) start = i;
        if (delim && start >= 0) {
            out.Append(Str(s.s + start, i - start));
            start = -1;
        }
    }
}

// TextUtilities.dehyphenizeHard: a hyphen is dropped when the joined word is in the dictionary
static TempStr DehyphenizeHardTemp(Str text) {
    str::Builder res;
    StrVec sections;
    SplitAny(text, StrL("-"), sections);
    bool hyphen = false;
    bool failure = false;
    Str lastToken;
    for (int k = 0; k < len(sections); k++) {
        Str section = JavaTrim(sections[k]);
        if (hyphen) {
            StrVec words;
            SplitAny(section, StrL(" ,.);!"), words);
            if (len(words) > 0) {
                Str firstToken = words[0];
                TempStr hyphenToken = str::JoinTemp(lastToken, firstToken);
                if (GrobidInDictionary(GrobidLowerTemp(hyphenToken)) && !GrobidHasDigit(hyphenToken)) {
                    res.Append(firstToken);
                    section = SkipChars(section, Utf16Len(firstToken));
                } else {
                    res.AppendChar('-');
                    failure = true;
                }
            } else {
                res.AppendChar('-');
            }
        }
        hyphen = true;
        lastToken = LastToken(section);
        if (failure) {
            res.Append(section);
            failure = false;
        } else {
            res.AppendChar(' ');
            res.Append(section);
        }
    }
    TempStr out = str::ReplaceTemp(ToStrTemp(res), StrL(" . "), StrL(". "));
    out = str::ReplaceTemp(out, StrL("  "), StrL(" "));
    return str::DupTemp(JavaTrim(out));
}

static bool HasDoi(Str s) {
    for (int i = 0; i < len(s); i++) {
        if (GrobidMatchDoiAt(s, i) > i) return true;
    }
    return false;
}

// Document.getDOIMatches: distinct DOIs in the blocks of the first pages
static void DocDois(GrobidDoc* doc, StrVec& dois) {
    for (int p = 0; p < len(doc->pages) && p < kMaxDoiPages; p++) {
        GrobidPage& page = doc->pages[p];
        for (int b = page.firstBlock; b < page.firstBlock + page.nBlocks && b < len(doc->blocks); b++) {
            Str text = JavaTrim(GrobidBlockTextTemp(doc, b));
            for (int i = 0; i < len(text);) {
                int end = GrobidMatchDoiAt(text, i);
                if (end <= i) {
                    i++;
                    continue;
                }
                Str doi = Str(text.s + i, end - i);
                if (!dois.Contains(doi)) dois.Append(doi);
                i = end;
            }
        }
    }
}

//--- persons

static bool IsBlank(Str s) {
    return len(GrobidNormalizeSpaceTemp(s)) == 0;
}

// Person.sanityCheck: a person needs a last name
static void SanityCheck(Vec<GrobidPerson>& persons) {
    for (int i = len(persons) - 1; i >= 0; i--) {
        if (IsBlank(persons[i].last)) VecRemoveAt(persons, i);
    }
}

static Str FirstChar(Str s) {
    int n = 1;
    while (n < len(s) && ((u8)s.s[n] & 0xC0) == 0x80) n++;
    return Str(s.s, n);
}

// lowercase with '-' and '.' removed; null stays null
static Str NameKey(Str s) {
    if (!s.s) return {};
    return RemoveCharsTemp(GrobidLowerTemp(s), StrL("-."));
}

// first / middle name clash test of Person.deduplicate
static bool NamesClash(Str a, Str b) {
    if (!a.s || !b.s) return false;
    if (Utf16Len(a) == 1 && Utf16Len(b) == 1) return !str::Eq(a, b);
    return !str::Eq(a, b) && !str::StartsWith(a, b) && !str::StartsWith(b, a);
}

static void TakeLonger(Str& local, Str other) {
    if (!other.s) return;
    if (!local.s || Utf16Len(other) > Utf16Len(local)) local = other;
}

// Person.deduplicate: persons with the same surname and first initial and no clashing
// forenames are merged into the first, keeping the longest name parts
static void Deduplicate(Vec<GrobidPerson>& persons) {
    StrVec sigs;
    for (GrobidPerson& p : persons) {
        if (IsBlank(p.last)) {
            sigs.Append(Str());
            continue;
        }
        Str sig = GrobidLowerTemp(p.last);
        if (!IsBlank(p.first)) sig = str::JoinTemp(sig, StrL("_"), FirstChar(p.first));
        sigs.Append(sig);
    }

    Vec<GrobidPerson*> removed;
    for (int g = 0; g < len(persons); g++) {
        Str sig = sigs[g];
        if (len(sig) == 0 || sigs.Find(sig) != g) continue;
        Vec<int> group;
        for (int k = g; k < len(persons); k++) {
            if (str::Eq(sigs[k], sig)) VecAppend(group, k);
        }
        if (len(group) < 2) continue;

        Vec<int> kept;
        for (int j : group) {
            int nbClash = 0;
            for (int k : group) {
                if (k == j) continue;
                bool clash = NamesClash(NameKey(persons[j].first), NameKey(persons[k].first)) ||
                             NamesClash(NameKey(persons[j].middle), NameKey(persons[k].middle));
                if (clash) nbClash++;
            }
            if (nbClash == 0) VecAppend(kept, j);
        }
        if (len(kept) < 2) continue;

        GrobidPerson& local = persons[kept[0]];
        bool anyShortFirstName = local.first.s && Utf16Len(local.first) == 1;
        for (int i = 1; i < len(kept); i++) {
            GrobidPerson& other = persons[kept[i]];
            if (other.first.s && Utf16Len(other.first) == 1) anyShortFirstName = true;
            TakeLonger(local.first, other.first);
            TakeLonger(local.middle, other.middle);
            TakeLonger(local.title, other.title);
            TakeLonger(local.suffix, other.suffix);
            VecAppend(removed, &other);
        }
        // drop a middle initial left by splitting an all-caps forename ("YI" -> "Y" + "I")
        Str lf = local.first.s ? GrobidLowerTemp(local.first) : Str();
        Str lm = local.middle.s ? GrobidLowerTemp(local.middle) : Str();
        if (anyShortFirstName && lm.s && Utf16Len(lm) == 1 && lf.s && Utf16Len(lf) == 2 && str::EndsWith(lf, lm)) {
            local.middle = {};
        }
    }

    for (int i = len(persons) - 1; i >= 0; i--) {
        for (GrobidPerson* r : removed) {
            if (r != &persons[i]) continue;
            VecRemoveAt(persons, i);
            break;
        }
    }
}

//--- fields

struct HeadFields {
    bool haveTitle = false;
    Str title;
    Vec<HeadCluster> authorClusters;
    bool haveAbstract = false;
    Str abstract;
    Str keyword;
    Str affiliation;
    Str email;
    Str date;
    Str doi;
    Str journal;
    Str meeting;
};

static Str JoinField(Str cur, const char* sep, Str add) {
    return cur.s ? str::JoinTemp(cur, Str(sep), add) : add;
}

// HeaderParser.resultExtraction for the fields we keep
static void ExtractFields(GrobidDoc* doc, HeadRows& hr, Vec<HeadCluster>& clusters, HeadFields& f) {
    for (HeadCluster& c : clusters) {
        Str content = DehyphenTextTemp(doc, hr, c);
        Str raw = ClusterTextTemp(doc, hr, c);
        if (IsStr(c.label, "<title>")) {
            if (!f.haveTitle) f.title = GrobidNormalizeSpaceTemp(content);
            f.haveTitle = true;
        } else if (IsStr(c.label, "<author>")) {
            VecAppend(f.authorClusters, c);
        } else if (IsStr(c.label, "<date>")) {
            if (!f.date.s || Utf16Len(f.date) < Utf16Len(raw)) f.date = GrobidNormalizeSpaceTemp(raw);
        } else if (IsStr(c.label, "<abstract>")) {
            if (!f.haveAbstract) f.abstract = CleanAbstractTemp(content);
            f.haveAbstract = true;
        } else if (IsStr(c.label, "<affiliation>")) {
            f.affiliation = JoinField(f.affiliation, " ; ", content);
        } else if (IsStr(c.label, "<email>")) {
            f.email = JoinField(f.email, "\t", raw);
        } else if (IsStr(c.label, "<pubnum>")) {
            // setPubnum + checkIdentifier: a pubnum holding a DOI becomes the DOI
            Str pubnum = GrobidNormalizeSpaceTemp(content);
            if (len(pubnum) == 0 || len(f.doi) > 0) continue;
            if (HasDoi(pubnum) || HasDoi(str::ReplaceTemp(pubnum, StrL(" "), StrL("")))) {
                f.doi = GrobidCleanDoiTemp(pubnum);
            }
        } else if (IsStr(c.label, "<keyword>")) {
            f.keyword = CleanKeywordsTemp(JoinField(f.keyword, " \n ", content));
        } else if (IsStr(c.label, "<journal>")) {
            if (!f.journal.s) f.journal = GrobidNormalizeSpaceTemp(content);
        } else if (IsStr(c.label, "<meeting>")) {
            f.meeting = JoinField(f.meeting, ", ", content);
        }
    }
}

// processingHeaderSection: title cleaning and the name-header model per author cluster
static TempStr AuthorsTemp(GrobidDoc* doc, HeadRows& hr, HeadFields& f) {
    Vec<GrobidPerson> persons;
    for (HeadCluster& c : f.authorClusters) {
        StrVec toks;
        for (int i = c.start; i < c.end; i++) toks.Append(doc->tokens[hr.toks[i]].text);
        GrobidParseNames(toks, StrL("name-header"), persons);
    }
    SanityCheck(persons);
    Deduplicate(persons);
    return GrobidFormatPersonsTemp(persons);
}

// BiblioItem.cleanTitles + dehyphenize + the leading "1" (a footnote mark) dropped
static TempStr CleanTitleTemp(Str title) {
    if (!title.s) return {};
    Str t = GrobidCleanFieldTemp(title);
    if (str::EndsWith(t, StrL(" y"))) t = Str(t.s, len(t) - 2);
    t = JavaTrim(NormaliseTemp(GrobidNormalizeSpaceTemp(t)));
    if (Utf16Len(t) > 1) {
        if (str::StartsWith(t, StrL("1"))) t = Str(t.s + 1, len(t) - 1);
        t = JavaTrim(t);
    }
    return str::DupTemp(t);
}

static TempStr EmailsTemp(Str email) {
    StrVec parts;
    Split(&parts, email, StrL("\t"), true);
    StrVec out;
    for (Str p : parts) {
        Str e = GrobidNormalizeSpaceTemp(p);
        if (len(e) > 0) out.Append(e);
    }
    return JoinTemp(&out, StrL("; "));
}

// Fills header with the metadata of the paper from its "<header>" pieces. False when there
// is no header or tagging fails.
bool GrobidParseHeader(GrobidDoc* doc, const Vec<GrobidPiece>& pieces, GrobidHeader* header) {
    *header = {};
    HeadRows hr;
    if (!doc || !HeaderRows(doc, pieces, hr) || len(hr.rows) == 0) return false;

    CrfModel* model = GrobidModel(StrL("header"));
    if (!model) return false;
    Vec<int> labels;
    if (!CrfTag(model, hr.rows, labels) || len(labels) != len(hr.rows)) return false;
    Vec<HeadCluster> clusters;
    if (!ClusterRows(doc, model, hr, labels, clusters)) return false;

    HeadFields f;
    ExtractFields(doc, hr, clusters, f);

    Str abstract = f.haveAbstract ? DehyphenizeHardTemp(f.abstract) : Str();
    Str title = CleanTitleTemp(f.title);
    Str authors = AuthorsTemp(doc, hr, f);
    Str keywords;
    if (f.keyword.s) keywords = CleanKeywordsTemp(CleanKeywordsTemp(NormaliseTemp(f.keyword)));

    // DOI pass: a single DOI on the first pages wins
    StrVec dois;
    DocDois(doc, dois);
    Str doi = f.doi;
    if (len(dois) == 1) doi = GrobidCleanDoiTemp(dois[0]);

    Str venue = len(f.journal) > 0 ? f.journal : GrobidNormalizeSpaceTemp(f.meeting);

    header->title = str::Dup(title);
    header->authors = str::Dup(authors);
    header->abstract = str::Dup(abstract);
    header->keywords = str::Dup(keywords);
    header->affiliation = str::Dup(f.affiliation);
    header->email = str::Dup(EmailsTemp(f.email));
    header->date = str::Dup(f.date);
    header->doi = str::Dup(doi);
    header->venue = str::Dup(venue);
    return true;
}

void GrobidHeaderFree(GrobidHeader* h) {
    if (!h) return;
    for (Str s :
         {h->title, h->authors, h->abstract, h->keywords, h->affiliation, h->email, h->date, h->doi, h->venue}) {
        str::Free(s);
    }
    *h = {};
}
