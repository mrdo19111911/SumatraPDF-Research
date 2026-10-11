/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"

#include "CrfModel.h"
#include "GrobidText.h"
#include "GrobidDoc.h"
#include "GrobidSegmentation.h"
#include "GrobidReferences.h"
#include "GrobidFulltext.h"

// Port of GROBID's FullTextParser: getBodyTextFeatured with FeaturesVectorFulltext.printVector,
// the fulltext model, LabelUtils.postProcessFulltextFixInvalidTableOrFigure and
// TaggingTokenClusteror. The "<body>" pieces and the "<annex>" pieces are two sequences.
//
//   tokens:   "1" " " "Introduction" "\n" "We" " " "study" ... "[" "12" "]" ...
//   labels:   I-<section> <section>     I-<paragraph> <paragraph> I-<citation_marker> ...
//   items:    {<section> "1 Introduction"} {<paragraph> "We study ..."} {<citation_marker> "[12]"}

// FullTextParser.NBBINS_POSITION
constexpr int kNbBinsPosition = 12;
// Document.MIN_DISTANCE: a graphic this close (vertically) to a block is "around" it
constexpr double kMinGraphicDistance = 100.0;

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

static TempStr RemoveChars(Str s, const char* chars) {
    str::Builder b;
    for (int i = 0; i < len(s); i++) {
        if (!strchr(chars, s.s[i])) b.AppendChar(s.s[i]);
    }
    return ToStrTemp(b);
}

//--- ReferenceMarkerMatcher (the parts the features use)

static bool IsAsciiUpper(char c) {
    return c >= 'A' && c <= 'Z';
}

static bool IsAsciiDigit(char c) {
    return c >= '0' && c <= '9';
}

// a byte that starts a letter: ASCII letter or any non-ASCII lead byte (\p{L}, approximately)
static bool IsLetterStart(u8 c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0xC0;
}

// AUTHOR_NAME_PATTERN "[A-Z][\p{L}]+" with find()
static bool FindAuthorName(Str s) {
    for (int i = 0; i + 1 < len(s); i++) {
        if (IsAsciiUpper(s.s[i]) && IsLetterStart((u8)s.s[i + 1])) return true;
    }
    return false;
}

// YEAR_PATTERN "[12][0-9]{3}[a-d]?" with find()
static bool FindYear(Str s) {
    for (int i = 0; i + 4 <= len(s); i++) {
        char c = s.s[i];
        if (c != '1' && c != '2') continue;
        if (IsAsciiDigit(s.s[i + 1]) && IsAsciiDigit(s.s[i + 2]) && IsAsciiDigit(s.s[i + 3])) return true;
    }
    return false;
}

static bool HasAsciiDigit(Str s) {
    for (int i = 0; i < len(s); i++) {
        if (IsAsciiDigit(s.s[i])) return true;
    }
    return false;
}

// Java's \s: [ \t\n\x0B\f\r]
static bool IsJavaSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static int SkipDigits(Str s, int i) {
    while (i < len(s) && IsAsciiDigit(s.s[i])) i++;
    return i;
}

// a dash at i: '-', and with allowMinus also U+2212 '−'; always U+2013 '–'. Its byte length or 0.
static int DashLen(Str s, int i, bool allowMinus) {
    if (i >= len(s)) return 0;
    if (s.s[i] == '-') return 1;
    Str rest(s.s + i, len(s) - i);
    if (str::StartsWith(rest, StrL("\xE2\x80\x93"))) return 3;
    if (allowMinus && str::StartsWith(rest, StrL("\xE2\x88\x92"))) return 3;
    return 0;
}

// the tail of NUMBERED_CITATION_PATTERN from i: "[ ]*(?:\d+[-–]\d+|\d+)\s*[\)\]]?" to the end
static bool MatchNumberedTail(Str s, int i) {
    while (i < len(s) && s.s[i] == ' ') i++;
    int d = SkipDigits(s, i);
    if (d == i) return false;
    int e = d;
    int dl = DashLen(s, d, false);
    if (dl > 0) {
        int d2 = SkipDigits(s, d + dl);
        if (d2 > d + dl) e = d2;
    }
    // both "\d+[-–]\d+" and "\d+" may be followed by the rest
    for (int end : {e, d}) {
        int j = end;
        while (j < len(s) && IsJavaSpace(s.s[j])) j++;
        if (j < len(s) && (s.s[j] == ')' || s.s[j] == ']')) j++;
        if (j == len(s)) return true;
    }
    return false;
}

