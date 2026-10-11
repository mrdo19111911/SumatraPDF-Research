/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// which page elements may name the PDF: embedded viewers only, or download links too
enum class PdfLink {
    Any,
    Embedded
};
TempStr ResearchPdfUrlTemp(Str pageUrl, Str html, PdfLink links = PdfLink::Any);
bool ResearchFetchPdf(Str url, Str output, Str& error, HANDLE cancel = nullptr, Str form = {});
bool ResearchUrlAllowed(Str url);
TempStr ResearchResolveUrlTemp(Str base, Str relative);
bool ResearchFetchText(Str url, Str& text, Str& finalUrl, HANDLE cancel = nullptr, Str* error = nullptr);
TempStr PaperFileNameTemp(Str title);
bool SciHubIsTitle(Str query);
TempStr SciHubUrlTemp(Str endpoint, Str query);
TempStr SciHubFormTemp(Str query);
TempStr SciHubFormPageTemp(Str endpoint, Str query);
