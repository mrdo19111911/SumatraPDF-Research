/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Ports of GROBID's reference-segmenter (split the bibliography into entries)
// and citation + name-citation models (fields of one entry).

struct GrobidDoc;
struct GrobidPiece;

struct GrobidRef {
    Str label;   // "[12]", "12." or empty
    Str raw;     // entry text as GROBID gives it, lines separated by '\n'
    Str title;   // fields below are filled by GrobidParseCitation
    Str authors; // "Surname, F.; Surname, F."
    Str year;
    Str venue; // journal or book title
    Str doi;
};

TempStr GrobidRefSegmenterFeaturesTemp(GrobidDoc* doc, const Vec<GrobidPiece>& pieces);
bool GrobidSegmentReferences(GrobidDoc* doc, const Vec<GrobidPiece>& pieces, Vec<GrobidRef*>& refs);
TempStr GrobidCitationFeaturesTemp(Str raw);
bool GrobidParseCitation(Str raw, GrobidRef* ref);
void GrobidRefFree(GrobidRef* ref);

// citation helpers shared with the header parser; strings are temp
struct GrobidPerson {
    Str first;
    Str middle;
    Str last;
    Str title;
    Str suffix;
};

void GrobidParseNames(const StrVec& toks, Str model, Vec<GrobidPerson>& persons);
TempStr GrobidFormatPersonsTemp(Vec<GrobidPerson>& persons);
void GrobidLocationFlags(const StrVec& toks, Vec<bool>& flags);
void GrobidPatternFlags(const StrVec& toks, int (*matchAt)(Str text, int i), Vec<bool>& flags);
int GrobidMatchUrlAt(Str text, int i);
int GrobidMatchDoiAt(Str text, int i);
TempStr GrobidNormalizeSpaceTemp(Str s);
TempStr GrobidCleanFieldTemp(Str s);
TempStr GrobidCleanDoiTemp(Str doi);
