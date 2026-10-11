import { mkdirSync, writeFileSync, readFileSync, existsSync, readdirSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, assemblePdf, runStandalone, tmpPath } from "./util";
import { sleep } from "./winapi";

export async function testit(): Promise<void> {
  const root = tmpPath(`library-${Date.now()}`);
  mkdirSync(root, { recursive: true });
  const pdf = join(root, "paper.pdf"),
    output = join(root, "references");
  const lines = [
    "References",
    "[1] A. Author. First matching paper. arXiv:2401.12345v2.",
    "[2] B. Author. Second matching paper. doi:10.1000/second.",
    "References",
    "[3] C. Author. Third matching paper. <unsafe>. doi:10.1000/third.",
    "[4] D. Author. Missing paper. doi:10.1000/missing.",
    "[5] A. Author. First matching paper. arXiv:2401.12345v2.",
    "[6] E. Author. Fourth matching paper.",
  ];
  const makePdf = (textLines: string[]) => {
    const content =
      "BT /F1 12 Tf 50 730 Td " + textLines.map((s, i) => `${i ? "0 -20 Td " : ""}(${s}) Tj`).join(" ") + " ET";
    return assemblePdf([
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
      `<< /Length ${content.length} >>\nstream\n${content}\nendstream`,
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]);
  };
  const payload = makePdf(lines);
  writeFileSync(pdf, payload, "latin1");
  const hits: string[] = [];
  let base = "";
  let hold = false;
  const server = Bun.serve({
    hostname: "127.0.0.1",
    port: 0,
    async fetch(req) {
      const u = new URL(req.url);
      const route = decodeURIComponent(u.pathname);
      hits.push(route + u.search);
      if (route === "/api") {
        if (hold) await sleep(1500);
        const id = u.searchParams.get("id_list");
        const entry = id
          ? `<entry><id>http://arxiv.org/abs/${id}</id><title>First matching paper</title><summary>Abstract &amp; idea</summary><author><name>A. Author</name></author><link title="pdf" href="${base}/first.pdf"/></entry>`
          : `<entry><id>http://arxiv.org/abs/2501.99999</id><title>Wrong unrelated paper</title><link title="pdf" href="${base}/wrong.pdf"/></entry>`;
        return new Response(`<feed xmlns="http://www.w3.org/2005/Atom">${entry}</feed>`);
      }
      if (route === "/sci/10.1000/second")
        return new Response(
          `<!doctype html><html><head><script>if (1 < 2) { const markup = '<span>text</span>'; }</script></head><body><object data="/second.pdf#navpanes=0&amp;view=FitH"></object></body></html>`,
        );
      if (route === "/sci/10.1000/fourth") return new Response(`<iframe src="/fourth.pdf"></iframe>`);
      if (
        route === "/web" &&
        u.searchParams.get("q") === "C. Author. Third matching paper. <unsafe>. doi:10.1000/third. pdf"
      )
        return new Response(
          `<a href="${base}/unrelated.pdf">PDF</a><a href="/url?q=${encodeURIComponent(base + "/third.pdf")}">PDF</a>`,
        );
      if (route === "/unrelated.pdf") return new Response(Buffer.from(makePdf(["Unrelated research paper"]), "latin1"));
      if (route === "/metadata")
        return new Response(
          JSON.stringify({
            message: {
              items: [
                { DOI: "10.1000/wrong", title: ["Unrelated mathematics"] },
                { DOI: "10.1000/fourth", title: ["Fourth matching paper"] },
              ],
            },
          }),
        );
      if (route === "/anna-api/search" && u.searchParams.get("q") === "10.1000/third")
        return new Response(
          JSON.stringify([
            { title: "Unrelated book", path: "/md5/wrong", file_info: { extension: "pdf" } },
            { title: "Third matching paper", path: "/md5/third", file_info: { extension: "pdf" } },
          ]),
        );
      if (route === "/anna-api/download" && u.searchParams.get("path") === "/md5/third")
        return new Response(
          JSON.stringify({
            title: "Third matching paper",
            publisher: "doi:10.1000/third",
            file_info: { extension: "pdf" },
            download_links: [
              { title: "Unavailable", url: base + "/unavailable" },
              { title: "PDF", url: base + "/third.pdf" },
            ],
          }),
        );
      if (route === "/anna-api/search" && u.searchParams.get("q") === "10.1000/ambiguous")
        return new Response(
          JSON.stringify([
            { title: "Third matching paper", path: "/md5/edition1", file_info: { extension: "pdf" } },
            { title: "Third matching paper", path: "/md5/edition2", file_info: { extension: "pdf" } },
          ]),
        );
      if (route === "/anna/search")
        return new Response(
          `<!--<a class="js-vim-focus" href="/md5/html"><div class="text-xs">English [en], pdf, 1MB</div><h3>Third matching paper</h3></a>-->`,
        );
      if (route === "/md5/html")
        return new Response(
          `<div class="text-3xl font-bold">🔍 Third matching paper</div><div class="text-sm text-gray-500">English [en], pdf, 1MB</div><div>doi:10.1000/third</div><a class="js-download-link" href="/third.pdf">Slow download</a>`,
        );
      if (["/first.pdf", "/second.pdf", "/third.pdf", "/fourth.pdf", "/wrong.pdf"].includes(route))
        return new Response(Buffer.from(payload, "latin1"));
      return new Response("CAPTCHA / not found", { status: 404 });
    },
  });
  base = `http://127.0.0.1:${server.port}`;
  writeFileSync(join(root, "SumatraPDF-settings.txt"), "CheckForUpdates = false\nUiLanguage = en\n");
  try {
    await withControlledSumatra(
      EXE,
      async (client) => {
        async function probe(action: string, a = "", b = "", c = "") {
          const r = await client.request(ControlCommand.TestResearchTools, [action, a, b, c]);
          if (r[0] !== 0) throw new Error(`${action}: ${r}`);
          return String(r[1]);
        }
        const extracted = JSON.parse(await probe("library-extract", pdf));
        if (extracted.length !== 5 || extracted[0].arxiv !== "2401.12345v2" || extracted[1].doi !== "10.1000/second")
          throw new Error(`Reference extraction/dedup failed: ${JSON.stringify(extracted)}`);
        const headless = join(root, "headless.pdf"),
          numbered = join(root, "numbered.pdf");
        writeFileSync(
          headless,
          makePdf([
            "Acknowledgements",
            "[1] A. Author. First paper. 2018. [2] B. Author. Second paper. 2019.",
            "[3] C. Author. Third paper. 2020.",
          ]),
          "latin1",
        );
        writeFileSync(
          numbered,
          makePdf([
            "Proof",
            "[1] First claim follows directly.",
            "[2] Second claim follows directly.",
            "[3] Third claim follows directly.",
          ]),
          "latin1",
        );
        if (JSON.parse(await probe("library-extract", headless)).length !== 3)
          throw new Error("Headless bibliography not detected");
        if (JSON.parse(await probe("library-extract", numbered)).length !== 0)
          throw new Error("Numbered proof mistaken for bibliography");
        const symbols = join(root, "symbols.pdf");
        writeFileSync(
          symbols,
          makePdf(["References", "[1] A. Author. First paper. 2018.", "*", "[2] B. Author. Second paper. 2019."]),
          "latin1",
        );
        if (JSON.parse(await probe("library-extract", symbols)).length !== 2)
          throw new Error("Symbol-only line in bibliography");
        const extractRaw = async (name: string, textLines: string[]) => {
          const file = join(root, name);
          writeFileSync(file, makePdf(textLines), "latin1");
          // these fixtures join entries on one line, which only the rule-based fallback sees
          return JSON.parse(await probe("library-extract", file, "rules")).map(
            (r: { raw: string }) => r.raw,
          ) as string[];
        };
        // Author-year: wrapped lines, references joined on one line, appendix after the list.
        const authorYear = await extractRaw("author-year.pdf", [
          "References",
          "Betancourt, M.",
          "The fundamental incompatibility of scalable HMC. In Proc. ICML, pp. 533-540, 2015.",
          "de Leon, M. and Rodrigues, P. R. Methods of Differential Geometry, volume 158",
          "of Studies. North-Holland, 1989. Hairer, E., Lubich, C., and Wanner, G. Geometric",
          "Numerical Integration. Springer, New York, 2006.",
          "Leimkuhler, B. and Reich, S. Simulating Hamiltonian Dynamics. Cambridge University Press,",
          "2004.",
          "A. GEOMETRIC CONSTRUCTION OF THE DYNAMICS",
          "Symplectic integrators are applicable to Hamiltonian dynamics over any manifold.",
        ]);
        const authorYearTitles = JSON.parse(await probe("library-extract", join(root, "author-year.pdf"), "rules")).map(
          (r: { title: string }) => r.title,
        );
        if (
          authorYearTitles[0] !== "The fundamental incompatibility of scalable HMC" ||
          authorYearTitles[2] !== "Geometric Numerical Integration"
        )
          throw new Error(`Author-year titles: ${JSON.stringify(authorYearTitles)}`);
        const starts = authorYear.map((r) => r.split(",")[0]).join("|");
        if (starts !== "Betancourt|de Leon|Hairer|Leimkuhler" || !authorYear[3]!.endsWith("2004."))
          throw new Error(`Author-year bibliography: ${JSON.stringify(authorYear)}`);
        const apa = await extractRaw("apa.pdf", [
          "References",
          "Bishop, C. M. 2006.",
          "Pattern Recognition and Machine Learning. Springer, New York.",
          "Geyer, C. J. 1992.",
          "Practical Markov Chain Monte Carlo. Statistical Science 473-483.",
          "Neal, R. M. 1994. An Improved Acceptance Procedure. Journal of Physics 111 194-203.",
        ]);
        if (apa.length !== 3) throw new Error(`Year-first author-year bibliography: ${JSON.stringify(apa)}`);
        const fullNames = await extractRaw("full-names.pdf", [
          "References",
          "Ralph Abraham and Jerrold E. Marsden. Foundations of Mechanics. AMS Chelsea Pub-",
          "lishing, Redwood City, CA, 1978.",
          "P.-A. Absil, R. Mahony, and R. Sepulchre. Optimization algorithms on matrix manifolds.",
          "Princeton University Press, Princeton, NJ, 2008.",
          "Jorge Nocedal and Stephen J. Wright. Numerical optimization. Springer, 2006.",
        ]);
        if (fullNames.length !== 3) throw new Error(`Full-name bibliography: ${JSON.stringify(fullNames)}`);
        const ownLine = await extractRaw("label-lines.pdf", [
          "References",
          "[1]",
          "A. Author. First paper. 2018.",
          "[2]",
          "B. Author. Second paper. 2019.",
        ]);
        if (ownLine.length !== 2 || !ownLine[1]!.startsWith("B. Author"))
          throw new Error(`Labels on their own lines: ${JSON.stringify(ownLine)}`);
        if (process.argv.includes("--live-fetch")) {
          const url = process.env.RESEARCH_LIVE_FETCH_URL;
          if (!url) throw new Error("Set RESEARCH_LIVE_FETCH_URL");
          const target = join(root, "mirror-live.pdf");
          if (process.env.RESEARCH_NATIVE_HTML) {
            const html = process.env.RESEARCH_NATIVE_HTML;
            const response = await probe("library-fetch-text", url, html);
            console.log(response);
            const finalUrl = response.split(" url=")[1] ?? url;
            console.log(`Native PDF link: ${await probe("resolve", finalUrl, readFileSync(html, "utf8"))}`);
          }
          await probe("fetch", url, target);
          if (!existsSync(target)) throw new Error("Mirror did not produce PDF");
          console.log(`PASS native mirror download, parsed PDF: ${url}; ${readFileSync(target).length} bytes`);
          return;
        }
        if (process.argv.includes("--anna-live")) {
          const endpoint = process.env.RESEARCH_ANNA_URL ?? "https://annas-archive.gl/";
          const result = await client.request(ControlCommand.TestResearchTools, [
            "library-anna-find",
            "M. F. Atiyah and I. G. Macdonald. Introduction to Commutative Algebra. 1969.",
            endpoint,
            process.env.RESEARCH_ANNA_API ?? "",
          ]);
          console.log(`Real Anna lookup for Atiyah–Macdonald: ${JSON.stringify(result)}`);
          if (result[0] !== 0) throw new Error(`No book downloaded: ${result[1]}`);
          const target = join(root, "Atiyah-Macdonald.pdf");
          await probe("fetch", String(result[1]), target);
          console.log(`PASS real book PDF: ${target}; ${readFileSync(target).length} bytes`);
          return;
        }
        if (process.argv.includes("--real-extract")) {
          for (const source of (process.env.RESEARCH_REFERENCE_PDFS ?? "").split("|").filter(Boolean)) {
            const refs = JSON.parse(await probe("library-extract", source));
            writeFileSync(
              join(root, source.includes("1912") ? "native-refs-1912.json" : "native-refs-paper2.json"),
              JSON.stringify(refs, null, 2),
            );
            await probe(
              "extract",
              source,
              join(root, source.includes("1912") ? "native-text-1912.txt" : "native-text-paper2.txt"),
            );
            if (refs.length < 4) throw new Error(`Too few references: ${source}: ${refs.length}`);
            if (source.includes("1912.02928") && refs.length !== 40)
              throw new Error(`Merged references: expected 40, got ${refs.length}`);
            if (source.endsWith("paper2.pdf") && refs.length !== 51)
              throw new Error(`Expected 51 references, got ${refs.length}`);
            console.log(`PASS real bibliography ${source}: ${refs.length} entries; sample: ${refs[0].title}`);
          }
          return;
        }
        if (process.argv.includes("--live")) {
          await probe("library-open", "search");
          await probe("library-search", "id:1912.02928", "", "https://export.arxiv.org/api/query");
          const until = Date.now() + 150000;
          let live: any;
          do {
            live = JSON.parse(await probe("library-state"));
            if (!live.running) break;
            await sleep(100);
          } while (Date.now() < until);
          if (live.running || live.rows.length !== 1 || !live.rows[0].title.includes("Bregman"))
            throw new Error(`Real arXiv metadata unavailable: ${JSON.stringify(live)}`);
          await probe("fetch", live.rows[0].url, join(root, "real-arxiv.pdf"));
          await probe("library-close");
          console.log(`PASS real arXiv metadata and native PDF download: ${live.rows[0].title}`);
          return;
        }
        await probe("library-open", "collect");
        while (JSON.parse(await probe("library-state")).running) await sleep(25);
        mkdirSync(output, { recursive: true });
        writeFileSync(join(output, "collection.json"), "existing user report");
        await probe(
          "library-start",
          output,
          "",
          JSON.stringify({
            api: base + "/api",
            sci: base + "/sci/",
            anna: base + "/anna/",
            annaApi: base + "/anna-api/",
            metadata: base + "/metadata",
            web: base + "/web",
          }),
        );
        const deadline = Date.now() + 20000;
        let state: any;
        do {
          state = JSON.parse(await probe("library-state"));
          if (!state.running) break;
          await sleep(50);
        } while (Date.now() < deadline);
        if (state.running || state.rows.length !== 5)
          throw new Error(`Worker did not finish: ${JSON.stringify(state)}`);
        if (state.processed !== 5 || state.total !== 5)
          throw new Error(`Collection progress incorrect: ${JSON.stringify(state)}`);
        if (state.rows.map((r: any) => r.source).join(",") !== "arXiv,Sci-Hub,Google,,Sci-Hub")
          throw new Error(`Fallback/source order: ${JSON.stringify(state)}`);
        const missing = state.rows[3];
        if (
          !missing.attempts?.some((a: any) => a.source === "arXiv" && a.status === "No exact match") ||
          !missing.attempts?.some((a: any) => a.source === "Sci-Hub" && a.error.includes("404")) ||
          !missing.attempts?.some((a: any) => a.source === "Google" && a.error.includes("404"))
        )
          throw new Error(`Provider failures were lost: ${JSON.stringify(missing)}`);
        if (!state.rows[0].attempts?.some((a: any) => a.source === "arXiv" && a.status === "Downloaded"))
          throw new Error("Successful provider attempt missing");
        if (!state.rows[2].attempts.some((a: any) => a.source === "Google" && a.error.includes("rejected")))
          throw new Error("Unrelated Google PDF was not rejected");
        if (!state.report || !existsSync(state.report)) throw new Error("No generated HTML report");
        const html = readFileSync(state.report, "utf8");
        if (!html.includes("&lt;unsafe&gt;") || html.includes("<unsafe>") || !html.includes("HTTP 404"))
          throw new Error("HTML report lost diagnostics or unescaped citation");
        const uiUntil = Date.now() + 20000;
        let ui: any;
        do {
          const result = await probe("library-ui");
          ui = result ? JSON.parse(result)[0] : null;
          if (
            ui?.rows === 5 &&
            ui.heading.includes("Collect") &&
            ui.body.includes("5 / 5 references processed; 4 PDFs downloaded")
          )
            break;
          await sleep(50);
        } while (Date.now() < uiUntil);
        if (
          ui?.rows !== 5 ||
          !ui.buttons.includes("Collect all the papers") ||
          !ui.body.includes("Downloaded") ||
          !ui.body.includes("5 / 5 references processed; 4 PDFs downloaded")
        )
          throw new Error(`Library UI missing: ${JSON.stringify(ui)}; ${await probe("library-web-state")}`);
        const requests = hits.filter((h) => h.startsWith("/sci/") || h.startsWith("/web"));
        if (
          requests.findIndex((h) => h.startsWith("/sci/10.1000/third")) >=
          requests.findIndex(
            (h) =>
              new URL(h, base).searchParams.get("q") ===
              "C. Author. Third matching paper. <unsafe>. doi:10.1000/third. pdf",
          )
        )
          throw new Error(`Fallback ordering wrong: ${requests}`);
        if (hits.some((h) => h.startsWith("/wrong.pdf"))) throw new Error("Downloaded unrelated paper");
        if (
          readdirSync(output).filter((f) => f.endsWith(".pdf")).length !== 4 ||
          !existsSync(join(output, "collection.json"))
        )
          throw new Error("Missing PDFs / manifest");
        if (readFileSync(join(output, "collection.json"), "utf8") !== "existing user report")
          throw new Error("Existing report overwritten");
        for (const r of state.rows.filter((r: any) => r.path))
          if (readFileSync(r.path).toString("latin1") !== payload) throw new Error("Bad saved PDF");
        const manifestName = readdirSync(output).find((f) => f.endsWith(".json") && f !== "collection.json");
        if (!manifestName) throw new Error("No separate report created");
        const manifest = JSON.parse(readFileSync(join(output, manifestName), "utf8"));
        if (manifest.running || manifest.rows.filter((r: any) => r.path).length !== 4)
          throw new Error("Incomplete manifest");
        const firstHits = hits.length;
        await probe(
          "library-start",
          output,
          "",
          JSON.stringify({
            api: base + "/api",
            sci: base + "/sci/",
            anna: base + "/anna/",
            annaApi: base + "/anna-api/",
            metadata: base + "/metadata",
            web: base + "/web",
          }),
        );
        while (JSON.parse(await probe("library-state")).running) await sleep(50);
        if (hits.slice(firstHits).some((h) => h.endsWith(".pdf"))) throw new Error("Completed PDFs fetched again");
        await probe(
          "library-ui",
          "(function edit(){const b=document.getElementById('applyBtn');if(b.disabled)return setTimeout(edit,25);document.getElementById('citations').value='Edited reference one.\\nEdited reference two.';b.click()})()",
        );
        const editUntil = Date.now() + 4000;
        while (JSON.parse(await probe("library-state")).rows.length !== 2 && Date.now() < editUntil) await sleep(50);
        if (JSON.parse(await probe("library-state")).rows.length !== 2)
          throw new Error("Edit references button did not dispatch");
        await probe("library-close");
        await probe("library-open", "search");
        await probe("library-search", "id:2401.12345v2", "", base + "/api");
        do {
          state = JSON.parse(await probe("library-state"));
          if (!state.running) break;
          await sleep(50);
        } while (Date.now() < deadline);
        if (state.rows[0]?.title !== "First matching paper" || !state.rows[0].abstract.includes("&"))
          throw new Error("API result parse failed");
        const searchUiUntil = Date.now() + 10000;
        do {
          const result = await probe("library-ui");
          ui = result ? JSON.parse(result)[0] : null;
          if (ui?.rows === 1 && ui.heading.includes("search")) break;
          await sleep(50);
        } while (Date.now() < searchUiUntil);
        if (ui?.rows !== 1 || !ui.buttons.includes("Download")) throw new Error("Search result UI missing");
        const singleFolder = join(root, "single-download");
        await probe(
          "library-ui",
          `(function download(){const b=[...document.querySelectorAll('article button')].find(b=>b.textContent==='Download');if(b.disabled)return setTimeout(download,25);document.getElementById('folder').value=${JSON.stringify(singleFolder)};b.click()})()`,
        );
        const downloadUntil = Date.now() + 6000;
        do {
          state = JSON.parse(await probe("library-state"));
          if (state.rows[0]?.path && !state.running) break;
          await sleep(50);
        } while (Date.now() < downloadUntil);
        if (!state.rows[0]?.path || !existsSync(state.rows[0].path))
          throw new Error("Download button did not save PDF");
        await probe("library-close");
        hold = true;
        await probe("library-open", "collect");
        while (JSON.parse(await probe("library-state")).running) await sleep(25);
        const beforeClose = hits.length,
          cancelledFolder = join(root, "cancelled");
        await probe(
          "library-start",
          cancelledFolder,
          "",
          JSON.stringify({
            api: base + "/api",
            sci: base + "/sci/",
            anna: base + "/anna/",
            annaApi: base + "/anna-api/",
            metadata: base + "/metadata",
            web: base + "/web",
          }),
        );
        while (hits.length === beforeClose) await sleep(10);
        await probe("library-close");
        await sleep(1800);
        const ping = await client.request(ControlCommand.Ping);
        if (
          ping[0] !== "pong" ||
          (existsSync(cancelledFolder) && readdirSync(cancelledFolder).some((f) => f.endsWith(".pdf")))
        )
          throw new Error(
            `Closing collection did not cancel safely: ping=${JSON.stringify(ping)} files=${existsSync(cancelledFolder) ? readdirSync(cancelledFolder) : []}`,
          );
        hold = false;
        const htmlLink = await probe("library-anna-find", "Third matching paper. doi:10.1000/third.", base + "/anna/");
        if (htmlLink !== base + "/third.pdf") throw new Error(`Wrong Anna HTML link: ${htmlLink}`);
        const wrongDoi = await client.request(ControlCommand.TestResearchTools, [
          "library-anna-find",
          "Third matching paper. doi:10.1000/wrong.",
          base + "/anna/",
          "",
        ]);
        if (wrongDoi[0] === 0) throw new Error("Anna downloaded a mismatched DOI");
        const ambiguous = await client.request(ControlCommand.TestResearchTools, [
          "library-anna-find",
          "Third matching paper. doi:10.1000/ambiguous.",
          base + "/anna/",
          base + "/anna-api/",
        ]);
        if (ambiguous[0] === 0 || !String(ambiguous[1]).includes("Multiple"))
          throw new Error("Ambiguous Anna editions accepted");
        console.log(
          "PASS bibliography dedup, arXiv/Sci-Hub/Google order, PDF title verification, HTML report, manual Anna adapters, retry and cancellation.",
        );
      },
      ["-appdata", root, pdf],
    );
  } finally {
    server.stop();
  }
}
if (import.meta.main) await runStandalone(testit);
