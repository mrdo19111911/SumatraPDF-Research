/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/JsonParser.h"

#include "GrobidText.h"
#include "GrobidDoc.h"
#include "GrobidSegmentation.h"
#include "GrobidReferences.h"
#include "GrobidHeader.h"
#include "GrobidFulltext.h"
#include "Grobid.h"

// PDF -> zones -> bibliography entries -> fields. False when no references zone is found.
bool GrobidReadReferences(Str pdfPath, Vec<GrobidRef*>& refs) {
    GrobidDoc* doc = GrobidDocFromPdf(pdfPath);
    if (!doc) return false;

    Vec<GrobidPiece> pieces;
    bool ok = GrobidSegment(doc, pieces) && GrobidSegmentReferences(doc, pieces, refs);
    GrobidDocFree(doc);
    if (!ok) return false;

    for (GrobidRef* ref : refs) {
        GrobidParseCitation(ref->raw, ref);
    }
    return true;
}

constexpr int kOutlineLineMax = 300;
constexpr int kOutlineItemsMax = 400;

// one line of at most maxBytes, cut on a UTF-8 boundary: "Figure 1.\nCaption" -> "Figure 1. Caption"
static TempStr OneLineTemp(Str s, int maxBytes) {
    str::Builder out;
    for (int i = 0; i < len(s); i++) {
        char c = (u8)s.s[i] <= ' ' ? ' ' : s.s[i];
        if (c == ' ' && (len(out) == 0 || out.LastChar() == ' ')) continue;
        if (len(out) >= maxBytes && ((u8)c & 0xc0) != 0x80) {
            out.Append(StrL("..."));
            break;
        }
        out.AppendChar(c);
    }
    TempStr res = str::DupTemp(ToStr(out));
    while (len(res) > 0 && res.s[len(res) - 1] == ' ') res.len--;
    return res;
}

static void AppendOutlineField(str::Builder& out, const char* name, Str value) {
    if (len(value) == 0) return;
    out.Append(fmt("%s: %s\n", Str(name), OneLineTemp(value, kOutlineLineMax)));
}

static void AppendOutlineBody(str::Builder& out, Vec<GrobidBodyItem*>& items) {
    // sections, then figures/tables, then equation labels; all with physical pages
    int n = 0;
    out.Append(StrL("SECTIONS:\n"));
    for (GrobidBodyItem* it : items) {
        if (!str::Eq(it->label, StrL("<section>")) || n++ >= kOutlineItemsMax) continue;
        out.Append(fmt("- P%d: %s%s\n", it->page, OneLineTemp(it->text, kOutlineLineMax),
                       it->annex ? StrL(" (appendix)") : StrL("")));
    }

    n = 0;
    out.Append(StrL("FIGURES AND TABLES:\n"));
    for (GrobidBodyItem* it : items) {
        bool figure = str::Eq(it->label, StrL("<figure>"));
        if ((!figure && !str::Eq(it->label, StrL("<table>"))) || n++ >= kOutlineItemsMax) continue;
        out.Append(fmt("- P%d %s: %s\n", it->page, figure ? StrL("figure") : StrL("table"),
                       OneLineTemp(it->text, kOutlineLineMax)));
    }

    n = 0;
    str::Builder labels;
    for (GrobidBodyItem* it : items) {
        if (!str::Eq(it->label, StrL("<equation_label>")) || n++ >= kOutlineItemsMax) continue;
        labels.Append(fmt("%s%s P%d", len(labels) ? StrL(", ") : StrL(""), OneLineTemp(it->text, 40), it->page));
    }
    if (len(labels) > 0) out.Append(fmt("EQUATION LABELS: %s\n", ToStr(labels)));
}

static void AppendOutlineRefs(str::Builder& out, Vec<GrobidRef*>& refs) {
    out.Append(StrL("REFERENCES:\n"));
    for (int i = 0; i < len(refs) && i < kOutlineItemsMax; i++) {
        GrobidRef* r = refs[i];
        GrobidParseCitation(r->raw, r);
        Str title = len(r->title) ? r->title : r->raw;
        out.Append(fmt("- %s%s%s", r->label, len(r->label) ? StrL(" ") : StrL(""), OneLineTemp(title, 200)));
        if (len(r->authors)) out.Append(fmt(" - %s", OneLineTemp(r->authors, 120)));
        if (len(r->year)) out.Append(fmt(" (%s)", r->year));
        out.Append(StrL("\n"));
    }
}

