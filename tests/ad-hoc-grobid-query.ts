// Connected Papers / arXiv search: an arXiv ID on page 1 wins; otherwise, when the PDF has no
// usable Title metadata and its file name says nothing, the title GROBID reads from page 1.
import { copyFileSync } from "node:fs";
import { resolve } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, runStandalone, tmpPath } from "./util";

const kDir = "research/benchmark-2026-10-11/1912.02928v4";
// no Title metadata, no arXiv ID or DOI on page 1
const kNoIds = resolve(
  kDir,
  "0016-Cosymplectic and contact structures for time-dependent and dissipative Hamiltonian systems.pdf",
);
// garbage Title metadata, arXiv:1802.03653 stamped on page 1
const kArxiv = resolve(kDir, "0006-On symplectic optimization.pdf");

async function browserUrl(paper: string): Promise<string> {
  const pdf = tmpPath("paper.pdf");
  copyFileSync(paper, pdf);
  const url = await withControlledSumatra(
    EXE,
    async (client) => {
      const r = await client.request(ControlCommand.TestResearchTools, ["browser-url", "", "", ""]);
      if (r[0] !== 0) throw new Error(`browser-url failed: ${r[1]}`);
      return String(r[1]);
    },
    [pdf],
  );
  return decodeURIComponent(url.replace(/\+/g, " "));
}

export async function testit(): Promise<void> {
  const title = await browserUrl(kNoIds);
  if (!/Cosymplectic and contact structures for time-dependent and dissipative Hamiltonian systems/i.test(title))
    throw new Error(`query lacks the paper title: ${title}`);

  // the page-1 text read must not depend on a busy text cache
  for (let i = 0; i < 3; i++) {
    const url = await browserUrl(kArxiv);
    if (!url.endsWith("/api/redirect/arxiv/1802.03653")) throw new Error(`run ${i}: no arXiv redirect: ${url}`);
  }
}

if (import.meta.main) {
  await runStandalone(testit);
}
