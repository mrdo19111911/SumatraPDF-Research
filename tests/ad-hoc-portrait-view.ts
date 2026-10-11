// Targeted checks for the custom Windows portrait-PDF opening defaults.
import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlClient, ControlCommand, withControlledSumatra } from "./control";
import { EXE, assemblePdf, cmdId, runStandalone, tmpPath } from "./util";
import { findCanvas, findChildByClass, waitForFrame } from "./win-automation";
import {
  captureWindowToPng,
  getWindowMenu,
  getSubMenu,
  getMenuItemCount,
  getMenuItemId,
  getMenuItemChecked,
  getMenuItemDisabled,
  sendMessage,
  moveWindow,
  setProcessDpiAware,
  waitForWindowIdle,
  postMessage,
} from "./winapi";

export function checkReadingMenu(frame: number, enabled: boolean): void {
  const menu = getWindowMenu(frame);
  sendMessage(frame, 0x0117, getSubMenu(menu, 1), 1); // WM_INITMENUPOPUP refreshes menu state.
  function find(hmenu: bigint, id: number): { checked: boolean; disabled: boolean } | undefined {
    for (let pos = 0; pos < getMenuItemCount(hmenu); pos++) {
      if (getMenuItemId(hmenu, pos) === id)
        return { checked: getMenuItemChecked(hmenu, pos), disabled: getMenuItemDisabled(hmenu, pos) };
      const sub = getSubMenu(hmenu, pos);
      if (sub) {
        const found = find(sub, id);
        if (found) return found;
      }
    }
  }
  if (find(menu, cmdId("CmdAutoContentReading"))?.checked !== enabled)
    throw new Error("Auto Content Reading checkbox has the wrong state");
  for (const command of ["CmdSinglePageView", "CmdFacingView", "CmdBookView", "CmdToggleContinuousView"])
    if (find(menu, cmdId(command))?.disabled !== enabled)
      throw new Error(`${command}: menu should be ${enabled ? "greyed" : "enabled"}`);
}

