import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, assemblePdf, runStandalone, tmpPath } from "./util";
import { clickAt, waitForFrame } from "./win-automation";
import { captureWindowToPng, findChildWindow, setProcessDpiAware, waitForWindowIdle } from "./winapi";

export async function testit(): Promise<void> {
  setProcessDpiAware();
  const root = tmpPath(`fluent-tools-${Date.now()}`);
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
      const rect = (raw: string, name: string) => {
        const match = raw
          .split("\n")
          .find((line) => line.startsWith(name))
          ?.match(/rect=([\d,-]+)/);
        if (!match) throw Error(`Missing ${name}: ${raw}`);
        return match[1]!.split(",").map(Number);
      };
      const toggle = rect(await state(), "float-toggle");
      await clickAt(toolbar, (toggle[0]! + toggle[2]!) / 2, (toggle[1]! + toggle[3]!) / 2);
      const raw = await state();
      const rail = [
        rect(raw, "ai-header"),
        ...["File", "Pages", "View", "Papers", "Share", "Markup"].map((s) => rect(raw, `float-group=${s} `)),
      ];
      if (new Set(rail.map((r) => r[0])).size !== 1)
        throw Error("Categories must share one vertical rail, not wrap above the actions");
      const body = rect(raw, "group=AI ");
      if (body[0]! <= Math.max(...rail.map((r) => r[2]!))) throw Error("Actions must be beside the category rail");
      for (const r of rail) if (r[3]! - r[1]! < 40) throw Error("Category target too small");
      const dir = join("out", "fluent-native");
      mkdirSync(dir, { recursive: true });
      captureWindowToPng(frame, join(dir, "tools-light.png"));
      await client.request(ControlCommand.TestInvokeCommand, ["CmdToggleReadingDarkMode"]);
      await waitForWindowIdle(frame);
      await client.waitForRenderIdle();
      captureWindowToPng(frame, join(dir, "tools-dark.png"));
      console.log("PASS category rail beside readable actions, light/dark native screenshots");
    },
    ["-appdata", root, "-window-pos", "1500x1000@0x0", pdf],
  );
}
if (import.meta.main) await runStandalone(testit);
