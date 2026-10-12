import { mkdirSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, assemblePdf, cmdId, loadPng, pngPixel, runStandalone, tmpPath } from "./util";
import { waitForFrame } from "./win-automation";
import {
  captureWindowToPng,
  enumChildWindows,
  getWindowRect,
  moveWindow,
  setProcessDpiAware,
  waitForWindowIdle,
} from "./winapi";

const AI = [
  "CmdPaperChat",
  "CmdPaperMap",
  "CmdClearAIChat",
  "CmdResearchAsk",
  "CmdResearchSelection",
  "CmdResearchSymbols",
  "CmdResearchCompare",
  "CmdResearchVersions",
  "CmdResearchFigure",
  "CmdResearchReading",
  "CmdResearchNarration",
  "CmdResearchExportMp3",
  "CmdResearchTools",
];

export async function testit(): Promise<void> {
  setProcessDpiAware();
  const root = tmpPath(`toolbar-groups-${Date.now()}`);
  const evidence = resolve("out/toolbar-v473");
  mkdirSync(root, { recursive: true });
  mkdirSync(evidence, { recursive: true });
  const pdf = join(root, "paper.pdf");
  const stream = "0 g 150 100 300 600 re f";
  writeFileSync(
    pdf,
    assemblePdf([
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R >>",
      `<< /Length ${stream.length} >>\nstream\n${stream}\nendstream`,
    ]),
    "latin1",
  );
  for (const orientation of ["top", "right"]) {
    writeFileSync(
      join(root, "SumatraPDF-settings.txt"),
      `CheckForUpdates = false\nUiLanguage = en\nTheme = Light\nResearchFloatingTools = false\nToolbarPosition = ${orientation}\n`,
    );
    await withControlledSumatra(
      EXE,
      async (client, proc) => {
        const frame = await waitForFrame(proc.pid!);
        for (const [width, height] of [
          [1400, 850],
          [500, 900],
          [1400, 500],
        ]) {
          moveWindow(frame, 0, 0, width!, height!);
          await waitForWindowIdle(frame);
          await client.waitForRenderIdle();
          const raw = String((await client.request(ControlCommand.TestToolbarButtons))[1]);
          const host = /host=([\d,-]+)/.exec(raw)![1]!.split(",").map(Number);
          const groups = [...raw.matchAll(/group=(\w+) rect=([\d,-]+)/g)].map((m) => ({
            name: m[1]!,
            rect: m[2]!.split(",").map(Number),
          }));
          if (groups.length !== 7 || groups.filter((g) => g.name === "AI").length !== 1)
            throw new Error(`Expected seven frames and one AI group: ${raw}`);
          for (const g of groups) {
            const r = g.rect;
            if (r[0]! < 0 || r[1]! < 0 || r[2]! > host[2]! || r[3]! > host[3]!)
              throw new Error(`Clipped group: ${g.name}; ${raw}`);
          }
          for (const m of raw.matchAll(/idx=\d+ cmd=\d+ hidden=0 rect=([\d,-]+)/g)) {
            const r = m[1]!.split(",").map(Number);
            if (r[0]! < 0 || r[1]! < 0 || r[2]! > host[2]! || r[3]! > host[3]!)
              throw new Error(`Clipped button: ${m[0]}`);
          }
          for (const command of AI) {
            const icon = command === "CmdPaperMap" ? 0 : 1;
            if (!new RegExp(`state-cmd=${cmdId(command)} icon=${icon} enabled=\\d group=AI`).test(raw))
              throw new Error(`AI command outside group: ${command}`);
          }
          for (const command of ["CmdGoogleDrive", "CmdSendToZalo"]) {
            if (!new RegExp(`state-cmd=${cmdId(command)} icon=1 enabled=\\d group=Share`).test(raw))
              throw new Error(`Sharing command outside group: ${command}`);
          }
          if (width === 1400 && height === 850) {
            captureWindowToPng(frame, join(evidence, `${orientation}-light.png`));
            enumChildWindows(frame, (hwnd) => {
              const r = getWindowRect(hwnd);
              if (
                r.left === host[0] &&
                r.top === host[1] &&
                r.right - r.left === host[2] &&
                r.bottom - r.top === host[3]
              ) {
                captureWindowToPng(hwnd, join(evidence, `${orientation}-bar.png`));
              }
              return true;
            });
          }
        }
        await client.request(ControlCommand.TestInvokeCommand, ["CmdToggleReadingDarkMode"]);
        await waitForWindowIdle(frame);
        const dark = String((await client.request(ControlCommand.TestToolbarButtons))[1]);
        if (!dark.includes("reading-dark=1")) throw new Error("Dark mode failed");
        captureWindowToPng(frame, join(evidence, `${orientation}-dark.png`));
        writeFileSync(join(evidence, `${orientation}-state.txt`), dark);
        const darkHost = /host=([\d,-]+)/.exec(dark)![1]!.split(",").map(Number);
        const darkBar = join(evidence, `${orientation}-dark-bar.png`);
        enumChildWindows(frame, (hwnd) => {
          const r = getWindowRect(hwnd);
          if (
            r.left === darkHost[0] &&
            r.top === darkHost[1] &&
            r.right - r.left === darkHost[2] &&
            r.bottom - r.top === darkHost[3]
          )
            captureWindowToPng(hwnd, darkBar);
          return true;
        });
        const image = loadPng(darkBar);
        for (const command of ["CmdArxivSearch", "CmdCollectPapers"]) {
          const rect = new RegExp(`idx=\\d+ cmd=${cmdId(command)} hidden=0 rect=([\\d,-]+)`)
            .exec(dark)![1]!
            .split(",")
            .map(Number);
          let bright = 0;
          for (let y = rect[1]! + 8; y < rect[3]! - 8; y++)
            for (let x = rect[0]! + 8; x < rect[2]! - 8; x++) {
              const color = pngPixel(image, x, y);
              if (color.every((channel) => channel >= 170)) bright++;
            }
          if (bright < 8) throw new Error(`${command} disappears on the dark toolbar: ${bright} visible pixels`);
        }
      },
      ["-appdata", root, pdf],
    );
  }
  console.log(
    "PASS seven frames, all AI commands together, Share group, resizing, light/dark; no AI request or sharing performed.",
  );
}
if (import.meta.main) await runStandalone(testit);
