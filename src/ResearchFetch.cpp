/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/File.h"
#include "base/GuessFileType.h"
#include "base/HtmlTags.h"
#include "base/Http.h"
#include "base/Win.h"
#include <winhttp.h>
#include <shlwapi.h>
#include "gui/UIModels.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "GumboHtmlParser.h"
#include "SumatraConfig.h"
#include "ResearchFetch.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shlwapi.lib")

constexpr int kResearchHtmlLimit = 1024 * 1024;
constexpr int kResearchPdfLimit = 64 * 1024 * 1024;
constexpr int kResearchTimeoutMs = 15000;
constexpr int kResearchTotalMs = 120000;
constexpr int kResearchRedirects = 5;
constexpr int kPaperNameMaxBytes = 150;

static bool IsReservedDeviceName(Str name) {
    static const char* kNames[] = {"CON", "PRN", "AUX", "NUL"};
    for (const char* n : kNames) {
        if (str::EqI(name, Str(n))) return true;
    }
    bool numbered = len(name) == 4 && isdigit((u8)name.s[3]) && name.s[3] != '0';
    return numbered && (str::StartsWithI(name, StrL("COM")) || str::StartsWithI(name, StrL("LPT")));
}

// A paper title as a Windows file name: "Descent: a \"new\" view?" -> "Descent a new view.pdf".
// Drops characters Windows forbids, folds whitespace, trims trailing dots, avoids device names
// and cuts long titles on a UTF-8 character boundary.
TempStr PaperFileNameTemp(Str title) {
    str::Builder name;
    for (int i = 0; i < len(title); i++) {
        u8 c = (u8)title.s[i];
        if (c < ' ') c = ' ';
        if (strchr("<>:\"/\\|?*", c)) continue;
        if (c == ' ' && (len(name) == 0 || name.LastChar() == ' ')) continue;
        if (len(name) >= kPaperNameMaxBytes && (c & 0xc0) != 0x80) break;
        name.AppendChar((char)c);
    }
    while (len(name) > 0 && (name.LastChar() == ' ' || name.LastChar() == '.')) name.RemoveLast();
    if (len(name) == 0) return str::DupTemp(StrL("paper.pdf"));
    if (IsReservedDeviceName(ToStr(name))) name.AppendChar('_');
    return fmt("%s.pdf", ToStr(name));
}

bool ResearchUrlAllowed(Str url) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = (DWORD)-1;
    parts.dwUserNameLength = parts.dwPasswordLength = (DWORD)-1;
    TempWStr wide = ToWStrTemp(url);
    // Base.h supplies WinINet URL_COMPONENTS and its scheme enum; use its matching parser.
    if (!InternetCrackUrlW(wide.s, wide.len, 0, &parts) || !parts.dwHostNameLength || parts.dwUserNameLength ||
        parts.dwPasswordLength)
        return false;
    int authority = str::StartsWithI(url, StrL("https://")) ? 8 : 7;
    for (int i = authority; i < url.len && url.s[i] != '/' && url.s[i] != '?' && url.s[i] != '#'; i++) {
        if (url.s[i] == '@') return false;
    }
    if (parts.nScheme == INTERNET_SCHEME_HTTPS) return true;
    WStr host(parts.lpszHostName, (int)parts.dwHostNameLength);
    return parts.nScheme == INTERNET_SCHEME_HTTP &&
           (wstr::Eq(host, WStrL(L"127.0.0.1")) || wstr::EqI(host, WStrL(L"localhost")));
}

TempStr ResearchResolveUrlTemp(Str base, Str relative) {
    WCHAR result[8192]{};
    DWORD size = dimof(result);
    if (FAILED(UrlCombineW(CWStrTemp(base), CWStrTemp(relative), result, &size, 0))) return {};
    TempStr url = ToUtf8Temp(WStr(result));
    return ResearchUrlAllowed(url) ? url : TempStr();
}

