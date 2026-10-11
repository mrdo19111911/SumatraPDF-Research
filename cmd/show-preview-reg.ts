import { spawnSync } from "node:child_process";

// Must match src/RegistryPreview.h and src/RegistryPreview.cpp
const kThumbnailProviderClsid = "{e357fccd-a995-4576-b01f-234630154e96}";
const kPreviewHandlerClsid = "{8895b1c6-b41f-4c1c-a562-0d564250836f}";

// Must match src/RegistryPreview.h
const kPreview2Clsids: Record<string, string> = {
  PDF: "{A651836F-6704-411A-9741-161F4626E0B0}",
  XPS: "{00A18657-106C-4D67-89A8-F334D314EFA3}",
  DjVu: "{97D5D6B5-97C1-4EC9-AD7E-6FF237511F21}",
  EPUB: "{759D5E7C-0D51-475E-B50E-9DB00E710438}",
  FB2: "{F531AED7-D73B-4066-9D3B-279DB144D4C4}",
  MOBI: "{D5A460EF-399F-49C2-9755-525177F6CEC2}",
  CBX: "{B21A8BD1-C57D-469C-BF58-65BE2CAEC680}",
  TGA: "{B9A8466D-B640-47A1-B608-71E358CD8A05}",
};

const previewers = [
  { name: "PDF", clsid: kPreview2Clsids.PDF, exts: [".pdf"] },
  { name: "CBX", clsid: kPreview2Clsids.CBX, exts: [".cbz", ".cbr", ".cb7", ".cbt"] },
  { name: "TGA", clsid: kPreview2Clsids.TGA, exts: [".tga"] },
  { name: "DjVu", clsid: kPreview2Clsids.DjVu, exts: [".djvu"] },
  { name: "XPS", clsid: kPreview2Clsids.XPS, exts: [".xps", ".oxps"] },
  { name: "EPUB", clsid: kPreview2Clsids.EPUB, exts: [".epub"] },
  { name: "FB2", clsid: kPreview2Clsids.FB2, exts: [".fb2", ".fb2z", ".fbz", ".zfb2", ".fb2.zip"] },
  { name: "MOBI", clsid: kPreview2Clsids.MOBI, exts: [".mobi"] },
];

function regQuery(key: string, valueName?: string): string | null {
  const args = ["query", key];
  if (valueName !== undefined) {
    args.push("/v", valueName);
  } else {
    args.push("/ve"); // default value
  }
  const result = spawnSync("reg", args, { encoding: "utf-8", stdio: ["pipe", "pipe", "pipe"] });
  if (result.status !== 0) {
    return null;
  }
  // parse output: look for REG_SZ line
  const lines = result.stdout.split("\n");
  for (const line of lines) {
    const match = line.match(/REG_SZ\s+(.+)/);
    if (match) {
      return match[1].trim();
    }
  }
  return null;
}

function checkExt(ext: string, expectedClsid: string) {
  const roots = [
    { name: "HKCR", key: `HKEY_CLASSES_ROOT\\${ext}` },
    { name: "HKCU", key: `HKEY_CURRENT_USER\\Software\\Classes\\${ext}` },
    { name: "HKLM", key: `HKEY_LOCAL_MACHINE\\Software\\Classes\\${ext}` },
  ];

  // Check IThumbnailProvider
  console.log(`  IThumbnailProvider (${kThumbnailProviderClsid}):`);
  for (const root of roots) {
    const key = `${root.key}\\shellex\\${kThumbnailProviderClsid}`;
    const value = regQuery(key);
    if (value) {
      const match = value.toLowerCase() === expectedClsid.toLowerCase();
      const status = match ? "OK" : `MISMATCH (expected ${expectedClsid})`;
      console.log(`    ${root.name}: ${value} - ${status}`);
    } else {
      console.log(`    ${root.name}: (not set)`);
    }
  }

  // Check IPreviewHandler
  console.log(`  IPreviewHandler (${kPreviewHandlerClsid}):`);
  for (const root of roots) {
    const key = `${root.key}\\shellex\\${kPreviewHandlerClsid}`;
    const value = regQuery(key);
    if (value) {
      const match = value.toLowerCase() === expectedClsid.toLowerCase();
      const status = match ? "OK" : `MISMATCH (expected ${expectedClsid})`;
      console.log(`    ${root.name}: ${value} - ${status}`);
    } else {
      console.log(`    ${root.name}: (not set)`);
    }
  }
}

function checkClsidRegistration(name: string, clsid: string) {
  const roots = [
    { name: "HKCU", key: `HKEY_CURRENT_USER\\Software\\Classes\\CLSID\\${clsid}` },
    { name: "HKLM", key: `HKEY_LOCAL_MACHINE\\Software\\Classes\\CLSID\\${clsid}` },
  ];

  for (const root of roots) {
    const displayName = regQuery(root.key);
    const dllPath = regQuery(`${root.key}\\InprocServer32`);
    const appId = regQuery(root.key, "AppId");
    if (displayName || dllPath) {
      console.log(`  CLSID ${root.name}: ${displayName || "(no display name)"}`);
      if (dllPath) {
        console.log(`    InprocServer32: ${dllPath}`);
      }
      if (appId) {
        console.log(`    AppId: ${appId}`);
      }
    } else {
      console.log(`  CLSID ${root.name}: (not registered)`);
    }
  }
}

function checkPreviewHandlersKey(clsid: string, name: string) {
  const roots = [
    { name: "HKCU", key: "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers" },
    { name: "HKLM", key: "HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers" },
  ];
  for (const root of roots) {
    const value = regQuery(root.key, clsid);
    if (value) {
      console.log(`  PreviewHandlers ${root.name}: ${value}`);
    }
  }
}

function main() {
  console.log("SumatraPDF Preview/Thumbnail Registration Status\n");

  for (const prev of previewers) {
    console.log(`=== ${prev.name} (CLSID: ${prev.clsid}) ===`);
    checkClsidRegistration(prev.name, prev.clsid);
    checkPreviewHandlersKey(prev.clsid, prev.name);
    for (const ext of prev.exts) {
      console.log(`  --- ${ext} ---`);
      checkExt(ext, prev.clsid);
    }
    console.log("");
  }
}

main();
