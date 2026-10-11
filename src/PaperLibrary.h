/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

enum class LibraryMode {
    Search,
    Collect
};
void OpenPaperLibrary(MainWindow* win, LibraryMode mode);
TempStr PaperArxivIdTemp(Str text);
TempStr TestPaperLibraryTemp(MainWindow* win, Str action, Str a, Str b, Str c, int* code);