TempStr ResearchPdfUrlTemp(Str pageUrl, Str html, PdfLink links) {
    GumboDoc doc(html, GumboMode::Html);
    Vec<const GumboNode*> nodes;
    VecAppend(nodes, doc.Document());
    TempStr fallback;
    while (len(nodes)) {
        const GumboNode* node = VecPop(nodes);
        GumboPushChildren(nodes, node);
        TempStr src;
        if (GumboTagNameIs(node, StrL("iframe")) || GumboTagNameIs(node, StrL("embed")))
            src = GumboAttributeValueTemp(node, "src");
        else if (GumboTagNameIs(node, StrL("object")))
            src = GumboAttributeValueTemp(node, "data");
        if (len(src)) return ResearchResolveUrlTemp(pageUrl, src);
        if (links == PdfLink::Embedded || len(fallback) || !GumboTagNameIs(node, StrL("a"))) continue;
        TempStr href = GumboAttributeValueTemp(node, "href");
        TempStr label = GumboTextContentTemp(node, GumboTextMode::Descendants);
        if (str::ContainsI(href, StrL(".pdf")) || str::ContainsI(href, StrL("get.php")) ||
            str::ContainsI(href, StrL("/scimag/")) || str::EqI(label, StrL("GET")) || str::EqI(label, StrL("Download")))
            fallback = ResearchResolveUrlTemp(pageUrl, href);
    }
    return fallback;
}

// A non-empty `form` is POSTed (url-encoded) to `initial`.
static bool ResearchGet(Str initial, str::Builder& data, Str& finalUrl, ULONGLONG deadline, HANDLE cancel,
                        Str* error = nullptr, Str form = {}) {
    AutoFree<char> url(str::Dup(initial).s);
    TempWStr postTarget;
    DWORD redirectStatus = 0;
    for (int redirect = 0; redirect <= kResearchRedirects; redirect++) {
        if (!ResearchUrlAllowed(Str(url.Get())) || GetTickCount64() >= deadline ||
            (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0))
            return false;
        URL_COMPONENTS p{};
        p.dwStructSize = sizeof(p);
        p.dwHostNameLength = p.dwUrlPathLength = p.dwExtraInfoLength = (DWORD)-1;
        TempWStr wide = ToWStrTemp(Str(url.Get()));
        if (!InternetCrackUrlW(wide.s, wide.len, 0, &p)) return false;
        TempWStr host = str::DupTemp(WStr(p.lpszHostName, (int)p.dwHostNameLength));
        TempWStr target = str::DupTemp(WStr(p.lpszUrlPath, (int)(p.dwUrlPathLength + p.dwExtraInfoLength)));
        int fragment = wstr::IndexOfChar(target, L'#');
        if (fragment >= 0) target = str::DupTemp(WStr(target.s, fragment));

        // 307/308 keep a POST; 301-303 keep it only when a mirror moved (same path, new host)
        if (redirect == 0) postTarget = target;
        bool keepPost = redirectStatus == 307 || redirectStatus == 308 || wstr::Eq(target, postTarget);
        if (redirect > 0 && !keepPost) form = {};
        HINTERNET session = WinHttpOpen(L"SumatraPDF-Research", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return false;
        AutoCall closeSession(WinHttpCloseHandle, session);
        WinHttpSetTimeouts(session, kResearchTimeoutMs, kResearchTimeoutMs, kResearchTimeoutMs, kResearchTimeoutMs);
        HINTERNET connection = WinHttpConnect(session, host.s, p.nPort, 0);
        if (!connection) return false;
        AutoCall closeConnection(WinHttpCloseHandle, connection);
        HINTERNET request = WinHttpOpenRequest(connection, len(form) ? L"POST" : L"GET", target.len ? target.s : L"/",
                                               nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               p.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (!request) return false;
        AutoCall closeRequest(WinHttpCloseHandle, request);
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy))) return false;
        const WCHAR* headers = len(form) ? L"Content-Type: application/x-www-form-urlencoded\r\n" : nullptr;
        bool sent = WinHttpSendRequest(request, headers, headers ? (DWORD)-1L : 0, (void*)form.s, (DWORD)len(form),
                                       (DWORD)len(form), 0);
        if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
            if (error) str::ReplaceWithCopy(error, fmt("Request failed (%u): %s", GetLastError(), Str(url.Get())));
            return false;
        }
        DWORD status = 0, bytes = sizeof(status);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &bytes, WINHTTP_NO_HEADER_INDEX))
            return false;
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            WCHAR location[8192]{};
            bytes = sizeof(location);
            if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, location, &bytes,
                                     WINHTTP_NO_HEADER_INDEX))
                return false;
            TempStr next = ResearchResolveUrlTemp(Str(url.Get()), ToUtf8Temp(WStr(location)));
            if (!len(next)) return false;
            url.Set(str::Dup(next).s);
            redirectStatus = status;
            continue;
        }
        if (status != 200) {
            if (error) str::ReplaceWithCopy(error, fmt("HTTP %u: %s", status, Str(url.Get())));
            return false;
        }
        char buffer[16384];
        for (;;) {
            DWORD read = 0;
            if ((cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) || GetTickCount64() >= deadline ||
                !WinHttpReadData(request, buffer, sizeof(buffer), &read))
                return false;
            if (!read) break;
            int limit = str::StartsWith(ToStr(data), StrL("%PDF-")) ? kResearchPdfLimit : kResearchHtmlLimit;
            if ((i64)data.len + read > limit || !data.Append(Str(buffer, (int)read))) return false;
        }
        str::ReplaceWithCopy(&finalUrl, Str(url.Get()));
        return true;
    }
    return false;
}

