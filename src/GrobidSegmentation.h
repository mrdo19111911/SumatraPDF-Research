/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Port of GROBID's Segmentation model: labels each line as header, body,
// references, annex, ... and groups consecutive lines into pieces.

struct GrobidDoc;

// DocumentPiece: tokens [startToken, endToken] of consecutive lines sharing a label
struct GrobidPiece {
    Str label; // plain label without "I-", e.g. "<references>"; static string
    int block = 0;
    int startToken = 0;
    int endToken = 0;
};

TempStr GrobidSegmentationFeaturesTemp(GrobidDoc* doc);
bool GrobidSegment(GrobidDoc* doc, Vec<GrobidPiece>& pieces);
