/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Port of GROBID's header model (+ name-header): metadata of the paper itself.

struct GrobidDoc;
struct GrobidPiece;

// heap strings (str::Dup), freed by GrobidHeaderFree; empty when not found
struct GrobidHeader {
    Str title;
    Str authors; // "Surname, F.; Surname, F."
    Str abstract;
    Str keywords;
    Str affiliation;
    Str email;
    Str date;
    Str doi;
    Str venue; // journal / conference from the header, e.g. a "Published in" line
};

TempStr GrobidHeaderFeaturesTemp(GrobidDoc* doc, const Vec<GrobidPiece>& pieces);
bool GrobidParseHeader(GrobidDoc* doc, const Vec<GrobidPiece>& pieces, GrobidHeader* header);
void GrobidHeaderFree(GrobidHeader* header);
