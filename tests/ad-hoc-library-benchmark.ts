import { mkdirSync, readFileSync, writeFileSync, existsSync } from "node:fs";
import { basename, join, resolve } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, runStandalone, tmpPath } from "./util";
import { sleep } from "./winapi";

type Row = {
  raw: string;
  title: string;
  path: string;
  status: string;
  source: string;
  error: string;
  attempts?: { source: string; url: string; query: string }[];
};
type State = { running: boolean; message: string; folder: string; rows: Row[] };

export async function testit(): Promise<void> {
  const papers = (process.env.RESEARCH_BENCHMARK_PDFS || "").split("|").filter(Boolean);
  if (!papers.length) throw new Error("Set RESEARCH_BENCHMARK_PDFS to real PDF paths separated by |");
  const output = resolve(process.env.RESEARCH_BENCHMARK_OUTPUT || "research/benchmark-2026-10-11");
  mkdirSync(output, { recursive: true });
  const summaries: unknown[] = [];
  for (const paper of papers) {
    const name = basename(paper, ".pdf"),
      folder = join(output, name);
    if (existsSync(folder)) throw new Error(`Use a fresh benchmark folder: ${folder}`);
    mkdirSync(folder, { recursive: true });
    const profile = tmpPath(`benchmark-${Date.now()}`);
    mkdirSync(profile, { recursive: true });
    writeFileSync(join(profile, "SumatraPDF-settings.txt"), "CheckForUpdates = false\nUiLanguage = en\n");
    const started = Date.now();
    await withControlledSumatra(
      EXE,
      async (client) => {
        async function probe(action: string, a = "", b = "", c = "") {
          const result = await client.request(ControlCommand.TestResearchTools, [action, a, b, c]);
          if (result[0] !== 0) throw new Error(`${action}: ${result}`);
          return String(result[1]);
        }
        await probe("library-open", "collect");
        let state: State;
        do {
          state = JSON.parse(await probe("library-state"));
          if (state.running) await sleep(250);
        } while (state.running);
        if (!state.rows.length) throw new Error(`No references extracted: ${paper}`);
        writeFileSync(join(folder, "extracted.json"), JSON.stringify(state, null, 2));
        console.log(`${name}: ${state.rows.length} references; default folder ${state.folder}`);
        if (process.env.RESEARCH_RETRY_BASE) {
          const prior: State = JSON.parse(
            readFileSync(join(process.env.RESEARCH_RETRY_BASE, name, "collection.json"), "utf8"),
          );
          const match = process.env.RESEARCH_RETRY_MATCH;
          const remaining = prior.rows.filter((r) => !r.path && (!match || r.raw.includes(match)));
          writeFileSync(join(folder, "retry-input.json"), JSON.stringify(remaining, null, 2));
          const readyUntil = Date.now() + 20000;
          while (!(await probe("library-web-state")).includes("navigated=1")) {
            if (Date.now() > readyUntil) throw new Error("Library UI did not load");
            await sleep(100);
          }
          const citations = remaining.map((r, i) => `[${i + 1}] ${r.raw}`).join("\n");
          await probe(
            "library-ui",
            `document.getElementById('citations').value=${JSON.stringify(citations)};document.getElementById('applyBtn').click()`,
          );
          do {
            state = JSON.parse(await probe("library-state"));
            if (state.rows.length === remaining.length && state.rows.every((r, i) => r.raw === remaining[i]!.raw))
              break;
            if (Date.now() > readyUntil) throw new Error("Retry reference list did not apply");
            await sleep(100);
          } while (true);
          console.log(
            `${name}: retrying ${state.rows.length} references; keeping ${prior.rows.length - remaining.length} existing PDFs`,
          );
        }
        await probe(
          "library-start",
          folder,
          "",
          JSON.stringify({
            api: "https://export.arxiv.org/api/query",
            sci: "https://sci-hub.ru/",
            anna: "https://annas-archive.gl/",
            annaApi: "",
            metadata: "https://api.crossref.org/works",
          }),
        );
        let previous = "";
        const deadline = Date.now() + 60 * 60 * 1000;
        do {
          state = JSON.parse(await probe("library-state"));
          writeFileSync(join(folder, "latest-state.json"), JSON.stringify(state, null, 2));
          const downloaded = state.rows.filter((r) => r.path).length;
          const done = state.rows.filter((r) =>
            [
              "Downloaded",
              "Already downloaded",
              "Not found / browser needed",
              "File exists; choose another folder",
              "Cancelled",
            ].includes(r.status),
          ).length;
          const active = state.rows.find((r) => /Searching|Resolving/.test(r.status));
          const progress = `${name}: ${done}/${state.rows.length} processed; ${downloaded} PDFs; ${active?.status || state.message}`;
          if (progress !== previous) {
            console.log(progress);
            previous = progress;
          }
          if (!state.running) break;
          if (Date.now() > deadline) throw new Error(`Benchmark timed out: ${name}`);
          await sleep(1000);
        } while (state.running);
        const sources: Record<string, number> = {};
        for (const row of state.rows.filter((r) => r.path)) {
          const bytes = readFileSync(row.path);
          if (!bytes.subarray(0, 1024).includes(Buffer.from("%PDF-"))) throw new Error(`Invalid PDF: ${row.path}`);
          sources[row.source] = (sources[row.source] || 0) + 1;
        }
        const downloaded = state.rows.filter((r) => r.path).length;
        const google = state.rows
          .filter((r) => !r.path)
          .flatMap((r) => r.attempts || [])
          .find((a) => a.source === "Google" && a.query && a.url);
        if (google) {
          const result = await client.request(ControlCommand.TestResearchTools, [
            "library-fetch-text",
            google.url,
            join(folder, "google-response.html"),
            "",
          ]);
          writeFileSync(
            join(folder, "google-response-status.json"),
            JSON.stringify({ url: google.url, result }, null, 2),
          );
        }
        const summary = {
          paper,
          references: state.rows.length,
          downloaded,
          percent: (100 * downloaded) / state.rows.length,
          sources,
          elapsedSeconds: (Date.now() - started) / 1000,
          folder,
        };
        summaries.push(summary);
        writeFileSync(join(output, "summary.json"), JSON.stringify(summaries, null, 2));
        console.log(`RESULT ${JSON.stringify(summary)}`);
        await probe("library-close");
      },
      ["-appdata", profile, paper],
    );
  }
}
if (import.meta.main) await runStandalone(testit);
