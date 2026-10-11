/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"

#include "CrfModel.h"
#include "GrobidText.h"
#include "GrobidDoc.h"
#include "GrobidSegmentation.h"

// Port of GROBID's engines/Segmentation.java (getAllLinesFeatured,
// getFeatureVectorsAsString), features/FeaturesVectorSegmentation.java (printVector)
// and document/BasicStructureBuilder.java (generalResultSegmentation).
//
//   blocks -> lines -> one feature row per line -> segmentation.wapiti
//          -> label per line -> lines mapped back to token ranges -> pieces

constexpr int kNbBinsPosition = 12;
constexpr int kLineScale = 10;
constexpr double kMinGraphicDistance = 100.0; // Document.MIN_DISTANCE
constexpr int kMinPatternLen = 8;
constexpr int kMaxLinesPerBlock = 10000;

//--- Java String helpers (lengths are UTF-16 code units, as in Java)

static int Utf16Len(Str s) {
    int n = 0;
    for (int i = 0; i < len(s); i++) {
        u8 c = (u8)s.s[i];
        if ((c & 0xC0) == 0x80) continue;
        n += (c >= 0xF0) ? 2 : 1;
    }
    return n;
}

// String.split("[\\n\\r]"): trailing empty strings dropped, no match keeps the input
static void SplitLines(Str text, StrVec& lines) {
    bool matched = false;
    int start = 0;
    for (int i = 0; i < len(text); i++) {
        char c = text.s[i];
        if (c != '\n' && c != '\r') continue;
        matched = true;
        lines.Append(Str(text.s + start, i - start));
        start = i + 1;
    }
    lines.Append(Str(text.s + start, len(text) - start));
    if (!matched) return;
    while (len(lines) > 0 && len(lines.At(len(lines) - 1)) == 0) {
        lines.RemoveAt(len(lines) - 1);
    }
}

// String.trim(): strips chars <= ' ' at both ends
static Str JavaTrim(Str s) {
    int b = 0;
    int e = len(s);
    while (b < e && (u8)s.s[b] <= ' ') b++;
    while (e > b && (u8)s.s[e - 1] <= ' ') e--;
    return Str(s.s + b, e - b);
}

// StringUtils.isBlank() for the whitespace pdf text can hold
static bool IsBlank(Str s) {
    for (int i = 0; i < len(s); i++) {
        u8 c = (u8)s.s[i];
        bool ws = c == ' ' || (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x1F);
        if (!ws) return false;
    }
    return true;
}

// length of a StringTokenizer(" \t\f ") delimiter at s[i], 0 if none
static int DelimLen(Str s, int i) {
    char c = s.s[i];
    if (c == ' ' || c == '\t' || c == '\f') return 1;
    if ((u8)c == 0xC2 && i + 1 < len(s) && (u8)s.s[i + 1] == 0xA0) return 2;
    return 0;
}

// next StringTokenizer token starting at *pos; empty Str when none left
static Str NextToken(Str s, int* pos) {
    int i = *pos;
    while (i < len(s)) {
        int d = DelimLen(s, i);
        if (d == 0) break;
        i += d;
    }
    int start = i;
    while (i < len(s) && DelimLen(s, i) == 0) i++;
    *pos = i;
    return Str(s.s + start, i - start);
}

// text.replaceAll("[ \n\r]", "")
static TempStr RemoveSpaceNl(Str s) {
    str::Builder b;
    for (int i = 0; i < len(s); i++) {
        char c = s.s[i];
        if (c == ' ' || c == '\n' || c == '\r') continue;
        b.AppendChar(c);
    }
    return str::DupTemp(ToStr(b));
}

// FeatureFactory.linearScaling(int, int, int): float arithmetic
static int LinearScalingInt(int pos, int total, int nbBins) {
    if (pos >= total) return nbBins;
    if (pos <= 0) return 0;
    float rel = (float)pos / total;
    float rel2 = rel * nbBins;
    return (int)rel2;
}

//--- layout helpers

static bool IsTokenText(GrobidDoc* doc, int i, const char* t) {
    if (i < 0 || i >= len(doc->tokens)) return false;
    return str::Eq(doc->tokens[i].text, Str(t));
}

static Str TokenText(GrobidDoc* doc, int i) {
    if (i < 0 || i >= len(doc->tokens)) return {};
    return doc->tokens[i].text;
}

// Block.getText(), null (empty Str) for a block without tokens
static TempStr BlockText(GrobidDoc* doc, int block) {
    if (doc->blocks[block].nTokens == 0) return {};
    return GrobidBlockTextTemp(doc, block);
}