// Machine outline of a paper for AI prompts: metadata, sections, figures, equations and parsed
// references, with physical page numbers. Empty when the PDF cannot be analysed.
TempStr GrobidOutlineTemp(Str pdfPath) {
    GrobidDoc* doc = GrobidDocFromPdf(pdfPath);
    if (!doc) return {};
    Vec<GrobidPiece> pieces;
    if (!GrobidSegment(doc, pieces)) {
        GrobidDocFree(doc);
        return {};
    }

    str::Builder out;
    GrobidHeader h;
    GrobidParseHeader(doc, pieces, &h);
    AppendOutlineField(out, "TITLE", h.title);
    AppendOutlineField(out, "AUTHORS", h.authors);
    AppendOutlineField(out, "KEYWORDS", h.keywords);
    AppendOutlineField(out, "DOI", h.doi);
    GrobidHeaderFree(&h);

    Vec<GrobidBodyItem*> items;
    GrobidParseFulltext(doc, pieces, items);
    AppendOutlineBody(out, items);
    for (GrobidBodyItem* it : items) GrobidBodyItemFree(it);

    Vec<GrobidRef*> refs;
    GrobidSegmentReferences(doc, pieces, refs);
    AppendOutlineRefs(out, refs);
    for (GrobidRef* r : refs) GrobidRefFree(r);

    GrobidDocFree(doc);
    return str::DupTemp(ToStr(out));
}

// The paper's own title from GROBID's header model, on one line; empty when not found
TempStr GrobidPaperTitleTemp(Str pdfPath) {
    GrobidDoc* doc = len(pdfPath) > 0 ? GrobidDocFromPdf(pdfPath) : nullptr;
    if (!doc) return {};
    Vec<GrobidPiece> pieces;
    GrobidHeader h;
    if (GrobidSegment(doc, pieces)) GrobidParseHeader(doc, pieces, &h);
    GrobidDocFree(doc);

    TempStr title = OneLineTemp(h.title, kOutlineLineMax);
    GrobidHeaderFree(&h);
    return title;
}

static void AppendJsonField(str::Builder& out, const char* name, Str value) {
    out.Append(fmt("\"%s\":\"%s\"", Str(name), json::EscapeStrTemp(value)));
}

static TempStr RefsJsonTemp(Vec<GrobidRef*>& refs) {
    str::Builder out;
    out.Append(StrL("["));
    for (int i = 0; i < len(refs); i++) {
        GrobidRef* r = refs[i];
        if (i > 0) out.Append(StrL(",\n"));
        out.Append(StrL("{"));
        AppendJsonField(out, "label", r->label);
        out.Append(StrL(","));
        AppendJsonField(out, "raw", r->raw);
        out.Append(StrL(","));
        AppendJsonField(out, "title", r->title);
        out.Append(StrL(","));
        AppendJsonField(out, "authors", r->authors);
        out.Append(StrL(","));
        AppendJsonField(out, "year", r->year);
        out.Append(StrL(","));
        AppendJsonField(out, "venue", r->venue);
        out.Append(StrL(","));
        AppendJsonField(out, "doi", r->doi);
        out.Append(StrL("}"));
    }
    out.Append(StrL("]"));
    return str::DupTemp(ToStr(out));
}

static TempStr HeaderJsonTemp(GrobidDoc* doc, const Vec<GrobidPiece>& pieces, int* code) {
    GrobidHeader h;
    if (!GrobidParseHeader(doc, pieces, &h)) *code = 1;
    str::Builder out;
    out.Append(StrL("{"));
    AppendJsonField(out, "title", h.title);
    out.Append(StrL(","));
    AppendJsonField(out, "authors", h.authors);
    out.Append(StrL(","));
    AppendJsonField(out, "abstract", h.abstract);
    out.Append(StrL(","));
    AppendJsonField(out, "keywords", h.keywords);
    out.Append(StrL(","));
    AppendJsonField(out, "affiliation", h.affiliation);
    out.Append(StrL(","));
    AppendJsonField(out, "email", h.email);
    out.Append(StrL(","));
    AppendJsonField(out, "date", h.date);
    out.Append(StrL(","));
    AppendJsonField(out, "doi", h.doi);
    out.Append(StrL(","));
    AppendJsonField(out, "venue", h.venue);
    out.Append(StrL("}"));
    GrobidHeaderFree(&h);
    return str::DupTemp(ToStr(out));
}

