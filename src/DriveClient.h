/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Browser OAuth and streaming upload. The caller owns this job until Run ends.
struct DriveJob {
    Str clientId;
    Str clientSecret;
    Str filePath;
    Str tokenPath;
    Str url;
    Str error;
    HANDLE cancel = nullptr;
    HWND owner = nullptr;
    // Only accepted by the isolated test-control entry point.
    Str mockBase;
    Str authUrlPath;
    ~DriveJob();
};

bool DriveReadClient(DriveJob*, Str path);
bool DriveHasAccount(Str tokenPath);
bool DriveForgetAccount(Str tokenPath);
void DriveRun(DriveJob*);
