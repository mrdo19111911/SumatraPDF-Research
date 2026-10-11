import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, assemblePdf, runStandalone, tmpPath } from "./util";
import { waitForFrame } from "./win-automation";
import { waitForWindowIdle } from "./winapi";

export async function testit(): Promise<void> {
  const data = tmpPath(`chat-paper-${Date.now()}`);
  mkdirSync(data, { recursive: true });
  const pdf = assemblePdf([
    "<< /Type /Catalog /Pages 2 0 R >>",
    "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
    "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>",
  ]);
  const a = join(data, "paper-A.pdf"),
    b = join(data, "paper-B.pdf");
  writeFileSync(a, pdf, "latin1");
  writeFileSync(b, pdf, "latin1");
  writeFileSync(join(data, "SumatraPDF-settings.txt"), "UiLanguage = en\nCheckForUpdates = false\n");
  await withControlledSumatra(
    EXE,
    async (client, proc) => {
      const frame = await waitForFrame(proc.pid!);
      const invoke = async (cmd: string) => {
        const result = await client.request(ControlCommand.TestInvokeCommand, [cmd]);
        if (result[0] !== 0) throw new Error(`${cmd}: ${result}`);
        await waitForWindowIdle(frame);
      };
      await invoke("CmdAIChatWithClaudeCode");
      // This command must reset both the visible transcript and provider context.
      await invoke("CmdClearAIChat");
      const state = async () => String((await client.request(ControlCommand.TestChatPaper, ["get"]))[1]);
      await client.request(ControlCommand.TestChatPaper, ["seed", "PAPER-A-ONLY"]);
      const oldPath = String((await client.request(ControlCommand.TestCurrentTab))[1]);
      if (!(await state()).includes("PAPER-A-ONLY")) throw new Error("Seed missing");
      await invoke("CmdNextTab");
      if (String((await client.request(ControlCommand.TestCurrentTab))[1]) === oldPath) await invoke("CmdNextTab");
      await invoke("CmdAIChatWithClaudeCode");
      await Bun.sleep(700);
      if ((await state()).includes("PAPER-A-ONLY") || !(await state()).includes("session=\n"))
        throw new Error(`New paper inherited old context: ${await state()}`);
      await client.request(ControlCommand.TestChatPaper, ["seed", "PAPER-B-ONLY"]);
      await invoke("CmdClearAIChat");
      if ((await state()).includes("PAPER-B-ONLY") || !(await state()).includes("session=\n"))
        throw new Error(`Clear kept old context: ${await state()}`);
      await client.request(ControlCommand.TestChatPaper, ["late"]);
      if ((await state()).includes("STALE-RESPONSE")) throw new Error("Cleared chat accepted a late response");
      await invoke("CmdNextTab");
      if (String((await client.request(ControlCommand.TestCurrentTab))[1]) !== oldPath) await invoke("CmdNextTab");
      await invoke("CmdAIChatWithClaudeCode");
      if (!(await state()).includes("PAPER-A-ONLY")) throw new Error("Switching back lost first paper history");
    },
    ["-appdata", data, a, b],
  );
  console.log("PASS: separate paper context, Clear chat, late response rejection. No AI request sent.");
}
if (import.meta.main) await runStandalone(testit);
