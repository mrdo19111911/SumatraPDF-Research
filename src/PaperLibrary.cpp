/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/File.h"
#include "base/GuessFileType.h"
#include "base/HtmlTags.h"
#include "base/Http.h"
#include "base/JsonParser.h"
#include "base/Win.h"
#include "gui/UIModels.h"
#include "gui/Layout.h"
#include "gui/win/WinGui.h"
#include "gui/win/WebView.h"
#include "Settings.h"
#include "AppSettings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "GumboHtmlParser.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "SumatraPDF.h"
#include "SumatraConfig.h"
#include "SimpleBrowserWindow.h"
#include "ResearchFetch.h"
#include "PaperLibrary.h"

constexpr int kReferenceLimit = 2000;
constexpr int kReferenceTextLimit = 8 * 1024 * 1024;
constexpr DWORD kArxivIntervalMs = 3100;
constexpr UINT_PTR kLibraryTimer = 1;
constexpr UINT kLibraryPollMs = 250;
constexpr int kArxivResults = 20;
constexpr int kWebResults = 5;
constexpr int kArxivFirstYear = 1991;
constexpr int kEarliestCitationYear = 1600;
constexpr int kMinCandidateTitle = 21;
constexpr int kAuthorPrefixLimit = 180;
constexpr auto kGoogleSearch = StrL("https://www.google.com/search");
static ULONGLONG gArxivLastRequest = 0;
static SRWLOCK gArxivLock = SRWLOCK_INIT;

struct LibraryAttempt {
    Str source, query, url, status, error;
    ~LibraryAttempt() {
        for (Str s : {source, query, url, status, error}) str::Free(s);
    }
};

struct PaperRef {
    Str raw, key, title, doi, arxiv, abstract, authors, url, path, status, source, error;
    bool titleResolved = false;
    Vec<LibraryAttempt*> attempts;
    ~PaperRef() {
        for (Str s : {raw, key, title, doi, arxiv, abstract, authors, url, path, status, source, error}) str::Free(s);
        for (LibraryAttempt* a : attempts) delete a;
    }
};

enum class LibraryTask {
    Extract,
    Search,
    Collect,
    Download
};

struct LibraryJob {
    LONG refs = 1;
    CRITICAL_SECTION lock;
    HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool running = false;
    LibraryTask task = LibraryTask::Extract;
    int download = -1;
    int processed = 0, total = 0;
    Str paper, folder, query, api, sci, anna, annaApi, metadata, web, message, manifest, report;
    Vec<PaperRef*> rows;
    LibraryJob() { InitializeCriticalSection(&lock); }
    ~LibraryJob() {
        for (PaperRef* row : rows) delete row;
        for (Str s : {paper, folder, query, api, sci, anna, annaApi, metadata, web, message, manifest, report})
            str::Free(s);
        CloseHandle(cancel);
        DeleteCriticalSection(&lock);
    }
    void Release() {
        if (InterlockedDecrement(&refs) == 0) delete this;
    }
};

struct LibraryWindow {
    HWND hwnd = nullptr, owner = nullptr;
    WebviewWnd* web = nullptr;
    LibraryMode mode = LibraryMode::Search;
    LibraryJob* job = new LibraryJob();
    Str probe;
    int resources = 0;
    bool navigated = false;
    bool refresh = true, wasBusy = false;
    bool reportOpened = false;
    ~LibraryWindow() {
        SetEvent(job->cancel);
        delete web;
        job->Release();
        str::Free(probe);
    }
};
static LibraryWindow* gTestLibrary = nullptr;

static void TraceAttempt(LibraryJob* job, PaperRef* ref, Str source, Str query, Str url, Str status, Str error) {
    auto* attempt = new LibraryAttempt();
    attempt->source = str::Dup(source);
    attempt->query = str::Dup(query);
    attempt->url = str::Dup(url);
    attempt->status = str::Dup(status);
    attempt->error = str::Dup(error);
    EnterCriticalSection(&job->lock);
    VecAppend(ref->attempts, attempt);
    LeaveCriticalSection(&job->lock);
}

static TempStr TrimLibrary(Str s) {
    while (len(s) && (u8)s.s[0] <= ' ') {
        s.s++;
        s.len--;
    }
    while (len(s) && (u8)s.s[len(s) - 1] <= ' ') s.len--;
    return str::DupTemp(s);
}

static TempStr CleanPaperTemp(Str s) {
    s = str::ReplaceTemp(s, StrL("\xE2\x80\x99"), StrL("'"));
    s = str::ReplaceTemp(s, StrL("\xE2\x80\x98"), StrL("'"));
    s = str::ReplaceTemp(s, StrL("\xEF\xAC\x81"), StrL("fi"));
    s = str::ReplaceTemp(s, StrL("\xEF\xAC\x82"), StrL("fl"));
    str::Builder out;
    for (int i = 0; i < len(s); i++) {
        if (s.s[i] == '-' && i > 0 && isalpha((u8)s.s[i - 1])) {
            int next = i + 1;
            while (next < len(s) && isspace((u8)s.s[next])) next++;
            if (next > i + 1 && next < len(s) && islower((u8)s.s[next])) {
                i = next - 1;
                continue;
            }
        }
        out.AppendChar(s.s[i]);
    }
    return str::DupTemp(ToStr(out));
}

static TempStr NormalizePaper(Str s) {
    s = CleanPaperTemp(s);
    str::Builder out;
    bool gap = false;
    for (int i = 0; i < len(s); i++) {
        u8 ch = (u8)s.s[i];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch >= 128) {
            if (gap && len(out)) out.AppendChar(' ');
            out.AppendChar((char)(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch));
            gap = false;
        } else
            gap = true;
    }
    return str::DupTemp(ToStr(out));
}

static TempStr FindDoi(Str s) {
    for (int i = 0; i + 7 < len(s); i++) {
        if (s.s[i] != '1' || s.s[i + 1] != '0' || s.s[i + 2] != '.') continue;
        int j = i + 3;
        while (j < len(s) && s.s[j] >= '0' && s.s[j] <= '9') j++;
        if (j - i < 7 || j - i > 12 || j >= len(s) || s.s[j] != '/') continue;
        j++;
        while (j < len(s) && (u8)s.s[j] > ' ' && s.s[j] != '"' && s.s[j] != '<' && s.s[j] != '>') j++;
        while (j > i && (s.s[j - 1] == '.' || s.s[j - 1] == ',' || s.s[j - 1] == ';' || s.s[j - 1] == ']')) j--;
        // Remove unbalanced closing parentheses, preserving parentheses inside DOIs.
        int balance = 0;
        for (int k = i; k < j; k++) balance += s.s[k] == '(' ? 1 : s.s[k] == ')' ? -1 : 0;
        while (balance < 0 && j > i && s.s[j - 1] == ')') {
            j--;
            balance++;
        }
        if (j > i + 8) return str::DupTemp(Str(s.s + i, j - i));
    }
    return {};
}

static TempStr FindArxiv(Str s) {
    const Str markers[] = {StrL("arxiv:"), StrL("arxiv.org/abs/"), StrL("arxiv.org/pdf/")};
    for (Str marker : markers) {
        int offset = str::IndexOfI(s, marker);
        if (offset < 0) continue;
        int start = offset + len(marker), end = start;
        while (end < len(s)) {
            char ch = s.s[end];
            if (!(isalnum((u8)ch) || ch == '.' || ch == '-' || ch == '/')) break;
            end++;
        }
        Str id(s.s + start, end - start);
        str::TrimSuffix(id, StrL(".pdf"));
        while (len(id) && id.s[len(id) - 1] == '.') id.len--;
        if (len(id) >= 7) return str::DupTemp(id);
    }
    return {};
}

TempStr PaperArxivIdTemp(Str text) {
    return FindArxiv(text);
}

static bool TitleLetter(u8 ch) {
    return isalpha(ch) || ch >= 128;
}

static TempStr TrimTitle(Str s) {
    while (len(s) && ((u8)s.s[0] <= ' ' || strchr(",.;:", s.s[0]))) {
        s.s++;
        s.len--;
    }
    while (len(s) && ((u8)s.s[len(s) - 1] <= ' ' || strchr(",.;:", s.s[len(s) - 1]))) s.len--;
    return str::DupTemp(s);
}

static bool TitleVenue(Str s) {
    const Str markers[] = {StrL("jour"),         StrL("journal"), StrL("journals"),   StrL("j."),
                           StrL("proc"),         StrL("proce"),   StrL("proceed"),    StrL("proceeding"),
                           StrL("proceedings"),  StrL("conf"),    StrL("conference"), StrL("conferences"),
                           StrL("ann"),          StrL("annals"),  StrL("ieee"),       StrL("trans"),
                           StrL("transactions"), StrL("vol"),     StrL("volume")};
    for (int i = 0; i < len(s); i++) {
        if (i && (TitleLetter((u8)s.s[i - 1]) || isdigit((u8)s.s[i - 1]) || s.s[i - 1] == '_')) continue;
        for (Str marker : markers) {
            int end = i + len(marker);
            if (end > len(s) || !str::EqI(Str(s.s + i, len(marker)), marker)) continue;
            if (end == len(s) || (u8)s.s[end] <= ' ' || strchr(".,;:()[]{}", s.s[end])) return true;
        }
    }
    return false;
}

static bool TitleNames(Str s) {
    int words = 0;
    for (int i = 0; i < len(s);) {
        if (!TitleLetter((u8)s.s[i])) {
            i++;
            continue;
        }
        int start = i++;
        while (i < len(s) && TitleLetter((u8)s.s[i])) i++;
        Str word(s.s + start, i - start);
        words++;
        WStr wide = ToWStrTemp(word);
        WORD type = 0;
        if (len(wide) && GetStringTypeW(CT_CTYPE1, wide.s, 1, &type) && (type & C1_UPPER)) continue;
        bool particle = false;
        for (Str p :
             {StrL("and"), StrL("de"), StrL("del"), StrL("van"), StrL("von"), StrL("et"), StrL("al"), StrL("d")}) {
            if (str::EqI(word, p)) particle = true;
        }
        if (!particle) return false;
    }
    return words >= 2;
}

static bool TitleInitialDot(Str s, int i) {
    if (i < 1 || i + 1 >= len(s) || s.s[i] != '.' || !isupper((u8)s.s[i - 1])) return false;
    if (i >= 2 && (TitleLetter((u8)s.s[i - 2]) || isdigit((u8)s.s[i - 2]) || s.s[i - 2] == '_')) return false;
    u8 next = (u8)s.s[i + 1];
    return isspace(next) || isupper(next) || next == '-';
}

static bool TitleInitials(Str s) {
    bool letter = false;
    for (int i = 0; i < len(s); i++) {
        u8 ch = (u8)s.s[i];
        if (isspace(ch)) continue;
        if (ch == '.') {
            if (!letter) return false;
            letter = false;
            continue;
        }
        if (letter || (!isupper(ch) && ch != 'd')) return false;
        letter = true;
    }
    return len(s) > 0;
}

static bool TitleRoman(Str s) {
    s = TrimLibrary(s);
    int i = 0;
    while (i < len(s) && strchr("IVX", s.s[i])) i++;
    if (!i) return false;
    while (i < len(s) && isspace((u8)s.s[i])) i++;
    return i == len(s) || s.s[i] == '.';
}

