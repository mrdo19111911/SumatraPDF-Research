/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Ports of GROBID's FeatureFactory, TextUtilities and Lexicon (English) helpers,
// shared by every GROBID feature extractor. Data comes from research/grobid/.

struct CrfModel;

int GrobidLinearScaling(double pos, double total, int nbBins);
int GrobidLinearScaling(int pos, int total, int nbBins);
bool GrobidIsUpperFirst(Str s);
bool GrobidAllCapital(Str s);
bool GrobidHasDigit(Str s);
bool GrobidAllDigits(Str s);
bool GrobidIsYear(Str s);
bool GrobidIsEmail(Str s);
bool GrobidIsHttp(Str s);
bool GrobidIsPunct(Str s);
bool GrobidIsMonth(Str s);
bool GrobidInDictionary(Str s);
bool GrobidIsFirstName(Str s);
bool GrobidIsLastName(Str s);
bool GrobidFilterLine(Str s);
TempStr GrobidLowerTemp(Str s);
TempStr GrobidPrefixTemp(Str s, int n);
TempStr GrobidSuffixTemp(Str s, int n);
TempStr GrobidPunctProfileTemp(Str line);
TempStr GrobidPatternTemp(Str line);
void GrobidTokenize(Str text, StrVec& out);
Str GrobidDataTemp(Str name);
CrfModel* GrobidModel(Str name);
TempStr GrobidTestCrfTemp(Str model, Str featuresPath);
TempStr GrobidTestTextTemp(Str fn, Str arg);
