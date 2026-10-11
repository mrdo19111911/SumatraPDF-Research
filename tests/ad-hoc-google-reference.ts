import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, assemblePdf, runStandalone, tmpPath } from "./util";
import { sleep } from "./winapi";

export async function testit(): Promise<void> {
  const root = tmpPath(`google-reference-${Date.now()}`);
  mkdirSync(root, { recursive: true });
  const raw =
    "De Leon M. & Lainz Valcazar M. Singular Lagrangians and precontact Hamiltonian systems. International Journal of Geometric Methods in Modern Physics, 16 : 1950158, 2019.";
  const title = "Singular Lagrangians and precontact Hamiltonian systems";
  const blocked = "A. Author. JavaScript blocked research paper. Journal of Mathematics, 2021.";
  function pdf(lines: string[]) {
    const stream =
      "BT /F1 10 Tf 20 730 Td " + lines.map((s, i) => `${i ? "0 -20 Td " : ""}(${s}) Tj`).join(" ") + " ET";
    return assemblePdf([
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 2000 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
      `<< /Length ${stream.length} >>\nstream\n${stream}\nendstream`,
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]);
  }
  const input = join(root, "input.pdf");
  writeFileSync(input, pdf(["References", "[1] " + raw, "[2] " + blocked]), "latin1");
  writeFileSync(join(root, "SumatraPDF-settings.txt"), "CheckForUpdates = false\nUiLanguage = en\n");
  const queries: string[] = [];
  let base = "";
  const server = Bun.serve({
    hostname: "127.0.0.1",
    port: 0,
    fetch(req) {
      const u = new URL(req.url);
      if (u.pathname === "/metadata") return Response.json({ message: { items: [] } });
      if (u.pathname === "/api") {
        const id = u.searchParams.get("id_list");
        const entry = id
          ? `<entry><id>https://arxiv.org/abs/${id}</id><title>${id === "1904.11429" ? title : "Unrelated research paper"}</title><link title="pdf" href="${base}/${id}.pdf"/></entry>`
          : "";
        return new Response(`<feed xmlns="http://www.w3.org/2005/Atom">${entry}</feed>`);
      }
      if (u.pathname === "/web") {
        const query = u.searchParams.get("q") || "";
        queries.push(query);
        if (query.includes("JavaScript"))
          return new Response(
            '<html><noscript><a href="/httpservice/retry/enablejs">Enable JavaScript</a></noscript></html>',
          );
        if (query === raw + " pdf")
          return new Response(
            '<a href="https://arxiv.org/abs/1904.99999">Unrelated</a><a href="https://arxiv.org/abs/1904.11429">Singular Lagrangians</a>',
          );
        return new Response("No search results");
      }
      if (u.pathname === "/1904.11429.pdf") return new Response(Buffer.from(pdf([title]), "latin1"));
      if (u.pathname === "/1904.99999.pdf") throw new Error("Unrelated arXiv PDF must not be downloaded");
      return new Response("Missing", { status: 404 });
    },
  });
  base = `http://127.0.0.1:${server.port}`;
  try {
    await withControlledSumatra(
      EXE,
      async (client) => {
        async function probe(action: string, a = "", b = "", c = "") {
          const r = await client.request(ControlCommand.TestResearchTools, [action, a, b, c]);
          if (r[0] !== 0) throw new Error(`${action}: ${r}`);
          return String(r[1]);
        }
        await probe("library-open", "collect");
        while (JSON.parse(await probe("library-state")).running) await sleep(25);
        await probe(
          "library-start",
          join(root, "pdfs"),
          "",
          JSON.stringify({ api: base + "/api", sci: "", metadata: base + "/metadata", web: base + "/web" }),
        );
        let state: any;
        const deadline = Date.now() + 15000;
        do {
          state = JSON.parse(await probe("library-state"));
          if (!state.running) break;
          await sleep(25);
        } while (Date.now() < deadline);
        if (
          state.running ||
          !state.rows[0].path ||
          state.rows[0].title !== title ||
          state.rows[0].arxiv !== "1904.11429"
        )
          throw new Error(`Full citation did not recover the correct arXiv paper: ${JSON.stringify(state.rows[0])}`);
        if (queries[0] !== raw + " pdf" || queries.length !== 2) throw new Error(`Wrong Google queries: ${queries}`);
        if (!state.rows[0].attempts.some((a: any) => a.status === "arXiv result rejected"))
          throw new Error("Unrelated arXiv result was not checked");
        if (state.rows[1].path || !state.rows[1].error.includes("JavaScript"))
          throw new Error(`Google browser requirement was not reported: ${JSON.stringify(state.rows[1])}`);
        console.log(
          "PASS full-citation fallback, arXiv result verification, rejected wrong paper, explicit JavaScript requirement",
        );
      },
      ["-appdata", root, input],
    );
  } finally {
    server.stop();
  }
}

if (import.meta.main) await runStandalone(testit);
