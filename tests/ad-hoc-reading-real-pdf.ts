// Compare the real reader against independently extracted PDF body bounds.
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, runStandalone, tmpPath } from "./util";
import { findCanvas, waitForFrame } from "./win-automation";
import {
  captureWindowDCRegionPixels,
  captureWindowToPng,
  moveWindow,
  setProcessDpiAware,
  waitForWindowIdle,
} from "./winapi";

function arg(name: string): string {
  const i = process.argv.indexOf(name);
  if (i < 0 || !process.argv[i + 1]) throw new Error(`Required ${name} argument`);
  return process.argv[i + 1]!;
}

export async function testit(): Promise<void> {
  setProcessDpiAware();
  const adaptive = process.argv.includes("-adaptive");
  const pdf = arg("-pdf");
  const expected = JSON.parse(readFileSync(arg("-bounds"), "utf8"));
  for (const [label, pageNo, size, mode] of [
    ["portrait33", 33, "550x900", "single page"],
    ["wide1", 1, "1600x950", "facing"],
    ["wide7", 7, "1300x950", "facing"],
  ] as const) {
    const data = tmpPath(`reading-real-${label}`);
    mkdirSync(data, { recursive: true });
    writeFileSync(
      join(data, "SumatraPDF-settings.txt"),
      `CheckForUpdates = false\nUseTabs = false\nShowToc = false\nShowStartPage = false\nShowMenubar = true\nToolbarPosition = ${adaptive ? "auto" : "top"}\n`,
    );
    await withControlledSumatra(
      EXE,
      async (client, proc) => {
        await client.request(ControlCommand.SetNotificationsEnabled, [0]);
        await client.waitForRenderIdle();
        await client.request(ControlCommand.TestInvokeCommand, ["CmdAutoContentReading"]);
        await client.waitForRenderIdle();
        async function check(): Promise<void> {
          let raw = "";
          const deadline = Date.now() + 15000;
          do {
            raw = String((await client.request(ControlCommand.TestDisplayMode, ["reading-boxes"]))[1]);
            if (raw.includes("page=")) break;
            await Bun.sleep(100);
          } while (Date.now() < deadline);
          const vp = /viewport=(\d+),(\d+)/.exec(raw);
          if (!vp) throw new Error(raw);
          const lines = [
            ...raw.matchAll(/page=(\d+) content=([\d.,-]+) media=([\d.,-]+) screen=([\d.,-]+) frame=([\d.,-]+)/g),
          ];
          const frames: number[][] = [];
          if (!lines.length)
            throw new Error(
              `No visible page geometry: ${raw}; ${String((await client.request(ControlCommand.TestLayout, ["get"]))[1])}`,
            );
          for (const line of lines) {
            const n = Number(line[1]);
            const content = line[2]!.split(",").map(Number);
            const media = line[3]!.split(",").map(Number);
            const screen = line[4]!.split(",").map(Number);
            const frame = line[5]!.split(",").map(Number);
            const bounds: number[] = expected[n];
            if (!bounds) throw new Error(`No independent bounds for page ${n}`);
            if (
              content[0]! > bounds[0]! + 0.5 ||
              content[1]! > bounds[1]! + 0.5 ||
              content[0]! + content[2]! < bounds[2]! - 0.5 ||
              content[1]! + content[3]! < bounds[3]! - 0.5
            )
              throw new Error(`Body clipped on page ${n}: ${line[0]} expected ${bounds}`);
            if (content[1]! + content[3]! > 790) throw new Error(`Footer survived on page ${n}: ${line[0]}`);
            if (
              screen[0]! < 0 ||
              screen[1]! < 0 ||
              screen[0]! + screen[2]! > Number(vp[1]) ||
              screen[1]! + screen[3]! > Number(vp[2])
            )
              throw new Error(`Content outside viewport: ${line[0]}; ${vp[0]}`);
            if (!adaptive && label === "wide1" && (Math.abs(media[0]!) > 0.01 || Math.abs(media[2]! - 595.276) > 0.02))
              throw new Error(`Horizontal margins removed unnecessarily: ${line[0]}`);
            frames.push(frame);
          }
          if (frames.length === 2 && (frames[0]![1] !== frames[1]![1] || frames[0]![3] !== frames[1]![3]))
            throw new Error("Facing frames do not share top/bottom");
          if (label === "wide1") {
            const canvas = findCanvas(await waitForFrame(proc.pid!));
            const left = frames[0]!;
            const pixel = captureWindowDCRegionPixels(
              canvas,
              Math.round(left[0]! + left[2]! / 2),
              left[1]! + left[3]! - 5,
              1,
              1,
            );
            if (!pixel || pixel[0] !== 255 || pixel[1] !== 255 || pixel[2] !== 255)
              throw new Error(`Shorter page padding is not white: ${pixel}`);
          }
          writeFileSync(join("out", `v41-${label}-geometry.txt`), raw);
        }
        await check();
        const frame = await waitForFrame(proc.pid!);
        if (label === "wide7") {
          moveWindow(frame, 0, 0, 1600, 950);
          await waitForWindowIdle(frame);
          await client.waitForRenderIdle();
          await check();
          const before = join("out", "v41-cache-before.png");
          const after = join("out", "v41-cache-after.png");
          captureWindowToPng(findCanvas(frame), before);
          await client.request(ControlCommand.TestInvokeCommand, ["CmdReloadDocument"]);
          await client.waitForRenderIdle();
          await check();
          captureWindowToPng(findCanvas(frame), after);
          if (!readFileSync(before).equals(readFileSync(after)))
            throw new Error("Resized cached rendering differs from fresh reload");
        }
        captureWindowToPng(frame, join("out", `v41-${label}.png`));
        if (label === "portrait33" && process.argv.includes("-all-pages")) {
          await client.request(ControlCommand.TestInvokeCommand, ["CmdGoToFirstPage"]);
          for (let n = 1; n <= Object.keys(expected).length; n++) {
            await client.waitForRenderIdle();
            await check();
            if (n < Object.keys(expected).length)
              await client.request(ControlCommand.TestInvokeCommand, ["CmdGoToNextPage"]);
          }
          console.log(`PASS all ${Object.keys(expected).length} portrait pages: independent body bounds stay visible`);
        }
        console.log(`PASS ${label}: body visible, footer removed, frames aligned`);
      },
      [
        "-appdata",
        data,
        "-window-pos",
        `${size}@0x0`,
        "-view",
        mode,
        "-zoom",
        "fit content height",
        "-page",
        String(pageNo),
        pdf,
      ],
    );
  }
}

if (import.meta.main) await runStandalone(testit);
