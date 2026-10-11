import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, assemblePdf, runStandalone, tmpPath } from "./util";
import { sleep } from "./winapi";

// Sci-Hub by paper title: its form POSTs `request=<title>` to the endpoint, which
// redirects to the article page (/<doi>) or shows the home page again when nothing matches.
export async function testit(): Promise<void> {
  const root = tmpPath(`scihub-title-${Date.now()}`);
  mkdirSync(root, { recursive: true });
  const makePdf = (lines: string[]) => {
    const content =
      "BT /F1 10 Tf 40 730 Td " + lines.map((s, i) => `${i ? "0 -20 Td " : ""}(${s}) Tj`).join(" ") + " ET";
    return assemblePdf([
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
      `<< /Length ${content.length} >>\nstream\n${content}\nendstream`,
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]);
  };
  const found = "Hamiltonian flows on Lie groups",
    unknown = "Unknown results nobody has indexed anywhere",
    wrong = "Wrong paper behind this title",
    guarded = "Title behind a robot check";
  // real sci-hub.vn answers some searches with /match/<title>, a CAPTCHA page with no links
  const robot = `<html><title>Sci-Hub: are you a robot?</title><img src="/captcha/securimage_show.php"><form method="POST"><input name="answer"></form></html>`;
  const citations = makePdf([
    "References",
    `[1] A. Author. ${found}. J. Geom. Mech., 12:1-20, 2019.`,
    `[2] B. Author. ${unknown}. Ann. Math., 7:3-9, 2018.`,
    `[3] C. Author. ${wrong}. Proc. AMS, 4:5-6, 2017.`,
    "[4] D. Author. Paper known by its identifier. doi:10.1000/known.",
  ]);
  const flows = makePdf([found, "A. Author"]);
  const pdf = join(root, "citations.pdf");
  writeFileSync(pdf, citations, "latin1");
  const hits: string[] = [];
  const posts: { path: string; request: string; agent: string }[] = [];
  let base = "";
  // the real home page links its own PDF and torrents: neither is the searched article
  const home = `<html><form method="POST" action="/"><textarea name="request"></textarea></form><a href="/docs/SCI.pdf">about</a><a href="/scimag/">torrents</a></html>`;
  const server = Bun.serve({
    hostname: "127.0.0.1",
    port: 0,
    async fetch(req) {
      const u = new URL(req.url);
      const path = decodeURIComponent(u.pathname);
      hits.push(`${req.method} ${path}${u.search}`);
      if (req.method === "POST") {
        // PHP reads a form only with this content type
        const form = req.headers.get("content-type") === "application/x-www-form-urlencoded";
        const request = form ? String(new URLSearchParams(await req.text()).get("request") ?? "") : "";
        posts.push({ path, request, agent: req.headers.get("user-agent") ?? "" });
        if (path !== "/sci/") return new Response("missing", { status: 404 });
        if (request === found) return Response.redirect(`${base}/sci/10.1000/flows`, 302);
        if (request === wrong) return Response.redirect(`${base}/sci/10.1000/other`, 302);
        if (request === guarded) return Response.redirect(`${base}/sci/match/Title+behind`, 302);
        return new Response(home);
      }
      if (path === "/sci/10.1000/flows") return new Response(`<embed src="/flows.pdf#view=FitH">`);
      if (path === "/sci/10.1000/other") return new Response(`<iframe src="/other.pdf"></iframe>`);
      if (path === "/sci/10.1000/known") return new Response(`<iframe src="/known.pdf"></iframe>`);
      if (path === "/sci/match/Title+behind" || path === "/sci/10.1000/guarded") return new Response(robot);
      if (path === "/flows.pdf") return new Response(Buffer.from(flows, "latin1"));
      if (path === "/other.pdf") return new Response(Buffer.from(makePdf(["Unrelated research paper"]), "latin1"));
      if (path === "/known.pdf") return new Response(Buffer.from(makePdf(["Paper known by its identifier"]), "latin1"));
      if (path === "/docs/SCI.pdf") return new Response(Buffer.from(makePdf([unknown]), "latin1"));
      if (path === "/api") return new Response(`<feed xmlns="http://www.w3.org/2005/Atom"></feed>`);
      if (path === "/metadata") return Response.json({ message: { items: [] } });
      if (path === "/web") return new Response("<html>no results</html>");
      return new Response("missing", { status: 404 });
    },
  });
  base = `http://127.0.0.1:${server.port}`;
  // a mirror that moved to another host keeps the path: sci-hub.ru/ -> sci-hub.vn/
  const moved = Bun.serve({
    hostname: "127.0.0.1",
    port: 0,
    fetch(req) {
      posts.push({ path: "moved " + new URL(req.url).pathname, request: "", agent: req.method });
      return Response.redirect(`${base}/sci/`, 302);
    },
  });
  writeFileSync(
    join(root, "SumatraPDF-settings.txt"),
    `CheckForUpdates = false\nUiLanguage = en\nResearchSciHubUrl = ${base}/sci/\n`,
  );
  try {
    await withControlledSumatra(
      EXE,
      async (client) => {
        const probe = async (action: string, a = "", b = "", c = "") => {
          const r = await client.request(ControlCommand.TestResearchTools, [action, a, b, c]);
          return { ok: r[0] === 0, text: String(r[1]) };
        };

        // manual lookup: identifiers stay GET paths, anything else is a title POST
        const requestOf = async (query: string) => (await probe("scihub-url", `${base}/sci/`, query)).text;
        const expected: [string, string][] = [
          ["10.1000/known", `GET ${base}/sci/10.1000%2Fknown`],
          ["PMID: 12345678", `GET ${base}/sci/12345678`],
          ["https://doi.org/10.1000/known", `GET ${base}/sci/https%3A%2F%2Fdoi.org%2F10.1000%2Fknown`],
          [found, `POST ${base}/sci/ request=Hamiltonian%20flows%20on%20Lie%20groups`],
        ];
        for (const [query, want] of expected) {
          const got = await requestOf(query);
          if (got !== want) throw new Error(`Sci-Hub request for ${query}: ${got}, want ${want}`);
        }

        // manual fetch by title, through a mirror that moved (the redirect must keep the POST)
        const manual = join(root, "manual.pdf");
        const fetched = await probe("scihub-fetch", `http://127.0.0.1:${moved.port}/sci/`, found, manual);
        if (!fetched.ok || !existsSync(manual) || readFileSync(manual).toString("latin1") !== flows)
          throw new Error(`Title fetch failed: ${fetched.text}; ${hits}`);
        if (
          !posts.some((p) => p.path === "moved /sci/" && p.agent === "POST") ||
          !posts.some((p) => p.path === "/sci/" && p.request === found)
        )
          throw new Error(`Moved mirror lost the title POST: ${JSON.stringify(posts)}`);
        const missing = join(root, "missing.pdf");
        const notFound = await probe("scihub-fetch", `${base}/sci/`, unknown, missing);
        if (
          notFound.ok ||
          existsSync(missing) ||
          !notFound.text.includes("no article") ||
          hits.some((h) => h.includes("SCI.pdf"))
        )
          throw new Error(`Unknown title: ${JSON.stringify(notFound)}`);

        // a CAPTCHA page is reported as such, for a title search and for a DOI
        for (const query of [guarded, "10.1000/guarded"]) {
          const out = join(root, "guarded.pdf");
          const r = await probe("scihub-fetch", `${base}/sci/`, query, out);
          if (r.ok || existsSync(out) || !/CAPTCHA/.test(r.text) || !/Browse/.test(r.text))
            throw new Error(`${query}: CAPTCHA not reported: ${JSON.stringify(r)}`);
        }

        // collection: a reference without DOI goes to Sci-Hub by its title
        await probe("library-open", "collect");
        while (JSON.parse((await probe("library-state")).text).running) await sleep(25);
        const started = await probe(
          "library-start",
          join(root, "pdfs"),
          "",
          JSON.stringify({
            api: base + "/api",
            sci: base + "/sci/",
            anna: "",
            metadata: base + "/metadata",
            web: base + "/web",
          }),
        );
        if (!started.ok) throw new Error(`library-start: ${started.text}`);
        let state: any;
        const deadline = Date.now() + 20000;
        do {
          state = JSON.parse((await probe("library-state")).text);
          if (!state.running) break;
          await sleep(50);
        } while (Date.now() < deadline);
        if (state.running || state.rows.length !== 4) throw new Error(`Collection: ${JSON.stringify(state)}`);
        const sci = (row: any) => row.attempts.filter((a: any) => a.source === "Sci-Hub");
        if (state.rows.some((r: any) => sci(r).some((a: any) => a.status.startsWith("Skipped"))))
          throw new Error(`Sci-Hub skipped a titled reference: ${JSON.stringify(state)}`);
        const [byTitle, notIndexed, mismatch, byDoi] = state.rows;
        if (byTitle.source !== "Sci-Hub" || !existsSync(byTitle.path) || sci(byTitle)[0]?.query !== found)
          throw new Error(`Title lookup did not download: ${JSON.stringify(byTitle)}`);
        if (readFileSync(byTitle.path).toString("latin1") !== flows) throw new Error("Wrong PDF saved for title");
        if (notIndexed.path || !sci(notIndexed)[0]?.error.includes("no article"))
          throw new Error(`Unknown title not reported: ${JSON.stringify(notIndexed)}`);
        if (!notIndexed.attempts.some((a: any) => a.source === "Google"))
          throw new Error("Unknown title did not fall through to Google");
        if (mismatch.path || !sci(mismatch)[0]?.error.includes("verified"))
          throw new Error(`Mismatched Sci-Hub PDF accepted: ${JSON.stringify(mismatch)}`);
        if (byDoi.source !== "Sci-Hub" || sci(byDoi)[0]?.query !== "10.1000/known")
          throw new Error(`DOI lookup changed: ${JSON.stringify(byDoi)}`);
        if (posts.some((p) => p.request.includes("identifier"))) throw new Error("DOI reference used title search");
        await probe("library-close");

        // embedded browser: Search with a title posts the Sci-Hub form
        const before = posts.length;
        const opened = await probe("scihub-browse", found);
        if (!opened.ok) throw new Error(`scihub-browse: ${opened.text}`);
        const browseBy = Date.now() + 15000;
        while (Date.now() < browseBy && !posts.slice(before).some((p) => p.request === found)) await sleep(100);
        if (!posts.slice(before).some((p) => p.request === found && p.path === "/sci/"))
          throw new Error(`Browser did not post the title: ${JSON.stringify(posts.slice(before))}`);
        console.log("PASS Sci-Hub title POST (manual, moved mirror, browser), not-found, PDF title check, DOI first.");
      },
      ["-appdata", root, pdf],
    );
  } finally {
    server.stop();
    moved.stop();
  }
}
if (import.meta.main) await runStandalone(testit);
