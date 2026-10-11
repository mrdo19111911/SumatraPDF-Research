// Check real WebView2 rendering through a CDP connection to the throwaway app.
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { chromium } from "../.work/markdown-toolchain/node_modules/playwright";
import { EXE, ROOT, runStandalone, tmpPath } from "./util";
import { withControlledSumatra } from "./control";

async function check(label: string, markdown: string, full: boolean): Promise<object> {
  const data = tmpPath(`markdown-${label}-${Date.now()}`);
  mkdirSync(data, { recursive: true });
  const file = join(data, `${label}.md`);
  writeFileSync(file, markdown);
  writeFileSync(
    join(data, "SumatraPDF-settings.txt"),
    "CheckForUpdates = false\nShowStartPage = false\nMarkdownUI [\n UseFixedPageUI = false\n]\n",
  );
  const socket = Bun.serve({ hostname: "127.0.0.1", port: 0, fetch: () => new Response() });
  const port = socket.port;
  socket.stop(true);
  return withControlledSumatra(
    EXE,
    async (_client, proc) => {
      const endpoint = `http://127.0.0.1:${port}`;
      const deadline = Date.now() + 30000;
      while (true) {
        try {
          if ((await fetch(endpoint + "/json/version")).ok) break;
        } catch {}
        if (Date.now() > deadline) throw new Error("WebView2 CDP endpoint did not start");
        await Bun.sleep(100);
      }
      const browser = await chromium.connectOverCDP(endpoint);
      try {
        let page;
        while (!page && Date.now() < deadline) {
          page = browser
            .contexts()
            .flatMap((context) => context.pages())
            .find((p) => p.url().startsWith("https://sumatrapdf.markdown/") && p.url().includes(label));
          if (!page) await Bun.sleep(100);
        }
        if (!page) throw new Error("Markdown browser page not found");
        await page.waitForFunction(() => (window as any).__sumatraMarkdownStats?.done, { timeout: 60000 });
        const result = await page.evaluate(() => {
          const stats = (window as any).__sumatraMarkdownStats;
          return {
            ...stats,
            math: document.querySelectorAll(".katex").length,
            mathErrors: document.querySelectorAll(".katex-error").length,
            highlighted: document.querySelectorAll(".hljs-keyword").length,
            notes: document.querySelectorAll(".sumatra-note").length,
            footnotes: document.querySelectorAll("[data-footnote-ref],.footnote-ref").length,
            mermaid: document.querySelectorAll("pre.mermaid svg").length,
            diagrams: [...document.querySelectorAll(".sumatra-diagram[data-renderer]")].map((node) =>
              node.getAttribute("data-renderer"),
            ),
            resources: performance.getEntriesByType("resource").map((entry) => entry.name),
          };
        });
        const memory = Bun.spawnSync([
          "powershell",
          "-NoProfile",
          "-Command",
          `$all = @(Get-CimInstance Win32_Process); $ids = @(${proc.pid}); do { $children = @($all | Where-Object { $_.ParentProcessId -in $ids -and $_.ProcessId -notin $ids } | ForEach-Object ProcessId); $ids += $children } while ($children.Count); $measure = Get-Process -Id $ids -ErrorAction SilentlyContinue | Measure-Object PrivateMemorySize64 -Sum; [math]::Round($measure.Sum / 1MB, 1)`,
        ]);
        const privateMemoryMB = Number(memory.stdout.toString().trim());
        if (memory.exitCode || !privateMemoryMB) throw new Error("Could not measure isolated app memory");
        if (result.errors.length) throw new Error(JSON.stringify(result));
        if (full) {
          if (
            result.math < 3 ||
            result.mathErrors ||
            !result.highlighted ||
            result.notes !== 2 ||
            !result.footnotes ||
            result.mermaid !== 1
          )
            throw new Error("Basic Markdown extensions missing: " + JSON.stringify(result));
          const expected = ["dot", "plantuml", "plantuml", "vega-lite", "vega", "plotly"];
          if (result.diagrams.join() !== expected.join())
            throw new Error("Missing diagrams: " + JSON.stringify(result));
          if (result.resources.some((url: string) => !url.startsWith("https://sumatrapdf.markdown/")))
            throw new Error("An enhancement fetched an external resource: " + JSON.stringify(result.resources));
          await page.screenshot({ path: join(ROOT, "out/markdown-demo.png"), fullPage: true });
        } else if (result.loaded.length)
          throw new Error("Plain Markdown eagerly loaded renderers: " + JSON.stringify(result));
        console.log(`PASS ${label}: ${result.milliseconds.toFixed(1)}ms, loaded=${result.loaded.join(",") || "none"}`);
        return { ...result, privateMemoryMB };
      } finally {
        await browser.close();
      }
    },
    ["-appdata", data, "-window-pos", "950x760@0x0", file],
    {
      env: { ...process.env, WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS: `--remote-debugging-port=${port}` },
    },
  );
}

export async function testit(): Promise<void> {
  const plain = await check("plain", "# Plain Markdown\n\nOrdinary text and **bold**.\n", false);
  const full = await check("enhanced", readFileSync(join(ROOT, "tests/ad-hoc-markdown-demo.md"), "utf8"), true);
  writeFileSync(join(ROOT, "out/markdown-benchmark.json"), JSON.stringify({ plain, full }, null, 2));
}

if (import.meta.main) await runStandalone(testit);
