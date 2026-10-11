import { copyFileSync, mkdirSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { EXE, ROOT, runStandalone, tmpPath } from "./util";
import { enumWindows, getClassName, getWindowPid, killAndWait, sleep } from "./winapi";

const HELPERS = [
  "Install-Vietnamese-Voice.ps1",
  "Speak-Vietnamese.ps1",
  "THIRD-PARTY.txt",
  "SciHub-MIT-LICENSE.txt",
  "Anna-API-MIT-LICENSE.txt",
];

export async function testit(): Promise<void> {
  const root = tmpPath(`research-installer-${Date.now()}`);
  mkdirSync(root, { recursive: true });
  const installer = join(root, "SumatraPDF-Research-install.exe");
  copyFileSync(EXE, installer);
  const dest = join(root, "extracted");
  const p = Bun.spawnSync([installer, "-for-testing", "-x", "-d", dest], { timeout: 60000 });
  if (p.exitCode !== 0) throw Error(`Installer extraction failed: ${p.stderr.toString()}`);
  for (const name of HELPERS) {
    const source = readFileSync(join(ROOT, "research", name));
    const installed = readFileSync(join(dest, "research", name));
    if (!source.equals(installed)) throw Error(`Installer changed helper: ${name}`);
  }
  for (const name of ["SumatraPDF.exe", "libsumatrapdf.dll", "PdfFilter.dll", "PdfPreview.dll", "sumatrapdf-tool.exe"])
    if (readFileSync(join(dest, name)).length === 0) throw Error(`Empty installer payload: ${name}`);
  const proc = Bun.spawn([installer, "-for-testing", "-d", join(root, "ui-target")], {
    stdout: "ignore",
    stderr: "ignore",
  });
  try {
    let shown = false;
    const deadline = Date.now() + 12000;
    while (!shown && Date.now() < deadline) {
      enumWindows((hwnd) => {
        if (getWindowPid(hwnd) === proc.pid && getClassName(hwnd) === "SUMATRA_PDF_INSTALLER_FRAME") shown = true;
        return !shown;
      });
      if (!shown) await sleep(50);
    }
    if (!shown) throw Error("Renamed executable did not open the installer UI");
  } finally {
    await killAndWait(proc);
  }
  console.log(
    "PASS installer UI and extraction of binaries, Vietnamese helpers and third-party licenses; no installation performed.",
  );
}

if (import.meta.main) await runStandalone(testit);
