// Wheel page turns must skip the paper margins in Fit Content Height.
import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlClient, ControlCommand, withControlledSumatra } from "./control";
import { EXE, assemblePdf, runStandalone, tmpPath } from "./util";
import { findCanvas, waitForFrame } from "./win-automation";
import { getScrollPos, sendMessage, SB_VERT } from "./winapi";

const WM_MOUSEWHEEL = 0x020a;
const WHEEL_DOWN = 0xff880000n;
const WHEEL_UP = 0x00780000n;
const WHEEL_HALF_DOWN = 0xffc40000n;

function makePdf(): string {
  const objects = [
    "<< /Type /Catalog /Pages 2 0 R >>",
    "<< /Type /Pages /Count 6 /Kids [3 0 R 5 0 R 7 0 R 9 0 R 11 0 R 13 0 R] >>",
  ];
  for (let i = 0; i < 6; i++) {
    const stream = `0 g 80 200 450 ${250 + i * 20} re f`;
    objects.push(
      `<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents ${4 + i * 2} 0 R >>`,
      `<< /Length ${stream.length} >>\nstream\n${stream}\nendstream`,
    );
  }
  return assemblePdf(objects);
}

async function page(client: ControlClient): Promise<number> {
  const result = await client.request(ControlCommand.TestFavoriteNav, ["page", 0]);
  const match = /OK page=(\d+)/.exec(String(result[1] ?? ""));
  if (!match) throw new Error(`could not read page: ${result[1]}`);
  return Number(match[1]);
}

async function check(mode: "single page" | "facing"): Promise<void> {
  const pdf = tmpPath("content-height-wheel.pdf");
  writeFileSync(pdf, makePdf(), "latin1");
  const data = tmpPath(`content-height-wheel-${mode.replaceAll(" ", "-")}`);
  mkdirSync(data, { recursive: true });
  writeFileSync(
    join(data, "SumatraPDF-settings.txt"),
    "CheckForUpdates = false\nShowStartPage = false\nShowToc = false\nSmoothScroll = false\nMouseWheelTurnsPage = false\nScrollbarInSinglePage = true\n",
  );
  await withControlledSumatra(
    EXE,
    async (client, proc) => {
      await client.waitForRenderIdle();
      const canvas = findCanvas(await waitForFrame(proc.pid!));
      const expectedNext = mode === "facing" ? 3 : 2;
      const y = getScrollPos(canvas, SB_VERT);
      sendMessage(canvas, WM_MOUSEWHEEL, WHEEL_HALF_DOWN, 0n);
      if ((await page(client)) !== 1 || getScrollPos(canvas, SB_VERT) !== y) {
        throw new Error(`${mode}: half a notch scrolled into the blank margin`);
      }
      sendMessage(canvas, WM_MOUSEWHEEL, WHEEL_HALF_DOWN, 0n);
      if ((await page(client)) !== expectedNext) throw new Error(`${mode}: one notch did not turn to ${expectedNext}`);
      await client.waitForRenderIdle();
      sendMessage(canvas, WM_MOUSEWHEEL, WHEEL_UP, 0n);
      if ((await page(client)) !== 1) throw new Error(`${mode}: wheel up did not return to page 1`);
      sendMessage(canvas, WM_MOUSEWHEEL, WHEEL_UP, 0n);
      if ((await page(client)) !== 1) throw new Error(`${mode}: wheel up passed the first page`);
      for (let i = 0; i < 6; i++) {
        sendMessage(canvas, WM_MOUSEWHEEL, WHEEL_DOWN, 0n);
        await page(client);
      }
      const last = mode === "facing" ? 5 : 6;
      if ((await page(client)) !== last) throw new Error(`${mode}: wheel did not stop at the last page/spread`);
      const state = String((await client.request(ControlCommand.TestDisplayMode, ["get"]))[1]);
      if (!state.includes("zoom=fit content height")) throw new Error(`${mode}: page turning lost content fit`);
      console.log(`PASS ${mode}: wheel skips margins, preserves zoom, respects first/last page`);
    },
    ["-appdata", data, "-window-pos", "950x550@0x0", "-view", mode, "-zoom", "fit content height", pdf],
  );
}

export async function testit(): Promise<void> {
  await check("single page");
  await check("facing");
}

if (import.meta.main) await runStandalone(testit);
