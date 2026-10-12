import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, assemblePdf, runStandalone, tmpPath } from "./util";
import { clickAt, waitForFrame } from "./win-automation";
import { findChildWindow, setProcessDpiAware } from "./winapi";

export async function testit(): Promise<void> {
  setProcessDpiAware();
  const root = tmpPath(`floating-groups-${Date.now()}`);
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
      const frame = await waitForFrame(proc.pid!);
      const toolbar = findChildWindow(frame, "SUMATRA_VIRT_TOOLBAR");
      const state = async () => String((await client.request(ControlCommand.TestToolbarButtons))[1]);
      const launcher = /float-toggle rect=([\d,-]+)/
        .exec(await state())![1]!
        .split(",")
        .map(Number);
      await clickAt(toolbar, launcher[0]! + 8, launcher[1]! + 8);
      for (const [name, tools] of [
        ["File", ["Open", "Print"]],
        ["Pages", ["Previous Page", "Find"]],
        ["View", ["AUTO", "dark reading"]],
        ["Papers", ["Connected Papers", "arXiv"]],
        ["Share", ["Google Drive", "Zalo"]],
        ["Markup", ["Edit PDF"]],
      ] as const) {
        const raw = await state();
        if (!raw.includes("expanded=1 reserved=0,0")) throw new Error("Toolbox reserves navigation space");
        const match = new RegExp(`float-group=${name} rect=([\\d,-]+)`).exec(raw);
        if (!match) throw new Error(`Floating ${name} group disappeared`);
        const r = match[1]!.split(",").map(Number);
        if (r[2]! <= r[0]! || r[3]! <= r[1]!) throw new Error(`${name} group has no bounds`);
        await clickAt(toolbar, r[0]! + 8, r[1]! + 8);
        const menu = await state();
        if (!menu.split("\n").some((line) => line.startsWith(`group=${name} rect=`)))
          throw new Error(`${name} does not open`);
        for (const tool of tools)
          if (!menu.toLowerCase().includes(tool.toLowerCase())) throw new Error(`${name} lost ${tool}`);
      }
      console.log("PASS floating category tabs select one group without reserving navigation space");
    },
    ["-appdata", root, pdf],
  );
}
if (import.meta.main) await runStandalone(testit);
