/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

TempStr ResearchPdfUrlTemp(Str pageUrl, Str html);
bool ResearchFetchPdf(Str url, Str output, Str& error, HANDLE cancel = nullptr);
bool ResearchUrlAllowed(Str url);
TempStr ResearchResolveUrlTemp(Str base, Str relative);
bool ResearchFetchText(Str url, Str& text, Str& finalUrl, HANDLE cancel = nullptr, Str* error = nullptr);