bool ResearchFetchText(Str url, Str& text, Str& finalUrl, HANDLE cancel, Str* error) {
    str::Builder body;
    bool ok = ResearchGet(url, body, finalUrl, GetTickCount64() + kResearchTotalMs, cancel, error);
    if (!ok || len(body) > kResearchHtmlLimit) return false;
    text = str::Dup(ToStr(body));
    return true;
}

// `form`: POST body of the first request, e.g. a Sci-Hub title search (see SciHubFormTemp)
bool ResearchFetchPdf(Str url, Str output, Str& error, HANDLE cancel, Str form) {
    str::ReplaceWithCopy(&error, {});
    str::Builder body;
    AutoFree<char> finalUrl;
    Str resolved;
    ULONGLONG deadline = GetTickCount64() + kResearchTotalMs;
    bool ok = ResearchGet(url, body, resolved, deadline, cancel, &error, form);
    finalUrl.Set(resolved.s);
    constexpr int kPdfPageHops = 3;
    for (int hop = 0; ok && !str::StartsWith(ToStr(body), StrL("%PDF-")) && hop < kPdfPageHops; hop++) {
        // a search answers with the article's embedded PDF; links on a search page are the site's own
        bool search = hop == 0 && len(form);
        TempStr pdfUrl =
            ResearchPdfUrlTemp(Str(finalUrl.Get()), ToStr(body), search ? PdfLink::Embedded : PdfLink::Any);
        // e.g. sci-hub.vn answers automated searches with /match/<title>, a "are you a robot?" page
        if (!len(pdfUrl) && str::ContainsI(ToStr(body), StrL("captcha")))
            str::ReplaceWithCopy(&error,
                                 fmt("The site asks for a CAPTCHA: %s. Open it with Browse.", Str(finalUrl.Get())));
        else if (!len(pdfUrl) && search)
            str::ReplaceWithCopy(&error, fmt("The search found no article: %s", Str(finalUrl.Get())));
        else if (!len(pdfUrl))
            str::ReplaceWithCopy(&error, fmt("No PDF link found on %s", Str(finalUrl.Get())));
        body.Reset();
        finalUrl.Set(nullptr);
        resolved = {};
        ok = len(pdfUrl) && ResearchGet(pdfUrl, body, resolved, deadline, cancel, &error);
        finalUrl.Set(resolved.s);
    }
    if (!ok || !str::StartsWith(ToStr(body), StrL("%PDF-"))) {
        if (!len(error))
            error = str::Dup(
                StrL("No PDF was downloaded. Open the embedded browser to handle CAPTCHA or change the endpoint."));
        return false;
    }
    EngineBase* parsed = CreateEngineMupdfFromData(ToStr(body), StrL("download.pdf"), nullptr);
    if (!parsed) {
        str::ReplaceWithCopy(&error, StrL("The server returned an unreadable PDF. No file was saved."));
        return false;
    }
    parsed->Release();
    if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) return false;
    HANDLE file = CreateFileW(CWStrTemp(output), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        str::ReplaceWithCopy(&error, StrL("Cannot create the PDF; choose a new name in a writable folder."));
        return false;
    }
    DWORD written = 0;
    ok = WriteFile(file, ToStr(body).s, body.len, &written, nullptr) && written == (DWORD)body.len;
    CloseHandle(file);
    if (!ok) {
        file::Delete(output);
        str::ReplaceWithCopy(&error, StrL("Could not finish saving the PDF."));
    }
    return ok;
}

