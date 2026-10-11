/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Native port of GROBID's pipeline: segmentation, then header, fulltext, reference-segmenter, citation.

struct GrobidRef;

bool GrobidReadReferences(Str pdfPath, Vec<GrobidRef*>& refs);
TempStr GrobidOutlineTemp(Str pdfPath);
TempStr GrobidPaperTitleTemp(Str pdfPath);
TempStr TestGrobidTemp(Str action, Str a, Str b, int* code);
