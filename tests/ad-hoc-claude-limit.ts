// A Claude Code usage-limit reply ("success" subtype with is_error) must fail the AI job, not be saved as its answer.
import { existsSync, mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, runStandalone, tmpPath } from "./util";

export async function testit(): Promise<void> {
  const root = tmpPath(`claude-limit-${Date.now()}`);
  mkdirSync(root, { recursive: true });
  const mock = join(root, "claude.exe");
  const compile = Bun.spawnSync([
    process.execPath,
    "build",
    "--compile",
    "tests/research-mock-claude.ts",
    "--outfile",
    mock,
  ]);
  if (compile.exitCode) throw Error(compile.stderr.toString());
  writeFileSync(
    join(root, "SumatraPDF-settings.txt"),
    `CheckForUpdates = false\nUiLanguage = en\nClaudeCode [\n ExecutablePath = ${mock}\n]\n`,
  );

  const pages = "--- PAGE 1 ---\nPAGE-1-COMPLETE\n--- PAGE 2 ---\nPAGE-2-COMPLETE\n";
  const okSource = join(root, "ok.txt"),
    limitSource = join(root, "limit.txt");
  writeFileSync(okSource, pages);
  writeFileSync(limitSource, pages + "MOCK-RATE-LIMIT\n");
  const okOut = join(root, "ok.out.txt"),
    limitOut = join(root, "limit.out.txt");

  await withControlledSumatra(
    EXE,
    async (client) => {
      const ok = await client.request(ControlCommand.TestResearchTools, ["narrate", okSource, okOut]);
      if (ok[0] !== 0 || !existsSync(okOut)) throw Error(`normal reply failed: ${ok}`);

      const r = await client.request(ControlCommand.TestResearchTools, ["narrate", limitSource, limitOut]);
      if (r[0] === 0) throw Error("usage-limit reply was treated as success");
      if (!String(r[1]).includes("weekly limit")) throw Error(`error lacks the CLI message: ${r[1]}`);
      if (existsSync(limitOut)) throw Error("usage-limit text was saved as the answer");

      // selection translation (backend 0 = Claude) parses the same stream itself
      const kClaude = 0;
      const tr = await client.request(ControlCommand.TestSelectionTranslate, [kClaude, "English", "Polish", "In July"]);
      if (tr[0] !== 0 || tr[1] !== "W lipcu") throw Error(`normal translation failed: ${tr}`);
      const trLimit = await client.request(ControlCommand.TestSelectionTranslate, [
        kClaude,
        "English",
        "Polish",
        "MOCK-RATE-LIMIT",
      ]);
      if (trLimit[0] === 0) throw Error(`usage-limit text was shown as a translation: ${trLimit[1]}`);
      if (!String(trLimit[1]).includes("weekly limit"))
        throw Error(`translation error lacks the CLI message: ${trLimit}`);
    },
    ["-appdata", root],
  );
}

if (import.meta.main) {
  await runStandalone(testit);
}