function makePdf(width: number, height: number, rectangles: string[], footer = "", header = ""): string {
  const objects = [
    "<< /Type /Catalog /Pages 2 0 R >>",
    `<< /Type /Pages /Count ${rectangles.length} /Kids [${rectangles.map((_, i) => `${3 + i * 2} 0 R`).join(" ")}] >>`,
  ];
  for (const [i, rectangle] of rectangles.entries()) {
    const stream =
      (rectangle ? `0 g ${rectangle} re f` : "") +
      (footer ? `\nBT /F1 12 Tf 290 30 Td (${footer}) Tj ET` : "") +
      (header ? `\nBT /F1 12 Tf 290 ${height - 30} Td (${header}) Tj ET` : "");
    objects.push(
      `<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ${width} ${height}] /Resources << /Font << /F1 ${3 + rectangles.length * 2} 0 R >> >> /Contents ${4 + i * 2} 0 R >>`,
      `<< /Length ${stream.length} >>\nstream\n${stream}\nendstream`,
    );
  }
  objects.push("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
  return assemblePdf(objects);
}

async function state(client: ControlClient): Promise<{ mode: string; zoom: string; real: number }> {
  const deadline = Date.now() + 20_000;
  for (;;) {
    const result = await client.request(ControlCommand.TestDisplayMode, ["get"]);
    const raw = String(result[1] ?? "").trim();
    const match = /OK mode=(.+) presentation=\d+ fullscreen=\d+ zoom=(.+)$/.exec(raw);
    if (match) {
      const real = String((await client.request(ControlCommand.TestDisplayMode, ["zoom-real"]))[1]);
      return { mode: match[1]!, zoom: match[2]!, real: Number(/zoomReal=([\d.]+)/.exec(real)?.[1]) };
    }
    if (Date.now() > deadline || result[0] !== 2) throw new Error(`view not ready: ${raw}`);
    await Bun.sleep(100);
  }
}

async function invoke(client: ControlClient, name: string): Promise<void> {
  const result = await client.request(ControlCommand.TestInvokeCommand, [name]);
  if (result[0] !== 0) throw new Error(`${name}: ${result[1]}`);
}

async function check(
  label: string,
  width: number,
  height: number,
  rectangles: string[],
  windowSize: string,
  mode: string,
  contentHeight?: number,
  extraArgs: string[] = [],
): Promise<void> {
  const pdf = tmpPath(`portrait-view-${label}.pdf`);
  const footer =
    label === "footer-four" ? "-12-" : label === "footer-dense" ? "This is a substantial footer with real content" : "";
  const header = label === "header-sparse" || label === "both-sparse" ? "12" : "";
  writeFileSync(pdf, makePdf(width, height, rectangles, label === "both-sparse" ? "12" : footer, header), "latin1");
  const data = tmpPath(`portrait-view-${label}-${Date.now()}`);
  mkdirSync(data, { recursive: true });
  writeFileSync(
    join(data, "SumatraPDF-settings.txt"),
    `ShowStartPage = false\nUseTabs = false\nShowMenubar = true\nCheckForUpdates = false\nDefaultDisplayMode = continuous\nDefaultZoom = fit page\n` +
      `RememberStatePerDocument = true\nFileStates [\n [\n FilePath = ${pdf}\n UseDefaultState = false\n AutoContentReading = ${label === "menu-mode" || label === "normal-open" ? "false" : "true"}\n DisplayMode = continuous\n Zoom = fit page\n ]\n]\n`,
  );
  await withControlledSumatra(
    EXE,
    async (client, proc) => {
      await state(client);
      if (label === "menu-mode") {
        const toolbar = String((await client.request(ControlCommand.TestToolbarButtons))[1]);
        if (!toolbar.includes(`cmd=${cmdId("CmdAutoContentReading")} hidden=0`))
          throw new Error("AUTO button missing from toolbar");
        const before = await state(client);
        if (before.mode !== "continuous" || before.zoom !== "fit page")
          throw new Error("reading mode enabled without selecting it");
        await clickAuto(client, await waitForFrame(proc.pid!));
      }
      let contentIdle = await client.waitForRenderIdle();
      let opened = await state(client);
      const zoom = contentHeight === undefined ? "fit page" : "fit content height";
      if (opened.mode !== mode || opened.zoom !== zoom) {
        throw new Error(`${label}: expected ${mode}/${zoom}, got ${JSON.stringify(opened)}`);
      }
      if (label === "menu-mode" || label === "letter-tall" || label === "letter-wide") {
        checkReadingMenu(await waitForFrame(proc.pid!), true);
        if (label === "menu-mode")
          captureWindowToPng(await waitForFrame(proc.pid!), join("out", "auto-toolbar-on.png"));
        for (const command of ["CmdFacingView", "CmdSinglePageView", "CmdBookView", "CmdToggleContinuousView"]) {
          await invoke(client, command);
          const overridden = await state(client);
          if (overridden.mode !== mode || overridden.zoom !== zoom)
            throw new Error(`automatic reading lost control after ${command}: ${JSON.stringify(overridden)}`);
        }
      }
      await client.waitForRenderIdle();
      if (label === "unequal-heights" || label === "large-gutter") {
        const layout = String((await client.request(ControlCommand.TestLayout, ["get"]))[1]);
        const sizes = [...layout.matchAll(/page n=\d+ shown=1 pos=\d+,\d+,(\d+),(\d+)/g)];
        if (sizes.length !== 2) throw new Error("Facing should show two pages");
        if (label === "unequal-heights" && sizes[0]![2] !== sizes[1]![2])
          throw new Error("Facing page frames do not have equal heights");
        if (label === "large-gutter") {
          const vp = Number(/vp=(\d+)x/.exec(contentIdle)?.[1]);
          const total = Number(sizes[0]![1]) + Number(sizes[1]![1]) + 8;
          if (Math.abs(total - (vp - 12)) > 4)
            throw new Error(`Too much horizontal margin removed: frame width ${total}, available ${vp - 12}`);
        }
      }
      if (label === "center-asymmetric") {
        const screenshot = join("out", "single-centered.png");
        const frame = await waitForFrame(proc.pid!);
        if (!captureWindowToPng(findCanvas(frame), screenshot)) throw new Error("Could not capture Single Page");
        const measurement = Bun.spawnSync([
          "powershell",
          "-NoProfile",
          "-Command",
          `Add-Type -AssemblyName System.Drawing; $b=[System.Drawing.Bitmap]::FromFile('${screenshot}'); $left=$b.Width; $right=-1; $y=[int]($b.Height/2); for($x=0;$x -lt $b.Width;$x++){ $c=$b.GetPixel($x,$y); if($c.R -lt 32 -and $c.G -lt 32 -and $c.B -lt 32){$left=[math]::Min($left,$x); $right=$x} }; $gap=$b.Width-1-$right; $b.Dispose(); Write-Output ([math]::Abs($left-$gap))`,
        ]);
        const difference = Number(measurement.stdout.toString().trim());
        if (measurement.exitCode || difference > 4)
          throw new Error(`Single Page content is not centered: margin difference ${difference}px`);
      }
      if (label === "letter-wide") {
        const frame = await waitForFrame(proc.pid!);
        const screenshot = join("out", "portrait-view-facing.png");
        if (!captureWindowToPng(findCanvas(frame), screenshot)) throw new Error("could not capture Facing view");
      }
      if (label === "letter-tall") {
        const frame = await waitForFrame(proc.pid!);
        if (!moveWindow(frame, 0, 0, 950, 550)) throw new Error("could not resize window");
        await waitForWindowIdle(frame);
        contentIdle = await client.waitForRenderIdle();
        const resized = await state(client);
        if (resized.mode !== "facing" || resized.zoom !== opened.zoom || resized.real === opened.real) {
          throw new Error(`wide window did not switch to Facing and recompute content fit: ${JSON.stringify(resized)}`);
        }
        if (!moveWindow(frame, 0, 0, 420, 650)) throw new Error("could not restore portrait window");
        await waitForWindowIdle(frame);
        contentIdle = await client.waitForRenderIdle();
        opened = await state(client);
        if (opened.mode !== "single page") throw new Error("portrait window kept Facing after resize");
      }
      if (label === "next-page") {
        await invoke(client, "CmdGoToNextPage");
        contentIdle = await client.waitForRenderIdle();
        const next = await state(client);
        if (next.mode !== mode || next.zoom !== opened.zoom || next.real >= opened.real) {
          throw new Error(`next page did not fit its taller content: ${JSON.stringify(next)}`);
        }
        opened = next;
        contentHeight = 400;
      }
      if (contentHeight !== undefined) {
        await invoke(client, "CmdZoomFitHeight");
        const pageIdle = await client.waitForRenderIdle();
        const pageFit = await state(client);
        const ratio = opened.real / pageFit.real;
        const contentViewport = Number(/vp=\d+x(\d+)/.exec(contentIdle)?.[1]);
        const pageViewport = Number(/vp=\d+x(\d+)/.exec(pageIdle)?.[1]);
        const contentWidth = label === "blank" ? width : Math.max(...rectangles.map((r) => Number(r.split(" ")[2])));
        const viewportWidth = Number(/vp=(\d+)x/.exec(contentIdle)?.[1]);
        const columns = opened.mode === "facing" ? Math.min(rectangles.length, 2) : 1;
        // Default window margins including page shadows: 12 horizontal, 12 vertical; gutter 8px.
        const expectedZoom = Math.min(
          (contentViewport - 12) / contentHeight,
          (viewportWidth - 12 - (columns - 1) * 8) / (contentWidth * columns),
        );
        const expected = expectedZoom / ((pageViewport - 12) / height);
        if (!Number.isFinite(ratio) || Math.abs(ratio / expected - 1) > 0.04) {
          throw new Error(
            `${label}: content-height/page-height zoom ratio ${ratio}, expected ${expected}; ${JSON.stringify(opened)} / ${JSON.stringify(pageFit)}; ${contentIdle} / ${pageIdle}`,
          );
        }
      }
      if (label === "menu-mode") {
        await clickAuto(client, await waitForFrame(proc.pid!));
        const off = await state(client);
        if (off.zoom !== "fit page") throw new Error("disabling reading mode did not restore page fit");
        const frame = await waitForFrame(proc.pid!);
        checkReadingMenu(frame, false);
        captureWindowToPng(frame, join("out", "auto-toolbar-off.png"));
        await invoke(client, "CmdFacingView");
        if ((await state(client)).mode !== "facing")
          throw new Error("Facing is still blocked after unchecking Auto Content Reading");
        await invoke(client, "CmdSinglePageView");
        if ((await state(client)).mode !== "single page")
          throw new Error("Single Page is still blocked after unchecking Auto Content Reading");
        moveWindow(frame, 0, 0, 950, 550);
        await waitForWindowIdle(frame);
        if ((await state(client)).mode !== "single page") throw new Error("disabled mode still auto-switches layout");
        await invoke(client, "CmdAutoContentReading");
        const enabled = await state(client);
        if (enabled.mode !== "facing" || enabled.zoom !== "fit content height")
          throw new Error("menu did not reactivate automatic content view");
      }
      console.log(`PASS ${label}: ${opened.mode}, ${opened.zoom}, zoom=${opened.real}%`);
    },
    ["-appdata", data, "-window-pos", `${windowSize}@0x0`, ...extraArgs, pdf],
  );
}

async function clickAuto(client: ControlClient, frame: number): Promise<void> {
  const dump = String((await client.request(ControlCommand.TestToolbarButtons))[1]);
  const match = new RegExp(`cmd=${cmdId("CmdAutoContentReading")} hidden=0 rect=(\\d+),(\\d+),(\\d+),(\\d+)`).exec(
    dump,
  );
  if (!match) throw new Error("AUTO toolbar button is not visible");
  const toolbar = findChildByClass(frame, "SUMATRA_VIRT_TOOLBAR");
  if (!toolbar) throw new Error("Toolbar window not found");
  const x = Math.round((Number(match[1]) + Number(match[3])) / 2);
  const y = Math.round((Number(match[2]) + Number(match[4])) / 2);
  postMessage(toolbar, 0x0201, 1, (y << 16) | x);
  postMessage(toolbar, 0x0202, 0, (y << 16) | x);
  await waitForWindowIdle(frame);
}

export async function testit(): Promise<void> {
  setProcessDpiAware();
  await check("center-asymmetric", 612, 2000, ["50 200 100 1500"], "420x650", "single page", 1500);
  if (process.argv.includes("-centering")) return;
  await check("footer-four", 612, 792, ["70 200 470 250"], "420x650", "single page", 250);
  await check("header-sparse", 612, 792, ["70 200 470 250"], "420x650", "single page", 250);
  await check("both-sparse", 612, 792, ["70 200 470 250"], "420x650", "single page", 250);
  await check("footer-dense", 612, 792, ["70 200 470 250"], "420x650", "single page", 423);
  await check("normal-open", 612, 792, ["70 200 470 250"], "420x650", "continuous");
  await check("menu-mode", 612, 792, ["70 200 470 250"], "420x650", "single page", 250);
  await check("letter-tall", 612, 792, ["70 200 470 250"], "420x650", "single page", 250);
  await check("letter-wide", 612, 792, ["70 100 470 400", "70 200 470 400"], "950x550", "facing", 400);
  await check("large-gutter", 612, 792, ["206 100 200 400", "206 200 200 400"], "950x550", "facing", 400);
  await check("unequal-heights", 612, 792, ["70 200 470 400", "70 100 470 600"], "950x550", "facing", 600);
  await check("preserve-side-margins", 612, 792, ["206 100 200 600"], "950x550", "facing", 600);
  await check("a4-wide", 595, 842, ["60 200 475 500", "60 200 475 500"], "950x550", "facing", 500);
  await check("blank", 612, 792, [""], "420x650", "single page", 792);
  await check("next-page", 612, 792, ["70 200 100 250", "70 100 470 400"], "420x650", "single page", 250);
  await check("landscape", 792, 612, ["70 100 650 400"], "950x550", "single page", 400);
  await check("already-facing", 1224, 792, ["70 100 1084 500", "70 100 1084 500"], "950x550", "single page", 500);
  await check("explicit-view", 612, 792, ["70 200 470 250"], "950x550", "single page", undefined, [
    "-view",
    "single page",
    "-zoom",
    "fit page",
  ]);
  await check("explicit-content-height", 612, 792, ["70 200 470 250"], "420x650", "single page", 250, [
    "-view",
    "single page",
    "-zoom",
    "fit content height",
  ]);
}

if (import.meta.main) await runStandalone(testit);
