// The GROBID outline (header, sections, figures, equations, references) heads each
// DOCUMENT in the source file the knowledge map and Ask send to the AI.
import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, runStandalone, tmpPath } from "./util";

const kPaper = resolve("research/benchmark-2026-10-11/1912.02928v4/0006-On symplectic optimization.pdf");

export async function testit(): Promise<void> {
  const source = tmpPath("grobid-outline.source.txt");
  await withControlledSumatra(EXE, async (client) => {
    const r = await client.request(ControlCommand.TestResearchTools, ["assistant-source", kPaper, source, ""]);
    if (r[0] !== 0) throw new Error(`assistant-source failed: ${r[1]}`);
  });
  const text = readFileSync(source, "utf8");

  const start = text.indexOf("STRUCTURE OF DOCUMENT 1");
  const firstPage = text.indexOf("--- DOCUMENT 1 PAGE 1 ---");
  if (start < 0) throw new Error("no STRUCTURE block");
  if (firstPage < start) throw new Error("STRUCTURE must come before the page text");
  const outline = text.slice(start, firstPage);

  const expect = (what: string, re: RegExp) => {
    if (!re.test(outline)) throw new Error(`outline lacks ${what}:\n${outline.slice(0, 3000)}`);
  };
  expect("title", /^TITLE: On symplectic optimization$/im);
  expect("authors", /^AUTHORS: .*Betancourt/m);
  expect("an Introduction section with its page", /^- P\d+: (\d+\.?\s*)?Introduction/im);
  expect("a figure with page and caption", /^- P\d+ figure: .*\S/m);
  expect("a parsed reference", /^- .*The fundamental incompatibility of scalable Hamiltonian Monte Carlo.*\(2015\)/m);

  // sections are listed in page order
  const pages = [...outline.matchAll(/^- P(\d+): /gm)].map((m) => Number(m[1]));
  if (pages.length < 3) throw new Error(`too few sections: ${pages.length}`);
  for (let i = 1; i < pages.length; i++) {
    if (pages[i] < pages[i - 1]) throw new Error(`sections out of order: ${pages.join(",")}`);
  }
}

if (import.meta.main) {
  await runStandalone(testit);
}
