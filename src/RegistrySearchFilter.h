/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#define kPdfFilterClsid "{C0CB5416-8247-4C1F-8D0B-A5D302D816D0}"
#define kPdfFilterHandler "{4A3C0E4C-369E-4E5B-ACAE-5DCDD263135A}"

#define kTexFilterClsid "{59C9A81D-2DD4-4C35-B7E1-2CD78C70D8E7}"
#define kTexFilterHandler "{8B3E3582-6246-4314-A4EC-3A3000DAFFBE}"

#define kEpubFilterClsid "{233BB8A1-1D93-4915-A4C6-E7DEA27F1BC6}"
#define kEpubFilterHandler "{69142277-163D-4A89-A9F2-51FC64B1D2C2}"

bool InstallSearchFilter(Str dllPath, bool allUsers);
bool UninstallSearchFilter();
bool IsSearchFilterInstalled();