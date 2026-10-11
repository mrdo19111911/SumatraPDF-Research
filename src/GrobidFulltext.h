/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Port of GROBID's fulltext model: the structure of the body (and annex).

struct GrobidDoc;
struct GrobidPiece;

// one labelled span of the body, in document order
struct GrobidBodyItem {
    Str label;          // plain label, static string: "<section>", "<paragraph>", "<figure>", "<table>",
                        // "<equation>", "<equation_label>", "<citation_marker>", "<figure_marker>", ...
    Str text;           // heap (str::Dup), lines joined by '\n' as in the document
    bool annex = false; // from an "<annex>" piece rather than "<body>"
    int startToken = 0;
    int endToken = 0;
    int page = 0; // page of the first token, 1-based
    float x = 0;  // union box of the item's tokens on that page
    float y = 0;
    float w = 0;
    float h = 0;
};

TempStr GrobidFulltextFeaturesTemp(GrobidDoc* doc, const Vec<GrobidPiece>& pieces);
bool GrobidParseFulltext(GrobidDoc* doc, const Vec<GrobidPiece>& pieces, Vec<GrobidBodyItem*>& items);
void GrobidBodyItemFree(GrobidBodyItem* item);
