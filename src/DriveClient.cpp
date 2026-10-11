/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include <winhttp.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include "base/Crypto.h"
#include "base/File.h"
#include "base/Http.h"
#include "base/JsonParser.h"
#include "base/Win.h"
#include "DriveClient.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ws2_32.lib")

constexpr DWORD kReplyLimit = 1024 * 1024;
constexpr DWORD kTransferChunk = 8 * 1024 * 1024; // Drive requires multiples of 256 KiB.
constexpr DWORD kIoTimeout = 15000;
constexpr DWORD kLoginTimeout = 180000;
constexpr DWORD kHttpOk = 200;
constexpr DWORD kHttpCreated = 201;
constexpr DWORD kHttpResume = 308;

DriveJob::~DriveJob() {
    str::Free(clientId);
    if (clientSecret.s) SecureZeroMemory(clientSecret.s, len(clientSecret));
    str::Free(clientSecret);
    str::Free(filePath);
    str::Free(tokenPath);
    str::Free(url);
    str::Free(error);
    str::Free(mockBase);
    str::Free(authUrlPath);
    if (cancel) CloseHandle(cancel);
}

static bool Cancelled(DriveJob* job) {
    return job->cancel && WaitForSingleObject(job->cancel, 0) == WAIT_OBJECT_0;
}

static void DriveError(DriveJob* job, Str message) {
    str::ReplaceWithCopy(&job->error, message);
}

// Copy only the requested primitive; credentials never enter logs or UI errors.
struct JsonStringCtx {
    Str a, b;
    str::Builder out;
};

static void ReadJsonString(JsonStringCtx* ctx, json::Value* v) {
    if (v->type == json::Type::String && json::PathMatch(v->path, ctx->a, ctx->b)) {
        ctx->out.Reset();
        ctx->out.Append(v->value);
    }
}

static TempStr JsonString(Str data, Str a, Str b = {}) {
    JsonStringCtx ctx;
    ctx.a = a;
    ctx.b = b;
    if (!json::Parse(data, MkFunc1(ReadJsonString, &ctx))) return {};
    return ToStrTemp(ctx.out);
}

bool DriveReadClient(DriveJob* job, Str path) {
    if (len(path) == 0) return false;
    if (file::GetSize(path) <= 0 || file::GetSize(path) > 65536) return false;
    Str data = file::ReadFile(path);
    AutoFree freeData(data.s);
    Str id = JsonString(data, StrL("/installed"), StrL("/client_id"));
    Str secret = JsonString(data, StrL("/installed"), StrL("/client_secret"));
    if (len(id) > 512 || !str::EndsWith(id, StrL(".apps.googleusercontent.com"))) return false;
    if (len(secret) > 512 || str::Contains(id, StrL("\r")) || str::Contains(id, StrL("\n"))) return false;
    str::ReplaceWithCopy(&job->clientId, id);
    str::ReplaceWithCopy(&job->clientSecret, secret);
    return true;
}

static TempStr Base64Url(const u8* data, DWORD size) {
    DWORD count = 0;
    if (!CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &count)) return {};
    auto* buf = (char*)AllocTemp(count);
    if (!CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, buf, &count)) return {};
    Str s(buf, (int)count);
    while (len(s) && (s.s[s.len - 1] == '=' || s.s[s.len - 1] == 0)) s.len--;
    for (int i = 0; i < len(s); i++) {
        if (s.s[i] == '+') s.s[i] = '-';
        if (s.s[i] == '/') s.s[i] = '_';
    }
    return s;
}

static TempStr RandomToken() {
    u8 bytes[32];
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
    return Base64Url(bytes, sizeof(bytes));
}

static TempStr ProtectToken(Str data, bool encrypt) {
    DATA_BLOB input{(DWORD)len(data), (BYTE*)data.s}, output{};
    bool ok = encrypt
                  ? CryptProtectData(&input, L"SumatraPDF Google Drive", nullptr, nullptr, nullptr,
                                     CRYPTPROTECT_UI_FORBIDDEN, &output)
                  : CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output);
    if (!ok) return {};
    TempStr result = str::DupTemp(Str((char*)output.pbData, (int)output.cbData));
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
}

static TempStr ReadToken(Str path) {
    if (file::GetSize(path) <= 0 || file::GetSize(path) > 65536) return {};
    Str blob = file::ReadFile(path);
    AutoFree freeBlob(blob.s);
    return ProtectToken(blob, false);
}