static bool VenueStart(Str s) {
    if (!len(s)) return false;
    if (isdigit((u8)s.s[0])) return true;
    const Str prefixes[] = {StrL("Math."),       StrL("Springer"), StrL("Cambridge"),    StrL("American"),
                            StrL("Amer."),       StrL("Adv."),     StrL("SIAM"),         StrL("arXiv"),
                            StrL("Arch."),       StrL("Bull."),    StrL("Canadian"),     StrL("Russian"),
                            StrL("Princeton"),   StrL("Oxford"),   StrL("EMS"),          StrL("Preprint"),
                            StrL("In Advances"), StrL("Grad."),    StrL("Lecture Notes")};
    for (Str prefix : prefixes) {
        if (str::StartsWithI(s, prefix)) return true;
    }
    int end = 0;
    while (end < len(s) && s.s[end] != ',') end++;
    return TitleVenue(Str(s.s, end));
}

static TempStr StructuredTitle(Str raw) {
    int start = -1;
    for (int i = 3; i < len(raw) && i <= kAuthorPrefixLimit; i++) {
        if (raw.s[i] != '(' || !isspace((u8)raw.s[i - 1]) || !TitleNames(Str(raw.s, i))) continue;
        int end = i + 1;
        while (end < len(raw) && isdigit((u8)raw.s[end])) end++;
        if (end - i != 5 || !(raw.s[i + 1] == '1' || raw.s[i + 1] == '2')) continue;
        if (end < len(raw) && islower((u8)raw.s[end])) end++;
        if (end >= len(raw) || raw.s[end++] != ')') continue;
        if (end < len(raw) && raw.s[end] == '.') end++;
        if (end < len(raw) && isspace((u8)raw.s[end])) start = end;
        if (start >= 0) break;
    }
    for (int i = 0; start < 0 && i + 1 < len(raw); i++) {
        if ((raw.s[i] != ':' && raw.s[i] != '.') || !isspace((u8)raw.s[i + 1]) || TitleInitialDot(raw, i)) continue;
        Str prefix(raw.s, i);
        if (!TitleNames(prefix)) continue;
        int lastComma = -1;
        for (int j = 0; j < len(prefix); j++) {
            if (prefix.s[j] == ',') lastComma = j;
        }
        if (raw.s[i] == ':' && lastComma >= 0) {
            Str tail = TrimTitle(Str(prefix.s + lastComma + 1, len(prefix) - lastComma - 1));
            if (!TitleNames(tail) && !TitleInitials(tail)) continue;
        }
        start = i + 1;
    }
    for (int i = 0; start < 0 && i + 1 < len(raw); i++) {
        if (raw.s[i] != ',' || !isspace((u8)raw.s[i + 1]) || !TitleNames(Str(raw.s, i))) continue;
        int end = i + 1;
        while (end < len(raw) && raw.s[end] != ',') end++;
        Str next = TrimLibrary(Str(raw.s + i + 1, end - i - 1));
        if (TitleNames(next) || (len(next) > 2 && isupper((u8)next.s[0]) && TitleInitialDot(next, 1))) continue;
        start = i + 1;
    }
    if (start < 0) return {};
    Str rest = TrimLibrary(Str(raw.s + start, len(raw) - start));
    int end = len(rest), depth = 0;
    for (int i = 0; i + 1 < len(rest); i++) {
        char ch = rest.s[i];
        if (ch == '(' || ch == '[' || ch == '{') depth++;
        if ((ch == ')' || ch == ']' || ch == '}') && depth) depth--;
        if (depth || !isspace((u8)rest.s[i + 1])) continue;
        Str tail = TrimLibrary(Str(rest.s + i + 1, len(rest) - i - 1));
        if ((ch == ',' && VenueStart(tail)) || (ch == '.' && !TitleInitialDot(rest, i) && !TitleRoman(tail))) {
            end = i;
            break;
        }
    }
    return TrimTitle(Str(rest.s, end));
}

static TempStr RankedTitlePart(Str raw, char separator, int rank) {
    int start = 0, separators = 0;
    TempStr best, second;
    for (int i = 0; i <= len(raw); i++) {
        if (i < len(raw) && (raw.s[i] != separator || (separator == '.' && i + 1 < len(raw) && (u8)raw.s[i + 1] > ' ')))
            continue;
        if (i < len(raw)) separators++;
        TempStr piece = TrimTitle(Str(raw.s + start, i - start));
        start = i + 1;
        if (TitleVenue(piece) || len(FindDoi(piece))) continue;
        bool letters = false;
        int chars = 0;
        for (int j = 0; j < len(piece); j++) {
            u8 c = (u8)piece.s[j];
            if ((c & 0xc0) != 0x80) chars++;
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 128) letters = true;
        }
        if (!letters || chars < kMinCandidateTitle) continue;
        if (len(piece) > len(best)) {
            second = best;
            best = piece;
        } else if (len(piece) > len(second))
            second = piece;
    }
    if (!separators) return {};
    TempStr result = rank == 0 ? best : second;
    while (len(result) && result.s[len(result) - 1] == '.') result.len--;
    return result;
}

static TempStr GuessPaperTitle(Str raw) {
    raw = CleanPaperTemp(raw);
    TempStr title = StructuredTitle(raw);
    if (!len(title)) title = RankedTitlePart(raw, ',', 0);
    if (!len(title)) title = RankedTitlePart(raw, '.', 0);
    return len(title) ? title : TrimLibrary(raw);
}

static bool ReferenceStart(Str line, int& prefix) {
    prefix = 0;
    if (len(line) < 3) return false;
    if (line.s[0] == '[') {
        int close = 1;
        while (close < len(line) && close < 24 && line.s[close] != ']') close++;
        if (close < len(line) && line.s[close] == ']' && close + 1 < len(line) && line.s[close + 1] == ' ') {
            prefix = close + 2;
            return true;
        }
    }
    int n = 0;
    while (n < len(line) && n < 5 && isdigit((u8)line.s[n])) n++;
    if (n && n + 1 < len(line) && (line.s[n] == '.' || line.s[n] == ')') && line.s[n + 1] == ' ') {
        prefix = n + 2;
        return true;
    }
    return false;
}

static void AddReference(Vec<PaperRef*>& rows, Str raw) {
    raw = TrimLibrary(raw);
    if (len(raw) < 5 || len(rows) >= kReferenceLimit) return;
    TempStr doi = FindDoi(raw), id = FindArxiv(raw), title = GuessPaperTitle(raw);
    TempStr key = NormalizePaper(raw);
    for (PaperRef* row : rows) {
        if (len(doi) && len(row->doi) && str::EqI(doi, row->doi)) return;
        if (len(id) && len(row->arxiv) && str::Eq(id, row->arxiv)) return;
        if (!len(doi) && !len(id) && str::Eq(key, row->key)) return;
    }
    auto* row = new PaperRef();
    row->raw = str::Dup(raw);
    row->key = str::Dup(key);
    row->doi = str::Dup(doi);
    row->arxiv = str::Dup(id);
    row->title = str::Dup(title);
    row->status = str::Dup(StrL("Pending"));
    VecAppend(rows, row);
}

static bool HasRefDate(Str text) {
    if (len(FindDoi(text)) || len(FindArxiv(text))) return true;
    for (int i = 0; i + 3 < len(text); i++) {
        if (i && isdigit((u8)text.s[i - 1])) continue;
        if (text.s[i] != '1' && text.s[i] != '2') continue;
        if (!(text.s[i] == '1' && text.s[i + 1] == '9') && !(text.s[i] == '2' && text.s[i + 1] == '0')) continue;
        if (!isdigit((u8)text.s[i + 2]) || !isdigit((u8)text.s[i + 3])) continue;
        if (i + 4 == len(text) || !isdigit((u8)text.s[i + 4])) return true;
    }
    return false;
}

// MuPDF may join adjacent citations; split only the next numeric label after a date/identifier.
static TempStr SplitRefLabels(Str text) {
    str::Builder out;
    StrVec lines;
    Split(&lines, text, StrL("\n"));
    int previous = 0;
    for (Str line : lines) {
        line = TrimLibrary(line);
        int start = 0;
        for (int i = 0; i < len(line); i++) {
            if (line.s[i] != '[' || (i && line.s[i - 1] != ' ')) continue;
            int prefix = 0, number = 0;
            if (!ReferenceStart(Str(line.s + i, len(line) - i), prefix)) continue;
            Str rest = str::Parse(Str(line.s + i + 1, prefix - 3), "%d", &number);
            if (str::IsNull(rest) || len(rest)) continue;
            if (i && (number != previous + 1 || !HasRefDate(Str(line.s + start, i - start)))) continue;
            if (i) {
                out.Append(Str(line.s + start, i - start));
                out.AppendChar('\n');
                start = i;
            }
            previous = number;
            i += prefix - 1;
        }
        out.Append(Str(line.s + start, len(line) - start));
        out.AppendChar('\n');
    }
    return str::DupTemp(ToStr(out));
}

static void ParseReferences(Str text, Vec<PaperRef*>& rows) {
    str::Builder current;
    bool labelled = false;
    StrVec lines;
    Split(&lines, SplitRefLabels(text), StrL("\n"));
    for (Str line : lines) {
        line = TrimLibrary(line);
        int prefix = 0;
        if (ReferenceStart(line, prefix)) {
            AddReference(rows, ToStr(current));
            current.Reset();
            current.Append(Str(line.s + prefix, len(line) - prefix));
            labelled = true;
        } else if (len(line)) {
            if (len(current)) current.AppendChar(' ');
            current.Append(line);
            if (!labelled) {
                AddReference(rows, ToStr(current));
                current.Reset();
            }
        }
    }
    AddReference(rows, ToStr(current));
}

// A heading-free bibliography needs three ordered labels and citation dates/identifiers.
static TempStr HeadlessRefs(Str text) {
    StrVec lines;
    Split(&lines, SplitRefLabels(text), StrL("\n"));
    str::Builder refs, citation;
    int expected = 1, dated = 0;
    bool accepted = false;
    for (Str line : lines) {
        line = TrimLibrary(line);
        if (accepted) {
            refs.Append(line);
            refs.AppendChar('\n');
            continue;
        }
        int prefix = 0;
        if (ReferenceStart(line, prefix)) {
            int number = 0;
            Str label = line.s[0] == '[' ? Str(line.s + 1, prefix - 3) : Str(line.s, prefix - 2);
            Str rest = str::Parse(label, "%d", &number);
            if (str::IsNull(rest) || len(rest) || number != expected) {
                expected = 1;
                dated = 0;
                refs.Reset();
                citation.Reset();
                if (number != 1) continue;
            }
            if (expected > 1 && HasRefDate(ToStr(citation))) dated++;
            if (expected == 4 && dated >= 2) {
                accepted = true;
                refs.Append(line);
                refs.AppendChar('\n');
                continue;
            }
            citation.Reset();
            expected++;
        }
        if (expected == 1) continue;
        refs.Append(line);
        refs.AppendChar('\n');
        citation.Append(line);
        citation.AppendChar(' ');
    }
    if (accepted || (expected == 4 && dated + (HasRefDate(ToStr(citation)) ? 1 : 0) >= 2))
        return str::DupTemp(ToStr(refs));
    return {};
}