static TempStr FulltextJsonTemp(GrobidDoc* doc, const Vec<GrobidPiece>& pieces, int* code) {
    Vec<GrobidBodyItem*> items;
    if (!GrobidParseFulltext(doc, pieces, items)) *code = 1;
    str::Builder out;
    out.Append(StrL("["));
    for (int i = 0; i < len(items); i++) {
        GrobidBodyItem* it = items[i];
        if (i > 0) out.Append(StrL(",\n"));
        out.Append(StrL("{"));
        AppendJsonField(out, "label", it->label);
        out.Append(StrL(","));
        AppendJsonField(out, "text", it->text);
        out.Append(fmt(",\"annex\":%s,\"start\":%d,\"end\":%d,\"page\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}",
                       it->annex ? StrL("true") : StrL("false"), it->startToken, it->endToken, it->page, (int)it->x,
                       (int)it->y, (int)it->w, (int)it->h));
        GrobidBodyItemFree(it);
    }
    out.Append(StrL("]"));
    return str::DupTemp(ToStr(out));
}

static void FreeRefs(Vec<GrobidRef*>& refs) {
    for (GrobidRef* r : refs) GrobidRefFree(r);
    VecReset(refs);
}

// -dbg-control TestResearchTools actions "grobid-*", used by tests/ad-hoc-grobid-*.ts
TempStr TestGrobidTemp(Str action, Str a, Str b, int* code) {
    *code = 1;
    if (str::Eq(action, StrL("grobid-crf"))) {
        *code = 0;
        return GrobidTestCrfTemp(a, b);
    }
    if (str::Eq(action, StrL("grobid-text"))) {
        *code = 0;
        return GrobidTestTextTemp(a, b);
    }
    if (str::Eq(action, StrL("grobid-citation"))) {
        GrobidRef ref;
        ref.raw = str::Dup(a);
        Vec<GrobidRef*> refs;
        VecAppend(refs, &ref);
        *code = GrobidParseCitation(a, &ref) ? 0 : 1;
        TempStr res = RefsJsonTemp(refs);
        str::Free(ref.raw);
        str::Free(ref.label);
        str::Free(ref.title);
        str::Free(ref.authors);
        str::Free(ref.year);
        str::Free(ref.venue);
        str::Free(ref.doi);
        return res;
    }
    if (str::Eq(action, StrL("grobid-citfeatures"))) {
        *code = 0;
        return GrobidCitationFeaturesTemp(a);
    }
    if (str::Eq(action, StrL("grobid-refs"))) {
        Vec<GrobidRef*> refs;
        bool ok = GrobidReadReferences(a, refs);
        TempStr res = RefsJsonTemp(refs);
        FreeRefs(refs);
        *code = ok ? 0 : 1;
        return res;
    }

    GrobidDoc* doc = GrobidDocFromPdf(a);
    if (!doc) return str::DupTemp(StrL("cannot open pdf"));
    TempStr res = str::DupTemp(StrL("unknown grobid action"));
    if (str::Eq(action, StrL("grobid-layout"))) {
        *code = 0;
        res = GrobidLayoutDumpTemp(doc);
    } else if (str::Eq(action, StrL("grobid-segfeatures"))) {
        *code = 0;
        res = GrobidSegmentationFeaturesTemp(doc);
    } else if (str::Eq(action, StrL("grobid-segment")) || str::Eq(action, StrL("grobid-reffeatures"))) {
        Vec<GrobidPiece> pieces;
        bool ok = GrobidSegment(doc, pieces);
        *code = ok ? 0 : 1;
        if (str::Eq(action, StrL("grobid-reffeatures"))) {
            res = GrobidRefSegmenterFeaturesTemp(doc, pieces);
        } else {
            // one line per piece: label, token range, text
            str::Builder out;
            for (GrobidPiece& p : pieces) {
                str::Builder text;
                for (int i = p.startToken; i <= p.endToken && i < len(doc->tokens); i++) {
                    text.Append(doc->tokens[i].text);
                }
                out.Append(fmt("%s\t%d\t%d\t%s\n", p.label, p.startToken, p.endToken, ToStr(text)));
            }
            res = str::DupTemp(ToStr(out));
        }
    } else if (str::StartsWith(action, StrL("grobid-head")) || str::StartsWith(action, StrL("grobid-full"))) {
        Vec<GrobidPiece> pieces;
        *code = GrobidSegment(doc, pieces) ? 0 : 1;
        if (str::Eq(action, StrL("grobid-headfeatures"))) {
            res = GrobidHeaderFeaturesTemp(doc, pieces);
        } else if (str::Eq(action, StrL("grobid-header"))) {
            res = HeaderJsonTemp(doc, pieces, code);
        } else if (str::Eq(action, StrL("grobid-fullfeatures"))) {
            res = GrobidFulltextFeaturesTemp(doc, pieces);
        } else if (str::Eq(action, StrL("grobid-fulltext"))) {
            res = FulltextJsonTemp(doc, pieces, code);
        }
    }
    GrobidDocFree(doc);
    return res;
}