bool DriveHasAccount(Str tokenPath) {
    return len(JsonString(ReadToken(tokenPath), StrL("/refresh_token"))) > 0;
}

bool DriveForgetAccount(Str tokenPath) {
    return !file::Exists(tokenPath) || DeleteFileW(CWStrTemp(tokenPath));
}

static bool SaveToken(DriveJob* job, Str refresh) {
    Str json = fmt("{\"client_id\":\"%{0}\",\"refresh_token\":\"%{1}\"}", json::EscapeStrTemp(job->clientId),
                   json::EscapeStrTemp(refresh));
    Str encrypted = ProtectToken(json, true);
    if (len(encrypted) == 0) return false;
    Str tmp = fmt("%{0}.tmp", job->tokenPath);
    if (!file::WriteFile(tmp, encrypted)) return false;
    bool ok =
        MoveFileExW(CWStrTemp(tmp), CWStrTemp(job->tokenPath), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(CWStrTemp(tmp));
    return ok;
}

struct DriveReply {
    DWORD status = 0;
    str::Builder body;
    Str location;
    Str range;
    ~DriveReply() {
        str::Free(location);
        str::Free(range);
    }
};

static bool IsMock(DriveJob* job, const URL_COMPONENTS& uc) {
    return len(job->mockBase) > 0 && uc.nScheme == INTERNET_SCHEME_HTTP &&
           wstr::Eq(WStr(uc.lpszHostName, (int)uc.dwHostNameLength), L"127.0.0.1");
}

// Restrict destinations before applying bearer credentials, including upload Location.
static bool AllowedUrl(DriveJob* job, Str url) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUserNameLength = (DWORD)-1;
    uc.dwPasswordLength = (DWORD)-1;
    if (!InternetCrackUrlW(CWStrTemp(url), 0, 0, &uc) || uc.dwUserNameLength || uc.dwPasswordLength) return false;
    if (IsMock(job, uc)) return str::StartsWith(url, fmt("%{0}/", job->mockBase));
    if (len(job->mockBase) > 0) return false;
    WStr host(uc.lpszHostName, (int)uc.dwHostNameLength);
    return uc.nScheme == INTERNET_SCHEME_HTTPS && uc.nPort == INTERNET_DEFAULT_HTTPS_PORT &&
           (wstr::Eq(host, L"oauth2.googleapis.com") || wstr::Eq(host, L"www.googleapis.com"));
}

// Small bodies and upload chunks share one transport. Redirects are never followed.
static bool DriveRequest(DriveJob* job, Str method, Str url, Str headers, Str body, DriveReply& reply) {
    if (Cancelled(job) || !AllowedUrl(job, url)) return false;
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = (DWORD)-1;
    if (!InternetCrackUrlW(CWStrTemp(url), 0, 0, &uc)) return false;
    TempWStr host = str::DupTemp(WStr(uc.lpszHostName, (int)uc.dwHostNameLength));
    TempWStr target = str::DupTemp(WStr(uc.lpszUrlPath, (int)(uc.dwUrlPathLength + uc.dwExtraInfoLength)));
    HINTERNET session = WinHttpOpen(L"SumatraPDF-Drive", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    AutoCall closeSession(WinHttpCloseHandle, session);
    WinHttpSetTimeouts(session, kIoTimeout, kIoTimeout, kIoTimeout, kIoTimeout);
    HINTERNET conn = WinHttpConnect(session, host.s, uc.nPort, 0);
    if (!conn) return false;
    AutoCall closeConn(WinHttpCloseHandle, conn);
    HINTERNET request =
        WinHttpOpenRequest(conn, CWStrTemp(method), target.s, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                           uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!request) return false;
    AutoCall closeReq(WinHttpCloseHandle, request);
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy))) return false;
    DWORD disable = WINHTTP_DISABLE_COOKIES;
    WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
    WStr h = ToWStrTemp(headers);
    if (!WinHttpSendRequest(request, h.s, len(h), WINHTTP_NO_REQUEST_DATA, 0, (DWORD)len(body), 0)) return false;
    int pos = 0;
    while (pos < len(body)) {
        DWORD written = 0;
        DWORD n = (DWORD)(len(body) - pos);
        if (n > 64 * 1024) n = 64 * 1024;
        if (Cancelled(job) || !WinHttpWriteData(request, body.s + pos, n, &written) || written == 0) return false;
        pos += (int)written;
    }
    if (Cancelled(job) || !WinHttpReceiveResponse(request, nullptr)) return false;
    DWORD size = sizeof(reply.status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &reply.status,
                             &size, nullptr))
        return false;
    WCHAR location[8192];
    size = sizeof(location);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, nullptr, location, &size, nullptr)) {
        reply.location = ToUtf8(WStr(location));
    }
    size = sizeof(location);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, L"Range", location, &size, nullptr)) {
        reply.range = ToUtf8(WStr(location));
    }
    char buffer[8192];
    for (;;) {
        DWORD read = 0;
        if (Cancelled(job) || !WinHttpReadData(request, buffer, sizeof(buffer), &read)) return false;
        if (read == 0) return true;
        if (len(reply.body) + read > kReplyLimit || !reply.body.Append(Str(buffer, (int)read))) return false;
    }
}

