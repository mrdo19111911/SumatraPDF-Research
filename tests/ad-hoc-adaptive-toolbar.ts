import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, assemblePdf, runStandalone, tmpPath } from "./util";
import { findCanvas, waitForFrame } from "./win-automation";
import {
  captureWindowToPng,
  getClientRect,
  getFocusedHwnd,
  moveWindow,
  setProcessDpiAware,
  setForegroundWindow,
  waitForWindowIdle,
} from "./winapi";

export async function testit(): Promise<void> {
  setProcessDpiAware();
  const overlay = process.argv.includes("-overlay");
  const pdf = tmpPath("adaptive-toolbar.pdf");
  const streams = ["0 g 180 120 240 600 re f", "0 g 30 330 540 180 re f"];
  const objects = ["<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Count 2 /Kids [3 0 R 5 0 R] >>"];
  for (const [i, stream] of streams.entries()) {
    objects.push(`<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 840] /Contents ${4 + i * 2} 0 R >>`);
    objects.push(`<< /Length ${stream.length} >>\nstream\n${stream}\nendstream`);
  }
  writeFileSync(pdf, assemblePdf(objects), "latin1");
  const data = tmpPath(`adaptive-toolbar-${Date.now()}`);
  mkdirSync(data, { recursive: true });
  writeFileSync(
    join(data, "SumatraPDF-settings.txt"),
    `CheckForUpdates = false\nUseTabs = false\nShowToc = false\nShowMenubar = false\nToolbarPosition = auto\nToolbar = ${overlay ? "overlay" : "show"}\n`,
  );
  await withControlledSumatra(
    EXE,
    async (client, proc) => {
      const frame = await waitForFrame(proc.pid!);
      if (overlay) {
        setForegroundWindow(frame);
        await client.request(ControlCommand.TestInvokeCommand, ["CmdGoToPage"]);
      }
      async function check(label: string, expected: string) {
        await client.waitForRenderIdle();
        await waitForWindowIdle(frame);
        const raw = String((await client.request(ControlCommand.TestToolbarButtons))[1]);
        if (!raw.includes(`orientation=${expected}`)) throw new Error(`${label}: expected ${expected}; ${raw}`);
        const host = /host=([\d,-]+)/.exec(raw)![1]!.split(",").map(Number);
        for (const m of raw.matchAll(/idx=\d+ cmd=\d+ hidden=0 rect=([\d,-]+)/g)) {
          const rect = m[1]!.split(",").map(Number);
          if (rect[0]! < 0 || rect[1]! < 0 || rect[2]! > host[2]! || rect[3]! > host[3]!)
            throw new Error(`Hidden toolbar button: ${m[0]}; ${host}`);
        }
        const canvas = getClientRect(findCanvas(frame));
        if (!canvas || canvas.right <= 0 || canvas.bottom <= 0) throw new Error("No reading canvas");
        captureWindowToPng(frame, join("out", `adaptive-${label}.png`));
        console.log(`PASS ${label}: ${expected}, canvas=${canvas.right}x${canvas.bottom}`);
      }
      await check("tall-content", "right");
      await client.request(ControlCommand.TestInvokeCommand, ["CmdGoToNextPage"]);
      await check("wide-content", "top");
      setForegroundWindow(frame);
      await waitForWindowIdle(frame);
      const go = await client.request(ControlCommand.TestInvokeCommand, ["CmdGoToPage"]);
      if (go[0] !== 0) throw new Error(`Go to page failed: ${go}`);
      await Bun.sleep(100);
      const focused = getFocusedHwnd(frame);
      if (!focused || focused === frame) throw new Error("Page input did not get focus");
      moveWindow(frame, 0, 0, 1800, 550);
      await check("short-window", "right");
      if (getFocusedHwnd(frame) !== focused) throw new Error("Rotating the toolbar lost page input focus");
      moveWindow(frame, 0, 0, 500, 950);
      await check("narrow-window", "top");
      await client.request(ControlCommand.TestInvokeCommand, ["CmdGoToPrevPage"]);
      // Framed groups widen the right strip; this narrow window fits better with top.
      await check("narrow-tall-content", "top");
      await client.request(ControlCommand.TestInvokeCommand, ["CmdToggleEditPDF"]);
      await client.waitForRenderIdle();
      await waitForWindowIdle(frame);
      const annotations = String((await client.request(ControlCommand.TestToolbarButtons))[1]);
      if (!annotations.includes("annotationButtons=28 visible=1")) throw new Error("Annotation strip did not open");
      const annotationHost = /host=([\d,-]+)/.exec(annotations)![1]!.split(",").map(Number);
      for (const m of annotations.matchAll(/annotation-idx=\d+ cmd=\d+ hidden=0 enabled=\d+ rect=([\d,-]+)/g)) {
        const r = m[1]!.split(",").map(Number);
        if (r[0]! < 0 || r[1]! < 0 || r[2]! > annotationHost[2]! || r[3]! > annotationHost[3]!)
          throw new Error(`Annotation button outside toolbar: ${m[0]}`);
      }
      captureWindowToPng(frame, join("out", "adaptive-annotations.png"));
      console.log("PASS page input focus and annotation buttons survive rotation");
    },
    ["-appdata", data, "-window-pos", "700x900@0x0", "-view", "single page", "-zoom", "fit content height", pdf],
  );
}

if (import.meta.main) await runStandalone(testit);
