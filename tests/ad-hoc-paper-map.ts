import { mkdirSync, writeFileSync, readFileSync } from "node:fs";
import { join, resolve } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, assemblePdf, runStandalone, tmpPath, loadPng, pngPixel } from "./util";
import { clickAt, waitForFrame } from "./win-automation";
import { sleep, setProcessDpiAware, waitForWindowIdle, captureWindowToPng, moveWindow } from "./winapi";

export async function testit(): Promise<void> {
  setProcessDpiAware();
  const root = tmpPath(`paper-map-${Date.now()}`),
    evidence = resolve("out/paper-map-v474");
  mkdirSync(root, { recursive: true });
  mkdirSync(evidence, { recursive: true });
  const pdf = join(root, "paper-A.pdf"),
    second = join(root, "paper-B.pdf");
  const objects = ["<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Count 2 /Kids [3 0 R 5 0 R] >>"];
  for (let page = 1; page <= 2; page++) {
    let text = "BT /F1 12 Tf 70 720 Td ";
    for (let line = 0; line < 21; line++)
      text += `(${line === 10 ? `Before context PAGE-${page}-COMPLETE after context.` : "A full line of research content with assumptions and results."}) Tj 0 -28 Td `;
    text += "ET";
    objects.push(
      `<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 7 0 R >> >> /Contents ${page * 2 + 2} 0 R >>`,
      `<< /Length ${text.length} >>\nstream\n${text}\nendstream`,
    );
  }
  objects.push("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
  writeFileSync(pdf, assemblePdf(objects), "latin1");
  writeFileSync(second, assemblePdf(objects), "latin1");
  const mock = join(root, "claude.exe");
  const compile = Bun.spawnSync([
    process.execPath,
    "build",
    "--compile",
    "tests/assistant-mock-claude.ts",
    "--outfile",
    mock,
  ]);
  if (compile.exitCode) throw Error(compile.stderr.toString());
  writeFileSync(
    join(root, "SumatraPDF-settings.txt"),
    `UiLanguage = en\nCheckForUpdates = false\nToolbarPlacement = top\nClaudeCode [\n ExecutablePath = ${mock}\n]\n`,
  );
  await withControlledSumatra(
    EXE,
    async (client, proc) => {
      const frame = await waitForFrame(proc.pid!);
      async function invoke(cmd: string) {
        const r = await client.request(ControlCommand.TestInvokeCommand, [cmd]);
        if (r[0]) throw Error(String(r));
        await waitForWindowIdle(frame);
      }
      async function native(action: string, a = "") {
        const r = await client.request(ControlCommand.TestResearchTools, [action, a]);
        if (r[0]) throw Error(String(r));
        return String(r[1]);
      }
      async function until(check: () => Promise<boolean>, what: string) {
        const deadline = Date.now() + 20000;
        while (Date.now() < deadline) {
          if (await check()) return;
          await sleep(40);
        }
        throw Error(`Timeout: ${what}`);
      }
      const current = async () => String((await client.request(ControlCommand.TestCurrentTab))[1]);
      async function choose(path: string) {
        for (let i = 0; i < 5; i++) {
          if ((await current()).startsWith(`path=${path} page=`)) return;
          await invoke("CmdNextTab");
        }
        throw Error("Cannot select paper");
      }
      await choose(pdf);
      const initial = await native("map-state");
      await invoke("CmdPaperMap");
      let data: any;
      await until(async () => {
        const r = await client.request(ControlCommand.TestResearchTools, ["map-probe"]);
        if (r[0] || !r[1]) return false;
        data = JSON.parse(String(r[1]))[0];
        return data.nodes?.length === 3;
      }, "KG generation and WebView load");
      const opened = await native("map-state");
      const selectPane = async (name: string) => {
        const tabs = await native("pane-tabs");
        const r = new RegExp(`tab=${name} .*rect=([\\d,-]+)`).exec(tabs)![1]!.split(",").map(Number);
        await clickAt(frame, (r[0]! + r[2]!) / 2, (r[1]! + r[3]!) / 2);
        await waitForWindowIdle(frame);
      };
      await selectPane("KG");
      if (!(await native("map-state")).includes("open=1")) throw Error("Selected KG tab unexpectedly closes the pane");
      await selectPane("Chat");
      if ((await native("map-state")).includes("open=1")) throw Error("Chat tab did not replace KG");
      const chat = String((await client.request(ControlCommand.TestChatPaper, ["backend"]))[1]);
      if (!chat.includes("visible=1")) throw Error("Chat tab did not open chat");
      await selectPane("KG");
      if (!(await native("map-state")).includes("open=1")) throw Error("KG tab lost the existing map");
      if (data.images !== 0) throw Error("KG rendered provider labels as executable markup");
      if (!opened.includes("open=1") || !opened.includes("auto=1")) throw Error(`KG not docked with AUTO: ${opened}`);
      const width = (s: string) => Number(/canvas=(\d+)x/.exec(s)![1]);
      if (width(opened) >= width(initial)) throw Error("KG overlays the PDF instead of reserving space");
      const noKgMode = String((await client.request(ControlCommand.TestDisplayMode, ["get"]))[1]);
      if (!noKgMode.includes("mode=single page")) throw Error(`KG must pair with one PDF page: ${noKgMode}`);
      await native("map-js", "document.querySelector('[data-node=n2]').dispatchEvent(new MouseEvent('mouseenter'))");
      await until(async () => {
        data = JSON.parse(await native("map-probe"))[0];
        return (
          data.tooltip.includes("Before context") &&
          data.tooltip.includes("after context") &&
          data.tooltip.includes("Đoạn gốc PDF")
        );
      }, "source text hover");
      await native(
        "map-js",
        "const inspect=paperMap.inspect;paperMap.inspect=()=>({...inspect(),tipRight:document.getElementById('tooltip').getBoundingClientRect().right,viewWidth:innerWidth,tipScrollWidth:document.getElementById('tooltip').scrollWidth,tipWidth:document.getElementById('tooltip').clientWidth})",
      );
      await until(async () => {
        data = JSON.parse(await native("map-probe"))[0];
        return data.viewWidth > 0;
      }, "hover geometry");
      if (data.tipRight > data.viewWidth || data.tipScrollWidth > data.tipWidth + 1)
        throw Error(`KG source tooltip clips its text: ${JSON.stringify(data)}`);
      if (!(await native("map-state")).includes("page=1")) throw Error("Hover navigated the PDF");
      await native("map-js", "document.querySelector('[data-node=n2]').click()");
      await until(async () => {
        const r = await client.request(ControlCommand.TestResearchTools, ["assistant-state"]);
        return String(r[1]).includes("page=2 highlight=1");
      }, "click source highlight");
      await client.waitForRenderIdle();
      await waitForWindowIdle(frame);
      await until(async () => {
        data = JSON.parse(await native("map-probe"))[0];
        return data.status.includes("Đã đến");
      }, "navigation feedback");
      if (data.tipRight > data.viewWidth || data.tipScrollWidth > data.tipWidth + 1)
        throw Error(`KG tooltip clips after navigation: ${JSON.stringify(data)}`);
      await native("map-image", join(evidence, "kg-surface.png"));
      await until(async () => {
        try {
          const png = loadPng(join(evidence, "kg-surface.png"));
          let ink = 0;
          for (let y = 0; y < png.h; y += 4)
            for (let x = 0; x < png.w; x += 4)
              if (
                pngPixel(png, x, y)
                  .slice(0, 3)
                  .every((c) => c < 140)
              )
                ink++;
          return ink > 100;
        } catch {
          return false;
        }
      }, "visible KG browser surface");
      captureWindowToPng(frame, join(evidence, "pdf-with-kg.png"));
      await native(
        "map-js",
        "document.querySelector('[data-node=n3]').dispatchEvent(new MouseEvent('mouseenter'));document.querySelector('[data-node=n3]').click()",
      );
      await until(async () => {
        data = JSON.parse(await native("map-probe"))[0];
        return data.status.includes("Không xác minh");
      }, "reject invented quote");
      if (!(await native("map-state")).includes("page=2")) throw Error("Invalid anchor moved the PDF");
      moveWindow(frame, 0, 0, 2600, 700);
      await waitForWindowIdle(frame);
      const wide = String((await client.request(ControlCommand.TestDisplayMode, ["get"]))[1]);
      if (!wide.includes("mode=single page")) throw Error(`KG must pair with one PDF page on wide windows: ${wide}`);
      moveWindow(frame, 0, 0, 1800, 1000);
      await waitForWindowIdle(frame);
      await choose(second);
      if (!(await native("map-state")).includes("open=0")) throw Error("Second paper inherited first KG");
      await choose(pdf);
      if (!(await native("map-state")).includes("open=1")) throw Error("Returning lost first KG");
      await native("map-js", "document.getElementById('close').click()");
      await until(async () => {
        const s = await native("map-state");
        return s.includes("open=0") && width(s) === width(initial);
      }, "close restores PDF area");
      const closedMode = String((await client.request(ControlCommand.TestDisplayMode, ["get"]))[1]);
      if (!closedMode.includes("zoom=fit content height")) throw Error(`Closing KG lost AUTO zoom: ${closedMode}`);
      await invoke("CmdPaperMap");
      if (!(await native("map-state")).includes("open=1")) throw Error("KG toggle did not reopen");
      await invoke("CmdReloadDocument");
      if (!(await native("map-state")).includes("open=0")) throw Error("Reload kept a map tied to the old PDF engine");
      await invoke("CmdPaperMap");
      await until(async () => {
        const r = await client.request(ControlCommand.TestResearchTools, ["map-probe"]);
        return r[0] === 0 && r[1] && JSON.parse(String(r[1]))[0].nodes?.length === 3;
      }, "regenerate after reload");
      moveWindow(frame, 0, 0, 650, 950);
      await waitForWindowIdle(frame);
      const narrow = String((await client.request(ControlCommand.TestDisplayMode, ["get"]))[1]);
      if (!narrow.includes("mode=single page")) throw Error(`Portrait PDF pane still facing: ${narrow}`);
      await invoke("CmdToggleReadingDarkMode");
      await waitForWindowIdle(frame);
      captureWindowToPng(frame, join(evidence, "kg-narrow-dark.png"));
      await invoke("CmdPaperMap");
      if (!(await native("map-state")).includes("open=0")) throw Error("KG toggle did not close");
      await invoke("CmdPaperMap");
      await invoke("CmdCloseCurrentDocument");
      if ((await current()).startsWith(`path=${pdf} page=`)) throw Error("KG blocked paper close");
      writeFileSync(
        join(evidence, "result.txt"),
        `${initial}\n${opened}\n${noKgMode}\n${closedMode}\n${narrow}\nPASS hover, click, invalid source, per-paper state, AUTO fallback, dark mode, close.\n`,
      );
      const request = readFileSync(join(root, "ResearchReports/assistant-request.json"), "utf8");
      if (!request.includes("knowledge map") || !request.includes("PAGE-2-COMPLETE"))
        throw Error("Incomplete KG input");
    },
    ["-appdata", root, "-window-pos", "1800x1000@0x0", pdf, second],
  );
  console.log(
    "PASS KG beside PDF, original context hover, exact source navigation, per-paper state and AUTO fallback (mock AI).",
  );
}
if (import.meta.main) await runStandalone(testit);
