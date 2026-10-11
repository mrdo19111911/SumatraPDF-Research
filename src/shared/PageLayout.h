/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Structured text of one page for layout analysis (GROBID), shaped like pdfalto's
// ALTO: blocks > lines > words (grouped with pdfalto's rules), bitmap images and
// vector paths. Coordinates are PDF points relative to the page's top-left, y down.
// (Not EngineBase's PageLayout, which is the preferred page display mode.)

class EngineBase;

// PDF FontDescriptor /Flags bits
constexpr int kPdfFontFlagItalic = 1 << 6;
constexpr int kPdfFontFlagForceBold = 1 << 18;

struct PageLayoutWord {
    Str text; // UTF-8, never contains a space
    Str font; // font name as in the PDF (may carry a subset prefix); empty if unknown
    float fontSize = 0;
    int pdfFontFlags = -1; // FontDescriptor /Flags, -1 when not a resolvable PDF font
    bool bold = false;     // mupdf's guess (font style or name)
    bool italic = false;
    bool rotated = false; // baseline not left-to-right horizontal
    float baseline = 0;   // y of the baseline (x for vertical text)
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;
};

struct PageLayoutLine {
    int firstWord = 0;
    int nWords = 0;
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;
};

struct PageLayoutBlock {
    int firstLine = 0;
    int nLines = 0;
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;
};

struct PageLayoutImage {
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;
};

struct PageLayoutVector {
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;
    bool stroked = false;
    bool decoration = false; // underline, strike-out or highlight of text
};

struct PageTextLayout {
    Arena* arena = nullptr; // set by the caller, owns word strings; temp arena when null
    float width = 0;
    float height = 0;
    Vec<PageLayoutWord> words;
    Vec<PageLayoutLine> lines;
    Vec<PageLayoutBlock> blocks;
    Vec<PageLayoutImage> images;
    Vec<PageLayoutVector> vectors;
};

bool ExtractPageLayout(EngineBase* engine, int pageNo, PageTextLayout& out);
