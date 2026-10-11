import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, assemblePdf, runStandalone, tmpPath } from "./util";
import { sleep } from "./winapi";

export async function testit(): Promise<void> {
  const root = tmpPath(`arxiv-priority-${Date.now()}`);
  mkdirSync(root, { recursive: true });
  const lines = [
    "References",
    "[1] A. Author. Canonical research title. A much longer journal and publisher description. doi:10.1000/canonical.",
    "[2] M. Agrawal, N. Kayal, N. Saxena, PRIMES is in P, Ann. of Math., 2004.",
    "[3] De Leon M. & Lainz Valcazar M. Singular Lagrangians and precontact Hamilto-",
    "nian systems. International Journal of Geometric Methods in Modern Physics, 16 : 1950158, 2019.",
  ];
  // 10pt keeps every line inside the page: GROBID drops text past the page edge
  const content = "BT /F1 10 Tf 30 730 Td " + lines.map((s, i) => `${i ? "0 -20 Td " : ""}(${s}) Tj`).join(" ") + " ET";
  const payload = assemblePdf([
    "<< /Type /Catalog /Pages 2 0 R >>",
    "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
    "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
    `<< /Length ${content.length} >>\nstream\n${content}\nendstream`,
    "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
  ]);
  const pdf = join(root, "citations.pdf");
  writeFileSync(pdf, payload, "latin1");
  writeFileSync(join(root, "SumatraPDF-settings.txt"), "CheckForUpdates = false\nUiLanguage = en\n");
  let base = "";
  const hits: string[] = [];
  const server = Bun.serve({
    hostname: "127.0.0.1",
    port: 0,
    fetch(req) {
      const u = new URL(req.url);
      hits.push(u.pathname + u.search);
      if (u.pathname.startsWith("/metadata")) {
        const canonical = { DOI: "10.1000/canonical", title: ["<i>Canonical</i> research title"] };
        const primes = {
          DOI: "10.1000/primes",
          title: ["PRIMES is in P"],
          author: [{ family: "Agrawal" }, { family: "Kayal" }, { family: "Saxena" }],
        };
        const singular = {
          DOI: "10.1000/singular",
          title: ["Singular Lagrangians and precontact Hamiltonian systems"],
        };
        const singularCopy = { ...singular, DOI: "10.1000/singular-copy" };
        return Response.json({
          message: u.pathname.endsWith("canonical") ? canonical : { items: [primes, singular, singularCopy] },
        });
      }
      if (u.pathname === "/api") {
        const query = u.searchParams.get("search_query") || "";
        let entry = "";
        if (query === 'ti:"Canonical research title"')
          entry = `<entry><id>http://arxiv.org/abs/2401.12345</id><title>Canonical research title</title><link title="pdf" href="${base}/first.pdf"/></entry>`;
        if (query.includes("10.1000/primes"))
          entry = `<entry><id>http://arxiv.org/abs/2401.12346</id><title>PRIMES is in P</title><doi>10.1000/primes</doi><link title="pdf" href="${base}/second.pdf"/></entry>`;
        if (query === 'ti:"Singular Lagrangians and precontact Hamiltonian systems"')
          entry = `<entry><id>http://arxiv.org/abs/2401.12347</id><title>Singular Lagrangians and precontact Hamiltonian systems</title><link title="pdf" href="${base}/third.pdf"/></entry>`;
        return new Response(`<feed xmlns="http://www.w3.org/2005/Atom">${entry}</feed>`);
      }
      if (u.pathname.endsWith(".pdf")) return new Response(Buffer.from(payload, "latin1"));
      return new Response("No PDF", { status: 404 });
    },
  });
  base = `http://127.0.0.1:${server.port}`;
  try {
    await withControlledSumatra(
      EXE,
      async (client) => {
        const probe = async (action: string, a = "", b = "", c = "") => {
          const r = await client.request(ControlCommand.TestResearchTools, [action, a, b, c]);
          if (r[0] !== 0) throw new Error(`${action}: ${r}`);
          return JSON.parse(String(r[1]) === "opened" || String(r[1]) === "started" ? "null" : String(r[1]));
        };
        await probe("library-open", "collect");
        while ((await probe("library-state")).running) await sleep(25);
        await probe(
          "library-start",
          join(root, "pdfs"),
          "",
          JSON.stringify({
            api: base + "/api",
            sci: base + "/sci/",
            anna: base + "/anna/",
            metadata: base + "/metadata",
            web: base + "/web",
          }),
        );
        let state: any;
        const deadline = Date.now() + 15000;
        do {
          state = await probe("library-state");
          if (!state.running) break;
          await sleep(25);
        } while (Date.now() < deadline);
        if (state.running || state.rows.length !== 3 || state.rows.some((r: any) => r.source !== "arXiv"))
          throw new Error(`Journal citations did not prefer arXiv: ${JSON.stringify(state)}`);
        if (hits.some((h) => h.startsWith("/sci/") || h.startsWith("/anna/")))
          throw new Error(`Unnecessary fallback: ${hits}`);
        if (!hits.some((h) => h.startsWith("/metadata/10.1000%2Fcanonical")))
          throw new Error("Known DOI metadata was not used");
        const queries = state.rows[1].attempts
          .filter((a: any) => a.source === "arXiv" && a.query)
          .map((a: any) => a.query);
        if (queries.join("|") !== 'ti:"PRIMES is in P"|au:"Agrawal"|all:"10.1000/primes"')
          throw new Error(`Wrong arXiv query priority: ${queries}`);
        if (!state.report) throw new Error("No generated HTML report");
        console.log("PASS canonical title from DOI, short title with author evidence, arXiv before PDF fallbacks");
      },
      ["-appdata", root, pdf],
    );
  } finally {
    server.stop();
  }
}
if (import.meta.main) await runStandalone(testit);