static TempStr TokenUrl(DriveJob* job) {
    return len(job->mockBase) ? fmt("%{0}/token", job->mockBase) : StrL("https://oauth2.googleapis.com/token");
}

static TempStr FormField(Str name, Str value) {
    return fmt("%{0}=%{1}", name, URLEncodeMayTruncateTemp(value, 65536));
}

static TempStr TokenRequest(DriveJob* job, Str fields, Str oldRefresh = {}) {
    str::Builder form;
    form.Append(fields);
    form.Append(fmt("&%{0}", FormField(StrL("client_id"), job->clientId)));
    if (len(job->clientSecret)) form.Append(fmt("&%{0}", FormField(StrL("client_secret"), job->clientSecret)));
    DriveReply reply;
    if (!DriveRequest(job, StrL("POST"), TokenUrl(job), StrL("Content-Type: application/x-www-form-urlencoded\r\n"),
                      ToStr(form), reply) ||
        reply.status != kHttpOk)
        return {};
    Str access = JsonString(ToStr(reply.body), StrL("/access_token"));
    if (len(access) == 0 || len(access) > 8192 || str::Contains(access, StrL("\r")) ||
        str::Contains(access, StrL("\n")))
        return {};
    Str refresh = JsonString(ToStr(reply.body), StrL("/refresh_token"));
    if (len(refresh) == 0) refresh = oldRefresh;
    if (len(refresh) > 0 && !SaveToken(job, refresh)) {
        DriveError(job, StrL("Signed in, but could not save the encrypted account token. Check the settings folder."));
        return {};
    }
    return access;
}

static int HexDigit(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static TempStr DecodeQuery(Str value) {
    str::Builder decoded;
    for (int i = 0; i < len(value); i++) {
        char ch = value.s[i];
        if (ch == '%') {
            if (i + 2 >= len(value)) return {};
            int hi = HexDigit(value.s[++i]), lo = HexDigit(value.s[++i]);
            if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) return {};
            ch = (char)(hi * 16 + lo);
        } else if (ch == '+')
            ch = ' ';
        decoded.AppendChar(ch);
    }
    return ToStrTemp(decoded);
}

static TempStr QueryParam(Str query, Str name) {
    StrVec parts;
    Split(&parts, query, StrL("&"));
    str::Builder out;
    bool found = false;
    for (Str part : parts) {
        int at = str::IndexOfChar(part, '=');
        if (at < 0 || !str::Eq(Str(part.s, at), name)) continue;
        if (found) return {}; // Reject duplicate state/code parameters.
        found = true;
        out.Append(DecodeQuery(Str(part.s + at + 1, part.len - at - 1)));
    }
    return ToStrTemp(out);
}

