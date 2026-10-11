import { readFileSync, mkdirSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, runStandalone } from "./util";

function norm(text: string): string {
  return text
    .replace(/\\operatorname\{([^}]+)\}/g, "$1")
    .replace(/\\([A-Za-z]+)/g, "$1")
    .normalize("NFKD")
    .replace(/[^a-z0-9]/gi, "")
    .toLowerCase();
}

export async function testit(): Promise<void> {
  const folder = resolve("research/title-benchmark-2026-10-11");
  const expected = JSON.parse(readFileSync(join(folder, "native-expected.json"), "utf8"));
  const cases: [string, string][] = [
    ["Author, A.: A theorem on compact spaces. Math. Journal 8, 20-30 (1999).", "A theorem on compact spaces"],
    ["Alice Author and Bob Writer. A theorem on compact spaces. Journal, 1999.", "A theorem on compact spaces"],
    ["A. Author (1999) Convex analysis. Journal, 1999.", "Convex analysis"],
    ["K.-H. Author. A theorem on compact spaces. Journal, 1999.", "A theorem on compact spaces"],
    ["A. Author, Density: an almost every result, Journal, 1999.", "Density: an almost every result"],
    [
      "A. Author, A theorem on GL(2, 3), where F is a local field, Journal, 1999.",
      "A theorem on GL(2, 3), where F is a local field",
    ],
    ["A. Author (1999) Estimates in small intervals. III . Journal, 1999.", "Estimates in small intervals. III"],
    [
      "A. Author, B. Writer, Prevalence: an almost every theorem, Bull. Math. 7 (1992), 1-10.",
      "Prevalence: an almost every theorem",
    ],
    ["J. S. Baker. Algebraic Groups: A Theory of Schemes. Press, 2001.", "Algebraic Groups: A Theory of Schemes"],
  ];
  await withControlledSumatra(EXE, async (client) => {
    const title = async (raw: string, separator = "", rank = "0") => {
      const result = await client.request(ControlCommand.TestResearchTools, ["library-title", raw, separator, rank]);
      if (result[0] !== 0) throw new Error(`Title probe failed: ${result}`);
      return String(result[1]);
    };
    for (const [raw, wanted] of cases) {
      const actual = await title(raw);
      if (actual !== wanted) throw new Error(`Citation: ${raw}; expected ${wanted}; got ${actual}`);
    }
    for (const marker of [
      "Jour",
      "Journal",
      "J.",
      "Proce",
      "Proceedings",
      "Conference",
      "Conf",
      "Ann.",
      "IEEE",
      "Trans",
      "Transactions",
      "Vol.",
      "Volume",
    ]) {
      const wanted = "A theorem on compact metric spaces";
      const raw = `A. Author, ${wanted}, ${marker} of exceptionally long publication information, 2020.`;
      if ((await title(raw, ",")) !== wanted) throw new Error(`Venue retained: ${marker}`);
    }
    for (const word of ["Annular", "Transport", "Transversality", "Volterra", "Volumetric", "Processes"]) {
      const wanted = `${word} methods on compact metric spaces`;
      if ((await title(`Author, ${wanted}, 2020.`, ",")) !== wanted) throw new Error(`False venue marker: ${word}`);
    }
    for (const size of [20, 21]) {
      const wanted = "ắ".repeat(size);
      const actual = await title(`Author, ${wanted}, 2020.`, ",");
      if (actual !== (size > 20 ? wanted : "")) throw new Error(`Wrong Unicode length: ${size}`);
    }
    const rows = [];
    for (const ref of expected) {
      const actual = await title(ref.raw);
      if (norm(actual) !== norm(ref.python))
        throw new Error(`Python/C++ mismatch: ${ref.key}; ${actual}; ${ref.python}`);
      rows.push({ ...ref, actual, correct: [ref.gold, ...ref.aliases].some((t: string) => norm(t) === norm(actual)) });
    }
    const correct = rows.filter((r) => r.correct).length;
    if (rows.length !== 30 || correct !== 29) throw new Error(`Native benchmark regressed: ${correct}/${rows.length}`);
    const output = join(folder, "native-production");
    mkdirSync(output, { recursive: true });
    writeFileSync(join(output, "results.json"), JSON.stringify({ correct, total: rows.length, rows }, null, 2));
    console.log(
      `PASS ${cases.length} structures, 13 venue markers, 6 word boundaries, Unicode threshold; C++ matches Python on 30 references (${correct}/30 correct). No network calls.`,
    );
  });
}
if (import.meta.main) await runStandalone(testit);