static Str ReadReferences(Str source, HANDLE cancel, Str& error) {
    EngineBase* engine = CreateEngineMupdfFromFile(source, FileType::PDF, 72);
    AutoRelease release(engine);
    if (!engine || !engine->allowsCopyingText) {
        error = str::Dup(StrL("Cannot extract text; check PDF copying permissions/password."));
        return {};
    }
    str::Builder refs, tail;
    bool found = false;
    for (int page = 1; page <= engine->PageCount(); page++) {
        if (WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) return {};
        StrVec lines;
        Split(&lines, engine->GetTextForPage(page), StrL("\n"));
        for (Str line : lines) {
            if (page > engine->PageCount() - std::max(3, engine->PageCount() / 4)) {
                tail.Append(line);
                tail.AppendChar('\n');
                if (len(tail) > kReferenceTextLimit) {
                    error = str::Dup(StrL("Bibliography exceeds 8 MB; paste a smaller reference list."));
                    return {};
                }
            }
            TempStr norm = NormalizePaper(line);
            if (str::Eq(norm, StrL("references")) || str::Eq(norm, StrL("bibliography")) ||
                str::EndsWith(norm, StrL(" references")) || str::Eq(norm, StrL("literature cited"))) {
                found = true;
                continue;
            }
            if (found && len(TrimLibrary(line)) && !(len(norm) < 5 && isdigit((u8)norm.s[0]))) {
                refs.Append(line);
                refs.AppendChar('\n');
            }
            if (len(refs) > kReferenceTextLimit) {
                error = str::Dup(StrL("Bibliography exceeds 8 MB; paste a smaller reference list."));
                return {};
            }
        }
    }
    if (!found) {
        TempStr headless = HeadlessRefs(ToStr(tail));
        if (len(headless)) return str::Dup(headless);
        error = str::Dup(
            StrL("No References/Bibliography heading found. Paste one citation per line; scanned PDFs need OCR."));
    }
    return str::Dup(ToStr(refs));
}

static TempStr NodeText(const GumboNode* node, Str tag) {
    const GumboNode* child = GumboFindDescendantByTag(node, tag, HtmlNameMatch::Local);
    return child ? TrimLibrary(GumboTextContentTemp(child, GumboTextMode::Descendants)) : TempStr();
}

static void ParseArxiv(Str xml, Vec<PaperRef*>& rows) {
    GumboDoc doc(xml, GumboMode::XmlFragment);
    Vec<const GumboNode*> stack;
    VecAppend(stack, doc.Document());
    while (len(stack) && len(rows) < kArxivResults) {
        const GumboNode* node = VecPop(stack);
        if (!node) continue;
        if (!GumboTagNameIs(node, StrL("entry"), HtmlNameMatch::Local)) {
            GumboPushChildren(stack, node);
            continue;
        }
        TempStr abs = NodeText(node, StrL("id"));
        TempStr id = FindArxiv(abs);
        if (!len(id)) continue;
        auto* row = new PaperRef();
        row->title = str::Dup(NodeText(node, StrL("title")));
        row->raw = str::Dup(row->title);
        row->arxiv = str::Dup(id);
        row->doi = str::Dup(NodeText(node, StrL("doi")));
        row->abstract = str::Dup(NodeText(node, StrL("summary")));
        str::Builder authors;
        Vec<const GumboNode*> children;
        GumboPushChildren(children, node);
        while (len(children)) {
            const GumboNode* child = VecPop(children);
            if (GumboTagNameIs(child, StrL("author"))) {
                if (len(authors)) authors.Append(StrL(", "));
                authors.Append(NodeText(child, StrL("name")));
            }
            if (GumboTagNameIs(child, StrL("link"))) {
                TempStr type = GumboAttributeValueTemp(child, "title");
                if (str::Eq(type, StrL("pdf"))) {
                    TempStr url = GumboAttributeValueTemp(child, "href");
                    if (str::StartsWith(url, StrL("http://arxiv.org/")))
                        url = fmt("https://%s", Str(url.s + 7, len(url) - 7));
                    if (ResearchUrlAllowed(url) &&
                        (str::StartsWith(url, StrL("https://arxiv.org/pdf/")) || gForTesting))
                        str::ReplaceWithCopy(&row->url, url);
                }
            }
        }
        if (!len(row->url)) row->url = str::Dup(fmt("https://arxiv.org/pdf/%s", id));
        row->authors = str::Dup(ToStr(authors));
        row->status = str::Dup(StrL("Found"));
        VecAppend(rows, row);
    }
}

static TempStr ArxivQueryUrl(LibraryJob* job, Str query) {
    bool idQuery = str::StartsWith(query, StrL("id:"));
    Str value = idQuery ? Str(query.s + 3, len(query) - 3) : query;
    return fmt("%s?%s=%s&start=0&max_results=%d", job->api, idQuery ? StrL("id_list") : StrL("search_query"),
               URLEncodeMayTruncateTemp(value), kArxivResults);
}

static bool ArxivRequest(LibraryJob* job, Str query, Vec<PaperRef*>& rows, Str* error = nullptr) {
    // Serialize API calls across windows; the wait is cancellable and off the UI thread.
    while (!TryAcquireSRWLockExclusive(&gArxivLock)) {
        if (WaitForSingleObject(job->cancel, 50) == WAIT_OBJECT_0) return false;
    }
    DWORD interval = gForTesting && str::StartsWith(job->api, StrL("http://127.0.0.1:")) ? 1 : kArxivIntervalMs;
    ULONGLONG now = GetTickCount64();
    if (now < gArxivLastRequest + interval &&
        WaitForSingleObject(job->cancel, (DWORD)(gArxivLastRequest + interval - now)) == WAIT_OBJECT_0) {
        ReleaseSRWLockExclusive(&gArxivLock);
        return false;
    }
    gArxivLastRequest = GetTickCount64();
    TempStr url = ArxivQueryUrl(job, query);
    Str text, finalUrl;
    bool ok = ResearchFetchText(url, text, finalUrl, job->cancel, error);
    ReleaseSRWLockExclusive(&gArxivLock);
    if (ok) ParseArxiv(text, rows);
    str::Free(text);
    str::Free(finalUrl);
    return ok;
}

static bool SameArxiv(Str a, Str b) {
    if (str::Eq(a, b)) return true;
    // An unversioned citation accepts the current version; explicit versions must match.
    int va = str::IndexOfChar(a, 'v'), vb = str::IndexOfChar(b, 'v');
    if (va >= 0) return false;
    return vb >= 0 && str::Eq(a, Str(b.s, vb));
}

static bool PaperMatches(PaperRef* ref, PaperRef* candidate) {
    if (len(ref->arxiv)) return SameArxiv(ref->arxiv, candidate->arxiv);
    if (len(ref->doi) && len(candidate->doi)) return str::EqI(ref->doi, candidate->doi);
    TempStr title = NormalizePaper(candidate->title), citation = NormalizePaper(ref->raw);
    if (len(title) < 8 || !str::Contains(citation, title)) return false;
    if (len(title) >= 16) return true;
    int comma = str::IndexOfChar(candidate->authors, ',');
    TempStr author = NormalizePaper(comma < 0 ? candidate->authors : Str(candidate->authors.s, comma));
    return len(author) >= 4 && str::Contains(citation, author);
}

static TempStr FirstAuthor(PaperRef* ref) {
    Str text = len(ref->authors) ? ref->authors : ref->raw;
    if (len(ref->authors)) {
        int comma = str::IndexOfChar(text, ',');
        return TrimLibrary(comma < 0 ? text : Str(text.s, comma));
    }
    for (int start = 0; start < len(text);) {
        int end = start;
        while (end < len(text) && text.s[end] != ' ' && text.s[end] != ',') end++;
        Str word(text.s + start, end - start);
        while (len(word) && word.s[len(word) - 1] == '.') word.len--;
        if (len(word) >= 3) return str::DupTemp(word);
        start = end + 1;
    }
    return {};
}

static int CitationYear(Str raw) {
    int year = 0;
    for (int i = 0; i + 4 <= len(raw); i++) {
        int before = i - 1;
        while (before >= 0 &&
               (raw.s[before] == ' ' || raw.s[before] == '\t' || raw.s[before] == '\n' || raw.s[before] == '\r'))
            before--;
        if (before < 0 || (raw.s[before] != '(' && raw.s[before] != ',')) continue;
        int end = i + 4;
        if (end < len(raw) && raw.s[end] != ')' && raw.s[end] != '.' && raw.s[end] != ',' && raw.s[end] != ';')
            continue;
        int value = 0;
        for (int j = i; j < end; j++) {
            if (raw.s[j] < '0' || raw.s[j] > '9') {
                value = 0;
                break;
            }
            value = value * 10 + raw.s[j] - '0';
        }
        if (value >= kEarliestCitationYear && value > year) year = value;
    }
    return year;
}

static bool ArxivEligible(Str raw, Str id) {
    int year = CitationYear(raw);
    return len(id) || !year || year >= kArxivFirstYear;
}

static Str FindArxivPdf(LibraryJob* job, PaperRef* ref) {
    if (!ArxivEligible(ref->raw, ref->arxiv)) {
        TraceAttempt(job, ref, StrL("arXiv"), {}, {},
                     fmt("Skipped: citation year %d precedes arXiv", CitationYear(ref->raw)), {});
        return {};
    }
    StrVec queries;
    if (len(ref->arxiv)) queries.Append(fmt("id:%s", ref->arxiv));
    if (len(ref->title)) queries.Append(fmt("ti:\"%s\"", ref->title));
    if (!ref->titleResolved) {
        TempStr raw = CleanPaperTemp(ref->raw);
        const char separators[] = {',', '.'};
        for (char separator : separators) {
            for (int rank = 0; rank < 2; rank++) {
                TempStr title = RankedTitlePart(raw, separator, rank);
                if (!len(title) || len(FindArxiv(title))) continue;
                TempStr query = fmt("ti:\"%s\"", title);
                if (queries.Find(query) < 0) queries.Append(query);
            }
        }
    }
    TempStr author = FirstAuthor(ref);
    if (len(author)) queries.Append(fmt("au:\"%s\"", author));
    if (len(ref->doi)) queries.Append(fmt("all:\"%s\"", ref->doi));
    for (Str query : queries) {
        if (WaitForSingleObject(job->cancel, 0) == WAIT_OBJECT_0) break;
        Vec<PaperRef*> candidates;
        Str error;
        bool ok = ArxivRequest(job, query, candidates, &error);
        Str result;
        int matches = 0;
        for (PaperRef* item : candidates) {
            if (PaperMatches(ref, item)) {
                matches++;
                str::ReplaceWithCopy(&result, item->url);
            }
            delete item;
        }
        TraceAttempt(job, ref, StrL("arXiv"), query, matches == 1 ? result : ArxivQueryUrl(job, query),
                     !ok            ? StrL("Request failed")
                     : matches == 1 ? StrL("Matched")
                     : matches > 1  ? StrL("Ambiguous matches")
                                    : StrL("No exact match"),
                     error);
        str::Free(error);
        if (matches == 1) return result;
        str::Free(result);
    }
    return {};
}

constexpr int kAnnaResults = 50;
constexpr int kAnnaLinks = 10;

struct AnnaItem {
    Str title, path, format, info;
    StrVec links;
    ~AnnaItem() {
        for (Str s : {title, path, format, info}) str::Free(s);
    }
};

struct AnnaData {
    Vec<AnnaItem*> items;
    ~AnnaData() {
        for (AnnaItem* item : items) delete item;
    }
};

static void AnnaField(AnnaItem* item, Str key, Str value) {
    if (str::Eq(key, StrL("title")))
        str::ReplaceWithCopy(&item->title, value);
    else if (str::Eq(key, StrL("path")))
        str::ReplaceWithCopy(&item->path, value);
    else if (str::Eq(key, StrL("extension")))
        str::ReplaceWithCopy(&item->format, value);
    else if (str::Eq(key, StrL("publisher")) || str::Eq(key, StrL("description")))
        str::ReplaceWithCopy(&item->info, fmt("%s %s", item->info, value));
}

static void ReadAnnaSearch(AnnaData* data, json::Value* value) {
    if (value->type != json::Type::String) return;
    int index = json::PathSegIndex(json::PathNth(value->path, 0));
    if (index < 0 || index >= kAnnaResults) return;
    while (len(data->items) <= index) VecAppend(data->items, new AnnaItem());
    if (json::PathMatch(value->path, StrL("*"), StrL("/title")) ||
        json::PathMatch(value->path, StrL("*"), StrL("/path")) ||
        json::PathMatch(value->path, StrL("*"), StrL("/publisher")))
        AnnaField(data->items[index], json::PathSegKey(json::PathNth(value->path, 1)), value->value);
    else if (json::PathMatch(value->path, StrL("*"), StrL("/file_info"), StrL("/extension")))
        AnnaField(data->items[index], StrL("extension"), value->value);
}

