import { mkdirSync, writeFileSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, assemblePdf, runStandalone, tmpPath } from "./util";
import {
  sleep,
  captureWindowToPng,
  enumWindows,
  getWindowPid,
  getWindowText,
  setProcessDpiAware,
  captureWindowPixels,
} from "./winapi";

export async function testit(): Promise<void> {
  setProcessDpiAware();
  const root = tmpPath(`assistant-${Date.now()}`);
  mkdirSync(root, { recursive: true });
  const pdf = join(root, "paper.pdf"),
    second = join(root, "version.pdf");
  const objects = ["<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Count 2 /Kids [3 0 R 5 0 R] >>"];
  for (let i = 0; i < 2; i++) {
    const text = `BT /F1 16 Tf 70 700 Td (PAGE-${i + 1}-COMPLETE) Tj ET`;
    objects.push(
      `<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 7 0 R >> >> /Contents ${4 + i * 2} 0 R >>`,
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
  if (compile.exitCode) throw new Error(compile.stderr.toString());
  writeFileSync(
    join(root, "SumatraPDF-settings.txt"),
    `CheckForUpdates = false\nUiLanguage = en\nClaudeCode [\n ExecutablePath = ${mock}\n]\n`,
  );
  await withControlledSumatra(
    EXE,
    async (client, proc) => {
      async function probe(args: string[], ok = true) {
        const r = await client.request(ControlCommand.TestResearchTools, args);
        if ((r[0] === 0) !== ok) throw new Error(`${args[0]}: ${r}`);
        return String(r[1]);
      }
      if (process.argv.includes("--history")) {
        await client.request(ControlCommand.TestInvokeCommand, ["CmdAIChatWithClaudeCode"]);
        const before = String((await client.request(ControlCommand.TestChatPaper, ["get"]))[1]);
        await client.request(ControlCommand.TestInvokeCommand, ["CmdResearchAsk"]);
        const deadline = Date.now() + 12000;
        let shown = false;
        while (Date.now() < deadline) {
          const report = await client.request(ControlCommand.TestResearchTools, ["assistant-report", "probe"]);
          if (report[0] === 0 && report[1]) {
            shown = true;
            break;
          }
          await sleep(50);
        }
        if (!shown) throw new Error("No research report");
        const after = String((await client.request(ControlCommand.TestChatPaper, ["get"]))[1]);
        if (after !== before) throw new Error("Research report added history the chat provider has never received");
        await probe(["assistant-report", "close"]);
        console.log("PASS standalone report preserves the provider's conversation.");
        return;
      }
      if (process.argv.includes("--lifetime")) {
        const reports = join(root, "ResearchReports");
        mkdirSync(reports, { recursive: true });
        writeFileSync(join(reports, "assistant-hold"), "");
        await client.request(ControlCommand.TestInvokeCommand, ["CmdResearchAsk"]);
        const ready = Date.now() + 5000;
        while (!(await Bun.file(join(reports, "assistant-request.json")).exists()) && Date.now() < ready)
          await sleep(25);
        if (!(await Bun.file(join(reports, "assistant-request.json")).exists()))
          throw new Error("Provider not started");
        await client.request(ControlCommand.TestInvokeCommand, ["CmdCloseCurrentDocument"]);
        const deadline = Date.now() + 2000;
        while ((await probe(["running"])) !== "idle" && Date.now() < deadline) await sleep(25);
        if ((await probe(["running"])) !== "idle") throw new Error("Closing paper did not cancel its AI process");
        console.log("PASS closing paper cancels running research provider.");
        return;
      }
      const modes = ["ask", "selection", "symbols", "compare", "versions", "figure", "reading"];
      for (const mode of modes) {
        const prompt = await probe(["assistant-prompt", mode]);
        if (!prompt.includes("sumatra-source:") || !prompt.includes("source data") || !prompt.includes("Vietnamese"))
          throw new Error(`Ungrounded ${mode}`);
      }
      const source = join(root, "indexed.txt");
      await probe(["assistant-source", pdf, source, second]);
      const indexed = readFileSync(source, "utf8");
      for (const tag of ["DOCUMENT 1", "DOCUMENT 2", "PAGE 1", "PAGE 2", "PAGE-2-COMPLETE"]) {
        if (!indexed.includes(tag)) throw new Error(`Missing ${tag}`);
      }
      for (const mode of modes) {
        const output = join(root, `${mode}.md`);
        await probe(["assistant-run", mode, second, output]);
        if (!readFileSync(output, "utf8").includes("sumatra-source:1/2#PAGE-2-COMPLETE"))
          throw new Error(`Lost citation ${mode}`);
        if (mode === "compare") {
          await probe(["assistant-cite", "sumatra-source:2/2#PAGE-2-COMPLETE"]);
          if (!String((await client.request(ControlCommand.TestCurrentTab))[1]).startsWith(`path=${second} page=`))
            throw new Error("Second paper citation opened wrong file");
          await client.request(ControlCommand.TestInvokeCommand, ["CmdPrevTab"]);
          if (!String((await client.request(ControlCommand.TestCurrentTab))[1]).startsWith(`path=${pdf} page=`))
            throw new Error("Cannot return to source paper");
        }
      }
      await probe(["assistant-cite", "sumatra-source:1/2#PAGE-2-COMPLETE"]);
      const cited = await probe(["assistant-state"]);
      if (!cited.includes("page=2") || !cited.includes("highlight=1"))
        throw new Error(`No exact citation highlight ${cited}`);
      await probe(["assistant-cite", "sumatra-source:1/2#rect=70,70,200,150"]);
      if (!(await probe(["assistant-state"])).includes("highlight=1"))
        throw new Error("Figure region did not highlight");
      await probe(["assistant-cite", "sumatra-source:1/2#NOT-ON-PAGE"], false);
      for (const bad of [
        "sumatra-source:1/99",
        "sumatra-source:0/1",
        "sumatra-source:3/1",
        "file:///C:/secret",
        "sumatra-source:1/2/extra",
        "sumatra-source:1/2#rect=70,70,9999,9999",
        "sumatra-source:1/2#rect=-1,0,50,50",
      ])
        await probe(["assistant-cite", bad], false);
      await probe(["assistant-clear"]);
      await probe(["assistant-cite", "sumatra-source:1/2#PAGE-2-COMPLETE"]);
      await probe(["assistant-cite", "sumatra-source:2/2#PAGE-2-COMPLETE"]);
      if (!String((await client.request(ControlCommand.TestCurrentTab))[1]).startsWith(`path=${second} page=`))
        throw new Error("Clearing one conversation invalidated another conversation's source");
      await client.request(ControlCommand.TestInvokeCommand, ["CmdPrevTab"]);
      if (process.argv.includes("--sources")) {
        console.log("PASS source IDs remain stable across Clear chat.");
        return;
      }
      const invoked = await client.request(ControlCommand.TestInvokeCommand, ["CmdResearchReading"]);
      if (invoked[0] !== 0) throw new Error("Reading command missing");
      const responsive = Date.now();
      await probe(["running"]);
      if (Date.now() - responsive > 1000) throw new Error("Reading worker blocked UI");
      async function waitReport(check: (state: any) => boolean) {
        const until = Date.now() + 12000;
        let last: any;
        while (Date.now() < until) {
          const r = await client.request(ControlCommand.TestResearchTools, ["assistant-report", "probe"]);
          if (r[0] === 0 && r[1]) {
            const state = JSON.parse(String(r[1]))[0];
            last = state;
            if (check(state)) return state;
          }
          await sleep(50);
        }
        throw new Error(`Reading webview did not reach expected state: ${JSON.stringify(last)?.slice(0, 800)}`);
      }
      await waitReport(
        (s) =>
          s.font === "18px" &&
          s.links.includes("sumatra-source:1/2#PAGE-2-COMPLETE") &&
          s.text.includes("Original PDF D1 / page 2") &&
          !s.images.length,
      );
      await waitReport((s) => s.math === 4 && s.text.includes("$code$") && s.text.includes("\\(code\\)"));
      await client.request(ControlCommand.TestInvokeCommand, ["CmdToggleReadingDarkMode"]);
      await waitReport((s) => s.dark === true);
      await client.request(ControlCommand.TestInvokeCommand, ["CmdToggleReadingDarkMode"]);
      await waitReport((s) => s.dark === false && s.math === 4);
      await probe(["assistant-report", "dark"]);
      await waitReport((s) => s.dark === true && s.math === 4);
      await probe(["assistant-report", "dark"]);
      await waitReport((s) => s.dark === false);
      await probe(["assistant-report", "larger"]);
      await waitReport((s) => s.font === "20px");
      await probe(["assistant-report", "original"]);
      await waitReport((s) => s.images.some((im: any) => im.width > 100));
      enumWindows((hwnd) => {
        if (getWindowPid(hwnd) === proc.pid && getWindowText(hwnd).startsWith("AI Reading view —"))
          captureWindowToPng(hwnd, join("out", "research-reading-v46.png"));
        if (getWindowPid(hwnd) === proc.pid && getWindowText(hwnd).startsWith("AI Reading view —")) {
          const pixels = captureWindowPixels(hwnd);
          if (!pixels) throw new Error("Cannot inspect report header");
          let ghost = 0;
          for (let y = 0; y < 32; y++)
            for (let x = 0; x < 7; x++) {
              const offset = (y * pixels.w + x) * 4;
              if (pixels.data[offset]! < 120 && pixels.data[offset + 1]! < 120 && pixels.data[offset + 2]! < 120)
                ghost++;
            }
          if (ghost) throw new Error("Window title painted behind report buttons");
        }
        return true;
      });
      await probe(["assistant-report", "click"]);
      if (!(await probe(["assistant-state"])).includes("page=2"))
        throw new Error("Webview citation did not navigate PDF");
      await probe(["assistant-report", "close"]);
      await client.request(ControlCommand.TestInvokeCommand, ["CmdSelectAll"]);
      await client.request(ControlCommand.TestInvokeCommand, ["CmdResearchSelection"]);
      await waitReport((s) => s.links.includes("sumatra-source:1/2#PAGE-2-COMPLETE"));
      await probe(["assistant-report", "close"]);
      await client.request(ControlCommand.TestInvokeCommand, ["CmdResearchAsk"]);
      const started = Date.now() + 5000;
      const requests = join(root, "ResearchReports", "assistant-request.json");
      while (Date.now() < started) {
        try {
          if (JSON.parse(readFileSync(requests, "utf8")).prompt.includes("Explain the paper's problem")) break;
        } catch {}
        await sleep(25);
      }
      if (!JSON.parse(readFileSync(requests, "utf8")).prompt.includes("Explain the paper's problem"))
        throw new Error("Async question did not start provider");
      await probe(["assistant-clear"]);
      const stopped = Date.now() + 8000;
      while ((await probe(["running"])) !== "idle" && Date.now() < stopped) await sleep(50);
      if ((await probe(["running"])) !== "idle") throw new Error("Clear failed to stop the provider");
      const noReport = await client.request(ControlCommand.TestResearchTools, ["assistant-report", "probe"]);
      if (noReport[0] === 0) throw new Error("Clear delivered a stale report");
      console.log(
        "PASS seven actions, indexed multi-paper sources, provider flow, page-local quote and rejected references.",
      );
    },
    ["-appdata", root, pdf, ...(process.argv.includes("--lifetime") ? [second] : [])],
  );
}
if (import.meta.main) await runStandalone(testit);
