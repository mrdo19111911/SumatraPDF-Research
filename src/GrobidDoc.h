/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// GROBID's Document model (what its PDFALTOSaxHandler builds from pdfalto ALTO),
// built here from mupdf structured text. Token, block and page indexes and the
// " " / "\n" separator tokens follow GROBID exactly: feature extractors depend on them.

class EngineBase;

struct GrobidToken {
    Str text; // a sub-token of a word, " " or "\n"
    Str font; // lowercased font name, "default" when unknown
    float fontSize = 0;
    bool bold = false;
    bool italic = false;
    bool superscript = false;
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;
    int page = 0; // 1-based, as in GROBID
};

struct GrobidBlock {
    int startToken = -1; // Block.getStartToken()
    int endToken = -1;   // Block.getEndToken()
    int firstToken = 0;  // tokens owned by the block (Block.getTokens()): [firstToken, firstToken + nTokens)
    int nTokens = 0;
    int page = 0;
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;
};

struct GrobidImage {
    int page = 0;
    bool vector = false;
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;
};

struct GrobidPage {
    int firstBlock = 0;
    int nBlocks = 0;
    int lengthChar = 0; // Page.getPageLengthChar(): tokens of its blocks
    float width = 0;
    float height = 0;
    float mainX = 0; // Page.getMainArea()
    float mainY = 0;
    float mainW = 0;
    float mainH = 0;
};

struct GrobidDoc {
    Vec<GrobidToken> tokens;
    Vec<GrobidBlock> blocks;
    Vec<GrobidPage> pages;
    Vec<GrobidImage> images;
    int documentLengthChar = 0; // Document.getDocumentLenghtChar()
    double maxBlockSpacing = 0;
    double minBlockSpacing = 0;
    Arena* arena = nullptr; // owns token strings
};

GrobidDoc* GrobidDocFromEngine(EngineBase* engine);
GrobidDoc* GrobidDocFromPdf(Str path);
void GrobidDocFree(GrobidDoc*);
TempStr GrobidBlockTextTemp(GrobidDoc* doc, int block);
TempStr GrobidLayoutDumpTemp(GrobidDoc* doc);
