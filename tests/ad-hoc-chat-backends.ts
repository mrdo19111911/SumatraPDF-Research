import { mkdirSync, writeFileSync, copyFileSync, existsSync } from "node:fs";
import { join, dirname } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, assemblePdf, runStandalone, tmpPath } from "./util";
import { enumChildWindows, getClassName, getControlText, sendMessage, sleep } from "./winapi";
import { waitForFrame } from "./win-automation";

export async function testit(): Promise<void> {
  const root = tmpPath(`chat-backends-${Date.now()}`);
  mkdirSync(root, { recursive: true });
  const mock = join(root, "claude.exe");
  const built = Bun.spawnSync([
    process.execPath,
    "build",
    "--compile",
    "tests/chat-backend-mock.ts",
    "--outfile",
    mock,
  ]);
  if (built.exitCode) throw new Error(built.stderr.toString());
  copyFileSync(mock, join(root, "codex.exe"));
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
  writeFileSync(
    join(root, "SumatraPDF-settings.txt"),
    `CheckForUpdates = false\nAIChatDefaultBackend = auto\nClaudeCode [\n ExecutablePath = ${mock}\n]\n`,
  );
  const app = join(root, "SumatraPDF.exe");
  copyFileSync(EXE, app);
  for (const name of ["libsumatrapdf.dll", "PdfFilter.dll", "PdfPreview.dll", "sumatrapdf-tool.exe"]) {
    const source = join(dirname(EXE), name);
    if (existsSync(source)) copyFileSync(source, join(root, name));
  }
  const env = Object.fromEntries(Object.entries(process.env).filter(([key]) => key.toLowerCase() !== "path"));
  env.Path = `${root};${process.env.SystemRoot}\\System32`;
  {
    await withControlledSumatra(
      app,
      async (client, proc) => {
        await client.request(ControlCommand.TestInvokeCommand, ["CmdPaperChat"]);
        const frame = await waitForFrame(proc.pid!);
        let selector = 0;
        enumChildWindows(frame, (hwnd) => {
          if (getClassName(hwnd) === "ComboBox" && getControlText(hwnd).startsWith("Auto")) selector = hwnd;
          return true;
        });
        if (!selector || Number(sendMessage(selector, 0x0146, 0, 0)) !== 5)
          throw new Error("Provider selector lacks Auto and four backends");
        let state = String((await client.request(ControlCommand.TestChatPaper, ["backend"]))[1]);
        if (!state.includes("provider=0") || !state.includes("default=auto") || !state.includes("visible=1"))
          throw new Error(`Auto does not bind mock Claude: ${state}`);
        await client.request(ControlCommand.TestChatPaper, ["send-button", "FALLBACK-QUESTION"]);
        const deadline = Date.now() + 12000;
        let log = "";
        while (Date.now() < deadline) {
          state = String((await client.request(ControlCommand.TestChatPaper, ["backend"]))[1]);
          log = String((await client.request(ControlCommand.TestChatPaper, ["get"]))[1]);
          if (state.includes("provider=2") && log.includes("CODEX-FALLBACK-OK")) break;
          await sleep(40);
        }
        if (!state.includes("provider=2") || !log.includes("CODEX-FALLBACK-OK"))
          throw new Error(`Quota fallback failed: ${state}\n${log}`);
        await client.request(ControlCommand.TestInvokeCommand, ["CmdPaperChat"]);
        if (!String((await client.request(ControlCommand.TestChatPaper, ["backend"]))[1]).includes("visible=0"))
          throw new Error("Generic Chat cannot close");
        await client.request(ControlCommand.TestInvokeCommand, ["CmdAIChatWithClaudeCode"]);
        await client.request(ControlCommand.TestChatPaper, ["send", "FALLBACK-QUESTION"]);
        await sleep(700);
        state = String((await client.request(ControlCommand.TestChatPaper, ["backend"]))[1]);
        if (!state.includes("provider=0") || !state.includes("default=claude"))
          throw new Error("Explicit provider selection was ignored");
        console.log(
          "PASS Auto detects backends; real Claude quota parser falls back to mock Codex; question retained; manual selection and toggle work",
        );
      },
      ["-appdata", root, pdf],
      { cwd: root, env },
    );
  }
}
if (import.meta.main) await runStandalone(testit);