static void ReadAnnaDetail(AnnaItem* item, json::Value* value) {
    if (value->type != json::Type::String) return;
    if (json::PathMatch(value->path, StrL("/title")) || json::PathMatch(value->path, StrL("/publisher")) ||
        json::PathMatch(value->path, StrL("/description")))
        AnnaField(item, json::PathSegKey(value->path), value->value);
    else if (json::PathMatch(value->path, StrL("/file_info"), StrL("/extension")))
        AnnaField(item, StrL("extension"), value->value);
    else if (json::PathMatch(value->path, StrL("/download_links"), StrL("*"), StrL("/url")) &&
             len(item->links) < kAnnaLinks)
        item->links.Append(value->value);
}

static bool AnnaClass(const GumboNode* node, Str name) {
    TempStr classes = fmt(" %s ", GumboAttributeValueTemp(node, "class"));
    return str::Contains(classes, fmt(" %s ", name));
}

// The upstream scraper exposes search cards inside HTML comments.
static TempStr AnnaHtmlTemp(Str html) {
    str::Builder out;
    for (int i = 0; i < len(html);) {
        Str rest(html.s + i, len(html) - i);
        if (str::StartsWith(rest, StrL("<!--")))
            i += 4;
        else if (str::StartsWith(rest, StrL("-->")))
            i += 3;
        else
            out.AppendChar(html.s[i++]);
    }
    return str::DupTemp(ToStr(out));
}

static void AnnaHtmlSearch(Str html, AnnaData& data) {
    GumboDoc doc(AnnaHtmlTemp(html), GumboMode::Html);
    Vec<const GumboNode*> nodes;
    VecAppend(nodes, doc.Document());
    while (len(nodes) && len(data.items) < kAnnaResults) {
        const GumboNode* node = VecPop(nodes);
        GumboPushChildren(nodes, node);
        if (!GumboTagNameIs(node, StrL("a"))) continue;
        TempStr path = GumboAttributeValueTemp(node, "href");
        if (!str::StartsWith(path, StrL("/md5/"))) continue;
        const GumboNode* heading = GumboFindDescendantByTag(node, StrL("h3"));
        if (!heading) continue;
        auto* item = new AnnaItem();
        item->title = str::Dup(TrimLibrary(GumboTextContentTemp(heading, GumboTextMode::Descendants)));
        item->path = str::Dup(path);
        item->info = str::Dup(GumboTextContentTemp(node, GumboTextMode::Descendants));
        Vec<const GumboNode*> fields;
        GumboPushChildren(fields, node);
        while (len(fields)) {
            const GumboNode* field = VecPop(fields);
            GumboPushChildren(fields, field);
            if (!AnnaClass(field, StrL("text-xs"))) continue;
            TempStr text = NormalizePaper(GumboTextContentTemp(field, GumboTextMode::Descendants));
            if (str::Contains(fmt(" %s ", text), StrL(" pdf "))) str::ReplaceWithCopy(&item->format, StrL("pdf"));
        }
        VecAppend(data.items, item);
    }
}

static void AnnaHtmlDetail(Str html, AnnaItem& item) {
    GumboDoc doc(html, GumboMode::Html);
    str::Builder info;
    Vec<const GumboNode*> nodes;
    VecAppend(nodes, doc.Document());
    while (len(nodes)) {
        const GumboNode* node = VecPop(nodes);
        GumboPushChildren(nodes, node);
        if (node && node->type == GUMBO_NODE_TEXT) {
            info.Append(Str(node->v.text.text));
            info.AppendChar(' ');
        }
        if (AnnaClass(node, StrL("text-3xl")) && AnnaClass(node, StrL("font-bold")))
            str::ReplaceWithCopy(&item.title, GumboTextContentTemp(node, GumboTextMode::Descendants));
        if (AnnaClass(node, StrL("text-sm")) && AnnaClass(node, StrL("text-gray-500"))) {
            TempStr text = NormalizePaper(GumboTextContentTemp(node, GumboTextMode::Descendants));
            if (str::Contains(fmt(" %s ", text), StrL(" pdf "))) str::ReplaceWithCopy(&item.format, StrL("pdf"));
        }
        if (GumboTagNameIs(node, StrL("a")) && AnnaClass(node, StrL("js-download-link")) &&
            len(item.links) < kAnnaLinks)
            item.links.Append(GumboAttributeValueTemp(node, "href"));
    }
    item.info = str::Dup(ToStr(info));
}

static bool AnnaTitleMatches(PaperRef* ref, Str title) {
    TempStr key = NormalizePaper(str::ReplaceTemp(title, StrL("\xF0\x9F\x94\x8D"), StrL("")));
    return len(key) >= 16 && str::Contains(NormalizePaper(ref->raw), key);
}

static bool AnnaPdfLinks(LibraryJob* job, PaperRef* ref, StrVec& links, Str& error) {
    Str endpoint = len(job->annaApi) ? job->annaApi : job->anna;
    if (!ResearchUrlAllowed(endpoint)) return false;
    Str query = len(ref->doi) ? ref->doi : ref->title;
    TempStr search = fmt("%ssearch?q=%s&ext=pdf", endpoint, URLEncodeMayTruncateTemp(query));
    Str text, finalUrl;
    bool ok = ResearchFetchText(search, text, finalUrl, job->cancel, &error);
    AnnaData results;
    if (ok && len(job->annaApi))
        ok = json::Parse(text, MkFunc1(ReadAnnaSearch, &results));
    else if (ok)
        AnnaHtmlSearch(text, results);
    str::Free(text);
    str::Free(finalUrl);
    text = {};
    finalUrl = {};
    if (!ok) {
        if (!len(error)) str::ReplaceWithCopy(&error, fmt("Anna search unavailable: %s", search));
        return false;
    }
    AnnaItem* match = nullptr;
    int count = 0;
    for (AnnaItem* item : results.items) {
        if (!str::EqI(item->format, StrL("pdf")) || !str::StartsWith(item->path, StrL("/md5/"))) continue;
        if (!AnnaTitleMatches(ref, item->title) && !(len(ref->doi) && str::EqI(FindDoi(item->info), ref->doi)))
            continue;
        if (match && str::Eq(match->path, item->path)) continue;
        match = item;
        count++;
    }
    if (count != 1) {
        str::ReplaceWithCopy(&error, count ? StrL("Multiple Anna editions/results; choose one in Browse.")
                                           : StrL("No exact Anna match; unavailable pages may require Browse."));
        return false;
    }
    TempStr record = len(job->annaApi) ? fmt("%sdownload?path=%s", endpoint, URLEncodeMayTruncateTemp(match->path))
                                       : ResearchResolveUrlTemp(endpoint, match->path);
    AnnaItem detail;
    ok = ResearchFetchText(record, text, finalUrl, job->cancel, &error);
    if (ok && len(job->annaApi))
        ok = json::Parse(text, MkFunc1(ReadAnnaDetail, &detail));
    else if (ok)
        AnnaHtmlDetail(text, detail);
    if (ok && str::EqI(detail.format, StrL("pdf")) && AnnaTitleMatches(ref, detail.title) &&
        (!len(ref->doi) || str::EqI(FindDoi(detail.info), ref->doi))) {
        for (Str href : detail.links) {
            TempStr url = ResearchResolveUrlTemp(len(job->annaApi) ? job->anna : finalUrl, href);
            if (len(url)) links.Append(url);
        }
    }
    str::Free(text);
    str::Free(finalUrl);
    if (!len(links))
        str::ReplaceWithCopy(&error, StrL("Anna metadata mismatch or no usable download link; use Browse."));
    return len(links) > 0;
}

struct ReferenceMetadata {
    Vec<PaperRef*> rows;
    ~ReferenceMetadata() {
        for (PaperRef* row : rows) delete row;
    }
};

static void ReadRefMetadata(ReferenceMetadata* ctx, json::Value* value) {
    if (value->type != json::Type::String) return;
    bool list = str::Eq(json::PathSegKey(json::PathNth(value->path, 1)), StrL("items"));
    bool doi = list ? json::PathMatch(value->path, StrL("/message"), StrL("/items"), StrL("*"), StrL("/DOI"))
                    : json::PathMatch(value->path, StrL("/message"), StrL("/DOI"));
    bool title =
        list ? json::PathMatch(value->path, StrL("/message"), StrL("/items"), StrL("*"), StrL("/title"), StrL("i0"))
             : json::PathMatch(value->path, StrL("/message"), StrL("/title"), StrL("i0"));
    bool author = list ? json::PathMatch(value->path, StrL("/message"), StrL("/items"), StrL("*"), StrL("/author"),
                                         StrL("*"), StrL("/family"))
                       : json::PathMatch(value->path, StrL("/message"), StrL("/author"), StrL("*"), StrL("/family"));
    if (!doi && !title && !author) return;
    int index = list ? json::PathSegIndex(json::PathNth(value->path, 2)) : 0;
    constexpr int kMetadataResults = 5;
    if (index < 0 || index >= kMetadataResults) return;
    while (len(ctx->rows) <= index) VecAppend(ctx->rows, new PaperRef());
    PaperRef* row = ctx->rows[index];
    if (author) {
        str::ReplaceWithCopy(&row->authors,
                             len(row->authors) ? fmt("%s, %s", row->authors, value->value) : value->value);
        return;
    }
    if (doi) {
        str::ReplaceWithCopy(&row->doi, value->value);
        return;
    }
    TempStr text = str::DupTemp(value->value);
    if (str::Contains(text, StrL("<"))) {
        GumboDoc doc(text, GumboMode::Html);
        text = GumboTextContentTemp(GumboFindDescendantByTag(doc.Document(), StrL("body")), GumboTextMode::Descendants);
    }
    str::ReplaceWithCopy(&row->title, CleanPaperTemp(text));
}

static void ResolveRefMetadata(LibraryJob* job, PaperRef* ref) {
    if (!ResearchUrlAllowed(job->metadata)) return;
    TempStr url = len(ref->doi) ? fmt("%s/%s", job->metadata, URLEncodeMayTruncateTemp(ref->doi))
                                : fmt("%s?query.bibliographic=%s&rows=5&select=DOI,title,author", job->metadata,
                                      URLEncodeMayTruncateTemp(ref->raw));
    Str text, finalUrl, error;
    bool ok = ResearchFetchText(url, text, finalUrl, job->cancel, &error);
    int matches = 0;
    bool sameTitle = true, sameDoi = true, sameAuthors = true;
    if (ok) {
        ReferenceMetadata ctx;
        if (json::Parse(text, MkFunc1(ReadRefMetadata, &ctx))) {
            PaperRef* match = nullptr;
            for (PaperRef* row : ctx.rows) {
                if (!len(FindDoi(row->doi)) || !len(row->title) || !PaperMatches(ref, row)) continue;
                matches++;
                if (!match) {
                    match = row;
                    continue;
                }
                sameTitle = sameTitle && str::Eq(NormalizePaper(match->title), NormalizePaper(row->title));
                sameDoi = sameDoi && str::EqI(match->doi, row->doi);
                sameAuthors = sameAuthors && str::Eq(NormalizePaper(match->authors), NormalizePaper(row->authors));
            }
            if (matches && sameTitle) {
                EnterCriticalSection(&job->lock);
                if (sameDoi) str::ReplaceWithCopy(&ref->doi, match->doi);
                str::ReplaceWithCopy(&ref->title, match->title);
                ref->titleResolved = true;
                if (sameAuthors) str::ReplaceWithCopy(&ref->authors, match->authors);
                LeaveCriticalSection(&job->lock);
            }
        }
    }
    TraceAttempt(job, ref, StrL("Crossref"), ref->raw, url,
                 !ok                               ? StrL("Request failed")
                 : matches && sameTitle && sameDoi ? StrL("Metadata resolved")
                 : matches && sameTitle            ? StrL("Title resolved; DOI ambiguous")
                 : matches > 1                     ? StrL("Ambiguous matches")
                                                   : StrL("No exact match"),
                 error);
    str::Free(text);
    str::Free(finalUrl);
    str::Free(error);
}

