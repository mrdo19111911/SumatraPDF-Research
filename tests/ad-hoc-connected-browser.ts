import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, assemblePdf, runStandalone, tmpPath } from "./util";
import { enumWindows, getWindowPid, getWindowText, moveWindow, postMessage, sleep } from "./winapi";

export async function testit(): Promise<void> {
  const root = tmpPath(`connected-browser-${Date.now()}`);
  mkdirSync(root, { recursive: true });
  const pdf = join(root, "1904.11429v1.pdf");
  writeFileSync(
    pdf,
    assemblePdf([
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>",
    ]),
    "latin1",
  );
  writeFileSync(join(root, "SumatraPDF-settings.txt"), "CheckForUpdates = false\nUiLanguage = en\n");
  const server = Bun.serve({
    hostname: "127.0.0.1",
    port: 0,
    fetch: () =>
      new Response("<!doctype html><title>Profile fixture</title><p>Local profile test</p>", {
        headers: { "Content-Type": "text/html" },
      }),
  });
  try {
    for (let run = 0; run < 2; run++)
      await withControlledSumatra(
        EXE,
        async (client, proc) => {
          async function check(args: string[]) {
            const result = await client.request(ControlCommand.TestResearchTools, args);
            if (result[0] !== 0) throw new Error(`${args}: ${result}`);
            return String(result[1]);
          }
          await client.request(ControlCommand.TestInvokeCommand, ["CmdAIChatWithClaudeCode"]);
          const url = await client.request(ControlCommand.TestResearchTools, ["browser-url"]);
          if (url[0] !== 0 || url[1] !== "https://www.connectedpapers.com/api/redirect/arxiv/1904.11429")
            throw new Error(`Current paper does not route to its graph: ${url}`);
          for (const [query, expected] of [
            ["https://arxiv.org/pdf/1912.02928v4.pdf", "api/redirect/arxiv/1912.02928"],
            ["arXiv:math/0309136v2", "api/redirect/arxiv/math/0309136"],
            ["arXiv:solv-int/9901001v2", "api/redirect/arxiv/solv-int/9901001"],
            ["10.1142/S0219887819501585", "api/redirect/doi/10.1142%2FS0219887819501585"],
            ["A title & another title", "search?q=A%20title%20%26%20another%20title"],
          ]) {
            const actual = await check(["browser-url", query!]);
            if (actual !== `https://www.connectedpapers.com/${expected}`) throw new Error(`Wrong route: ${actual}`);
          }
          await check(["browser-open", `http://127.0.0.1:${server.port}/`]);
          const findBrowser = () => {
            let found = 0;
            enumWindows((hwnd) => {
              if (getWindowPid(hwnd) === proc.pid && getWindowText(hwnd).startsWith("Connected Papers")) found = hwnd;
              return true;
            });
            return found;
          };
          let deadline = Date.now() + 5000;
          while (!findBrowser() && Date.now() < deadline) await sleep(50);
          if (!findBrowser()) throw new Error("Browser did not open");
          postMessage(findBrowser(), 0x0010, 0, 0);
          while (findBrowser() && Date.now() < deadline) await sleep(50);
          if (findBrowser()) throw new Error("WM_CLOSE leaves Connected Papers open");
          await check(["browser-open", `http://127.0.0.1:${server.port}/`]);
          deadline = Date.now() + 15000;
          let state = "";
          while (Date.now() < deadline) {
            await check([
              "browser-js",
              "window.__sumatra__.notify('probe',{title:document.title,cookie:document.cookie,saved:localStorage.getItem('research-test')})",
            ]);
            state = await check(["browser-state"]);
            if (state.includes('"title":"Profile fixture"')) break;
            await sleep(100);
          }
          if (!state.includes('"title":"Profile fixture"') || !state.includes("separate=1"))
            throw new Error(`Browser not ready: ${state}`);
          if (run === 0)
            await check([
              "browser-js",
              "localStorage.setItem('research-test','persisted');document.cookie='research-test=persisted;max-age=86400;path=/;SameSite=Lax';window.__sumatra__.notify('probe',{saved:localStorage.getItem('research-test')})",
            ]);
          else if (!state.includes('"saved":"persisted"') || !state.includes("research-test=persisted"))
            throw new Error(`Login data lost on restart: ${state}`);
          for (let i = 0; i < 8; i++) moveWindow(findBrowser(), 0, 0, 800 + i * 15, 650 + i * 10);
          postMessage(findBrowser(), run === 0 ? 0x0111 : 0x0010, run === 0 ? 104 : 0, 0);
          deadline = Date.now() + 5000;
          while (findBrowser() && Date.now() < deadline) await sleep(50);
          if (findBrowser() || (await check(["browser-state"])) !== "closed")
            throw new Error("Loaded browser cannot close");
          console.log(
            `PASS: run ${run + 1}, graph routes, close during startup/after resize, persistent cookies and localStorage.`,
          );
        },
        ["-appdata", root, pdf],
      );
  } finally {
    server.stop();
  }
}

if (import.meta.main) await runStandalone(testit);
