import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, assemblePdf, runStandalone, tmpPath } from "./util";
import { clickAt, waitForFrame } from "./win-automation";
import {
  enumWindows,
  getClassName,
  getWindowPid,
  getWindowRect,
  findChildWindow,
  setProcessDpiAware,
  sleep,
  captureWindowToPng,
} from "./winapi";

export async function testit(): Promise<void> {
  setProcessDpiAware();
  const root = tmpPath(`ai-menu-${Date.now()}`);
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
  writeFileSync(
    join(root, "SumatraPDF-settings.txt"),
    "CheckForUpdates = false\nToolbarPosition = top\nUiLanguage = en\n",
  );
  await withControlledSumatra(
    EXE,
    async (client, proc) => {
      const frame = await waitForFrame(proc.pid!);
      let raw = String((await client.request(ControlCommand.TestToolbarButtons))[1]);
      const launcher = /float-toggle rect=([\d,-]+)/.exec(raw)![1]!.split(",").map(Number);
      const toolbar = findChildWindow(frame, "SUMATRA_VIRT_TOOLBAR");
      await clickAt(toolbar, launcher[0]! + 8, launcher[1]! + 8);
      raw = String((await client.request(ControlCommand.TestToolbarButtons))[1]);
      const match = /ai-header rect=([\d,-]+)/.exec(raw) || /group=AI rect=([\d,-]+)/.exec(raw);
      if (!match) throw new Error("No AI group");
      const rect = match[1]!.split(",").map(Number);
      await clickAt(toolbar, rect[0]! + 8, rect[1]! + 8);
      const state = String((await client.request(ControlCommand.TestToolbarButtons))[1]);
      if (!state.split("\n").some((line) => line.startsWith("group=AI rect=")))
        throw new Error("AI tab is not selected");
      for (const name of ["Chat with paper", "Knowledge map", "Explain selection", "Symbol index"]) {
        if (!state.toLowerCase().includes(name.toLowerCase())) throw new Error(`Missing named tool: ${name}`);
      }
      const bounds = getWindowRect(toolbar);
      if (bounds.right - bounds.left < 200) throw new Error("AI menu does not expand");
      mkdirSync(join("out", "floating-tools"), { recursive: true });
      captureWindowToPng(toolbar, join("out", "floating-tools", "ai-menu.png"));
      console.log("PASS opening Tools and selecting AI shows named actions");
    },
    ["-appdata", root, pdf],
  );
}
if (import.meta.main) await runStandalone(testit);