static TempStr SafePaperName(PaperRef* row, int index) {
    str::Builder name;
    Str title = len(row->title) ? row->title : row->raw;
    for (int i = 0; i < len(title) && len(name) < 100; i++) {
        u8 c = (u8)title.s[i];
        name.AppendChar((char)(c < ' ' || strchr("<>:\"/\\|?*", c) ? '_' : c));
    }
    while (len(name) && ((ToStr(name).s[len(name) - 1] == ' ') || (ToStr(name).s[len(name) - 1] == '.'))) name.len--;
    return fmt("%04d-%s.pdf", index + 1, ToStr(name));
}

static void SetRowStatus(LibraryJob* job, PaperRef* row, Str status) {
    EnterCriticalSection(&job->lock);
    str::ReplaceWithCopy(&row->status, status);
    LeaveCriticalSection(&job->lock);
}

static TempStr LibraryState(LibraryJob* job) {
    EnterCriticalSection(&job->lock);
    str::Builder out;
    out.Append(
        fmt("{\"running\":%s,\"processed\":%d,\"total\":%d,\"message\":\"%s\",\"folder\":\"%s\",\"report\":\"%s\","
            "\"rows\":[",
            job->running ? StrL("true") : StrL("false"), job->processed, job->total, json::EscapeStrTemp(job->message),
            json::EscapeStrTemp(job->folder), json::EscapeStrTemp(job->report)));
    for (int i = 0; i < len(job->rows); i++) {
        if (i) out.AppendChar(',');
        PaperRef* r = job->rows[i];
        out.Append(fmt(
            "{\"raw\":\"%s\",\"title\":\"%s\",\"doi\":\"%s\",\"arxiv\":\"%s\",\"abstract\":\"%s\",\"authors\":\"%"
            "s\",\"url\":\"%s\",\"path\":\"%s\",\"status\":\"%s\",\"source\":\"%s\",\"error\":\"%s\",\"attempts\":[",
            json::EscapeStrTemp(r->raw), json::EscapeStrTemp(r->title), json::EscapeStrTemp(r->doi),
            json::EscapeStrTemp(r->arxiv), json::EscapeStrTemp(r->abstract), json::EscapeStrTemp(r->authors),
            json::EscapeStrTemp(r->url), json::EscapeStrTemp(r->path), json::EscapeStrTemp(r->status),
            json::EscapeStrTemp(r->source), json::EscapeStrTemp(r->error)));
        for (int j = 0; j < len(r->attempts); j++) {
            if (j) out.AppendChar(',');
            LibraryAttempt* a = r->attempts[j];
            out.Append(fmt("{\"source\":\"%s\",\"query\":\"%s\",\"url\":\"%s\",\"status\":\"%s\",\"error\":\"%s\"}",
                           json::EscapeStrTemp(a->source), json::EscapeStrTemp(a->query), json::EscapeStrTemp(a->url),
                           json::EscapeStrTemp(a->status), json::EscapeStrTemp(a->error)));
        }
        out.Append(StrL("]}"));
    }
    out.Append(StrL("]}"));
    TempStr result = str::DupTemp(ToStr(out));
    LeaveCriticalSection(&job->lock);
    return result;
}

static TempStr GoogleLinkTemp(Str href) {
    if (!str::StartsWith(href, StrL("/url?"))) return str::DupTemp(href);
    int start = str::IndexOf(href, StrL("q="));
    int prefix = 2;
    if (start < 0) {
        start = str::IndexOf(href, StrL("url="));
        prefix = 4;
    }
    if (start < 0) return {};
    Str value(href.s + start + prefix, len(href) - start - prefix);
    int end = str::IndexOfChar(value, '&');
    if (end >= 0) value.len = end;
    WCHAR* decoded = CWStrTemp(ToWStrTemp(value));
    DWORD count = (DWORD)wcslen(decoded);
    if (FAILED(UrlUnescapeW(decoded, nullptr, &count, URL_UNESCAPE_INPLACE))) return {};
    return ToUtf8Temp(WStr(decoded));
}

static bool WebPdfMatches(PaperRef* ref, Str output) {
    EngineBase* engine = CreateEngineMupdfFromFile(output, FileType::PDF, 72);
    AutoRelease release(engine);
    if (!engine) return false;
    TempStr title = NormalizePaper(ref->title);
    if (len(title) < 8) return false;
    return str::Contains(NormalizePaper(engine->GetTextForPage(1)), title);
}

enum class WebSearchState {
    Results,
    BrowserRequired
};

static Str GoogleArxivPdf(LibraryJob* job, PaperRef* ref, Str url, Str& error) {
    TempStr query = fmt("id:%s", FindArxiv(url));
    Vec<PaperRef*> candidates;
    bool ok = ArxivRequest(job, query, candidates, &error);
    Str result;
    PaperRef* match = nullptr;
    int matches = 0;
    for (PaperRef* candidate : candidates) {
        if (!PaperMatches(ref, candidate)) continue;
        match = candidate;
        matches++;
    }
    if (ok && matches == 1 && ResearchUrlAllowed(match->url)) {
        result = str::Dup(match->url);
        EnterCriticalSection(&job->lock);
        str::ReplaceWithCopy(&ref->title, match->title);
        str::ReplaceWithCopy(&ref->authors, match->authors);
        str::ReplaceWithCopy(&ref->arxiv, match->arxiv);
        LeaveCriticalSection(&job->lock);
    }
    TraceAttempt(job, ref, StrL("Google"), query, url,
                 len(result) ? StrL("arXiv metadata matched") : StrL("arXiv result rejected"), error);
    for (PaperRef* candidate : candidates) delete candidate;
    return result;
}

static Str GooglePdfQuery(LibraryJob* job, PaperRef* ref, Str output, Str query, Str& error, WebSearchState& state) {
    TempStr search = fmt("%s?q=%s", job->web, URLEncodeMayTruncateTemp(query));
    Str text, finalUrl;
    bool ok = ResearchFetchText(search, text, finalUrl, job->cancel, &error);
    StrVec links;
    if (ok) {
        GumboDoc doc(text, GumboMode::Html);
        Vec<const GumboNode*> nodes;
        VecAppend(nodes, doc.Document());
        while (len(nodes) && len(links) < kWebResults) {
            const GumboNode* node = VecPop(nodes);
            GumboPushChildren(nodes, node);
            if (!GumboTagNameIs(node, StrL("a"))) continue;
            TempStr url = GoogleLinkTemp(GumboAttributeValueTemp(node, "href"));
            TempStr label = NormalizePaper(GumboTextContentTemp(node, GumboTextMode::Descendants));
            if (!ResearchUrlAllowed(url) ||
                (!len(FindArxiv(url)) && !str::ContainsI(url, StrL(".pdf")) && !str::Contains(label, StrL("pdf"))))
                continue;
            if (links.Find(url) < 0) links.Append(url);
        }
    }
    if (ok && !len(links) && str::Contains(text, StrL("/httpservice/retry/enablejs"))) {
        state = WebSearchState::BrowserRequired;
        str::ReplaceWithCopy(&error, StrL("Google requires JavaScript; use Browse Google with the full citation."));
    }
    TraceAttempt(job, ref, StrL("Google"), query, search,
                 !ok                                        ? StrL("Request failed")
                 : state == WebSearchState::BrowserRequired ? StrL("Browser required")
                 : len(links)                               ? StrL("PDF candidates")
                                                            : StrL("No PDF links in response"),
                 error);
    str::Free(text);
    str::Free(finalUrl);
    for (Str url : links) {
        if (WaitForSingleObject(job->cancel, 0) == WAIT_OBJECT_0) break;
        str::Free(error);
        error = {};
        Str arxivPdf;
        if (len(FindArxiv(url))) {
            arxivPdf = GoogleArxivPdf(job, ref, url, error);
            if (!len(arxivPdf)) continue;
        }
        Str download = len(arxivPdf) ? arxivPdf : url;
        ok = ResearchFetchPdf(download, output, error, job->cancel);
        if (ok && !WebPdfMatches(ref, output)) {
            DeleteFileW(CWStrTemp(output));
            error = str::Dup(StrL("PDF title could not be verified on its first page; rejected."));
            ok = false;
        }
        TraceAttempt(job, ref, StrL("Google"), query, download, ok ? StrL("Downloaded") : StrL("Download failed"),
                     error);
        Str result = ok ? str::Dup(download) : Str{};
        str::Free(arxivPdf);
        if (ok) return result;
    }
    if (!len(error)) error = str::Dup(StrL("Google returned no verified PDF; open the search to inspect its results."));
    return {};
}

static Str GooglePdf(LibraryJob* job, PaperRef* ref, Str output, Str& error) {
    TempStr query = fmt("%s pdf", len(ref->raw) ? ref->raw : ref->title);
    WebSearchState state = WebSearchState::Results;
    return GooglePdfQuery(job, ref, output, query, error, state);
}

static void CollectOne(LibraryJob* job, PaperRef* ref, int index) {
    if (len(ref->path) && file::Exists(ref->path)) {
        SetRowStatus(job, ref, StrL("Already downloaded"));
        return;
    }
    if (job->task == LibraryTask::Collect && !len(ref->arxiv)) {
        SetRowStatus(job, ref, StrL("Reading citation metadata"));
        ResolveRefMetadata(job, ref);
    }
    TempStr output = path::JoinTemp(job->folder, SafePaperName(ref, index));
    if (file::Exists(output)) {
        SetRowStatus(job, ref, StrL("File exists; choose another folder"));
        return;
    }
    Str url, error;
    const Str sources[] = {StrL("arXiv"), StrL("Sci-Hub"), StrL("Google")};
    for (int step = 0; step < 3; step++) {
        if (WaitForSingleObject(job->cancel, 0) == WAIT_OBJECT_0) break;
        SetRowStatus(job, ref, fmt("Searching %s", sources[step]));
        if (step == 0) url = job->task == LibraryTask::Download ? str::Dup(ref->url) : FindArxivPdf(job, ref);
        if (step == 1 && len(ref->doi) && ResearchUrlAllowed(job->sci))
            url = str::Dup(fmt("%s%s", job->sci, URLEncodeMayTruncateTemp(ref->doi)));
        bool ok = false;
        if (step == 2) {
            url = GooglePdf(job, ref, output, error);
            ok = len(url) > 0;
        } else
            ok = len(url) && ResearchFetchPdf(url, output, error, job->cancel);
        Str status = ok                                           ? StrL("Downloaded")
                     : len(url)                                   ? StrL("Download failed")
                     : step == 1 && !len(ref->doi)                ? StrL("Skipped: no DOI")
                     : step == 1 && !ResearchUrlAllowed(job->sci) ? StrL("Skipped: endpoint disabled")
                                                                  : StrL("No PDF link");
        TraceAttempt(job, ref, sources[step], step == 1 ? ref->doi : Str{}, url, status, error);
        if (ok) {
            EnterCriticalSection(&job->lock);
            str::ReplaceWithCopy(&ref->path, output);
            str::ReplaceWithCopy(&ref->source, sources[step]);
            str::ReplaceWithCopy(&ref->url, url);
            str::ReplaceWithCopy(&ref->error, {});
            LeaveCriticalSection(&job->lock);
            SetRowStatus(job, ref, StrL("Downloaded"));
        } else {
            EnterCriticalSection(&job->lock);
            str::ReplaceWithCopy(
                &ref->error, len(error) ? error : StrL("No exact match, missing identifier or unavailable endpoint."));
            LeaveCriticalSection(&job->lock);
        }
        str::Free(url);
        url = {};
        str::Free(error);
        error = {};
        if (ok) return;
        if (job->task == LibraryTask::Download) break;
    }
    SetRowStatus(
        job, ref,
        WaitForSingleObject(job->cancel, 0) == WAIT_OBJECT_0 ? StrL("Cancelled") : StrL("Not found / browser needed"));
}

