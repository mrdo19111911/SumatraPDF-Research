import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, assemblePdf, runStandalone, tmpPath } from "./util";
import { enumWindows, getClassName, getWindowPid, getWindowRect, isWindowVisible, setProcessDpiAware } from "./winapi";

export async function testit(): Promise<void> {
  setProcessDpiAware();
  const root = tmpPath(`tooltip-gap-${Date.now()}`);
  mkdirSync(root, { recursive: true });
  const pdf = join(root, "fixture.pdf");
  writeFileSync(
    pdf,
    assemblePdf([
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>",
    ]),
    "latin1",
  );
  writeFileSync(join(root, "SumatraPDF-settings.txt"), "CheckForUpdates = false\n");
  await withControlledSumatra(
    EXE,
    async (client, proc) => {
      const cursor = { x: 100, y: 100 };
      const result = await client.request(ControlCommand.TestResearchTools, ["tooltip-show"]);
      if (result[0] !== 0) throw new Error(`Tooltip probe unavailable: ${result}`);
      let tooltip = 0;
      enumWindows((hwnd) => {
        if (getWindowPid(hwnd) === proc.pid && getClassName(hwnd) === "tooltips_class32" && isWindowVisible(hwnd))
          tooltip = hwnd;
        return true;
      });
      if (!tooltip) throw new Error("No tooltip window");
      const r = getWindowRect(tooltip);
      const gap = Math.max(r.top - cursor.y, cursor.y - r.bottom, r.left - cursor.x, cursor.x - r.right);
      if (gap < 48) throw new Error(`Tooltip too close to cursor: ${gap}px`);
      await client.request(ControlCommand.TestResearchTools, ["tooltip-hide"]);
      const edge = await client.request(ControlCommand.TestResearchTools, ["tooltip-edge"]);
      if (edge[0] !== 0) throw new Error(`Edge probe: ${edge}`);
      const [cx, cy, x, y, w, h, left, top, right, bottom] = String(edge[1]).split(",").map(Number);
      if (x! < left! || y! < top! || x! + w! > right! || y! + h! > bottom!)
        throw new Error(`Tooltip clipped at screen edge: ${edge}`);
      if (y! + h! > cy! - 16) throw new Error(`Edge tooltip covers cursor: ${edge}`);
      console.log(`PASS tooltip clear of cursor by ${gap}px`);
    },
    ["-appdata", root, pdf],
  );
}
if (import.meta.main) await runStandalone(testit);
