/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct MainWindow;
struct WindowTab;
struct ILayout;
ILayout* NewResearchPane(MainWindow* win);
enum class ResearchAction {
    Menu,
    Narration,
    ExportText,
    ExportMp3
};
void OnResearchTools(MainWindow* win, ResearchAction action = ResearchAction::Menu);
void OpenResearchBrowser(MainWindow* win, int service);
void OpenSciHubBrowser(MainWindow* win, Str endpoint, Str query);
HWND ResearchBrowserHwnd(MainWindow* win);
void RelayoutResearchBrowser(MainWindow* win);
void HideResearchBrowser(MainWindow* win);
enum class PaperAction {
    Ask,
    Selection,
    Symbols,
    Compare,
    Versions,
    Figure,
    Reading,
    Graph
};
void OnResearchAssistant(MainWindow* win, PaperAction action);
TempStr PaperPromptTemp(PaperAction action, Str selection = {}, int page = 1);
int RegisterPaper(WindowTab* tab, Str path);
bool FollowPaperSource(MainWindow* win, Str url);
void ClearResearchAssistant(MainWindow* win);
void ResearchForgetTab(WindowTab* tab);
void UpdateResearchTheme();
void TogglePaperMap(MainWindow* win);
void HidePaperMap(MainWindow* win);
HWND PaperMapHwnd(MainWindow* win);
void RelayoutPaperMap(MainWindow* win);
TempStr TestResearchToolsTemp(MainWindow* win, Str action, Str a, Str b, Str c, int* code);