static bool SaveLibraryManifest(LibraryJob* job, Str state) {
    if (!len(job->manifest)) {
        TempStr target = path::JoinTemp(job->folder, StrL("collection.json"));
        for (int attempt = 0; attempt < 5; attempt++) {
            TempStr candidate = MakeUniqueFilePathTemp(target);
            HANDLE claim = CreateFileW(CWStrTemp(candidate), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
            if (claim == INVALID_HANDLE_VALUE) continue;
            CloseHandle(claim);
            str::ReplaceWithCopy(&job->manifest, candidate);
            break;
        }
    }
    return len(job->manifest) && file::WriteFile(job->manifest, state);
}

static TempStr HtmlTextTemp(Str text) {
    text = str::ReplaceTemp(text, StrL("&"), StrL("&amp;"));
    text = str::ReplaceTemp(text, StrL("<"), StrL("&lt;"));
    text = str::ReplaceTemp(text, StrL(">"), StrL("&gt;"));
    text = str::ReplaceTemp(text, StrL("\""), StrL("&quot;"));
    return str::ReplaceTemp(text, StrL("'"), StrL("&#39;"));
}

static bool SaveLibraryReport(LibraryJob* job) {
    if (!len(job->manifest)) return false;
    str::Builder html;
    html.Append(StrL(R"HTML(<!doctype html><html lang="vi"><meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'">
<title>Kết quả tải reference</title><style>body{font:15px Segoe UI,sans-serif;margin:24px;background:#f6f7f9;color:#202124}table{border-collapse:collapse;width:100%;background:white}td,th{border:1px solid #ddd;padding:10px;text-align:left;vertical-align:top}th{background:#e8edf3}p{margin:6px 0}small{display:block;color:#555}a{overflow-wrap:anywhere}td:nth-child(2){min-width:260px}td:last-child{min-width:340px}.failed{color:#a32323}.ok{color:#17643a}</style>
<h1>Các reference chưa tải được</h1><p>Thứ tự tải: arXiv ID → tên bài → tác giả → DOI → Sci-Hub → Google (citation đầy đủ + PDF). Bài trước 1991 không có ID bỏ qua arXiv. Crossref chỉ tra metadata.</p>)HTML"));
    EnterCriticalSection(&job->lock);
    int missing = 0;
    for (PaperRef* row : job->rows)
        if (!len(row->path)) missing++;
    html.Append(fmt("<p>Paper gốc: %s</p><p>%d / %d reference chưa tải được. %s</p>", HtmlTextTemp(job->paper), missing,
                    len(job->rows), HtmlTextTemp(job->message)));
    for (int group = 0; group < 2; group++) {
        html.Append(group ? StrL("<h2>Đã tải được</h2>") : StrL("<h2>Chưa tải được — nguồn đã thử và lỗi</h2>"));
        html.Append(
            StrL("<table><thead><tr><th>#</th><th>Tên bài / nguồn trong paper</th><th>Kết quả</th><th>Từng nguồn đã "
                 "thử</th></tr></thead><tbody>"));
        for (int i = 0; i < len(job->rows); i++) {
            PaperRef* r = job->rows[i];
            if ((len(r->path) > 0) != (group == 1)) continue;
            html.Append(
                fmt("<tr><td>%d</td><td><b>%s</b><p>%s</p><small>DOI: %s; arXiv ID trong citation: %s</small></td><td "
                    "class=\"%s\">%s<p>%s</p><small>%s</small></td><td>",
                    i + 1, HtmlTextTemp(r->title), HtmlTextTemp(r->raw), HtmlTextTemp(r->doi), HtmlTextTemp(r->arxiv),
                    group ? StrL("ok") : StrL("failed"), HtmlTextTemp(r->status), HtmlTextTemp(r->error),
                    HtmlTextTemp(r->source)));
            for (Str source : {StrL("Crossref"), StrL("arXiv"), StrL("Sci-Hub"), StrL("Google")}) {
                bool tried = false;
                for (LibraryAttempt* a : r->attempts) {
                    if (!str::Eq(a->source, source)) continue;
                    tried = true;
                    html.Append(fmt("<p><b>%s</b>: %s<small>Query: %s</small><small>%s</small>", HtmlTextTemp(source),
                                    HtmlTextTemp(a->status), HtmlTextTemp(a->query), HtmlTextTemp(a->error)));
                    if (ResearchUrlAllowed(a->url))
                        html.Append(fmt("<a href=\"%s\">%s</a>", HtmlTextTemp(a->url), HtmlTextTemp(a->url)));
                    html.Append(StrL("</p>"));
                }
                if (!tried) html.Append(fmt("<p>%s: chưa thử.</p>", HtmlTextTemp(source)));
            }
            html.Append(StrL("</td></tr>"));
        }
        html.Append(StrL("</tbody></table>"));
    }
    html.Append(StrL("</html>"));
    TempStr report = MakeUniqueFilePathTemp(fmt("%s.html", path::GetPathNoExtTemp(job->manifest)));
    bool ok = file::WriteFile(report, ToStr(html));
    if (ok) str::ReplaceWithCopy(&job->report, report);
    LeaveCriticalSection(&job->lock);
    return ok;
}

static void OpenLibraryReport(LibraryWindow* w) {
    EnterCriticalSection(&w->job->lock);
    TempStr report = str::DupTemp(w->job->report);
    LeaveCriticalSection(&w->job->lock);
    if (!len(report) || !file::Exists(report)) return;
    WCHAR url[4096];
    DWORD count = dimof(url);
    if (FAILED(UrlCreateFromPathW(CWStrTemp(report), url, &count, 0))) return;
    SimpleBrowserCreateArgs args;
    args.title = StrL("Reference download report");
    args.url = ToUtf8Temp(WStr(url));
    args.dataDir = path::JoinTemp(path::GetDirTemp(GetSettingsPathTemp()), StrL("ResearchBrowser"));
    SimpleBrowserWindowCreate(args);
    w->reportOpened = true;
}

static void LibraryWorker(LibraryJob* job) {
    if (job->task == LibraryTask::Extract) {
        Str error;
        Str text = ReadReferences(job->paper, job->cancel, error);
        Vec<PaperRef*> rows;
        ParseReferences(text, rows);
        EnterCriticalSection(&job->lock);
        for (PaperRef* r : job->rows) delete r;
        job->rows = rows;
        str::ReplaceWithCopy(
            &job->message,
            len(error) ? error
                       : StrL("Review extracted references; wrapped or unnumbered citations may need editing."));
        LeaveCriticalSection(&job->lock);
        str::Free(text);
        str::Free(error);
    } else if (job->task == LibraryTask::Search) {
        Vec<PaperRef*> rows;
        bool ok = ArxivRequest(job, job->query, rows);
        EnterCriticalSection(&job->lock);
        for (PaperRef* r : job->rows) delete r;
        job->rows = rows;
        str::ReplaceWithCopy(&job->message,
                             ok ? StrL("arXiv results (up to 20). Use id: for an exact arXiv identifier.")
                                : StrL("arXiv request failed; retry or open the browser."));
        LeaveCriticalSection(&job->lock);
    } else {
        if (!dir::Create(job->folder) && !dir::Exists(job->folder)) {
            EnterCriticalSection(&job->lock);
            str::ReplaceWithCopy(&job->message, StrL("Cannot create the destination folder."));
            LeaveCriticalSection(&job->lock);
        } else {
            for (int i = 0; i < len(job->rows); i++) {
                AutoArenaSavepoint scratch;
                if (job->task == LibraryTask::Download && i != job->download) continue;
                if (WaitForSingleObject(job->cancel, 0) == WAIT_OBJECT_0) break;
                CollectOne(job, job->rows[i], i);
                EnterCriticalSection(&job->lock);
                if (!str::Eq(job->rows[i]->status, StrL("Cancelled"))) job->processed++;
                LeaveCriticalSection(&job->lock);
                if (!SaveLibraryManifest(job, LibraryState(job))) {
                    EnterCriticalSection(&job->lock);
                    str::ReplaceWithCopy(&job->message,
                                         StrL("Cannot save the collection report. Downloaded PDFs are kept."));
                    LeaveCriticalSection(&job->lock);
                    SetEvent(job->cancel);
                    break;
                }
            }
            EnterCriticalSection(&job->lock);
            str::ReplaceWithCopy(
                &job->message,
                WaitForSingleObject(job->cancel, 0) == WAIT_OBJECT_0
                    ? StrL("Stopped. Completed PDFs are kept; press Collect to retry remaining rows.")
                    : StrL("Finished. See each reference status; unresolved items can be opened in the browser."));
            LeaveCriticalSection(&job->lock);
        }
    }
    if (len(job->manifest)) {
        if ((job->task == LibraryTask::Collect || job->task == LibraryTask::Download) && !SaveLibraryReport(job)) {
            EnterCriticalSection(&job->lock);
            str::ReplaceWithCopy(&job->message,
                                 StrL("Could not save HTML report; JSON diagnostics and downloaded PDFs are kept."));
            LeaveCriticalSection(&job->lock);
        }
        TempStr state = str::ReplaceTemp(LibraryState(job), StrL("\"running\":true"), StrL("\"running\":false"));
        SaveLibraryManifest(job, state);
    }
    EnterCriticalSection(&job->lock);
    job->running = false;
    LeaveCriticalSection(&job->lock);
    job->Release();
}

static bool StartLibrary(LibraryWindow* w, LibraryTask task) {
    w->refresh = true;
    LibraryJob* job = w->job;
    EnterCriticalSection(&job->lock);
    if (job->running) {
        LeaveCriticalSection(&job->lock);
        return false;
    }
    job->task = task;
    job->running = true;
    job->processed = 0;
    str::ReplaceWithCopy(&job->report, {});
    w->reportOpened = false;
    job->total = task == LibraryTask::Collect ? len(job->rows) : task == LibraryTask::Download ? 1 : 0;
    str::ReplaceWithCopy(&job->message, task == LibraryTask::Extract  ? StrL("Reading references...")
                                        : task == LibraryTask::Search ? StrL("Searching arXiv...")
                                                                      : StrL("Collecting PDFs..."));
    ResetEvent(job->cancel);
    InterlockedIncrement(&job->refs);
    LeaveCriticalSection(&job->lock);
    ThreadHandle thread = StartThread(MkFunc0(LibraryWorker, job), StrL("PaperLibrary"));
    if (thread) {
        SafeCloseThreadHandle(&thread);
        return true;
    }
    EnterCriticalSection(&job->lock);
    job->running = false;
    LeaveCriticalSection(&job->lock);
    job->Release();
    return false;
}

static TempStr EndpointTemp(Str url) {
    return !len(url) || str::EndsWith(url, StrL("/")) ? str::DupTemp(url) : fmt("%s/", url);
}

static void ReadLibraryArgs(StrVec* args, json::Value* value) {
    if (value->type == json::Type::String) args->Append(value->value);
}

struct LibraryConfig {
    Str api, sci, anna, annaApi, metadata, web;
    ~LibraryConfig() {
        str::Free(api);
        str::Free(sci);
        str::Free(anna);
        str::Free(annaApi);
        str::Free(metadata);
        str::Free(web);
    }
};

static void ReadLibraryConfig(LibraryConfig* cfg, json::Value* value) {
    if (value->type != json::Type::String) return;
    if (json::PathMatch(value->path, StrL("/api"))) str::ReplaceWithCopy(&cfg->api, value->value);
    if (json::PathMatch(value->path, StrL("/sci"))) str::ReplaceWithCopy(&cfg->sci, value->value);
    if (json::PathMatch(value->path, StrL("/anna"))) str::ReplaceWithCopy(&cfg->anna, value->value);
    if (json::PathMatch(value->path, StrL("/annaApi"))) str::ReplaceWithCopy(&cfg->annaApi, value->value);
    if (json::PathMatch(value->path, StrL("/metadata"))) str::ReplaceWithCopy(&cfg->metadata, value->value);
    if (json::PathMatch(value->path, StrL("/web"))) str::ReplaceWithCopy(&cfg->web, value->value);
}

static void LibraryBrowser(Str url) {
    if (!ResearchUrlAllowed(url)) return;
    SimpleBrowserCreateArgs args;
    args.title = StrL("Research browser");
    args.url = url;
    args.dataDir = path::JoinTemp(path::GetDirTemp(GetSettingsPathTemp()), StrL("ResearchBrowser"));
    SimpleBrowserWindowCreate(args);
}

static TempStr PickLibraryFolder(HWND owner) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);
    dialog->SetTitle(L"Choose a folder for referenced papers");
    IShellItem* item = nullptr;
    PWSTR path = nullptr;
    TempStr result;
    if (SUCCEEDED(dialog->Show(owner)) && SUCCEEDED(dialog->GetResult(&item)) &&
        SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        result = ToUtf8Temp(WStr(path));
    if (path) CoTaskMemFree(path);
    if (item) item->Release();
    dialog->Release();
    return result;
}

static Str LibraryHtml() {
    return StrL(R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'">
<style>:root{color-scheme:light dark}body{font:15px Segoe UI,sans-serif;margin:18px}button,input,textarea{font:inherit;padding:7px}button{cursor:pointer}input{min-width:180px}#q,#folder{width:65%}textarea{width:98%;height:170px}header{position:sticky;top:0;background:Canvas;padding:6px 0}article{border-top:1px solid GrayText;padding:12px 0}h3{margin:4px 0}.small{font-size:13px;color:GrayText}.status{font-weight:600}summary{cursor:pointer}button:disabled{cursor:default;opacity:.5}</style></head><body>
<h2 id="heading"></h2><div id="search"><input id="q" placeholder="Title, author, keywords, or id:2401.12345"><button id="searchBtn">Search arXiv</button><button id="siteBtn">Website</button></div>
<details id="refs"><summary>Review / edit references (one complete citation per line)</summary><textarea id="citations"></textarea><button id="applyBtn">Apply edited list</button></details>
<details><summary>Fallback mirrors</summary><label>Sci-Hub HTTPS <input id="sci"></label><label>Anna website HTTPS <input id="anna"></label><label>Anna API HTTPS (optional) <input id="annaApi" placeholder="Your CrazyZard API server"></label><p class="small">Leave API empty to use the website directly. For CrazyZard API, enter the server base URL; /download returns links, not a PDF. CAPTCHA or queues need Browse.</p></details>
<header><input id="folder" placeholder="Destination folder"><button id="folderBtn">Browse folder</button><button id="collectBtn">Collect all the papers</button><button id="stopBtn">Stop</button><button id="openFolderBtn">Open folder</button><div id="message" role="status"></div><progress id="progress" aria-label="Reference collection progress" style="width:100%;height:20px" hidden></progress><div id="counts" role="status"></div></header><main id="results"></main>
<script>let state={rows:[]},mode='search',editing=false,lastRows='';
const el=id=>document.getElementById(id);const send=(m,...args)=>window.__sumatra__.notify(m,...args);
function init(m,sci,anna,annaApi,folder){mode=m;el('heading').textContent=m==='collect'?'Collect referenced papers':'arXiv paper search';el('search').hidden=m==='collect';el('refs').hidden=m!=='collect';el('collectBtn').hidden=m!=='collect';el('sci').value=sci;el('anna').value=anna;el('annaApi').value=annaApi;el('folder').value=folder}
function update(s){state=s;el('message').textContent=s.message;const progress=el('progress');progress.hidden=!s.running&&!s.total;if(s.total){progress.max=s.total;progress.value=s.processed}else progress.removeAttribute('value');el('counts').textContent=(s.total?s.processed+' / '+s.total+' references processed; ':s.rows.length+' references; ')+s.rows.filter(r=>r.path).length+' PDFs downloaded';for(const id of ['searchBtn','collectBtn','applyBtn','folderBtn'])el(id).disabled=s.running;el('stopBtn').disabled=!s.running;
if(mode==='collect'&&!editing&&el('citations').value==='')el('citations').value=s.rows.map(r=>r.raw).join('\n');
const key=JSON.stringify([s.running,s.rows]);if(key===lastRows)return;lastRows=key;el('results').replaceChildren();
s.rows.forEach((r,i)=>{const a=document.createElement('article'),h=document.createElement('h3');h.textContent=(i+1)+'. '+r.title;a.append(h);for(const text of [r.authors,r.doi||r.arxiv,r.raw!==r.title?r.raw:'']){if(!text)continue;const p=document.createElement('div');p.className='small';p.textContent=text;a.append(p)}if(r.abstract){const d=document.createElement('details'),s=document.createElement('summary'),p=document.createElement('p');s.textContent='Abstract';p.textContent=r.abstract;d.append(s,p);a.append(d)}const status=document.createElement('p');status.className='status';status.textContent=r.status+(r.source?' — '+r.source:'')+(r.error?' — '+r.error:'');a.append(status);
for(const [label,action]of [['Open PDF','open'],['Download','download'],['Browse arXiv','arxiv'],['Browse Sci-Hub','sci'],['Browse Google','google'],["Browse Anna's Archive",'anna']]){if(label==='Open PDF'&&!r.path||label==='Download'&&(!r.url||mode==='collect'))continue;const b=document.createElement('button');b.textContent=label;b.disabled=s.running&&action!=='arxiv'&&action!=='sci'&&action!=='anna'&&action!=='google';b.onclick=()=>send('row',String(i),action,el('folder').value,el('sci').value,el('anna').value,el('annaApi').value);a.append(b)}el('results').append(a)})}
el('citations').oninput=()=>editing=true;el('applyBtn').onclick=()=>send('edit',el('citations').value);el('searchBtn').onclick=()=>send('search',el('q').value);el('q').onkeydown=e=>{if(e.key==='Enter'&&!state.running)el('searchBtn').click()};el('siteBtn').onclick=()=>send('site');el('folderBtn').onclick=()=>send('folder');el('openFolderBtn').onclick=()=>send('open-folder');el('collectBtn').onclick=()=>send('collect',el('folder').value,el('sci').value,el('anna').value,el('annaApi').value);el('stopBtn').onclick=()=>send('stop');
</script></body></html>)HTML");
}

static bool LibraryResource(void* ctx, Str path, WebViewResourceResult* res) {
    if (!str::Eq(path, StrL("index.html")) && !str::Eq(path, StrL("/index.html"))) return false;
    ((LibraryWindow*)ctx)->resources++;
    Str html = LibraryHtml();
    res->data = (const u8*)html.s;
    res->dataLen = len(html);
    res->contentType = str::Dup(StrL("text/html; charset=utf-8"));
    res->ownsData = false;
    return true;
}

static bool LibraryBusy(LibraryWindow* w) {
    EnterCriticalSection(&w->job->lock);
    bool busy = w->job->running;
    LeaveCriticalSection(&w->job->lock);
    return busy;
}

static bool LibraryReady(LibraryWindow* w, Str folder, Str sci, Str anna, Str annaApi) {
    if (LibraryBusy(w) || !path::IsAbsolute(folder)) return false;
    if ((len(sci) && !ResearchUrlAllowed(sci)) || (len(anna) && !ResearchUrlAllowed(anna)) ||
        (len(annaApi) && !ResearchUrlAllowed(annaApi)))
        return false;
    if (!path::IsSame(w->job->folder, folder)) str::ReplaceWithCopy(&w->job->manifest, {});
    str::ReplaceWithCopy(&w->job->folder, folder);
    str::ReplaceWithCopy(&w->job->sci, EndpointTemp(sci));
    str::ReplaceWithCopy(&w->job->anna, EndpointTemp(anna));
    str::ReplaceWithCopy(&w->job->annaApi, EndpointTemp(annaApi));
    if (!gForTesting) {
        str::ReplaceWithCopy(&gSettings->researchSciHubUrl, w->job->sci);
        str::ReplaceWithCopy(&gSettings->researchAnnaUrl, w->job->anna);
        str::ReplaceWithCopy(&gSettings->researchAnnaApiUrl, w->job->annaApi);
        ScheduleSaveSettings();
    }
    return true;
}

static void LibraryNotify(void* ctx, Str method, Str params) {
    auto* w = (LibraryWindow*)ctx;
    if (gForTesting && str::Eq(method, StrL("probe"))) {
        str::ReplaceWithCopy(&w->probe, params);
        return;
    }
    w->refresh = true;
    StrVec args;
    if (len(params) > kReferenceTextLimit || !json::Parse(params, MkFunc1(ReadLibraryArgs, &args))) return;
    if (str::Eq(method, StrL("stop"))) {
        SetEvent(w->job->cancel);
        return;
    }
    if (str::Eq(method, StrL("site"))) {
        LibraryBrowser(StrL("https://arxiv.org/search/"));
        return;
    }
    if (str::Eq(method, StrL("folder")) && !LibraryBusy(w)) {
        TempStr folder = PickLibraryFolder(w->hwnd);
        if (len(folder)) {
            str::ReplaceWithCopy(&w->job->folder, folder);
            w->web->Eval(fmt("document.getElementById('folder').value=\"%s\"", json::EscapeStrTemp(folder)));
        }
        return;
    }
    if (str::Eq(method, StrL("open-folder"))) {
        if (dir::Exists(w->job->folder))
            ShellExecuteW(w->hwnd, L"open", CWStrTemp(w->job->folder), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (str::Eq(method, StrL("report")) && !LibraryBusy(w)) {
        OpenLibraryReport(w);
        return;
    }
    if (str::Eq(method, StrL("row")) && len(args) >= 2) {
        int index = -1;
        Str rest = str::Parse(args[0], "%d", &index);
        if (str::IsNull(rest) || len(rest)) return;
        EnterCriticalSection(&w->job->lock);
        bool valid = index >= 0 && index < len(w->job->rows);
        PaperRef* r = valid ? w->job->rows[index] : nullptr;
        Str path = r ? str::DupTemp(r->path) : TempStr();
        Str title = r ? str::DupTemp(r->title) : TempStr();
        Str raw = r ? str::DupTemp(r->raw) : TempStr();
        Str doi = r ? str::DupTemp(r->doi) : TempStr();
        Str id = r ? str::DupTemp(r->arxiv) : TempStr();
        LeaveCriticalSection(&w->job->lock);
        if (!valid) return;
        if (str::Eq(args[1], StrL("open"))) {
            MainWindow* owner = FindMainWindowByHwnd(w->owner);
            if (owner && file::Exists(path)) {
                LoadArgs load(path, owner);
                load.activateExisting = true;
                load.noSavePrefs = true;
                load.placeWindow = false;
                LoadDocument(&load);
            }
        } else if (str::Eq(args[1], StrL("download")) && len(args) == 6 &&
                   LibraryReady(w, args[2], args[3], args[4], args[5])) {
            w->job->download = index;
            StartLibrary(w, LibraryTask::Download);
        } else if (str::Eq(args[1], StrL("arxiv"))) {
            LibraryBrowser(
                len(id) ? fmt("https://arxiv.org/abs/%s", id)
                        : fmt("https://arxiv.org/search/?query=%s&searchtype=title", URLEncodeMayTruncateTemp(title)));
        } else if (str::Eq(args[1], StrL("sci")) && len(args) == 6 && ResearchUrlAllowed(args[3])) {
            LibraryBrowser(fmt("%s%s", EndpointTemp(args[3]), URLEncodeMayTruncateTemp(len(doi) ? doi : title)));
        } else if (str::Eq(args[1], StrL("anna")) && len(args) == 6 && ResearchUrlAllowed(args[4])) {
            LibraryBrowser(
                fmt("%ssearch?ext=pdf&q=%s", EndpointTemp(args[4]), URLEncodeMayTruncateTemp(len(doi) ? doi : title)));
        } else if (str::Eq(args[1], StrL("google"))) {
            LibraryBrowser(
                fmt("%s?q=%s", kGoogleSearch, URLEncodeMayTruncateTemp(fmt("%s pdf", len(raw) ? raw : title))));
        }
        return;
    }
    if (LibraryBusy(w)) return;
    if (str::Eq(method, StrL("edit")) && len(args) == 1) {
        for (PaperRef* r : w->job->rows) delete r;
        VecReset(w->job->rows);
        ParseReferences(args[0], w->job->rows);
    } else if (str::Eq(method, StrL("search")) && len(args) == 1 && len(TrimLibrary(args[0]))) {
        Str query = args[0];
        TempStr id = FindArxiv(query);
        if (len(id)) query = fmt("id:%s", id);
        str::ReplaceWithCopy(&w->job->query, query);
        StartLibrary(w, LibraryTask::Search);
    } else if (str::Eq(method, StrL("collect")) && len(args) == 4) {
        if (LibraryReady(w, args[0], args[1], args[2], args[3]))
            StartLibrary(w, LibraryTask::Collect);
        else
            str::ReplaceWithCopy(&w->job->message,
                                 StrL("Choose an absolute destination folder and valid HTTPS mirror addresses."));
    }
}

static LRESULT CALLBACK LibraryProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto* w = (LibraryWindow*)data;
    if (msg == WM_SIZE && w->web && w->web->hwnd) {
        RECT r{};
        GetClientRect(hwnd, &r);
        MoveWindow(w->web->hwnd, 0, 0, r.right, r.bottom, TRUE);
        w->web->UpdateWebviewSize();
    } else if (msg == WM_TIMER && wp == kLibraryTimer) {
        bool busy = LibraryBusy(w);
        if (w->refresh || busy || w->wasBusy) {
            w->web->Eval(fmt("if(typeof update==='function')update(%s)", LibraryState(w->job)));
            w->refresh = false;
        }
        w->wasBusy = busy;
        if (!busy && !w->reportOpened) OpenLibraryReport(w);
    } else if (msg == WM_NCDESTROY) {
        KillTimer(hwnd, kLibraryTimer);
        RemoveWindowSubclass(hwnd, LibraryProc, 1);
        if (gTestLibrary == w) gTestLibrary = nullptr;
        LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
        delete w;
        return result;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void OpenPaperLibrary(MainWindow* win, LibraryMode mode) {
    if (!win || (mode == LibraryMode::Collect && !win->CurrentTab())) return;
    if (!HasWebView()) {
        MessageBoxW(win->hwndFrame, L"Install Microsoft Edge WebView2 Runtime to use the paper library.",
                    L"Paper library", MB_OK);
        return;
    }
    auto* w = new LibraryWindow();
    w->owner = win->hwndFrame;
    w->mode = mode;
    WindowTab* tab = win->CurrentTab();
    w->job->paper = tab ? str::Dup(tab->filePath) : Str{};
    w->job->api = str::Dup(StrL("https://export.arxiv.org/api/query"));
    w->job->metadata = str::Dup(StrL("https://api.crossref.org/works"));
    w->job->web = str::Dup(kGoogleSearch);
    w->job->sci = str::Dup(EndpointTemp(gSettings->researchSciHubUrl));
    w->job->anna = str::Dup(EndpointTemp(gSettings->researchAnnaUrl));
    w->job->annaApi = str::Dup(EndpointTemp(gSettings->researchAnnaApiUrl));
    Str parent = len(w->job->paper) ? path::GetDirTemp(w->job->paper) : GetSpecialFolderTemp(CSIDL_PERSONAL);
    Str name = len(w->job->paper) ? fmt("%s-references", path::GetBaseNameTemp(w->job->paper)) : StrL("arXiv papers");
    w->job->folder = str::Dup(path::JoinTemp(parent, name));
    TempStr title = mode == LibraryMode::Collect ? fmt("Collect references — %s", path::GetBaseNameTemp(w->job->paper))
                                                 : str::DupTemp(StrL("arXiv paper search"));
    w->hwnd = CreateWindowExW(WS_EX_APPWINDOW, WC_STATIC, CWStrTemp(title), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1100, 800, win->hwndFrame, nullptr,
                              GetModuleHandle(nullptr), nullptr);
    if (!w->hwnd) {
        delete w;
        return;
    }
    SetWindowSubclass(w->hwnd, LibraryProc, 1, (DWORD_PTR)w);
    w->web = new WebviewWnd();
    w->web->dataDir = str::Dup(path::JoinTemp(path::GetDirTemp(GetSettingsPathTemp()), StrL("PaperLibrary")));
    w->web->resourceUriPrefix = wstr::Dup(WStrL(L"https://papers.sumatra/"));
    w->web->resourceProvider = {w, LibraryResource};
    w->web->events.ctx = w;
    w->web->events.jsNotify = LibraryNotify;
    w->web->events.navigationStarting = [](void*, Str url, bool) {
        return str::Eq(url, StrL("index.html")) || str::Eq(url, StrL("/index.html"));
    };
    w->web->events.navigationCompleted = [](void* ctx, Str, bool ok) {
        auto* window = (LibraryWindow*)ctx;
        window->navigated = ok;
        if (!ok) return;
        window->refresh = true;
        window->web->Eval(fmt("init(\"%s\",\"%s\",\"%s\",\"%s\",\"%s\")",
                              window->mode == LibraryMode::Collect ? StrL("collect") : StrL("search"),
                              json::EscapeStrTemp(window->job->sci), json::EscapeStrTemp(window->job->anna),
                              json::EscapeStrTemp(window->job->annaApi), json::EscapeStrTemp(window->job->folder)));
    };
    CreateWebViewArgs args;
    args.parent = w->hwnd;
    args.pos = Rect(0, 0, 1100, 800);
    w->web->Create(args);
    w->web->forwardAppAccelerators = false;
    w->web->Navigate(StrL("https://papers.sumatra/index.html"));
    SetTimer(w->hwnd, kLibraryTimer, kLibraryPollMs, nullptr);
    if (mode == LibraryMode::Collect) StartLibrary(w, LibraryTask::Extract);
    if (gForTesting) gTestLibrary = w;
    ShowWindow(w->hwnd, SW_SHOW);
    SendMessageW(w->hwnd, WM_SIZE, 0, 0);
}

TempStr TestPaperLibraryTemp(MainWindow* win, Str action, Str a, Str b, Str c, int* code) {
    *code = 1;
    if (!gForTesting) return {};
    if (str::Eq(action, StrL("library-title"))) {
        *code = 0;
        if (len(b)) return RankedTitlePart(a, b.s[0], str::Eq(c, StrL("1")) ? 1 : 0);
        return GuessPaperTitle(a);
    }
    if (str::Eq(action, StrL("library-anna-find"))) {
        LibraryJob job;
        job.anna = str::Dup(EndpointTemp(b));
        job.annaApi = str::Dup(EndpointTemp(c));
        PaperRef ref;
        ref.raw = str::Dup(a);
        ref.title = str::Dup(GuessPaperTitle(a));
        ref.doi = str::Dup(FindDoi(a));
        StrVec links;
        Str error;
        bool ok = AnnaPdfLinks(&job, &ref, links, error);
        TempStr result = ok ? str::DupTemp(links[0]) : str::DupTemp(error);
        str::Free(error);
        *code = ok ? 0 : 1;
        return result;
    }
    if (str::Eq(action, StrL("library-fetch-text"))) {
        Str text, url;
        bool ok = ResearchFetchText(a, text, url, nullptr);
        if (ok) ok = file::WriteFile(b, text);
        TempStr result = fmt("bytes=%d url=%s", len(text), url);
        str::Free(text);
        str::Free(url);
        *code = ok ? 0 : 1;
        return result;
    }
    if (str::Eq(action, StrL("library-extract"))) {
        LibraryJob job;
        Str error;
        Str text = ReadReferences(a, job.cancel, error);
        ParseReferences(text, job.rows);
        str::Free(text);
        str::Free(error);
        TempStr result = LibraryState(&job);
        int start = str::IndexOf(result, StrL("\"rows\":"));
        *code = 0;
        return str::DupTemp(Str(result.s + start + 7, len(result) - start - 8));
    }
    if (str::Eq(action, StrL("library-open"))) {
        OpenPaperLibrary(win, str::Eq(a, StrL("collect")) ? LibraryMode::Collect : LibraryMode::Search);
        *code = gTestLibrary ? 0 : 1;
        return str::DupTemp(StrL("opened"));
    }
    auto* w = gTestLibrary;
    if (!w) return str::DupTemp(StrL("no library window"));
    if (str::Eq(action, StrL("library-close"))) {
        DestroyWindow(w->hwnd);
        *code = 0;
        return str::DupTemp(StrL("closed"));
    }
    if (str::Eq(action, StrL("library-state"))) {
        *code = 0;
        return LibraryState(w->job);
    }
    if (str::Eq(action, StrL("library-ui"))) {
        if (len(a)) w->web->Eval(a);
        w->web->Eval(
            StrL("window.__sumatra__.notify('probe',{heading:document.getElementById('heading').textContent,body:"
                 "document.body.innerText,buttons:[...document.querySelectorAll('button')].map(b=>b.textContent),rows:"
                 "document.querySelectorAll('article').length})"));
        *code = 0;
        return str::DupTemp(w->probe);
    }
    if (str::Eq(action, StrL("library-web-state"))) {
        *code = 0;
        return fmt("started=%d failed=%d ready=%d resources=%d navigated=%d", w->web->initStarted, w->web->initFailed,
                   w->web->webview != nullptr, w->resources, w->navigated);
    }
    if (str::Eq(action, StrL("library-start")) && !LibraryBusy(w)) {
        LibraryConfig cfg;
        json::Parse(c, MkFunc1(ReadLibraryConfig, &cfg));
        if (!ResearchUrlAllowed(cfg.api) || !LibraryReady(w, a, cfg.sci, cfg.anna, cfg.annaApi))
            return str::DupTemp(StrL("bad configuration"));
        str::ReplaceWithCopy(&w->job->api, cfg.api);
        str::ReplaceWithCopy(&w->job->metadata, cfg.metadata);
        str::ReplaceWithCopy(&w->job->web, len(cfg.web) ? cfg.web : kGoogleSearch);
        *code = StartLibrary(w, LibraryTask::Collect) ? 0 : 1;
        return str::DupTemp(StrL("started"));
    }
    if (str::Eq(action, StrL("library-search")) && !LibraryBusy(w) && ResearchUrlAllowed(c)) {
        str::ReplaceWithCopy(&w->job->api, c);
        str::ReplaceWithCopy(&w->job->query, a);
        *code = StartLibrary(w, LibraryTask::Search) ? 0 : 1;
        return str::DupTemp(StrL("started"));
    }
    return str::DupTemp(StrL("unknown library action / busy"));
}