// Document.getConnectedGraphics(): images within MIN_DISTANCE vertically of the block
static void ConnectedGraphics(GrobidDoc* doc, GrobidBlock& block, bool* bitmap, bool* vector) {
    for (GrobidImage& img : doc->images) {
        if (block.page != img.page) continue;
        bool isNear = fabs((img.y + img.h) - block.y) < kMinGraphicDistance ||
                      fabs(img.y - (block.y + block.h)) < kMinGraphicDistance;
        if (!isNear) continue;
        if (img.vector) {
            *vector = true;
        } else {
            *bitmap = true;
        }
    }
}

// BoundingBox.contains() || BoundingBox.intersect() of block and page main area
static bool InMainArea(GrobidPage& page, GrobidBlock& block) {
    double ax1 = page.mainX, ay1 = page.mainY;
    double ax2 = ax1 + page.mainW, ay2 = ay1 + page.mainH;
    double bx1 = block.x, by1 = block.y;
    double bx2 = bx1 + block.w, by2 = by1 + block.h;
    bool contains = ax1 <= bx1 && ay1 <= by1 && ax2 >= bx2 && ay2 >= by2;
    if (contains) return true;
    if (ax2 < bx1 || ax1 > bx2 || ay2 < by1 || ay1 > by2) return false;
    return true;
}

//--- repetitive head/foot patterns (getAllLinesFeatured)

struct SegPatterns {
    StrVec patterns;
    Vec<int> counts;
    Vec<bool> firstTimeDone;
};

static bool IsHeadOrFootBlock(int blockIndex, int nBlocks) {
    return blockIndex < 2 || blockIndex > nBlocks - 2;
}

// first line of the two first and the last block of each page, counted by letter pattern
static void CollectPatterns(GrobidDoc* doc, SegPatterns& sp) {
    for (GrobidPage& page : doc->pages) {
        for (int bi = 0; bi < page.nBlocks; bi++) {
            if (!IsHeadOrFootBlock(bi, page.nBlocks)) continue;
            Str text = BlockText(doc, page.firstBlock + bi);
            if (len(text) == 0) continue;
            StrVec lines;
            SplitLines(text, lines);
            if (len(lines) == 0) continue;
            TempStr pattern = GrobidPatternTemp(lines.At(0));
            if (len(pattern) <= kMinPatternLen) continue;
            int idx = sp.patterns.Find(pattern);
            if (idx < 0) {
                sp.patterns.Append(pattern);
                VecAppend(sp.counts, 1);
                VecAppend(sp.firstTimeDone, false);
            } else {
                sp.counts[idx]++;
            }
        }
    }
}

//--- FeaturesVectorSegmentation

struct SegFeatures {
    Str string;
    Str secondString;
    const char* blockStatus = nullptr;
    const char* pageStatus = nullptr;
    const char* fontStatus = nullptr;
    const char* fontSize = nullptr;
    const char* capitalisation = nullptr;
    const char* digit = nullptr;
    bool bold = false;
    bool italic = false;
    bool singleChar = false;
    bool properName = false;
    bool commonName = false;
    bool year = false;
    bool month = false;
    bool email = false;
    bool http = false;
    int relativeDocumentPosition = -1;
    int relativePagePositionChar = -1;
    Str punctuationProfile;
    int lineLength = 0;
    bool bitmapAround = false;
    bool vectorAround = false;
    bool repetitivePattern = false;
    bool firstRepetitivePattern = false;
    bool inMainArea = true;
};

static void AppendFlag(str::Builder& b, bool v) {
    b.Append(v ? StrL(" 1") : StrL(" 0"));
}

static void AppendCol(str::Builder& b, Str s) {
    b.AppendChar(' ');
    b.Append(s);
}

