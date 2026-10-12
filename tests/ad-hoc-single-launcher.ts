import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, assemblePdf, runStandalone, tmpPath } from "./util";
import { clickAt, waitForFrame } from "./win-automation";
import { findChildWindow, setProcessDpiAware } from "./winapi";

export async function testit(): Promise<void> {
  setProcessDpiAware();
  const root = tmpPath(`single-launcher-${Date.now()}`);
  mkdirSync(root, { recursive: true });
  const pdf = join(root, "paper.pdf");
  writeFileSync(
    pdf,
    assemblePdf([
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>",
    ]),
    "latin1",
  );
  writeFileSync(join(root, "SumatraPDF-settings.txt"), "UiLanguage = en\nCheckForUpdates = false\n");
  await withControlledSumatra(
    EXE,
    async (client, proc) => {
      const toolbar = findChildWindow(await waitForFrame(proc.pid!), "SUMATRA_VIRT_TOOLBAR");
      const state = async () => String((await client.request(ControlCommand.TestToolbarButtons))[1]);
      let raw = await state();
      for (const line of raw
        .split("\n")
        .filter((line) => line.startsWith("float-group=") || line.startsWith("ai-header")))
        if (!line.trim().endsWith("rect=0,0,0,0")) throw new Error(`Extra button beside collapsed Tools: ${line}`);
      const r = /float-toggle rect=([\d,-]+)/.exec(raw)![1]!.split(",").map(Number);
      await clickAt(toolbar, (r[0]! + r[2]!) / 2, (r[1]! + r[3]!) / 2);
      raw = await state();
      if (!raw.includes("expanded=1") || !raw.includes("group=AI rect="))
        throw new Error("Tools does not open the AI toolbox");
      for (const group of ["File", "Pages", "View", "Papers", "Share", "Markup"])
        if (raw.split("\n").some((line) => line.startsWith(`group=${group} rect=`)))
          throw new Error("All groups still expand together");
      await clickAt(toolbar, (r[0]! + r[2]!) / 2, (r[1]! + r[3]!) / 2);
      if (!(await state()).includes("expanded=0")) throw new Error("Tools does not collapse");
      console.log("PASS one collapsed circle; opens one selected group; closes again");
    },
    ["-appdata", root, pdf],
  );
}
if (import.meta.main) await runStandalone(testit);
