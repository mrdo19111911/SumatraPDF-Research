import { mkdirSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, assemblePdf, runStandalone, tmpPath } from "./util";
import {
  captureWindowToPng,
  enumWindows,
  getWindowPid,
  getWindowText,
  moveWindow,
  setProcessDpiAware,
  sleep,
} from "./winapi";

export async function testit(): Promise<void> {
  setProcessDpiAware();
  const root = tmpPath(`anna-screen-${Date.now()}`);
  mkdirSync(root, { recursive: true });
  writeFileSync(join(root, "SumatraPDF-settings.txt"), "CheckForUpdates = false\nUiLanguage = en\n");
  const pdf = join(root, "book-reference.pdf");
  const stream =
    "BT /F1 12 Tf 40 730 Td (References) Tj 0 -20 Td ([1] M. F. Atiyah and I. G. Macdonald.) Tj 0 -20 Td (Introduction to Commutative Algebra. 1969.) Tj ET";
  writeFileSync(
    pdf,
    assemblePdf([
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
      `<< /Length ${stream.length} >>\nstream\n${stream}\nendstream`,
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]),
    "latin1",
  );
  const server = Bun.serve({
    hostname: "127.0.0.1",
    port: 0,
    fetch() {
      return new Response('<feed xmlns="http://www.w3.org/2005/Atom"/>');
    },
  });
  try {
    await withControlledSumatra(
      EXE,
      async (client, proc) => {
        const probe = async (action: string, a = "", b = "", c = "") => {
          const r = await client.request(ControlCommand.TestResearchTools, [action, a, b, c]);
          if (r[0] !== 0) throw new Error(`${action}: ${r}`);
          return String(r[1]);
        };
        await probe("library-open", "collect");
        const deadline = Date.now() + 20000;
        let ui: any;
        do {
          const result = await probe("library-ui");
          ui = result ? JSON.parse(result)[0] : null;
          if (ui?.heading) break;
          await sleep(50);
        } while (Date.now() < deadline);
        if (!ui?.heading) throw new Error("Library UI unavailable");
        while (JSON.parse(await probe("library-state")).rows.length !== 1 && Date.now() < deadline) await sleep(50);
        await probe(
          "library-start",
          join(root, "downloads"),
          "",
          JSON.stringify({
            api: `http://127.0.0.1:${server.port}/api`,
            sci: "",
            anna: "https://annas-archive.gl/",
            annaApi: "",
            metadata: "",
          }),
        );
        while (JSON.parse(await probe("library-state")).running) await sleep(50);
        const state = JSON.parse(await probe("library-state"));
        console.log(`Native book lookup: ${JSON.stringify(state.rows[0])}`);
        const uiDeadline = Date.now() + 5000;
        do {
          const result = await probe("library-ui");
          ui = result ? JSON.parse(result)[0] : null;
          if (ui?.body.includes(state.rows[0].status)) break;
          await sleep(50);
        } while (Date.now() < uiDeadline);
        let library = 0;
        enumWindows((hwnd) => {
          if (getWindowPid(hwnd) === proc.pid && getWindowText(hwnd).startsWith("Collect references")) library = hwnd;
          return true;
        });
        if (library) {
          moveWindow(library, 0, 0, 1100, 850);
          await sleep(300);
        }
        if (!library || !captureWindowToPng(library, resolve(".work/toolchain/anna-native-error.png")))
          throw new Error("Native error screenshot failed");
        await probe(
          "library-ui",
          "(function browse(){const b=[...document.querySelectorAll('article button')].find(b=>b.textContent===\"Browse Anna's Archive\");if(!b)return setTimeout(browse,50);b.click()})()",
        );
        let browser = 0;
        while (!browser && Date.now() < deadline) {
          enumWindows((hwnd) => {
            if (getWindowPid(hwnd) === proc.pid && getWindowText(hwnd).includes("Research browser")) browser = hwnd;
            return true;
          });
          await sleep(50);
        }
        if (!browser) throw new Error("Research browser unavailable");
        moveWindow(browser, 0, 0, 1280, 850);
        await sleep(12000);
        const target = resolve(".work/toolchain/anna-browser-error.png");
        if (!captureWindowToPng(browser, target)) throw new Error("Screenshot failed");
        console.log(`Actual embedded browser screenshot: ${target}`);
      },
      ["-appdata", root, pdf],
    );
  } finally {
    server.stop();
  }
}
if (import.meta.main) await runStandalone(testit);