// printVector(): 33 features (lineStatus, punctType are null so not printed) + label column "0"
static TempStr PrintVector(SegFeatures& f) {
    str::Builder b;
    b.Append(f.string);
    AppendCol(b, len(f.secondString) > 0 ? f.secondString : f.string);
    AppendCol(b, GrobidLowerTemp(f.string));
    for (int n = 1; n <= 4; n++) {
        AppendCol(b, GrobidPrefixTemp(f.string, n));
    }
    if (f.blockStatus) AppendCol(b, Str(f.blockStatus));
    AppendCol(b, Str(f.pageStatus));
    AppendCol(b, Str(f.fontStatus));
    AppendCol(b, Str(f.fontSize));
    AppendFlag(b, f.bold);
    AppendFlag(b, f.italic);
    bool allDigit = str::Eq(Str(f.digit), StrL("ALLDIGIT"));
    AppendCol(b, allDigit ? StrL("NOCAPS") : Str(f.capitalisation));
    AppendCol(b, Str(f.digit));
    AppendFlag(b, f.singleChar);
    AppendFlag(b, f.properName);
    AppendFlag(b, f.commonName);
    AppendFlag(b, false); // firstName: never set in GROBID
    AppendFlag(b, f.year);
    AppendFlag(b, f.month);
    AppendFlag(b, f.email);
    AppendFlag(b, f.http);
    b.Append(fmt(" %d %d", f.relativeDocumentPosition, f.relativePagePositionChar));
    if (len(f.punctuationProfile) == 0) {
        b.Append(StrL(" no 0"));
    } else {
        AppendCol(b, f.punctuationProfile);
        b.Append(fmt(" %d", Utf16Len(f.punctuationProfile)));
    }
    b.Append(fmt(" %d", f.lineLength));
    AppendFlag(b, f.bitmapAround);
    AppendFlag(b, f.vectorAround);
    AppendFlag(b, f.repetitivePattern);
    AppendFlag(b, f.firstRepetitivePattern);
    AppendFlag(b, f.inMainArea);
    b.Append(StrL(" 0"));
    return str::DupTemp(ToStr(b));
}

// lexical features of the line's first token
static void SetTextFeatures(SegFeatures& f, Str text) {
    if (Utf16Len(text) == 1) f.singleChar = true;
    if (GrobidIsUpperFirst(text)) f.capitalisation = "INITCAP";
    if (GrobidAllCapital(text)) f.capitalisation = "ALLCAP";
    if (GrobidHasDigit(text)) f.digit = "CONTAINSDIGITS";
    TempStr lower = GrobidLowerTemp(text);
    if (GrobidInDictionary(lower)) f.commonName = true;
    if (GrobidIsFirstName(lower) || GrobidIsLastName(lower)) f.properName = true;
    if (GrobidIsMonth(lower)) f.month = true;
    if (GrobidAllDigits(text)) f.digit = "ALLDIGIT";
    if (GrobidIsYear(text)) f.year = true;
    if (GrobidIsEmail(text)) f.email = true;
    if (GrobidIsHttp(text)) f.http = true;
}

