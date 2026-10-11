/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Linear-chain CRF tagger reading Wapiti text models (port of Wapiti's label mode).

struct CrfModel;

CrfModel* CrfModelLoad(Str wapitiText);
void CrfModelFree(CrfModel*);
int CrfLabelCount(CrfModel*);
Str CrfLabelName(CrfModel*, int label);
bool CrfTag(CrfModel*, const StrVec& rows, Vec<int>& labelsOut);
