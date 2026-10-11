/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/DirScan.h"
#include "base/File.h"
#include "base/GuessFileType.h"
#include "base/JsonParser.h"
#include "base/UITask.h"
#include "base/Win.h"
#include "gui/UIModels.h"
#include "Settings.h"
#include "AppSettings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "SumatraPDF.h"
#include "Flags.h"
#include "SumatraConfig.h"
#include "Notifications.h"
#include "DocumentShare.h"
#include "DriveClient.h"

static DriveJob* gDriveJob = nullptr; // UI thread owns and cancels; worker owns job fields.

static TempStr TokenPathTemp() {
    return path::JoinTemp(path::GetDirTemp(GetSettingsPathTemp()), StrL("google-drive-account.bin"));
}

static Str SharePath(MainWindow* win) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    if (!tab || !file::Exists(tab->filePath)) return {};
    EngineBase* engine = tab->GetEngine();
    if (engine && EngineHasUnsavedAnnotations(engine)) {
        MessageBoxW(win->hwndFrame, L"Save the document's annotation changes before sharing it.", L"Share document",
                    MB_OK | MB_ICONINFORMATION);
        return {};
    }
    return str::Dup(path::NormalizeTemp(tab->filePath));
}

static int ShareDialog(HWND owner, const WCHAR* title, const WCHAR* instruction, const WCHAR* content,
                       const TASKDIALOG_BUTTON* buttons, int count) {
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = owner;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_USE_COMMAND_LINKS;
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    config.pszWindowTitle = title;
    config.pszMainInstruction = instruction;
    config.pszContent = content;
    config.pButtons = buttons;
    config.cButtons = count;
    int selected = IDCANCEL;
    TaskDialogIndirect(&config, &selected, nullptr, nullptr);
    return selected;
}

static TempStr PickFile(HWND owner, const WCHAR* title, const WCHAR* filter, Str current = {}) {
    WCHAR name[32768]{};
    if (len(current)) wstr::BufSet(WStr(name, dimof(name)), ToWStrTemp(current));
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrTitle = title;
    ofn.lpstrFile = name;
    ofn.nMaxFile = dimof(name);
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_EXPLORER;
    return GetOpenFileNameW(&ofn) ? ToUtf8Temp(WStr(name)) : TempStr();
}

static bool ConfigureDrive(HWND owner) {
    const TASKDIALOG_BUTTON buttons[] = {
        {101, L"Browse...\nSelect a Google Desktop app OAuth client JSON."},
        {102, L"Open setup instructions\nCreate the application's OAuth client in Google Cloud."},
    };
    for (;;) {
        int result = ShareDialog(owner, L"Google Drive setup", L"Configure the Google application once",
                                 L"Sign-in opens in your browser. Google requires an application ID first.\n"
                                 L"Enable the Google Drive API and download an OAuth client of type Desktop app.\n"
                                 L"Your Google password is entered only on Google's website.",
                                 buttons, dimof(buttons));
        if (result == 102) {
            LaunchBrowser(StrL("https://developers.google.com/identity/protocols/oauth2/native-app"));
            continue;
        }
        if (result != 101) return false;
        Str path = PickFile(owner, L"Select Google Desktop OAuth client JSON", L"JSON files (*.json)\0*.json\0\0",
                            gSettings->googleDriveClientPath);
        if (len(path) == 0) return false;
        DriveJob check;
        if (!DriveReadClient(&check, path)) {
            MessageBoxW(
                owner,
                L"Select the JSON downloaded for an OAuth Desktop app client, not a service account or web client.",
                L"Google Drive setup", MB_OK | MB_ICONWARNING);
            continue;
        }
        str::ReplaceWithCopy(&gSettings->googleDriveClientPath, path);
        DriveForgetAccount(TokenPathTemp());
        ScheduleSaveSettings();
        return true;
    }
}

static void DriveFinished(DriveJob* job) {
    gDriveJob = nullptr;
    MainWindow* win = FindMainWindowByHwnd(job->owner);
    if (win) {
        RemoveNotificationsForGroup(win->hwndCanvas, kNotifActionResponse);
        if (len(job->url)) {
            const TASKDIALOG_BUTTON buttons[] = {
                {101, L"Open uploaded file in browser"},
                {102, L"Copy Google Drive link"},
            };
            int selected = ShareDialog(job->owner, L"Google Drive", L"Upload complete",
                                       CWStrTemp(path::GetBaseNameTemp(job->filePath)), buttons, dimof(buttons));
            if (selected == 101) LaunchBrowser(job->url);
            if (selected == 102) CopyTextToClipboard(job->url);
        } else {
            MessageBoxW(job->owner, CWStrTemp(job->error), L"Google Drive", MB_OK | MB_ICONINFORMATION);
        }
    }
    delete job;
}