//--- Sci-Hub requests

// "doi:10.1000/x" -> "10.1000/x", "PMID: 123" -> "123"; other queries unchanged
static TempStr SciHubIdTemp(Str query) {
    TempStr q = str::DupTemp(query);
    str::TrimWSInPlace(q, str::TrimOpt::Both);
    for (Str prefix : {StrL("doi:"), StrL("pmid:"), StrL("pmid")}) {
        if (!str::StartsWithI(q, prefix)) continue;
        q = str::DupTemp(Str(q.s + len(prefix), len(q) - len(prefix)));
        str::TrimWSInPlace(q, str::TrimOpt::Both);
        break;
    }
    return q;
}

// Sci-Hub opens a DOI, PMID or URL by path; anything else is a title for its search form.
bool SciHubIsTitle(Str query) {
    TempStr id = SciHubIdTemp(query);
    if (!len(id) || str::StartsWithI(id, StrL("http://")) || str::StartsWithI(id, StrL("https://"))) return false;
    if (str::StartsWith(id, StrL("10.")) && str::Contains(id, StrL("/")) && !str::Contains(id, StrL(" "))) return false;
    for (int i = 0; i < len(id); i++) {
        if (!isdigit((u8)id.s[i])) return true;
    }
    return false;
}

// identifiers: "<endpoint>/<id>"; titles: the endpoint itself, POSTed with SciHubFormTemp
TempStr SciHubUrlTemp(Str endpoint, Str query) {
    TempStr root = str::EndsWith(endpoint, StrL("/")) ? str::DupTemp(endpoint) : fmt("%s/", endpoint);
    if (SciHubIsTitle(query)) return root;
    return fmt("%s%s", root, URLEncodeMayTruncateTemp(SciHubIdTemp(query)));
}

// the home page form's "request" field, e.g. "request=Hamiltonian%20flows"; empty for identifiers
TempStr SciHubFormTemp(Str query) {
    if (!SciHubIsTitle(query)) return {};
    TempStr title = str::DupTemp(query);
    str::TrimWSInPlace(title, str::TrimOpt::Both);
    return fmt("request=%s", URLEncodeMayTruncateTemp(title));
}

static TempStr HtmlAttrTemp(Str s) {
    s = str::ReplaceTemp(s, StrL("&"), StrL("&amp;"));
    s = str::ReplaceTemp(s, StrL("<"), StrL("&lt;"));
    s = str::ReplaceTemp(s, StrL(">"), StrL("&gt;"));
    return str::ReplaceTemp(s, StrL("\""), StrL("&quot;"));
}

// A page that submits a title search, for browsers that can only open a URL.
TempStr SciHubFormPageTemp(Str endpoint, Str query) {
    TempStr title = str::DupTemp(query);
    str::TrimWSInPlace(title, str::TrimOpt::Both);
    return fmt(
        "<!doctype html><meta charset=\"utf-8\"><title>Sci-Hub</title>"
        "<form id=\"f\" method=\"POST\" action=\"%s\"><input type=\"hidden\" name=\"request\" "
        "value=\"%s\"><p>Searching Sci-Hub for: %s</p><button>Search</button></form>"
        "<script>document.getElementById('f').submit()</script>",
        HtmlAttrTemp(SciHubUrlTemp(endpoint, title)), HtmlAttrTemp(title), HtmlAttrTemp(title));
}