// "(?:\d+[-−–]\d+,|\d+,[ ]*)*" then the tail, with backtracking
static bool MatchNumberedGroups(Str s, int i) {
    if (MatchNumberedTail(s, i)) return true;
    int d = SkipDigits(s, i);
    if (d == i) return false;
    if (d < len(s) && s.s[d] == ',') {
        int j = d + 1;
        while (j < len(s) && s.s[j] == ' ') j++;
        if (MatchNumberedGroups(s, j)) return true;
    }
    int dl = DashLen(s, d, true);
    if (dl > 0) {
        int d2 = SkipDigits(s, d + dl);
        if (d2 > d + dl && d2 < len(s) && s.s[d2] == ',' && MatchNumberedGroups(s, d2 + 1)) return true;
    }
    return false;
}

// NUMBERED_CITATION_PATTERN.matches()
static bool MatchesNumbered(Str s) {
    int i = 0;
    if (i < len(s) && (s.s[i] == '(' || s.s[i] == '[')) {
        int j = i + 1;
        while (j < len(s) && IsJavaSpace(s.s[j])) j++;
        if (MatchNumberedGroups(s, j)) return true;
    }
    while (i < len(s) && IsJavaSpace(s.s[i])) i++;
    return MatchNumberedGroups(s, i);
}

// isNumberedCitationReference: the pattern alone, or with an author name ("Naze et al. [5]");
// find() of the pattern needs only one digit
static bool IsNumberedCitation(Str t) {
    t = JavaTrim(t);
    return MatchesNumbered(t) || (HasAsciiDigit(t) && FindAuthorName(t));
}

static bool IsAuthorCitationStyle(Str t) {
    t = JavaTrim(t);
    return FindYear(t) && FindAuthorName(t);
}

// StringUtils.stripStart / stripEnd with a set of ASCII chars
static Str StripChars(Str s, const char* lead, const char* trail) {
    int start = 0;
    int end = len(s);
    while (start < end && strchr(lead, s.s[start])) start++;
    while (end > start && strchr(trail, s.s[end - 1])) end--;
    return Str(s.s + start, end - start);
}

// BiblioItem.getFirstAuthorSurname from "Surname, F.; Surname, F."
static Str FirstAuthorSurname(Str authors) {
    Str first = authors;
    int semi = str::IndexOfChar(authors, ';');
    if (semi >= 0) first = Str(authors.s, semi);
    first = JavaTrim(first);
    int comma = str::IndexOfChar(first, ',');
    if (comma >= 0) return JavaTrim(Str(first.s, comma));
    int sp = str::LastIndexOfChar(first, ' ');
    if (sp >= 0) return Str(first.s + sp + 1, len(first) - sp - 1);
    return first;
}

// what getBodyTextFeatured takes from doc.getBibDataSets()
struct RefMarkers {
    StrVec labels;
    StrVec firstAuthors;
    const char* calloutType = "UNKNOWN";
};

static bool StrVecHas(const StrVec& v, Str s) {
    for (int i = 0; i < len(v); i++) {
        if (str::Eq(v.At(i), s)) return true;
    }
    return false;
}

// CitationParser.processingReferenceSection: entries the citation model accepts, their
// cleaned labels and first authors; the callout type is the majority label style
static void BuildRefMarkers(GrobidDoc* doc, const Vec<GrobidPiece>& pieces, RefMarkers& rm) {
    Vec<GrobidRef*> refs;
    GrobidSegmentReferences(doc, pieces, refs);
    int nBib = 0;
    int nbNumbType = 0;
    int nbAuthorType = 0;
    for (GrobidRef* ref : refs) {
        GrobidParseCitation(ref->raw, ref);
        // BiblioItem.rejectAsReference: no title, venue, author or doi
        bool reject = len(ref->title) == 0 && len(ref->venue) == 0 && len(ref->authors) == 0 && len(ref->doi) == 0;
        if (reject) continue;
        nBib++;
        Str surname = FirstAuthorSurname(ref->authors);
        if (len(surname) > 0) rm.firstAuthors.Append(surname);
        if (len(ref->label) == 0) continue;

        Str label = StripChars(ref->label, "([{<,. \n", ")}]>,.: \n");
        rm.labels.Append(label);
        if (IsNumberedCitation(label)) {
            nbNumbType++;
        } else if (IsAuthorCitationStyle(label)) {
            nbAuthorType++;
        }
    }
    for (GrobidRef* ref : refs) GrobidRefFree(ref);

    if (nbNumbType > nBib / 2) {
        rm.calloutType = "NUMBER";
    } else if (nbAuthorType > nBib / 2) {
        rm.calloutType = "AUTHOR";
    }
}

