import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, assemblePdf, cmdId, runStandalone, tmpPath, writeStoredZip } from "./util";
import { clickAt, waitForFrame } from "./win-automation";
import {
  captureWindowToPng,
  enumChildWindows,
  enumWindows,
  getWindowPid,
  getWindowRect,
  getWindowText,
  isWindowVisible,
  moveWindow,
  postMessage,
  setProcessDpiAware,
  waitForWindowIdle,
  WM_CLOSE,
} from "./winapi";

export async function testit(): Promise<void> {
  setProcessDpiAware();
  const root = tmpPath(`research-toolbar-${Date.now()}`);
  mkdirSync(root, { recursive: true });
  const pdf = join(root, "paper.pdf"),
    epub = join(root, "book.epub");
  const stream = "0 g 90 90 420 600 re f";
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
  writeStoredZip(epub, [
    { name: "mimetype", data: Buffer.from("application/epub+zip") },
    {
      name: "META-INF/container.xml",
      data: Buffer.from(
        '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="book.opf" media-type="application/oebps-package+xml"/></rootfiles></container>',
      ),
    },
    {
      name: "book.opf",
      data: Buffer.from(
        '<package version="2.0" xmlns="http://www.idpf.org/2007/opf" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">test-font</dc:identifier><dc:title>Font test</dc:title><dc:language>en</dc:language></metadata><manifest><item id="text" href="text.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="text"/></spine></package>',
      ),
    },
    {
      name: "text.xhtml",
      data: Buffer.from(
        `<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Font test</title></head><body>${"<p>A paragraph with real reflowing words.</p>".repeat(70)}</body></html>`,
      ),
    },
  ]);
  writeFileSync(
    join(root, "SumatraPDF-settings.txt"),
    "CheckForUpdates = false\nUiLanguage = en\nTheme = Light\nDocumentColorsFollowTheme = off\nResearchFloatingTools = false\nToolbarPosition = top\nEBookUI [\n FontSize = 12\n]\n",
  );
  const required = [
    "CmdToggleReadingDarkMode",
    "CmdIncreaseTextSize",
    "CmdDecreaseTextSize",
    "CmdPaperChat",
    "CmdClearAIChat",
    "CmdResearchNarration",
    "CmdResearchExportText",
    "CmdResearchExportMp3",
    "CmdConnectedPapers",
    "CmdArxivSearch",
    "CmdCollectPapers",
    "CmdSciHub",
    "CmdGoogleDrive",
    "CmdSendToZalo",
    "CmdResearchAsk",
    "CmdResearchSelection",
    "CmdResearchSymbols",
    "CmdResearchCompare",
    "CmdResearchVersions",
    "CmdResearchFigure",
    "CmdResearchReading",
  ];
  for (const doc of [pdf, epub]) {
    await withControlledSumatra(
      EXE,
      async (client, proc) => {
        const frame = await waitForFrame(proc.pid!);
        const dump = async () => String((await client.request(ControlCommand.TestToolbarButtons))[1]);
        const invoke = async (name: string) => {
          const result = await client.request(ControlCommand.TestInvokeCommand, [name]);
          if (result[0] !== 0) throw new Error(`${name}: ${result}`);
          await waitForWindowIdle(frame);
          await client.waitForRenderIdle();
        };
        const click = async (name: string) => {
          const raw = await dump();
          const host = /host=([\d,-]+)/.exec(raw)![1]!.split(",").map(Number);
          const button = new RegExp(`cmd=${cmdId(name)} hidden=0 rect=([\\d,-]+)`).exec(raw);
          if (!button) throw new Error(`No visible button for ${name}`);
          let toolbar = 0;
          enumChildWindows(frame, (hwnd) => {
            const r = getWindowRect(hwnd);
            if (r.left === host[0] && r.top === host[1] && r.right - r.left === host[2] && r.bottom - r.top === host[3])
              toolbar = hwnd;
            return true;
          });
          if (!toolbar) throw new Error("Toolbar window missing");
          const r = button[1]!.split(",").map(Number);
          await clickAt(toolbar, Math.floor((r[0]! + r[2]!) / 2), Math.floor((r[1]! + r[3]!) / 2));
        };
        if (doc === pdf) {
          let raw = await dump();
          for (const name of required) {
            const id = cmdId(name);
            if (!raw.includes(`cmd=${id} hidden=0 `)) throw new Error(`Toolbar button missing: ${name}`);
            if (!raw.includes(`state-cmd=${id} icon=1`)) throw new Error(`Not an icon: ${name}`);
          }
          for (const name of ["CmdIncreaseTextSize", "CmdDecreaseTextSize"]) {
            const id = cmdId(name);
            if (!raw.includes(`state-cmd=${id} icon=1 enabled=0`))
              throw new Error(`PDF font button pretends to reflow: ${name}`);
          }
          await click("CmdToggleReadingDarkMode");
          raw = await dump();
          if (!raw.includes("reading-dark=1 colors=smart")) throw new Error(`Dark reading not enabled: ${raw}`);
          await invoke("CmdToggleReadingDarkMode");
          if (!(await dump()).includes("reading-dark=0")) throw new Error("Dark toggle not reversible");
          for (const [name, title] of [
            ["CmdResearchExportText", "Save extracted text"],
            ["CmdResearchNarration", "Save Vietnamese narration"],
            ["CmdResearchExportMp3", "Select reviewed Vietnamese narration"],
          ]) {
            await click(name!);
            let dialog = 0;
            const deadline = Date.now() + 10000;
            while (!dialog && Date.now() < deadline) {
              enumWindows((hwnd) => {
                if (getWindowPid(hwnd) === proc.pid && isWindowVisible(hwnd) && getWindowText(hwnd) === title)
                  dialog = hwnd;
                return true;
              });
              if (!dialog) await Bun.sleep(30);
            }
            if (!dialog) throw new Error(`Direct icon did not open ${title}`);
            postMessage(dialog, WM_CLOSE, 0, 0);
            await waitForWindowIdle(frame);
          }
          for (const [w, h] of [
            [1400, 850],
            [500, 900],
          ]) {
            moveWindow(frame, 0, 0, w!, h!);
            await waitForWindowIdle(frame);
            raw = await dump();
            const host = /host=([\d,-]+)/.exec(raw)![1]!.split(",").map(Number);
            for (const m of raw.matchAll(/idx=\d+ cmd=\d+ hidden=0 rect=([\d,-]+)/g)) {
              const r = m[1]!.split(",").map(Number);
              if (r[0]! < 0 || r[1]! < 0 || r[2]! > host[2]! || r[3]! > host[3]!)
                throw new Error(`Clipped icon: ${m[0]}`);
            }
            captureWindowToPng(frame, join("out", `research-toolbar-${w}.png`));
          }
        } else {
          const before = await dump();
          if (!before.includes("text-size=12")) throw new Error(`Font fixture not loaded: ${before}`);
          const zoom = String((await client.request(ControlCommand.TestDisplayMode, ["get"]))[1]);
          const pages = (await client.chapterInfo()).chapterPageCount;
          await invoke("CmdIncreaseTextSize");
          if (!(await dump()).includes("text-size=14")) throw new Error(`A+ did not adjust font: ${await dump()}`);
          if (String((await client.request(ControlCommand.TestDisplayMode, ["get"]))[1]) !== zoom)
            throw new Error("A+ changed page zoom");
          if ((await client.chapterInfo()).chapterPageCount <= pages)
            throw new Error("Font setting changed without reflowing EPUB text");
          await invoke("CmdDecreaseTextSize");
          if (!(await dump()).includes("text-size=12")) throw new Error("A- did not restore font");
        }
      },
      ["-appdata", root, "-window-pos", "1400x850@0x0", doc],
    );
  }
  console.log(
    "PASS: direct icons, reading dark toggle, font/reflow separate from zoom, no clipped controls. No AI request sent.",
  );
}
if (import.meta.main) await runStandalone(testit);
