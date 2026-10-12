import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, assemblePdf, runStandalone, tmpPath } from "./util";
import { waitForFrame } from "./win-automation";
import { findChildWindow, getWindowRect, sendMessage, setProcessDpiAware } from "./winapi";

const point = (x: number, y: number) => (y << 16) | (x & 0xffff);
export async function testit(): Promise<void> {
  setProcessDpiAware();
  const root = tmpPath(`floating-drag-${Date.now()}`);
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
      const raw = await state();
      const r = /float-toggle rect=([\d,-]+)/.exec(raw)![1]!.split(",").map(Number);
      const x = Math.round((r[0]! + r[2]!) / 2),
        y = Math.round((r[1]! + r[3]!) / 2);
      const frameBefore = getWindowRect(frame);
      const before = getWindowRect(toolbar);
      sendMessage(toolbar, 0x0201, 1, point(x, y));
      sendMessage(toolbar, 0x0200, 1, point(x - 100, y - 60));
      sendMessage(toolbar, 0x0202, 0, point(x, y));
      const after = getWindowRect(toolbar);
      if (before.left === after.left && before.top === after.top)
        throw new Error("Dragging the Tools circle does not move the toolbox");
      if (!(await state()).includes("expanded=0")) throw new Error("Dragging Tools also activated its click");
      sendMessage(toolbar, 0x0201, 1, point(x, y));
      sendMessage(toolbar, 0x0200, 1, point(x + 1, y + 1));
      sendMessage(toolbar, 0x0202, 0, point(x + 1, y + 1));
      let next = await state();
      if (!next.includes("expanded=1")) throw new Error("A click with tiny movement cannot open Tools");
      const host = getWindowRect(toolbar);
      const blankX = host.right - host.left - 3,
        blankY = 3;
      sendMessage(toolbar, 0x0201, 1, point(blankX, blankY));
      sendMessage(toolbar, 0x0200, 1, point(blankX - 60, blankY - 40));
      sendMessage(toolbar, 0x0202, 0, point(blankX, blankY));
      const moved = getWindowRect(toolbar);
      if (host.left === moved.left && host.top === moved.top) throw new Error("Toolbox background cannot be dragged");
      sendMessage(toolbar, 0x0201, 1, point(x, y));
      sendMessage(toolbar, 0x001f, 0, 0);
      const canceled = getWindowRect(toolbar);
      sendMessage(toolbar, 0x0200, 1, point(x - 70, y - 40));
      const idle = getWindowRect(toolbar);
      if (canceled.left !== idle.left || canceled.top !== idle.top) throw new Error("Canceled drag keeps moving");
      next = await state();
      if (!next.includes("expanded=1")) throw new Error("Canceled drag activates Tools");
      const frameAfter = getWindowRect(frame);
      if (frameBefore.left !== frameAfter.left || frameBefore.top !== frameAfter.top)
        throw new Error("Toolbox drag moves the document window");
      console.log("PASS circle/background drag, click tolerance, cancellation; document window stays fixed");
    },
    ["-appdata", root, pdf],
  );
}
if (import.meta.main) await runStandalone(testit);