static void DriveWorker(DriveJob* job) {
    DriveRun(job);
    uitask::Post(MkFunc0(DriveFinished, job), "DriveFinished");
}

void OnGoogleDrive(MainWindow* win) {
    if (!win || !HasPermission(Perm::InternetAccess | Perm::DiskAccess)) return;
    HWND owner = win->hwndFrame;
    if (gDriveJob) {
        SetEvent(gDriveJob->cancel);
        ShowPlainNotification(win->hwndCanvas, StrL("Cancelling Google Drive operation..."));
        return;
    }
    Str path = SharePath(win);
    AutoFree freePath(path.s);
    if (len(path) == 0) return;
    bool connected = DriveHasAccount(TokenPathTemp());
    const TASKDIALOG_BUTTON buttons[] = {
        {101, connected ? L"Upload current document\nCreate a new file in My Drive."
                        : L"Sign in and upload\nGoogle sign-in opens in your browser."},
        {102, L"Open Google Drive in browser"},
        {103, L"Configure Google application..."},
        {104, L"Disconnect saved Google account\nRemove SumatraPDF's saved sign-in from this computer."},
    };
    int selected = ShareDialog(win->hwndFrame, L"Google Drive", CWStrTemp(path::GetBaseNameTemp(path)),
                               L"Upload the saved file as a new file in My Drive. Existing files are not replaced.",
                               buttons, connected ? dimof(buttons) : dimof(buttons) - 1);
    win = FindMainWindowByHwnd(owner);
    if (!win) return;
    if (selected == 102) {
        LaunchBrowser(StrL("https://drive.google.com/drive/my-drive"));
        return;
    }
    if (selected == 104) {
        bool ok = DriveForgetAccount(TokenPathTemp());
        MessageBoxW(win->hwndFrame,
                    ok ? L"Saved Google sign-in removed. The browser remains signed in."
                       : L"Could not remove the saved sign-in file.",
                    L"Google Drive", MB_OK);
        return;
    }
    if (selected == 103) {
        ConfigureDrive(win->hwndFrame);
        return;
    }
    if (selected != 101) return;
    auto* job = new DriveJob;
    if (!DriveReadClient(job, gSettings->googleDriveClientPath)) {
        if (!ConfigureDrive(owner) || !DriveReadClient(job, gSettings->googleDriveClientPath)) {
            delete job;
            return;
        }
    }
    win = FindMainWindowByHwnd(owner);
    if (!win || gDriveJob) {
        delete job;
        return;
    }
    job->owner = owner;
    job->filePath = str::Dup(path);
    job->tokenPath = str::Dup(TokenPathTemp());
    job->cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!job->cancel) {
        delete job;
        return;
    }
    gDriveJob = job;
    ShowPlainNotification(win->hwndCanvas, StrL("Google Drive: signing in or uploading. Click DRIVE again to cancel."),
                          kNotifNoTimeout);
    RunAsync(MkFunc0(DriveWorker, job), StrL("GoogleDriveUpload"));
}

static TempStr FindZaloTemp() {
    if (file::Exists(gSettings->zaloExecutablePath)) return gSettings->zaloExecutablePath;
    Str root = path::JoinTemp(GetSpecialFolderTemp(CSIDL_LOCAL_APPDATA), StrL("Programs\\Zalo"));
    Str direct = path::JoinTemp(root, StrL("Zalo.exe"));
    if (file::Exists(direct)) return direct;
    DirIter it(root);
    it.includeFiles = false;
    it.includeDirs = true;
    str::Builder best;
    for (auto* entry : it) {
        if (!str::StartsWithI(entry->name, StrL("Zalo-"))) continue;
        Str candidate = path::JoinTemp(entry->filePath, StrL("Zalo.exe"));
        if (file::Exists(candidate) && str::CmpNatural(candidate, ToStr(best)) > 0) {
            best.Reset();
            best.Append(candidate);
        }
    }
    return ToStrTemp(best);
}

// CF_HDROP contains a double-NUL-terminated UTF-16 list, never command-line text.
static HGLOBAL FileDrop(Str path) {
    WStr name = ToWStrTemp(path);
    size_t bytes = sizeof(DROPFILES) + ((size_t)len(name) + 2) * sizeof(WCHAR);
    HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!block) return nullptr;
    auto* drop = (DROPFILES*)GlobalLock(block);
    if (!drop) {
        GlobalFree(block);
        return nullptr;
    }
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    memcpy((u8*)drop + sizeof(DROPFILES), name.s, len(name) * sizeof(WCHAR));
    GlobalUnlock(block);
    return block;
}