// A loopback callback accepts only a state-matched response on /oauth2/callback.
static TempStr BrowserLogin(DriveJob* job) {
    WSADATA ws{};
    if (WSAStartup(MAKEWORD(2, 2), &ws) != 0) return {};
    AutoCall cleanup(WSACleanup);
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) return {};
    AutoCall closeListener(closesocket, listener);
    BOOL exclusive = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive, sizeof(exclusive));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (sockaddr*)&addr, sizeof(addr)) || listen(listener, 4)) return {};
    int size = sizeof(addr);
    if (getsockname(listener, (sockaddr*)&addr, &size)) return {};
    Str redirect = fmt("http://127.0.0.1:%{0}/oauth2/callback", (int)ntohs(addr.sin_port));
    Str state = RandomToken(), verifier = RandomToken();
    if (len(state) == 0 || len(verifier) == 0) return {};
    u8 hash[32];
    CalcSHA2Digest(verifier, hash);
    Str challenge = Base64Url(hash, sizeof(hash));
    Str authBase = len(job->mockBase) ? fmt("%{0}/authorize", job->mockBase)
                                      : StrL("https://accounts.google.com/o/oauth2/v2/auth");
    Str authUrl =
        fmt("%{0}?client_id=%{1}&redirect_uri=%{2}&response_type=code&scope=%{3}&state=%{4}&code_challenge=%{5}"
            "&code_challenge_method=S256&access_type=offline&prompt=consent",
            authBase, URLEncodeMayTruncateTemp(job->clientId), URLEncodeMayTruncateTemp(redirect),
            URLEncodeMayTruncateTemp(StrL("https://www.googleapis.com/auth/drive.file")), state, challenge);
    if (len(job->mockBase)) {
        if (!file::WriteFile(job->authUrlPath, authUrl)) return {};
    } else if (!LaunchBrowser(authUrl)) {
        DriveError(job, StrL("Could not open the browser for Google sign-in."));
        return {};
    }
    ULONGLONG deadline = GetTickCount64() + kLoginTimeout;
    while (!Cancelled(job) && GetTickCount64() < deadline) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listener, &set);
        timeval timeout{0, 200000};
        if (select(0, &set, nullptr, nullptr, &timeout) <= 0) continue;
        SOCKET incoming = accept(listener, nullptr, nullptr);
        if (incoming == INVALID_SOCKET) continue;
        AutoCall closeIncoming(closesocket, incoming);
        DWORD ioTimeout = 2000;
        setsockopt(incoming, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ioTimeout, sizeof(ioTimeout));
        setsockopt(incoming, SOL_SOCKET, SO_SNDTIMEO, (const char*)&ioTimeout, sizeof(ioTimeout));
        str::Builder request;
        char buffer[2048];
        while (len(request) < 8192 && !str::Contains(ToStr(request), StrL("\r\n\r\n"))) {
            int n = recv(incoming, buffer, sizeof(buffer), 0);
            if (n <= 0) break;
            request.Append(Str(buffer, n));
        }
        Str raw = ToStr(request);
        bool route = str::StartsWith(raw, StrL("GET /oauth2/callback?"));
        int end = route ? str::IndexOfChar(Str(raw.s + 21, raw.len - 21), ' ') : -1;
        Str query = end < 0 ? Str() : Str(raw.s + 21, end);
        bool valid = route && end >= 0 && str::Eq(QueryParam(query, StrL("state")), state);
        Str code = valid ? QueryParam(query, StrL("code")) : Str();
        bool denied = valid && len(QueryParam(query, StrL("error"))) > 0;
        Str response = valid ? StrL(
                                   "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
                                   "Cache-Control: no-store\r\nConnection: close\r\n\r\n"
                                   "Google sign-in received. Return to SumatraPDF; you can close this tab.")
                             : StrL("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nInvalid sign-in response.");
        send(incoming, response.s, len(response), 0);
        if (denied) {
            DriveError(job, StrL("Google sign-in was cancelled or access was not granted."));
            return {};
        }
        if (!valid || len(code) == 0) continue;
        Str fields = fmt("grant_type=authorization_code&%{0}&%{1}&%{2}", FormField(StrL("code"), code),
                         FormField(StrL("redirect_uri"), redirect), FormField(StrL("code_verifier"), verifier));
        return TokenRequest(job, fields);
    }
    DriveError(job, StrL("Google sign-in timed out or was cancelled. Click DRIVE to try again."));
    return {};
}

