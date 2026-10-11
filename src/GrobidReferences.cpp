/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"

#include "CrfModel.h"
#include "GrobidText.h"
#include "GrobidDoc.h"
#include "GrobidSegmentation.h"
#include "GrobidReferences.h"

// Port of GROBID's ReferenceSegmenterParser (getReferencesSectionFeatured,
// getExtractionResult) with FeaturesVectorReferenceSegmenter.printVector and
// TaggingTokenSynchronizer. One CRF row per non-space token of the
// "<references>" pieces; labels I-<label>/<label>, I-<reference>/<reference>, <other>.
//
//   tokens:  "[1]" " " "A." " " "Author," ... "\n" "[2]" ...
//   labels:  I-<label>   I-<reference> <reference> ... I-<label>
//   entries: {label "[1]", raw "A. Author, ..."}, {label "[2]", ...}

// ReferenceSegmenterParser.LINESCALE: projection scale for line length and position
constexpr int kLineScale = 10;
// printVector caps the punctuation profile length
constexpr int kMaxProfileLen = 10;

static bool IsStr(Str s, const char* lit) {
    return str::Eq(s, Str(lit));
}

static bool IsNewlineTok(Str s) {
    return IsStr(s, "\n") || IsStr(s, "\r");
}

static bool IsSpaceTok(Str s) {
    return IsStr(s, " ") || IsStr(s, "\t");
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

// StringUtils.isNotBlank
static bool IsNotBlank(Str s) {
    for (int i = 0; i < len(s); i++) {
        char c = s.s[i];
        bool ws = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v' || (c >= 0x1c && c <= 0x1f);
        if (!ws) return true;
    }
    return false;
}

// FeatureFactory.linearScaling(int, int, int): float arithmetic, as Java picks the int overload
static int LinearScalingInt(int pos, int total, int nbBins) {
    if (pos >= total) return nbBins;
    if (pos <= 0) return 0;
    float rel = (float)pos / (float)total;
    float rel2 = rel * (float)nbBins;
    return (int)rel2;
}

// FeaturesVectorReferenceSegmenter
struct RefSegFeatures {
    Str string;
    const char* blockStatus = nullptr;
    const char* lineStatus = nullptr;
    const char* alignmentStatus = nullptr;
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
    bool http = false;
    int relativePosition = -1;
    int lineLength = 0;
    Str punctuationProfile; // null when s == nullptr
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

// printVector() without the trailing newline; firstName and locationName are never set
static TempStr PrintVectorTemp(const RefSegFeatures& f) {
    Str s = f.string;
    str::Builder b;
    b.Append(s);
    AppendCol(b, GrobidLowerTemp(s));
    for (int i = 1; i <= 4; i++) AppendCol(b, GrobidPrefixTemp(s, i));
    for (int i = 1; i <= 4; i++) AppendCol(b, GrobidSuffixTemp(s, i));
    AppendCol(b, f.lineStatus);
    AppendCol(b, f.alignmentStatus);
    bool allDigit = f.digit && str::Eq(Str(f.digit), StrL("ALLDIGIT"));
    AppendCol(b, allDigit ? "NOCAPS" : f.capitalisation);
    AppendCol(b, f.digit);
    AppendFlag(b, f.singleChar);
    AppendFlag(b, f.properName);
    AppendFlag(b, f.commonName);
    AppendFlag(b, false); // firstName
    AppendFlag(b, false); // locationName
    AppendFlag(b, f.year);
    AppendFlag(b, f.month);
    AppendFlag(b, f.http);
    AppendCol(b, f.punctType);
    AppendCol(b, fmt("%d", f.relativePosition));
    AppendCol(b, fmt("%d", f.lineLength));
    AppendCol(b, f.blockStatus);
    if (!f.punctuationProfile.s || len(f.punctuationProfile) == 0) {
        AppendCol(b, StrL("no"));
    } else {
        int n = Utf16Len(f.punctuationProfile);
        AppendCol(b, fmt("%d", n > kMaxProfileLen ? kMaxProfileLen : n));
    }
    AppendCol(b, StrL("0")); // label
    return str::DupTemp(ToStr(b));
}

static void CollectRefPieces(const Vec<GrobidPiece>& pieces, Vec<GrobidPiece>& out) {
    for (const GrobidPiece& p : pieces) {
        if (str::Eq(p.label, StrL("<references>"))) VecAppend(out, p);
    }
}

// getReferencesSectionFeatured: one row per token (rowTokens = its doc token index),
// refTokens = every token of the pieces (tokenizationsReferences). False when there is nothing to feature.
static bool RefSegRows(GrobidDoc* doc, const Vec<GrobidPiece>& pieces, StrVec& rows, Vec<int>& rowTokens,
                       Vec<int>& refTokens) {
    Vec<GrobidPiece> refPieces;
    CollectRefPieces(pieces, refPieces);
    if (len(refPieces) == 0) return false;
    if (len(doc->blocks) == 0) return false;

    Vec<GrobidToken>& toks = doc->tokens;
    Vec<GrobidBlock>& blocks = doc->blocks;
    int nToks = len(toks);

    // max line length over the pieces; also the tokens the synchronizer walks
    int maxLineLength = 1;
    int currentLineLength = 0;
    for (GrobidPiece& piece : refPieces) {
        for (int i = piece.startToken; i <= piece.endToken && i < nToks; i++) {
            VecAppend(refTokens, i);
            currentLineLength += Utf16Len(toks[i].text);
            if (!IsNewlineTok(toks[i].text)) continue;
            if (currentLineLength > maxLineLength) maxLineLength = currentLineLength;
            currentLineLength = 0;
        }
    }

    bool havePrevious = false; // previousFeatures != null
    double lineStartX = NAN;
    bool indented = false;
    bool newline = false;

    for (GrobidPiece& piece : refPieces) {
        int nn = 0; // position in the line
        int tokenIndex = 0;
        int blockIndex = piece.block;
        GrobidBlock* block = nullptr;
        bool previousNewline = true;
        currentLineLength = 0;
        Str currentLineProfile; // null until computed for the line

        for (int n = piece.startToken; n <= piece.endToken && n < nToks; n++) {
            Str text = toks[n].text;

            // set corresponding block
            if (block && n > block->endToken) {
                blockIndex++;
                tokenIndex = 0;
                currentLineLength = 0;
                currentLineProfile = {};
            }

            bool startblock = false;
            bool endblock = false;
            if (blockIndex < len(blocks)) {
                block = &blocks[blockIndex];
                if (n == block->startToken) {
                    startblock = true;
                } else if (n == block->endToken) {
                    endblock = true;
                }
            } else {
                block = nullptr;
            }

            if (IsNewlineTok(text)) {
                previousNewline = true;
                nn = 0;
                currentLineLength = 0;
                currentLineProfile = {};
                continue;
            }
            newline = false;
            nn += Utf16Len(text);

            if (IsSpaceTok(text)) {
                nn++;
                continue;
            }
            if (len(JavaTrim(text)) == 0) continue;

            // find the token in the block's tokens; a failed search leaves the last one examined
            GrobidToken* token = nullptr;
            if (block) {
                for (int i = tokenIndex; i < block->nTokens; i++) {
                    token = &toks[block->firstToken + i];
                    if (str::Eq(text, token->text)) {
                        tokenIndex = i;
                        break;
                    }
                }
            }

            // indentation: line start moved more than one character width
            if (previousNewline) {
                newline = true;
                previousNewline = false;
                if (token && havePrevious) {
                    double previousLineStartX = lineStartX;
                    lineStartX = token->x;
                    double characterWidth = (double)token->w / Utf16Len(token->text);
                    if (!isnan(previousLineStartX)) {
                        if (lineStartX - previousLineStartX > characterWidth) {
                            indented = true;
                        } else if (previousLineStartX - lineStartX > characterWidth) {
                            indented = false;
                        }
                    }
                }
            }

            if (GrobidFilterLine(text)) continue;

            RefSegFeatures f;
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

            if (n == 0 || previousNewline) {
                f.lineStatus = "LINESTART";
                if (n == 0) f.blockStatus = "BLOCKSTART";
                nn = 0;
            }

            f.alignmentStatus = indented ? "LINEINDENT" : "ALIGNEDLEFT";

            // look ahead to the end of the line (over the whole document, as GROBID)
            bool endline = true;
            bool endloop = false;
            str::Builder accumulated;
            accumulated.Append(text);
            for (int ii = 1; n + ii < nToks && !endloop; ii++) {
                Str tok = toks[n + ii].text;
                if (!currentLineProfile.s) accumulated.Append(tok);
                bool lineDone = false;
                if (IsNewlineTok(tok)) {
                    lineDone = true;
                } else if (!IsSpaceTok(tok)) {
                    endline = false;
                } else if (GrobidFilterLine(tok)) {
                    lineDone = true;
                }
                if (lineDone) {
                    endloop = true;
                    if (currentLineLength == 0) currentLineLength = Utf16Len(ToStr(accumulated));
                    if (!currentLineProfile.s) {
                        currentLineProfile = GrobidPunctProfileTemp(ToStr(accumulated));
                        if (!currentLineProfile.s) currentLineProfile = str::DupTemp(StrL(""));
                    }
                }

                if (n + ii >= nToks - 1) {
                    endblock = true;
                    endline = true;
                }
                if (endline && block && n + ii == block->endToken) endblock = true;
            }

            if (!endline && !newline) {
                f.lineStatus = "LINEIN";
            } else if (!newline) {
                f.lineStatus = "LINEEND";
                previousNewline = true;
            }

            if (startblock) f.blockStatus = "BLOCKSTART";
            if (!endblock && !f.blockStatus) {
                f.blockStatus = "BLOCKIN";
            } else if (!f.blockStatus) {
                f.blockStatus = "BLOCKEND";
            }

            if (Utf16Len(text) == 1) f.singleChar = true;
            if (GrobidIsUpperFirst(text)) f.capitalisation = "INITCAP";
            if (GrobidAllCapital(text)) f.capitalisation = "ALLCAP";
            if (GrobidHasDigit(text)) f.digit = "CONTAINSDIGITS";

            TempStr lower = GrobidLowerTemp(text);
            if (GrobidInDictionary(GrobidLowerTemp(JavaTrim(text)))) f.commonName = true;
            if (GrobidIsFirstName(lower) || GrobidIsLastName(lower)) f.properName = true;
            if (GrobidIsMonth(lower)) f.month = true;
            if (GrobidAllDigits(text)) f.digit = "ALLDIGIT";
            if (GrobidIsYear(text)) f.year = true;
            if (GrobidIsHttp(text)) f.http = true;
            if (token && token->bold) f.bold = true;
            if (token && token->italic) f.italic = true;

            if (!f.capitalisation) f.capitalisation = "NOCAPS";
            if (!f.digit) f.digit = "NODIGIT";
            if (!f.punctType) f.punctType = "NOPUNCT";

            f.lineLength = LinearScalingInt(currentLineLength, maxLineLength, kLineScale);
            f.relativePosition = LinearScalingInt(nn, currentLineLength, kLineScale);
            f.punctuationProfile = currentLineProfile;

            // GROBID prints each row one token late; the output is the same
            rows.Append(PrintVectorTemp(f));
            VecAppend(rowTokens, n);
            havePrevious = true;
        }
    }
    return true;
}

// Reference-segmenter feature rows, one per line (printVector output).
TempStr GrobidRefSegmenterFeaturesTemp(GrobidDoc* doc, const Vec<GrobidPiece>& pieces) {
    StrVec rows;
    Vec<int> rowTokens;
    Vec<int> refTokens;
    if (!doc || !RefSegRows(doc, pieces, rows, rowTokens, refTokens)) return {};

    str::Builder out;
    for (int i = 0; i < len(rows); i++) {
        out.Append(rows[i]);
        out.AppendChar('\n');
    }
    return str::DupTemp(ToStr(out));
}

// LabeledTokensContainer
struct RefSegContainer {
    Str token;
    Str plainLabel;
    bool beginning = false;
    bool trailingSpace = false;
    bool trailingNewLine = false;
};

// TaggingTokenSynchronizer.next(): consume layout tokens up to the one matching the
// row token, then trailing spaces / newlines. False when they get out of sync.
static bool SyncNext(GrobidDoc* doc, const Vec<int>& refTokens, int& ptr, Str token, RefSegContainer& c) {
    bool stop = false;
    while (!stop && ptr < len(refTokens)) {
        Str t = doc->tokens[refTokens[ptr]].text;
        if (IsStr(t, "\n")) {
            c.trailingNewLine = true;
        } else if (IsStr(t, " ")) {
            c.trailingSpace = true;
        } else {
            // tokOriginal.replaceAll("[ \n]", "")
            str::Builder stripped;
            for (int i = 0; i < len(t); i++) {
                if (t.s[i] != ' ' && t.s[i] != '\n') stripped.AppendChar(t.s[i]);
            }
            if (str::Eq(ToStr(stripped), token)) {
                stop = true;
            } else if (len(t) != 0) {
                return false;
            }
        }
        ptr++;
    }

    while (ptr < len(refTokens)) {
        Str t = doc->tokens[refTokens[ptr]].text;
        if (IsStr(t, "\n")) {
            c.trailingNewLine = true;
        } else if (IsStr(t, " ")) {
            c.trailingSpace = true;
        } else {
            break;
        }
        ptr++;
    }
    return true;
}

// LabeledReferenceResult: label trimmed (empty for null), text trimmed, keeping its "\n" line
// ends: the citation parser dehyphenates across them
static GrobidRef* NewRef(Str label, Str reference) {
    auto ref = new GrobidRef();
    if (len(label) > 0) ref->label = str::Dup(JavaTrim(label));
    ref->raw = str::Dup(JavaTrim(reference));
    return ref;
}

// getExtractionResult: an I- label or I- reference starts a new entry once the
// current one has reference text; <other> tokens are dropped.
static bool ExtractRefs(GrobidDoc* doc, CrfModel* model, const StrVec& rows, const Vec<int>& rowTokens,
                        const Vec<int>& refTokens, const Vec<int>& labels, Vec<GrobidRef*>& refs) {
    str::Builder reference;
    str::Builder referenceLabel;
    int ptr = 0;
    int n = len(rows);
    for (int i = 0; i < n; i++) {
        RefSegContainer c;
        c.token = doc->tokens[rowTokens[i]].text;
        Str label = CrfLabelName(model, labels[i]);
        c.beginning = str::StartsWith(label, StrL("I-"));
        c.plainLabel = c.beginning ? Str(label.s + 2, len(label) - 2) : label;
        if (!SyncNext(doc, refTokens, ptr, c.token, c)) {
            logf("GrobidSegmentReferences: tokens out of sync at row %d '%s'\n", i, c.token);
            return false;
        }

        bool isLabel = str::Eq(c.plainLabel, StrL("<label>"));
        bool isReference = str::Eq(c.plainLabel, StrL("<reference>"));
        if ((isLabel || isReference) && c.beginning && IsNotBlank(ToStr(reference))) {
            VecAppend(refs, NewRef(ToStr(referenceLabel), ToStr(reference)));
            reference.Reset();
            referenceLabel.Reset();
        }
        if (isLabel) {
            referenceLabel.Append(c.token);
            if (c.trailingSpace || c.trailingNewLine) referenceLabel.AppendChar(' ');
        } else if (isReference) {
            reference.Append(c.token);
            if (c.trailingSpace) reference.AppendChar(' ');
            if (c.trailingNewLine) reference.AppendChar('\n');
        }

        // last one
        if (i == n - 1 && IsNotBlank(ToStr(reference))) {
            VecAppend(refs, NewRef(ToStr(referenceLabel), ToStr(reference)));
        }
    }
    return true;
}

// Splits the "<references>" pieces into entries (label + raw text). False when
// there is no references piece or tagging fails.
bool GrobidSegmentReferences(GrobidDoc* doc, const Vec<GrobidPiece>& pieces, Vec<GrobidRef*>& refs) {
    StrVec rows;
    Vec<int> rowTokens;
    Vec<int> refTokens;
    if (!doc || !RefSegRows(doc, pieces, rows, rowTokens, refTokens)) return false;
    if (len(rows) == 0) return true;

    CrfModel* model = GrobidModel(StrL("reference-segmenter"));
    if (!model) return false;
    Vec<int> labels;
    if (!CrfTag(model, rows, labels) || len(labels) != len(rows)) return false;
    return ExtractRefs(doc, model, rows, rowTokens, refTokens, labels, refs);
}
