// Downloaded papers are saved as "<paper title>.pdf" with characters Windows forbids removed.
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, runStandalone } from "./util";

const kCases: [string, string][] = [
  ["On symplectic optimization", "On symplectic optimization.pdf"],
  ['Hamiltonian descent: a "new" view?', "Hamiltonian descent a new view.pdf"],
  ["Input/output <maps> | A*B", "Inputoutput maps AB.pdf"],
  ["  Spaces\tand\nnew   lines  ", "Spaces and new lines.pdf"],
  ["Trailing dots...", "Trailing dots.pdf"],
  ["Phương pháp số: Ước lượng", "Phương pháp số Ước lượng.pdf"],
  ["CON", "CON_.pdf"],
  ["lpt1", "lpt1_.pdf"],
  ["", "paper.pdf"],
  ["???", "paper.pdf"],
];

export async function testit(): Promise<void> {
  await withControlledSumatra(EXE, async (client) => {
    const name = async (title: string) => {
      const r = await client.request(ControlCommand.TestResearchTools, ["paper-filename", title, "", ""]);
      if (r[0] !== 0) throw new Error(`paper-filename failed: ${r[1]}`);
      return String(r[1]);
    };
    for (const [title, want] of kCases) {
      const got = await name(title);
      if (got !== want)
        throw new Error(`${JSON.stringify(title)} -> ${JSON.stringify(got)}, want ${JSON.stringify(want)}`);
    }

    // long titles are cut on a character boundary, not inside a UTF-8 sequence
    const long = await name("Định lý ".repeat(40));
    if (Buffer.byteLength(long) > 160 || long.includes("�") || !long.endsWith(".pdf"))
      throw new Error(`bad long name: ${long}`);
  });
}

if (import.meta.main) {
  await runStandalone(testit);
}
