
#include "base/Base.h"

#include <objbase.h>
#include <thumbcache.h>

#pragma comment(lib, "gdiplus.lib")

constexpr const WCHAR* kPdfPreviewClsid = L"{A651836F-6704-411A-9741-161F4626E0B0}";
constexpr const WCHAR* kXpsPreviewClsid = L"{00A18657-106C-4D67-89A8-F334D314EFA3}";
constexpr const WCHAR* kDjVuPreviewClsid = L"{97D5D6B5-97C1-4EC9-AD7E-6FF237511F21}";
constexpr const WCHAR* kEpubPreviewClsid = L"{759D5E7C-0D51-475E-B50E-9DB00E710438}";
constexpr const WCHAR* kFb2PreviewClsid = L"{F531AED7-D73B-4066-9D3B-279DB144D4C4}";
constexpr const WCHAR* kMobiPreviewClsid = L"{D5A460EF-399F-49C2-9755-525177F6CEC2}";
constexpr const WCHAR* kCbxPreviewClsid = L"{B21A8BD1-C57D-469C-BF58-65BE2CAEC680}";
constexpr const WCHAR* kTgaPreviewClsid = L"{B9A8466D-B640-47A1-B608-71E358CD8A05}";

// Our GUID here:
LPCOLESTR myGuid = kPdfPreviewClsid;

typedef HRESULT ourDllGetClassObjectT(REFCLSID rclsid, REFIID riid, void** ppv);

void log(Str s) {
    if (len(s) == 0) {
        return;
    }
    OutputDebugStringA(s.s);
    fwrite(s.s, 1, (size_t)s.len, stdout);
}
void _uploadDebugReport(Str, Str, bool) {}

static Str kPdfPreviewDllName = StrL("PdfPreview.dll");

int main(int c, char** v) {
    GUID clsid{};
    IIDFromString(myGuid, &clsid);

    if (c < 2) {
        printf("not enough arguments: file name\n");
        return 1;
    }
    HRESULT r;
    IStream* pStream = NULL;
    HMODULE dll = NULL;

    dll = LoadLibraryA(kPdfPreviewDllName.s);
    if (!dll) {
        printf("can't open DLL\n");
        return 1;
    }

    ourDllGetClassObjectT* ourDllGetClassObject = (ourDllGetClassObjectT*)GetProcAddress(dll, "DllGetClassObject");

    IClassFactory* pFactory = NULL;
    r = ourDllGetClassObject(clsid, IID_IClassFactory, (void**)&pFactory);
    if (r != S_OK) {
        printf("failed: get factory: %08x\n", r);
        return 2;
    }

    IInitializeWithStream* pInit;
    r = pFactory->CreateInstance(NULL, IID_IInitializeWithStream, (void**)&pInit);
    if (r != S_OK) {
        printf("failed: get object\n");
        return 3;
    }
    pFactory->Release();

    IThumbnailProvider* pProvider;
    r = pInit->QueryInterface(IID_IThumbnailProvider, (void**)&pProvider);
    if (r != S_OK) {
        printf("failed: get provider\n");
        return 5;
    }

    wchar_t wfile[256]{};
    MultiByteToWideChar(CP_ACP, 0, v[1], -1, wfile, 256);
    r = SHCreateStreamOnFileEx(wfile, STGM_READ, 0, FALSE, NULL, &pStream);
    if (r != S_OK || !pStream) {
        printf("can't open file\n");
        return 10;
    }

    r = pInit->Initialize(pStream, 0);
    pInit->Release();
    pStream->Release();
    if (r != S_OK) {
        printf("pInit->Initialize() failed\n");
        return 11;
    }

    HBITMAP bmp;
    WTS_ALPHATYPE alpha;
    r = pProvider->GetThumbnail(256, &bmp, &alpha);
    pProvider->Release();
    if (r != S_OK) {
        printf("pProvider->GetThumbnail() failed\n");
        return 12;
    }

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    Gdiplus::Bitmap* gdipBmp = Gdiplus::Bitmap::FromHBITMAP(bmp, NULL);
    CLSID pngClsid;
    CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &pngClsid);
    Gdiplus::Status st = gdipBmp->Save(L"preview.png", &pngClsid);
    if (st != Gdiplus::Ok) {
        printf("failed: save png: %d\n", (int)st);
    } else {
        printf("saved preview.png\n");
    }
    delete gdipBmp;
    DeleteObject(bmp);
    Gdiplus::GdiplusShutdown(gdiplusToken);
}
