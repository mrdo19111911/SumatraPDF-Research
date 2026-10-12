import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, assemblePdf, runStandalone, tmpPath } from "./util";
import { captureWindowToPng, clickAt, waitForFrame } from "./win-automation";
import {
  findChildWindow,
  findVisibleChildWindow,
  getWindowRect,
  moveWindow,
  sendMessage,
  setProcessDpiAware,
  waitForWindowIdle,
} from "./winapi";

export async function testit(): Promise<void> {
  setProcessDpiAware();
  const root = tmpPath(`floating-${Date.now()}`);
  mkdirSync(root, { recursive: true });
  const evidence = join("out", "floating-tools");
  mkdirSync(evidence, { recursive: true });
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
  writeFileSync(join(root, "SumatraPDF-settings.txt"), "CheckForUpdates = false\nUiLanguage = en\n");
  await withControlledSumatra(
    EXE,
    async (client, proc) => {
      const frame = await waitForFrame(proc.pid!);
      const state = async () => String((await client.request(ControlCommand.TestToolbarButtons))[1]);
      let raw = await state();
      if (!raw.includes("floating=1 expanded=0 reserved=0,0")) throw new Error("Tools still occupy the navigation bar");
      const toolbar = findChildWindow(frame, "SUMATRA_VIRT_TOOLBAR");
      if (findVisibleChildWindow(toolbar, "Edit")) throw new Error("Collapsed tools expose the page box");
      captureWindowToPng(toolbar, join(evidence, "collapsed.png"));
      const grip = /float-toggle rect=([\d,-]+)/.exec(raw)![1]!.split(",").map(Number);
      const before = getWindowRect(toolbar);
      const x = grip[0]! + 6,
        y = grip[1]! + 6;
      sendMessage(toolbar, 0x0201, 1, (y << 16) | (x & 0xffff));
      sendMessage(toolbar, 0x0200, 1, ((y - 40) << 16) | ((x - 80) & 0xffff));
      sendMessage(toolbar, 0x0202, 0, ((y - 40) << 16) | ((x - 80) & 0xffff));
      const after = getWindowRect(toolbar);
      if (before.left === after.left && before.top === after.top) throw new Error("Floating tools cannot be dragged");
      raw = await state();
      const toggle = /float-toggle rect=([\d,-]+)/.exec(raw)![1]!.split(",").map(Number);
      if (toggle[2]! - toggle[0]! !== toggle[3]! - toggle[1]! || toggle[3]! - toggle[1]! < 80)
        throw new Error("Tools launcher is small or not circular");
      await clickAt(toolbar, toggle[0]! + 8, toggle[1]! + 8);
      raw = await state();
      if (!raw.includes("expanded=1 reserved=0,0")) throw new Error("Tools do not expand as an overlay");
      for (const label of ["Open", "Print", "Go to page", "Dark reading", "Symbol index", "Google Drive", "Zalo"])
        if (!raw.includes(`text=${label}`)) throw new Error(`Expanded tools lack label: ${label}`);
      captureWindowToPng(toolbar, join(evidence, "expanded.png"));
      const read = /float-group=View rect=([\d,-]+)/.exec(raw)![1]!.split(",").map(Number);
      await clickAt(toolbar, read[0]! + 8, read[1]! + 8);
      raw = await state();
      const dark = /idx=\d+ cmd=\d+ hidden=0 rect=([\d,-]+) text=Dark reading/.exec(raw)![1]!.split(",").map(Number);
      if (dark[3]! - dark[1]! < 56) throw new Error("Child buttons remain too small");
      await clickAt(toolbar, (dark[0]! + dark[2]!) / 2, (dark[1]! + dark[3]!) / 2);
      if (!(await state()).includes("reading-dark=1"))
        throw new Error("Large labeled buttons do not execute their command");
      await clickAt(toolbar, (dark[0]! + dark[2]!) / 2, (dark[1]! + dark[3]!) / 2);
      const ai = /ai-header rect=([\d,-]+)/
        .exec(await state())![1]!
        .split(",")
        .map(Number);
      await clickAt(toolbar, ai[0]! + 8, ai[1]! + 8);
      moveWindow(frame, 0, 0, 900, 600);
      await waitForWindowIdle(frame);
      raw = await state();
      const scroll = /floating-scroll y=(\d+) max=(\d+)/.exec(raw);
      if (!scroll || Number(scroll[2]) <= 0) throw new Error("Large tools have no scroll viewport in a short window");
      const host = getWindowRect(toolbar);
      const sx = host.right - 40,
        sy = host.bottom - 30;
      sendMessage(toolbar, 0x020a, -120 << 16, (sy << 16) | (sx & 0xffff));
      raw = await state();
      if (!/floating-scroll y=[1-9]\d*/.test(raw)) throw new Error("Mouse wheel cannot reach tools below the viewport");
      captureWindowToPng(toolbar, join(evidence, "scrolled.png"));
      await clickAt(toolbar, toggle[0]! + 8, toggle[1]! + 8);
      if (!(await state()).includes("expanded=0")) throw new Error("Tools do not collapse");
      if (findVisibleChildWindow(toolbar, "Edit")) throw new Error("Page box survives collapse");
      console.log("PASS floating launcher: no reserved bar, native drag, expand and collapse");
    },
    ["-appdata", root, pdf],
  );
}
if (import.meta.main) await runStandalone(testit);
