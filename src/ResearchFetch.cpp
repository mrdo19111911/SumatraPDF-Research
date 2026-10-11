/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/File.h"
#include "base/GuessFileType.h"
#include "base/HtmlTags.h"
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

TempStr ResearchPdfUrlTemp(Str pageUrl, Str html) {
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
        if (len(fallback) || !GumboTagNameIs(node, StrL("a"))) continue;
        TempStr href = GumboAttributeValueTemp(node, "href");
        TempStr label = GumboTextContentTemp(node, GumboTextMode::Descendants);
        if (str::ContainsI(href, StrL(".pdf")) || str::ContainsI(href, StrL("get.php")) ||
            str::ContainsI(href, StrL("/scimag/")) || str::EqI(label, StrL("GET")) || str::EqI(label, StrL("Download")))
            fallback = ResearchResolveUrlTemp(pageUrl, href);
    }
    return fallback;
}

static bool ResearchGet(Str initial, str::Builder& data, Str& finalUrl, ULONGLONG deadline, HANDLE cancel,
                        Str* error = nullptr) {
    AutoFree<char> url(str::Dup(initial).s);
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
        HINTERNET session = WinHttpOpen(L"SumatraPDF-Research", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return false;
        AutoCall closeSession(WinHttpCloseHandle, session);
        WinHttpSetTimeouts(session, kResearchTimeoutMs, kResearchTimeoutMs, kResearchTimeoutMs, kResearchTimeoutMs);
        HINTERNET connection = WinHttpConnect(session, host.s, p.nPort, 0);
        if (!connection) return false;
        AutoCall closeConnection(WinHttpCloseHandle, connection);
        HINTERNET request = WinHttpOpenRequest(connection, L"GET", target.len ? target.s : L"/", nullptr,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               p.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (!request) return false;
        AutoCall closeRequest(WinHttpCloseHandle, request);
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy))) return false;
        if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request, nullptr)) {
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

bool ResearchFetchPdf(Str url, Str output, Str& error, HANDLE cancel) {
    str::ReplaceWithCopy(&error, {});
    str::Builder body;
    AutoFree<char> finalUrl;
    Str resolved;
    ULONGLONG deadline = GetTickCount64() + kResearchTotalMs;
    bool ok = ResearchGet(url, body, resolved, deadline, cancel, &error);
    finalUrl.Set(resolved.s);
    constexpr int kPdfPageHops = 3;
    for (int hop = 0; ok && !str::StartsWith(ToStr(body), StrL("%PDF-")) && hop < kPdfPageHops; hop++) {
        TempStr pdfUrl = ResearchPdfUrlTemp(Str(finalUrl.Get()), ToStr(body));
        if (!len(pdfUrl)) str::ReplaceWithCopy(&error, fmt("No PDF link found on %s", Str(finalUrl.Get())));
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