//--- features (getBodyTextFeatured)

struct FtFeatures {
    int token = 0; // doc token index
    Str string;
    const char* blockStatus = nullptr;
    const char* lineStatus = nullptr;
    const char* alignmentStatus = nullptr;
    const char* fontStatus = nullptr;
    const char* fontSize = nullptr;
    bool bold = false;
    bool italic = false;
    const char* capitalisation = nullptr;
    const char* digit = nullptr;
    bool singleChar = false;
    const char* punctType = nullptr;
    int relativeDocumentPosition = -1;
    int relativePagePosition = -1;
    bool bitmapAround = false;
    const char* calloutType = nullptr;
    bool calloutKnown = false;
    bool superscript = false;
};

// one fulltext sequence: rows, the token each row is for, and every token walked (layoutTokens)
struct FtSequence {
    StrVec rows;
    Vec<int> rowTokens;
    Vec<int> layoutTokens;
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

// FeaturesVectorFulltext.printVector() without the trailing newline: 27 columns, no label
static TempStr PrintVectorTemp(const FtFeatures& f) {
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
    bool allDigit = f.digit && IsStr(Str(f.digit), "ALLDIGIT");
    AppendCol(b, allDigit ? "NOCAPS" : f.capitalisation);
    AppendCol(b, f.digit);
    AppendFlag(b, f.singleChar);
    AppendCol(b, f.punctType);
    AppendCol(b, fmt("%d", f.relativeDocumentPosition));
    AppendCol(b, fmt("%d", f.relativePagePosition));
    AppendFlag(b, f.bitmapAround);
    AppendCol(b, f.calloutType ? f.calloutType : "UNKNOWN");
    AppendFlag(b, f.calloutKnown);
    AppendFlag(b, f.superscript);
    return str::DupTemp(ToStr(b));
}

// Document.getConnectedGraphics(): bitmap / vector graphics vertically near the block
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

// the block of a piece's end pointer: the last block starting at or before its end token
static int EndBlock(GrobidDoc* doc, const GrobidPiece& piece) {
    int b = piece.block;
    while (b + 1 < len(doc->blocks)) {
        int start = doc->blocks[b + 1].startToken;
        if (start < 0 || start > piece.endToken) break;
        b++;
    }
    return b;
}

// getFulltextLength: chars of the pieces' tokens
static int FulltextLength(GrobidDoc* doc, const Vec<GrobidPiece>& parts) {
    int n = 0;
    for (const GrobidPiece& p : parts) {
        for (int i = p.startToken; i <= p.endToken && i < len(doc->tokens); i++) {
            n += Utf16Len(doc->tokens[i].text);
        }
    }
    return n;
}

static bool IsMarkerKnown(const RefMarkers& rm, Str text) {
    Str t = JavaTrim(text);
    return StrVecHas(rm.labels, t) || StrVecHas(rm.firstAuthors, t);
}

// getBodyTextFeatured over the pieces with one label ("<body>" or "<annex>"). False when there are none.
static bool FulltextRows(GrobidDoc* doc, const Vec<GrobidPiece>& pieces, Str label, const RefMarkers& rm,
                         FtSequence& seq) {
    Vec<GrobidPiece> parts;
    for (const GrobidPiece& p : pieces) {
        if (str::Eq(p.label, label)) VecAppend(parts, p);
    }
    if (len(parts) == 0 || len(doc->blocks) == 0) return false;

    Vec<GrobidToken>& toks = doc->tokens;
    Vec<FtFeatures> feats;
    Str currentFont;
    bool haveFont = false;
    int currentFontSize = -1;
    int mm = 0; // page position
    int nn = 0; // document position
    double lineStartX = NAN;
    bool indented = false;
    int currentPage = 0;
    int fulltextLength = FulltextLength(doc, parts);

    for (const GrobidPiece& piece : parts) {
        int startBlock = piece.block;
        int endBlock = EndBlock(doc, piece);
        for (int blockIndex = startBlock; blockIndex <= endBlock; blockIndex++) {
            GrobidBlock& block = doc->blocks[blockIndex];
            double pageHeight = doc->pages[block.page - 1].height;
            if (block.page != currentPage) {
                currentPage = block.page;
                mm = 0;
            }

            bool previousNewline = false;
            bool endblock = false;

            // Block.getText(), null for a block without tokens
            Str localText = block.nTokens == 0 ? Str() : GrobidBlockTextTemp(doc, blockIndex);
            if (GrobidFilterLine(localText)) continue;

            bool graphicBitmap = false;
            bool graphicVector = false;
            ConnectedGraphics(doc, block, &graphicBitmap, &graphicVector);

            int nTokens = block.nTokens;
            int n = 0; // token position in the block
            if (blockIndex == startBlock) n = piece.startToken - block.startToken;
            int lastPos = nTokens;
            if (blockIndex == endBlock) {
                lastPos = piece.endToken - block.startToken + 1;
                if (lastPos > nTokens) lastPos = nTokens;
            }

            bool isFirstBlockToken = true;
            for (; n < lastPos; n++) {
                int ti = block.firstToken + n;
                GrobidToken& token = toks[ti];
                VecAppend(seq.layoutTokens, ti);

                Str text = token.text;
                if (len(text) == 0) continue;
                text = RemoveChars(text, " ");
                if (len(text) == 0) {
                    mm++;
                    nn++;
                    continue;
                }
                bool newline = false;
                if (IsStr(text, "\n")) {
                    previousNewline = true;
                    mm++;
                    nn++;
                    continue;
                }

                text = RemoveChars(text, " \n");
                if (GrobidFilterLine(text)) continue;

                // indentation: line start moved more than one character width
                if (previousNewline) {
                    newline = true;
                    previousNewline = false;
                    if (len(feats) > 0) {
                        double previousLineStartX = lineStartX;
                        lineStartX = token.x;
                        double characterWidth = (double)token.w / Utf16Len(text);
                        if (!isnan(previousLineStartX)) {
                            if (previousLineStartX - lineStartX > characterWidth) {
                                indented = false;
                            } else if (lineStartX - previousLineStartX > characterWidth) {
                                indented = true;
                            }
                        }
                    }
                }

                FtFeatures f;
                f.token = ti;
                f.string = text;
                f.bitmapAround = graphicBitmap;
                FtFeatures* prev = len(feats) > 0 ? &feats[len(feats) - 1] : nullptr;

                // be sure that the previous token closes a line, unless it starts one
                if (newline) {
                    f.lineStatus = "LINESTART";
                    lineStartX = token.x;
                    if (prev && !IsStr(Str(prev->lineStatus), "LINESTART")) prev->lineStatus = "LINEEND";
                }

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

                f.alignmentStatus = indented ? "LINEINDENT" : "ALIGNEDLEFT";

                if (isFirstBlockToken) {
                    f.lineStatus = "LINESTART";
                    if (prev && !IsStr(Str(prev->lineStatus), "LINESTART")) prev->lineStatus = "LINEEND";
                    lineStartX = token.x;
                    f.blockStatus = "BLOCKSTART";
                } else if (n == nTokens - 1) {
                    f.lineStatus = "LINEEND";
                    previousNewline = true;
                    f.blockStatus = "BLOCKEND";
                    endblock = true;
                } else {
                    // look ahead to the end of the line: the next non-empty token
                    bool endline = false;
                    bool endloop = false;
                    for (int ii = 1; n + ii < nTokens && !endloop; ii++) {
                        Str toto = toks[block.firstToken + n + ii].text;
                        if (IsStr(toto, "\n")) {
                            endline = true;
                            endloop = true;
                        } else if (len(toto) != 0 && !str::StartsWith(toto, StrL("@IMAGE")) &&
                                   !str::StartsWith(toto, StrL("@PAGE")) && !str::Contains(text, StrL(".pbm")) &&
                                   !str::Contains(text, StrL(".svg")) && !str::Contains(text, StrL(".png")) &&
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

                if (Utf16Len(text) == 1) f.singleChar = true;
                if (GrobidIsUpperFirst(text)) f.capitalisation = "INITCAP";
                if (GrobidAllCapital(text)) f.capitalisation = "ALLCAP";
                if (GrobidHasDigit(text)) f.digit = "CONTAINSDIGITS";
                if (GrobidAllDigits(text)) f.digit = "ALLDIGIT";

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
                if (!f.punctType) f.punctType = "NOPUNCT";

                f.relativeDocumentPosition = GrobidLinearScaling(nn, fulltextLength, kNbBinsPosition);
                int pagePos = GrobidLinearScaling((double)token.y, pageHeight, kNbBinsPosition);
                if (pagePos > kNbBinsPosition) pagePos = kNbBinsPosition;
                f.relativePagePosition = pagePos;

                f.calloutType = rm.calloutType;
                if (IsMarkerKnown(rm, text)) f.calloutKnown = true;
                f.superscript = token.superscript;

                // the last character of a block can be a space or EOL: close the previous block
                if (prev && IsStr(Str(f.blockStatus), "BLOCKSTART") && IsStr(Str(prev->blockStatus), "BLOCKIN")) {
                    prev->blockStatus = "BLOCKEND";
                    prev->lineStatus = "LINEEND";
                }

                mm += Utf16Len(text);
                nn += Utf16Len(text);
                VecAppend(feats, f);
                isFirstBlockToken = false;
            }
        }
    }

    for (FtFeatures& f : feats) {
        seq.rows.Append(PrintVectorTemp(f));
        VecAppend(seq.rowTokens, f.token);
    }
    return true;
}

// Fulltext feature rows: the body sequence, then a blank line and the annex sequence if any.
TempStr GrobidFulltextFeaturesTemp(GrobidDoc* doc, const Vec<GrobidPiece>& pieces) {
    if (!doc) return {};
    RefMarkers rm;
    BuildRefMarkers(doc, pieces, rm);

    str::Builder out;
    const char* parts[] = {"<body>", "<annex>"};
    for (const char* part : parts) {
        FtSequence seq;
        if (!FulltextRows(doc, pieces, Str(part), rm, seq) || len(seq.rows) == 0) continue;
        if (len(out) > 0) out.AppendChar('\n');
        for (int i = 0; i < len(seq.rows); i++) {
            out.Append(seq.rows[i]);
            out.AppendChar('\n');
        }
    }
    return str::DupTemp(ToStr(out));
}

//--- labels -> items

// GenericTaggerUtils.getPlainLabel(): "I-<section>" -> "<section>"
static Str PlainLabel(Str label) {
    if (str::StartsWith(label, StrL("I-")) || str::StartsWith(label, StrL("B-"))) {
        return Str(label.s + 2, len(label) - 2);
    }
    return label;
}

// LabelUtils.postProcessFulltextFixInvalidTableOrFigure: an I-<figure> (I-<table>) right after
// another one continues it
static void FixInvalidTableOrFigure(Vec<Str>& labels) {
    Str prev;
    for (int i = 0; i < len(labels); i++) {
        Str label = labels[i];
        bool fig = IsStr(label, "I-<figure>") && IsStr(prev, "I-<figure>");
        bool tab = IsStr(label, "I-<table>") && IsStr(prev, "I-<table>");
        if (fig || tab) labels[i] = PlainLabel(label);
        prev = label;
    }
}

// one row's tokens in seq.layoutTokens: [from, to)
struct FtContainer {
    Str label; // plain
    bool beginning = false;
    int from = 0;
    int to = 0;
};

// TaggingTokenSynchronizer.next(): the layout tokens up to the row's token, then trailing
// spaces / newlines. A token that matches nothing is kept with the row (GROBID throws).
static void SyncNext(GrobidDoc* doc, const FtSequence& seq, int& ptr, Str rowText, FtContainer& c) {
    c.from = ptr;
    int n = len(seq.layoutTokens);
    while (ptr < n) {
        Str t = doc->tokens[seq.layoutTokens[ptr]].text;
        ptr++;
        if (IsStr(t, "\n") || IsStr(t, " ") || len(t) == 0) continue;
        if (str::Eq(RemoveChars(t, " \n"), rowText)) break;
        logf("GrobidParseFulltext: token '%s' does not match row '%s'\n", t, rowText);
    }
    while (ptr < n) {
        Str t = doc->tokens[seq.layoutTokens[ptr]].text;
        if (!IsStr(t, "\n") && !IsStr(t, " ")) break;
        ptr++;
    }
    c.to = ptr;
}

static bool IsSpaceToken(Str t) {
    return IsStr(t, " ") || IsStr(t, "\n") || len(t) == 0;
}

// one item from the tokens of a cluster: text, token range, page and box on its first page
static GrobidBodyItem* NewItem(GrobidDoc* doc, const FtSequence& seq, Str label, bool annex, int from, int to) {
    auto item = new GrobidBodyItem();
    item->label = label;
    item->annex = annex;
    item->startToken = seq.layoutTokens[from];
    item->endToken = seq.layoutTokens[to - 1];

    str::Builder text;
    bool haveBox = false;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (int i = from; i < to; i++) {
        GrobidToken& t = doc->tokens[seq.layoutTokens[i]];
        text.Append(t.text);
        if (IsSpaceToken(t.text)) continue;
        if (item->page == 0) item->page = t.page;
        if (t.page != item->page || t.w <= 0) continue;
        if (!haveBox) {
            x0 = t.x;
            y0 = t.y;
            x1 = t.x + t.w;
            y1 = t.y + t.h;
            haveBox = true;
            continue;
        }
        x0 = fmin(x0, (double)t.x);
        y0 = fmin(y0, (double)t.y);
        x1 = fmax(x1, (double)(t.x + t.w));
        y1 = fmax(y1, (double)(t.y + t.h));
    }
    item->text = str::Dup(JavaTrim(ToStr(text)));
    if (haveBox) {
        item->x = (float)x0;
        item->y = (float)y0;
        item->w = (float)(x1 - x0);
        item->h = (float)(y1 - y0);
    }
    return item;
}

// labelAndCapture + TaggingTokenClusteror.cluster(): a new cluster on "I-" or a label change
static bool TagSequence(GrobidDoc* doc, const FtSequence& seq, bool annex, Vec<GrobidBodyItem*>& items) {
    if (len(seq.rows) == 0) return true;
    CrfModel* model = GrobidModel(StrL("fulltext"));
    if (!model) return false;
    Vec<int> tags;
    if (!CrfTag(model, seq.rows, tags) || len(tags) != len(seq.rows)) return false;

    Vec<Str> labels;
    for (int t : tags) VecAppend(labels, CrfLabelName(model, t));
    FixInvalidTableOrFigure(labels);

    int ptr = 0;
    Str curLabel;
    int clusterFrom = -1;
    int clusterTo = -1;
    for (int i = 0; i < len(seq.rows); i++) {
        FtContainer c;
        c.label = PlainLabel(labels[i]);
        c.beginning = len(c.label) != len(labels[i]); // GenericTaggerUtils.isBeginningOfEntity: "I-" or "B-"
        // the row's first column is its token
        Str row = seq.rows.At(i);
        int sp = str::IndexOfChar(row, ' ');
        SyncNext(doc, seq, ptr, Str(row.s, sp < 0 ? len(row) : sp), c);

        bool newCluster = clusterFrom < 0 || c.beginning || !str::Eq(c.label, curLabel);
        if (newCluster) {
            if (clusterFrom >= 0) VecAppend(items, NewItem(doc, seq, curLabel, annex, clusterFrom, clusterTo));
            clusterFrom = c.from;
            curLabel = c.label;
        }
        clusterTo = c.to;
    }
    if (clusterFrom >= 0 && clusterTo > clusterFrom) {
        VecAppend(items, NewItem(doc, seq, curLabel, annex, clusterFrom, clusterTo));
    }
    return true;
}

// Labels the body and annex with the fulltext model; one item per cluster of tokens with one
// label, in document order. False when tagging fails.
bool GrobidParseFulltext(GrobidDoc* doc, const Vec<GrobidPiece>& pieces, Vec<GrobidBodyItem*>& items) {
    if (!doc) return false;
    RefMarkers rm;
    BuildRefMarkers(doc, pieces, rm);

    const char* parts[] = {"<body>", "<annex>"};
    for (int i = 0; i < 2; i++) {
        FtSequence seq;
        if (!FulltextRows(doc, pieces, Str(parts[i]), rm, seq)) continue;
        if (!TagSequence(doc, seq, i == 1, items)) return false;
    }
    return true;
}

void GrobidBodyItemFree(GrobidBodyItem* item) {
    if (!item) return;
    str::Free(item->text);
    delete item;
}