// getFeatureVectorsAsString(): one row per kept line; a row is printed one line late
// so that the last line of a page can become PAGEEND
static void BuildRows(GrobidDoc* doc, StrVec& rows) {
    SegPatterns sp;
    CollectPatterns(doc, sp);

    int documentLength = doc->documentLengthChar;
    Str currentFont;
    bool haveFont = false;
    int currentFontSize = -1;
    int nn = 0; // document position
    SegFeatures prev;
    bool havePrev = false;

    for (int pageNo = 0; pageNo < len(doc->pages); pageNo++) {
        GrobidPage& page = doc->pages[pageNo];
        bool newPage = true;
        int pageLength = page.lengthChar;
        int mm = 0; // page position
        if (page.nBlocks == 0) continue;

        for (int bi = 0; bi < page.nBlocks; bi++) {
            int blockNo = page.firstBlock + bi;
            GrobidBlock& block = doc->blocks[blockNo];
            bool graphicBitmap = false;
            bool graphicVector = false;
            ConnectedGraphics(doc, block, &graphicBitmap, &graphicVector);

            if (block.nTokens == 0) continue;
            Str localText = BlockText(doc, blockNo);
            bool inPageMainArea = InMainArea(page, block);

            StrVec lines;
            SplitLines(localText, lines);
            int maxLineLength = 0;
            for (int p = 0; p < len(lines); p++) {
                int n = Utf16Len(lines.At(p));
                if (n > maxLineLength) maxLineLength = n;
            }
            // layout features of every line come from the block's first token
            GrobidToken& token = doc->tokens[block.firstToken];

            for (int li = 0; li < len(lines); li++) {
                Str line = lines.At(li);
                SegFeatures f;

                if (IsHeadOrFootBlock(bi, page.nBlocks)) {
                    int idx = sp.patterns.Find(GrobidPatternTemp(line));
                    if (idx >= 0 && sp.counts[idx] > 1) {
                        f.repetitivePattern = true;
                        if (!sp.firstTimeDone[idx]) {
                            f.firstRepetitivePattern = true;
                            sp.firstTimeDone[idx] = true;
                        }
                    }
                }

                int pos = 0;
                Str text = NextToken(line, &pos);
                Str text2 = NextToken(line, &pos);
                if (len(text) == 0) continue;
                text = JavaTrim(RemoveSpaceNl(text));
                if (len(text) == 0 || GrobidFilterLine(line)) continue;

                f.string = text;
                f.secondString = str::DupTemp(text2); // outlives the block's lines
                f.lineLength = LinearScalingInt(Utf16Len(line), maxLineLength, kLineScale);
                f.punctuationProfile = GrobidPunctProfileTemp(line);
                f.bitmapAround = graphicBitmap;
                f.vectorAround = graphicVector;

                bool prevBlockEnd = havePrev && str::Eq(Str(prev.blockStatus), StrL("BLOCKEND"));
                if (li == 0 || prevBlockEnd) {
                    f.blockStatus = "BLOCKSTART";
                } else if (li == len(lines) - 1) {
                    f.blockStatus = "BLOCKEND";
                } else {
                    f.blockStatus = "BLOCKIN";
                }

                if (newPage) {
                    f.pageStatus = "PAGESTART";
                    newPage = false;
                    if (havePrev) prev.pageStatus = "PAGEEND";
                } else {
                    f.pageStatus = "PAGEIN";
                }

                SetTextFeatures(f, text);

                if (!haveFont || !str::Eq(currentFont, token.font)) {
                    currentFont = token.font;
                    haveFont = true;
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

                f.bold = token.bold;
                f.italic = token.italic;
                if (!f.capitalisation) f.capitalisation = "NOCAPS";
                if (!f.digit) f.digit = "NODIGIT";

                f.relativeDocumentPosition = LinearScalingInt(nn, documentLength, kNbBinsPosition);
                f.relativePagePositionChar = LinearScalingInt(mm, pageLength, kNbBinsPosition);
                f.inMainArea = inPageMainArea;

                if (havePrev) rows.Append(PrintVector(prev));
                prev = f;
                havePrev = true;
            }

            mm += block.nTokens;
            nn += block.nTokens;
        }
    }
    if (havePrev) rows.Append(PrintVector(prev));
}

TempStr GrobidSegmentationFeaturesTemp(GrobidDoc* doc) {
    if (!doc) return {};
    StrVec rows;
    BuildRows(doc, rows);
    str::Builder out;
    for (Str row : rows) {
        out.Append(row);
        out.AppendChar('\n');
    }
    return str::DupTemp(ToStr(out));
}

//--- BasicStructureBuilder.generalResultSegmentation

// GenericTaggerUtils.getPlainLabel(): "I-<body>" -> "<body>"
static Str PlainLabel(Str label) {
    if (str::StartsWith(label, StrL("I-")) || str::StartsWith(label, StrL("B-"))) {
        return Str(label.s + 2, len(label) - 2);
    }
    return label;
}

static bool IsSpaceOrTab(GrobidDoc* doc, int i) {
    return IsTokenText(doc, i, " ") || IsTokenText(doc, i, "\t");
}

static bool IsEol(GrobidDoc* doc, int i) {
    return IsTokenText(doc, i, "\n") || IsTokenText(doc, i, "\r");
}

static void AddPiece(Vec<GrobidPiece>& pieces, Str label, int block, int start, int end) {
    GrobidPiece p;
    p.label = label;
    p.block = block;
    p.startToken = start;
    p.endToken = end;
    VecAppend(pieces, p);
}

// map each labelled row back to its line in the blocks (lines filtered as when the
// features were made) and merge consecutive lines with the same plain label
static void BuildPieces(GrobidDoc* doc, const StrVec& firstTokens, Vec<Str>& labels, Vec<GrobidPiece>& pieces) {
    int nBlocks = len(doc->blocks);
    int blockIndex = 0;
    while (blockIndex < nBlocks && doc->blocks[blockIndex].nTokens == 0) blockIndex++;
    if (blockIndex == nBlocks) return;

    int indexLine = 0;
    int currentLineEndPos = 0;
    int currentLineStartPos = 0;
    int pointerABlock = blockIndex;
    int pointerAPos = doc->blocks[blockIndex].startToken;
    int lastPointerPos = 0;
    Str curPlainLabel;
    Str lastPlainLabel;
    bool haveLast = false;

    int lastTokenInd = -1;
    for (int i = nBlocks - 1; i >= 0; i--) {
        if (doc->blocks[i].endToken != -1) {
            lastTokenInd = doc->blocks[i].endToken;
            break;
        }
    }

    // one extra sentinel row so the last piece is closed inside the loop
    int nRows = len(labels);
    for (int p = 0; p <= nRows; p++) {
        Str rowToken = p < nRows ? firstTokens.At(p) : StrL("IgnoredToken");
        Str curLabel = p < nRows ? labels[p] : StrL("@IGNORED_LABEL@");

        bool haveLine = false;
        while (!haveLine && blockIndex < nBlocks) {
            GrobidBlock& block = doc->blocks[blockIndex];
            Str localText = BlockText(doc, blockIndex);
            StrVec lines;
            bool skipBlock = block.nTokens == 0 || IsBlank(localText);
            if (!skipBlock) {
                SplitLines(localText, lines);
                skipBlock = len(lines) == 0 || indexLine >= len(lines) || indexLine > kMaxLinesPerBlock;
            }
            if (skipBlock) {
                blockIndex++;
                indexLine = 0;
                if (blockIndex < nBlocks) currentLineStartPos = doc->blocks[blockIndex].startToken;
                continue;
            }

            Str line = lines.At(indexLine);
            indexLine++;
            if (len(JavaTrim(line)) == 0 || GrobidFilterLine(line)) continue;
            haveLine = true;
            if (currentLineStartPos > lastTokenInd) break;

            // skip leading spaces, then find the line starting with the row's first token
            while (IsSpaceOrTab(doc, currentLineStartPos) && currentLineStartPos != lastTokenInd) {
                currentLineStartPos++;
            }
            if (!str::StartsWith(rowToken, TokenText(doc, currentLineStartPos))) {
                while (currentLineStartPos < block.endToken) {
                    if (IsEol(doc, currentLineStartPos)) {
                        currentLineStartPos++;
                        while (IsSpaceOrTab(doc, currentLineStartPos) && currentLineStartPos != lastTokenInd) {
                            currentLineStartPos++;
                        }
                        if (currentLineStartPos != lastTokenInd &&
                            str::StartsWith(rowToken, TokenText(doc, currentLineStartPos))) {
                            break;
                        }
                    }
                    currentLineStartPos++;
                }
            }

            // last token of the line: the one before its "\n"
            currentLineEndPos = currentLineStartPos;
            while (currentLineEndPos < block.endToken) {
                if (IsEol(doc, currentLineEndPos)) {
                    currentLineEndPos--;
                    break;
                }
                currentLineEndPos++;
            }
        }

        curPlainLabel = PlainLabel(curLabel);
        if (blockIndex == nBlocks) break;

        // a new entity starts when the plain label changes
        if (haveLast && !str::Eq(curPlainLabel, lastPlainLabel)) {
            if (pointerAPos <= lastPointerPos && pointerAPos != -1) {
                AddPiece(pieces, lastPlainLabel, pointerABlock, pointerAPos, lastPointerPos);
            }
            pointerABlock = blockIndex;
            pointerAPos = currentLineStartPos;
        }

        lastPlainLabel = curPlainLabel;
        haveLast = true;
        lastPointerPos = currentLineEndPos;
        currentLineStartPos = currentLineEndPos + 2; // skip the EOL to the next line
    }

    if (blockIndex == nBlocks && haveLast && !str::Eq(curPlainLabel, lastPlainLabel)) {
        if (pointerAPos <= lastPointerPos && pointerAPos != -1) {
            AddPiece(pieces, lastPlainLabel, pointerABlock, pointerAPos, lastPointerPos);
        }
    }
}

// Segmentation.prepareDocument(): features -> CRF -> pieces. False when nothing was tagged.
bool GrobidSegment(GrobidDoc* doc, Vec<GrobidPiece>& pieces) {
    if (!doc || len(doc->blocks) == 0) return false;
    StrVec rows;
    BuildRows(doc, rows);
    if (len(rows) == 0) return false;

    CrfModel* model = GrobidModel(StrL("segmentation"));
    if (!model) return false;
    Vec<int> tags;
    if (!CrfTag(model, rows, tags) || len(tags) != len(rows)) return false;

    // GenericTaggerUtils.getTokensAndLabels(): first column and label of each row
    StrVec firstTokens;
    Vec<Str> labels; // model-owned strings: pieces keep pointing to them
    for (int i = 0; i < len(rows); i++) {
        Str row = rows.At(i);
        int sp = 0;
        while (sp < len(row) && row.s[sp] != ' ') sp++;
        firstTokens.Append(Str(row.s, sp));
        VecAppend(labels, CrfLabelName(model, tags[i]));
    }
    BuildPieces(doc, firstTokens, labels, pieces);
    return true;
}
