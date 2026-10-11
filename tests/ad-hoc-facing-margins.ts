import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, assemblePdf, runStandalone, tmpPath } from "./util";

export async function testit(): Promise<void> {
  const pdf = tmpPath("facing-inner-margins.pdf");
  const objects = ["<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Count 2 /Kids [3 0 R 5 0 R] >>"];
  const stream = "0 g 30 50 140 200 re f";
  for (let i = 0; i < 2; i++) {
    objects.push(`<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 300] /Contents ${4 + i * 2} 0 R >>`);
    objects.push(`<< /Length ${stream.length} >>\nstream\n${stream}\nendstream`);
  }
  writeFileSync(pdf, assemblePdf(objects), "latin1");
  const appdata = tmpPath(`facing-inner-margins-${Date.now()}`);
  mkdirSync(appdata, { recursive: true });
  writeFileSync(
    join(appdata, "SumatraPDF-settings.txt"),
    "CheckForUpdates = false\nShowToc = false\nToolbarPosition = top\n",
  );
  await withControlledSumatra(
    EXE,
    async (client) => {
      await client.waitForRenderIdle();
      await client.request(ControlCommand.TestInvokeCommand, ["CmdAutoContentReading"]);
      await client.waitForRenderIdle();
      const raw = String((await client.request(ControlCommand.TestDisplayMode, ["reading-boxes"]))[1]);
      const pages = [
        ...raw.matchAll(/page=(\d+) content=([\d.,-]+) media=([\d.,-]+) screen=([\d.,-]+) frame=([\d.,-]+)/g),
      ];
      if (pages.length !== 2) throw new Error(`Expected two facing pages: ${raw}`);
      for (const page of pages) {
        const media = page[3]!.split(",").map(Number);
        if (Math.abs(media[0]!) > 0.01 || Math.abs(media[2]! - 200) > 0.01)
          throw new Error(`Margins cropped despite spare width: ${page[0]}`);
      }
      const left = pages[0]![4]!.split(",").map(Number);
      const right = pages[1]![4]!.split(",").map(Number);
      const gap = right[0]! - left[0]! - left[2]!;
      const leftContent = pages[0]![2]!.split(",").map(Number);
      const rightContent = pages[1]![2]!.split(",").map(Number);
      const scale = left[2]! / leftContent[2]!;
      const expectedMargins = (200 - leftContent[0]! - leftContent[2]! + rightContent[0]!) * scale;
      if (gap < expectedMargins + 4)
        throw new Error(`Inner margins or gutter lost despite spare width: ${gap}px; ${raw}`);
      console.log(`PASS: both pages retain full horizontal margins; body gap ${gap}px`);
    },
    ["-appdata", appdata, "-window-pos", "1400x650@0x0", pdf],
  );
}

if (import.meta.main) {
  await runStandalone(testit);
}