static bool CopyShareFile(HWND owner, Str path) {
    HGLOBAL drop = FileDrop(path);
    if (!drop) return false;
    if (!OpenClipboard(owner)) {
        GlobalFree(drop);
        return false;
    }
    bool ok = EmptyClipboard() && SetClipboardData(CF_HDROP, drop);
    CloseClipboard();
    if (!ok) GlobalFree(drop);
    return ok;
}

void OnSendToZalo(MainWindow* win) {
    if (!win || !HasPermission(Perm::InternetAccess | Perm::DiskAccess | Perm::CopySelection)) return;
    HWND owner = win->hwndFrame;
    Str path = SharePath(win);
    AutoFree freePath(path.s);
    if (len(path) == 0) return;
    Str exe = FindZaloTemp();
    if (len(exe) == 0) {
        const TASKDIALOG_BUTTON buttons[] = {{101, L"Browse to Zalo.exe..."}};
        if (ShareDialog(win->hwndFrame, L"Zalo", L"Zalo PC was not found", L"Select your installed Zalo application.",
                        buttons, dimof(buttons)) != 101)
            return;
        if (!FindMainWindowByHwnd(owner)) return;
        exe = PickFile(owner, L"Locate Zalo PC", L"Zalo (Zalo.exe)\0Zalo.exe\0\0");
        if (!str::EqI(path::GetBaseNameTemp(exe), StrL("Zalo.exe")) || !file::Exists(exe)) return;
        str::ReplaceWithCopy(&gSettings->zaloExecutablePath, exe);
        ScheduleSaveSettings();
    }
    win = FindMainWindowByHwnd(owner);
    if (!win) return;
    if (!CopyShareFile(win->hwndFrame, path)) {
        MessageBoxW(win->hwndFrame, L"The clipboard is busy. Try again.", L"Zalo", MB_OK | MB_ICONWARNING);
        return;
    }
    bool opened = LaunchFileShell(exe);
    ShowPlainNotification(win->hwndCanvas,
                          opened
                              ? StrL("File ready for Zalo. Choose a conversation, press Ctrl+V, then review and send.")
                              : StrL("File copied, but Zalo could not open. Open Zalo and paste it in a conversation."),
                          kNotifNoTimeout);
}

// Offline protocol tests exercise the same backend without opening accounts or chats.
TempStr TestDocumentShareTemp(Str action, Str a, Str b, Str c, Str d, int* code) {
    *code = 1;
    if (!gForTesting) return StrL("requires -for-testing");
    if (str::Eq(action, StrL("zalo"))) {
        Str found = FindZaloTemp();
        *code = len(found) ? 0 : 1;
        return found;
    }
    if (str::Eq(action, StrL("drop"))) {
        HGLOBAL block = FileDrop(a);
        if (!block) return StrL("allocation failed");
        WCHAR name[32768]{};
        UINT n = DragQueryFileW((HDROP)block, 0, name, dimof(name));
        bool ok = DragQueryFileW((HDROP)block, 0xffffffff, nullptr, 0) == 1 && n == (UINT)len(ToWStrTemp(a));
        GlobalFree(block);
        *code = ok ? 0 : 1;
        return ToUtf8Temp(WStr(name));
    }
    DriveJob job;
    if (str::Eq(action, StrL("client"))) {
        *code = DriveReadClient(&job, a) ? 0 : 1;
        return *code == 0 ? StrL("valid desktop client") : StrL("invalid desktop client");
    }
    if (str::Eq(action, StrL("forget"))) {
        *code = DriveForgetAccount(a) && !DriveHasAccount(a) ? 0 : 1;
        return StrL("account removed");
    }
    if (!str::Eq(action, StrL("upload")) || !DriveReadClient(&job, a) || !str::StartsWith(c, StrL("http://127.0.0.1:")))
        return StrL("invalid mock upload arguments");
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = uc.dwUserNameLength = uc.dwPasswordLength = uc.dwUrlPathLength = uc.dwExtraInfoLength =
        (DWORD)-1;
    if (!InternetCrackUrlW(CWStrTemp(c), 0, 0, &uc) || uc.dwUserNameLength || uc.dwPasswordLength ||
        uc.dwUrlPathLength || uc.dwExtraInfoLength ||
        !wstr::Eq(WStr(uc.lpszHostName, (int)uc.dwHostNameLength), L"127.0.0.1")) {
        return StrL("mock must be a loopback origin");
    }
    job.filePath = str::Dup(b);
    job.mockBase = str::Dup(c);
    job.tokenPath = path::Join(d, StrL("account.bin"));
    job.authUrlPath = path::Join(d, StrL("auth-url.txt"));
    DriveRun(&job);
    *code = len(job.url) ? 0 : 1;
    return len(job.url) ? str::DupTemp(job.url) : str::DupTemp(job.error);
}
