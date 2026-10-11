/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#define kPdfPreviewClsid "{A651836F-6704-411A-9741-161F4626E0B0}"
#define kXpsPreviewClsid "{00A18657-106C-4D67-89A8-F334D314EFA3}"
#define kDjVuPreviewClsid "{97D5D6B5-97C1-4EC9-AD7E-6FF237511F21}"
#define kEpubPreviewClsid "{759D5E7C-0D51-475E-B50E-9DB00E710438}"
#define kFb2PreviewClsid "{F531AED7-D73B-4066-9D3B-279DB144D4C4}"
#define kMobiPreviewClsid "{D5A460EF-399F-49C2-9755-525177F6CEC2}"
#define kCbxPreviewClsid "{B21A8BD1-C57D-469C-BF58-65BE2CAEC680}"
#define kTgaPreviewClsid "{B9A8466D-B640-47A1-B608-71E358CD8A05}"

bool InstallPreviewDll(Str dllPath, bool allUsers);
bool UninstallPreviewDll();
void DisablePreviewInstallExts(Str cmdLine);
bool IsPreviewInstalled();

// opt-in file logging for the preview handler (PdfPreview.dll), controlled by a
// registry value so it can be toggled (via CmdTogglePdfPreviewLogging) without a
// rebuild. Files are written to the per-build data dir (keyed on the sibling
// SumatraPDF.exe's sha1) so they land next to the app's other logs/crash info.
#define kPdfPreviewLogPrefix "pdfpreview.log."

bool IsPdfPreviewLoggingEnabled();
void SetPdfPreviewLoggingEnabled(bool enable);
TempStr GetPdfPreviewLogDirTemp();
void StartPdfPreviewLoggingIfEnabled();