static bool UploadFile(DriveJob* job, Str access) {
    HANDLE file = CreateFileW(CWStrTemp(job->filePath), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        DriveError(job, StrL("Cannot read the document, or another program is changing it. Save it and try again."));
        return false;
    }
    AutoCall closeFile(CloseHandle, file);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart == 0) {
        DriveError(job, StrL("The document is empty or its size could not be read."));
        return false;
    }
    Str endpoint = len(job->mockBase)
                       ? fmt("%{0}/upload", job->mockBase)
                       : StrL("https://www.googleapis.com/upload/drive/v3/files?uploadType=resumable&fields=id");
    Str name = path::GetBaseNameTemp(job->filePath);
    Str mime =
        str::EqI(path::GetExtTemp(name), StrL(".pdf")) ? StrL("application/pdf") : StrL("application/octet-stream");
    Str metadata = fmt("{\"name\":\"%{0}\"}", json::EscapeStrTemp(name));
    Str headers =
        fmt("Authorization: Bearer %{0}\r\nContent-Type: application/json; charset=utf-8\r\n"
            "X-Upload-Content-Type: %{1}\r\nX-Upload-Content-Length: %{2}\r\n",
            access, mime, size.QuadPart);
    DriveReply session;
    if (!DriveRequest(job, StrL("POST"), endpoint, headers, metadata, session) || session.status != kHttpOk ||
        !AllowedUrl(job, session.location)) {
        DriveError(job, StrL("Google Drive could not start the upload. Check the connection and sign-in."));
        return false;
    }
    auto* buffer = (char*)Alloc(nullptr, (size_t)kTransferChunk);
    AutoFree freeBuffer(buffer);
    i64 offset = 0;
    while (offset < size.QuadPart && !Cancelled(job)) {
        i64 remaining = size.QuadPart - offset;
        DWORD wanted = remaining < kTransferChunk ? (DWORD)remaining : kTransferChunk;
        DWORD read = 0;
        if (!ReadFile(file, buffer, wanted, &read, nullptr) || read != wanted) {
            DriveError(job, StrL("The document could not be read completely."));
            return false;
        }
        Str chunkHeaders =
            fmt("Authorization: Bearer %{0}\r\nContent-Type: %{1}\r\n"
                "Content-Range: bytes %{2}-%{3}/%{4}\r\n",
                access, mime, offset, offset + read - 1, size.QuadPart);
        DriveReply reply;
        if (!DriveRequest(job, StrL("PUT"), session.location, chunkHeaders, Str(buffer, (int)read), reply)) {
            DriveError(job, StrL("The upload was interrupted. Its completion could not be confirmed."));
            return false;
        }
        offset += read;
        bool last = offset == size.QuadPart;
        if (!last && reply.status == kHttpResume) {
            if (str::Eq(reply.range, fmt("bytes=0-%{0}", offset - 1))) continue;
            DriveError(job, StrL("Google Drive acknowledged a different upload offset. Upload stopped."));
            return false;
        }
        if (last && (reply.status == kHttpOk || reply.status == kHttpCreated)) {
            Str id = JsonString(ToStr(reply.body), StrL("/id"));
            if (len(id) == 0 || len(id) > 256) return false;
            for (int i = 0; i < len(id); i++) {
                char ch = id.s[i];
                if (!(ch >= 'a' && ch <= 'z') && !(ch >= 'A' && ch <= 'Z') && !(ch >= '0' && ch <= '9') && ch != '_' &&
                    ch != '-')
                    return false;
            }
            job->url = str::Dup(fmt("https://drive.google.com/file/d/%{0}/view", id));
            return true;
        }
        DriveError(job, fmt("Google Drive rejected an upload chunk (HTTP %{0}).", reply.status));
        return false;
    }
    return false;
}

void DriveRun(DriveJob* job) {
    Str saved = ReadToken(job->tokenPath);
    Str refresh = str::Eq(JsonString(saved, StrL("/client_id")), job->clientId)
                      ? JsonString(saved, StrL("/refresh_token"))
                      : Str();
    Str access;
    if (len(refresh)) {
        access =
            TokenRequest(job, fmt("grant_type=refresh_token&%{0}", FormField(StrL("refresh_token"), refresh)), refresh);
        // Failed refresh does not send a stale token or launch another login unprompted.
        if (len(access) == 0 && len(job->error) == 0) {
            DriveError(
                job, StrL("Could not refresh Google sign-in. Check your connection, or disconnect and sign in again."));
        }
    } else
        access = BrowserLogin(job);
    if (len(access)) UploadFile(job, access);
    if (Cancelled(job)) DriveError(job, StrL("Google Drive operation cancelled."));
    if (len(job->url) == 0 && len(job->error) == 0) {
        DriveError(
            job,
            StrL("Google Drive could not complete sign-in or upload. Check the OAuth client and Drive API setup."));
    }
}
