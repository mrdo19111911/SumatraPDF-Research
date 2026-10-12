import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { checkReadingMenu } from "./ad-hoc-portrait-view";
import { EXE, assemblePdf, runStandalone, tmpPath } from "./util";
import { clickAt, findCanvas, waitForFrame } from "./win-automation";
import { captureWindowToPng, getClientRect, moveWindow, setProcessDpiAware, waitForWindowIdle } from "./winapi";

export async function testit(): Promise<void> {
  setProcessDpiAware();
  const pdf = tmpPath("auto-chat-layout.pdf");
  const stream = "0 g 56 56 500 680 re f";
  writeFileSync(
    pdf,
    assemblePdf([
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 2 /Kids [3 0 R 5 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R >>",
      `<< /Length ${stream.length} >>\nstream\n${stream}\nendstream`,
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 6 0 R >>",
      `<< /Length ${stream.length} >>\nstream\n${stream}\nendstream`,
    ]),
    "latin1",
  );
  for (const toolbar of ["top", "auto", "floating"]) {
    const data = tmpPath(`auto-chat-${toolbar}-${Date.now()}`);
    mkdirSync(data, { recursive: true });
    writeFileSync(
      join(data, "SumatraPDF-settings.txt"),
      `UiLanguage = en\nUseTabs = false\nShowMenubar = true\nCheckForUpdates = false\nResearchFloatingTools = ${toolbar === "floating"}\nToolbarPosition = ${toolbar === "floating" ? "top" : toolbar}\nRememberStatePerDocument = true\n` +
        `FileStates [\n [\n FilePath = ${pdf}\n UseDefaultState = false\n AutoContentReading = false\n Zoom = fit page\n DisplayMode = single page\n ]\n]\n`,
    );
    await withControlledSumatra(
      EXE,
      async (client, proc) => {
        const frame = await waitForFrame(proc.pid!);
        async function expect(label: string, mode: string): Promise<number> {
          await waitForWindowIdle(frame);
          await client.waitForRenderIdle();
          const raw = String((await client.request(ControlCommand.TestDisplayMode, ["get"]))[1]);
          const actual = /OK mode=(.+) presentation=\d+ fullscreen=\d+ zoom=(.+)$/.exec(raw.trim());
          if (actual?.[1] !== mode || actual?.[2] !== "fit content height") throw new Error(`${label}: ${raw}`);
          const zoom = String((await client.request(ControlCommand.TestDisplayMode, ["zoom-real"]))[1]);
          const value = Number(/zoomReal=([\d.]+)/.exec(zoom)?.[1]);
          const boxes = String((await client.request(ControlCommand.TestDisplayMode, ["reading-boxes"]))[1]);
          const viewport = /viewport=(\d+),(\d+)/.exec(boxes);
          if (!viewport) throw Error(`Missing viewport: ${boxes}`);
          const visible = [...boxes.matchAll(/screen=(-?\d+),(-?\d+),(\d+),(\d+)/g)];
          if (!visible.length) throw Error(`No retained content: ${boxes}`);
          for (const b of visible) {
            const [x, y, width, height] = b.slice(1).map(Number);
            if (x! < -1 || y! < -1 || x! + width! > Number(viewport[1]) + 1 || y! + height! > Number(viewport[2]) + 1)
              throw Error(`${toolbar}/${label}: AUTO clips retained PDF content: ${boxes}`);
          }
          console.log(`PASS ${toolbar}/${label}: ${mode}, ${zoom.trim()}`);
          return value;
        }
        await expect("wide-without-chat", "facing");
        checkReadingMenu(frame, true);
        await client.request(ControlCommand.TestInvokeCommand, ["CmdAIChatWithClaudeCode"]);
        await waitForWindowIdle(frame);
        await client.waitForRenderIdle();
        const canvas = getClientRect(findCanvas(frame))!;
        if (canvas.right <= canvas.bottom)
          throw new Error(`Fixture must remain wider than tall with chat open: ${canvas.right}x${canvas.bottom}`);
        await expect("chat-reduces-pdf-width", "single page");
        if (toolbar === "floating") {
          const tabs = String((await client.request(ControlCommand.TestResearchTools, ["pane-tabs"]))[1]);
          if (!tabs.includes("tab=Chat visible=1 active=1")) throw Error(`Chat tab is not active: ${tabs}`);
          const r = /tab=Chat .*rect=([\d,-]+)/.exec(tabs)![1]!.split(",").map(Number);
          await clickAt(frame, (r[0]! + r[2]!) / 2, (r[1]! + r[3]!) / 2);
          await expect("selected-chat-tab-keeps-pane-open", "single page");
        }
        captureWindowToPng(frame, join("out", `auto-chat-${toolbar}-single.png`));
        moveWindow(frame, 0, 0, 2300, 1000);
        await expect("wider-with-chat", "facing");
        moveWindow(frame, 0, 0, toolbar === "auto" ? 1900 : 1800, 1000);
        await expect("narrower-with-chat", "single page");
        moveWindow(frame, 0, 0, 700, 1100);
        await expect("portrait-with-chat", "single page");
        moveWindow(frame, 0, 0, toolbar === "auto" ? 1900 : 1800, 1000);
        await expect("restore-width-with-chat", "facing");
        if (toolbar === "floating") {
          const tabs = String((await client.request(ControlCommand.TestResearchTools, ["pane-tabs"]))[1]);
          const r = /tab=× .*rect=([\d,-]+)/.exec(tabs)![1]!.split(",").map(Number);
          await clickAt(frame, (r[0]! + r[2]!) / 2, (r[1]! + r[3]!) / 2);
        } else await client.request(ControlCommand.TestInvokeCommand, ["CmdAIChatWithClaudeCode"]);
        await expect("close-chat", "facing");
        await client.request(ControlCommand.TestInvokeCommand, ["CmdAutoContentReading"]);
        await waitForWindowIdle(frame);
        checkReadingMenu(frame, false);
        await client.request(ControlCommand.TestInvokeCommand, ["CmdSinglePageView"]);
        await client.request(ControlCommand.TestInvokeCommand, ["CmdReloadDocument"]);
        const manual = String((await client.request(ControlCommand.TestDisplayMode, ["get"]))[1]);
        if (!manual.includes("mode=single page") || !manual.includes("zoom=fit page"))
          throw new Error(`manual-off-survives-reload: ${manual}`);
        checkReadingMenu(frame, false);
        await client.request(ControlCommand.TestInvokeCommand, ["CmdAutoContentReading"]);
        await expect("auto-back-on", "facing");
        checkReadingMenu(frame, true);
      },
      ["-appdata", data, "-window-pos", toolbar === "auto" ? "2100x1000@0x0" : "1800x1000@0x0", pdf],
    );
  }
  console.log("PASS: AUTO follows usable PDF space through chat open/close and resize. No AI question sent.");
}

if (import.meta.main) await runStandalone(testit);
