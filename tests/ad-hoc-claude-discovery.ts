import { writeFileSync } from "node:fs";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, assemblePdf, runStandalone, tmpPath } from "./util";

export async function testit(): Promise<void> {
  const pdf = tmpPath("claude-discovery.pdf");
  const stream = "BT /F1 18 Tf 30 100 Td (Claude discovery test) Tj ET";
  writeFileSync(
    pdf,
    assemblePdf([
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 180] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
      `<< /Length ${stream.length} >>\nstream\n${stream}\nendstream`,
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]),
    "latin1",
  );
  await withControlledSumatra(
    EXE,
    async (client) => {
      await client.waitForRenderIdle();
      await client.seedTextSelection(1);
      const result = await client.request(ControlCommand.TestCommandVisibility, [
        "CmdTranslateSelectionWithClaudeCode",
      ]);
      const text = String(result[1] ?? "");
      if (result[0] !== 0 || !text.includes("vis=show")) {
        throw new Error(`Installed Claude Code was not discovered: ${text}`);
      }
      console.log("PASS: installed Claude Code discovered; no AI request sent");
    },
    [pdf],
  );
}

if (import.meta.main) {
  await runStandalone(testit);
}
