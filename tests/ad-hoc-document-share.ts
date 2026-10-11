import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, assemblePdf, cmdId, runStandalone, tmpPath } from "./util";
import {
  enumWindows,
  enumChildWindows,
  getWindowPid,
  getWindowText,
  getClassName,
  getControlText,
  isWindowVisible,
  postMessage,
  sleep,
  WM_COMMAND,
} from "./winapi";

function check(ok: unknown, message: string): asserts ok {
  if (!ok) throw new Error(message);
}

async function waitFor(what: string, fn: () => number | boolean): Promise<number | boolean> {
  const deadline = Date.now() + 10000;
  while (Date.now() < deadline) {
    const result = fn();
    if (result) return result;
    await sleep(30);
  }
  throw new Error(`Timed out: ${what}`);
}

function dialog(pid: number, title: string): number {
  let found = 0;
  enumWindows((hwnd) => {
    if (getWindowPid(hwnd) === pid && getWindowText(hwnd) === title && isWindowVisible(hwnd)) found = hwnd;
    return !found;
  });
  return found;
}

export async function testit(): Promise<void> {
  const scratch = tmpPath(`document-share-${Date.now()}`);
  mkdirSync(scratch, { recursive: true });
  const clientJson = join(scratch, "client.json");
  const invalidJson = join(scratch, "web-client.json");
  const clientId = "offline-test.apps.googleusercontent.com";
  writeFileSync(
    clientJson,
    JSON.stringify({ installed: { client_id: clientId, client_secret: "offline-client-secret" } }),
  );
  writeFileSync(invalidJson, JSON.stringify({ web: { client_id: clientId } }));
  const stream = "BT /F1 18 Tf 30 100 Td (Document sharing test) Tj ET";
  const pdf = join(scratch, "share.pdf");
  writeFileSync(
    pdf,
    assemblePdf([
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 180] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
      `<< /Length ${stream.length} >>\nstream\n${stream}\nendstream`,
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]),
    "latin1",
  );
  const upload = join(scratch, 'tài liệu "quoted".bin'.replaceAll('"', "'"));
  const payload = Buffer.alloc(17 * 1024 * 1024 + 123);
  for (let i = 0; i < payload.length; i++) payload[i] = i % 251;
  writeFileSync(upload, payload);
  const appdata = join(scratch, "settings");
  mkdirSync(appdata);
  writeFileSync(join(appdata, "SumatraPDF-settings.txt"), "UiLanguage = en\nCheckForUpdates = false\n");

  let auth: URL | undefined;
  let refreshes = 0,
    exchanges = 0,
    uploads = 0,
    offset = 0;
  let redirectAttack = false,
    badAck = false,
    revoked = false;
  const chunks: Buffer[] = [];
  const serverErrors: string[] = [];
  const refreshToken = "offline-refresh-never-store-in-plaintext";
  const server = Bun.serve({
    hostname: "127.0.0.1",
    port: 0,
    maxRequestBodySize: 10 * 1024 * 1024,
    async fetch(req) {
      try {
        const url = new URL(req.url);
        if (url.pathname === "/token") {
          const form = new URLSearchParams(await req.text());
          check(form.get("client_id") === clientId, "wrong OAuth client");
          if (form.get("grant_type") === "authorization_code") {
            exchanges++;
            check(form.get("code") === "mock-code+with spaces", "authorization code decode");
            check(form.get("redirect_uri") === auth?.searchParams.get("redirect_uri"), "callback redirect differs");
            const verifier = form.get("code_verifier")!;
            check(verifier.length >= 43 && verifier.length <= 128, "PKCE verifier length");
            check(
              createHash("sha256").update(verifier).digest("base64url") === auth?.searchParams.get("code_challenge"),
              "PKCE mismatch",
            );
          } else {
            refreshes++;
            check(
              form.get("grant_type") === "refresh_token" && form.get("refresh_token") === refreshToken,
              "bad refresh exchange",
            );
            if (revoked) return Response.json({ error: "invalid_grant" }, { status: 400 });
          }
          return Response.json({
            access_token: "mock-access-token",
            refresh_token: refreshToken,
            token_type: "Bearer",
          });
        }
        check(req.headers.get("authorization") === "Bearer mock-access-token", "missing bearer token");
        if (url.pathname === "/upload") {
          const metadata = (await req.json()) as { name: string };
          check(metadata.name === upload.split(/[\\/]/).at(-1), "Unicode filename not preserved");
          check(req.headers.get("x-upload-content-length") === String(payload.length), "wrong upload length");
          uploads++;
          offset = 0;
          chunks.length = 0;
          return new Response(null, {
            status: 200,
            headers: { Location: redirectAttack ? "https://example.invalid/stolen-token" : `${base}/session` },
          });
        }
        check(url.pathname === "/session" && req.method === "PUT", "unexpected network request");
        const chunk = Buffer.from(await req.arrayBuffer());
        check(
          req.headers.get("content-range") === `bytes ${offset}-${offset + chunk.length - 1}/${payload.length}`,
          "bad chunk range",
        );
        chunks.push(chunk);
        offset += chunk.length;
        if (offset < payload.length)
          return new Response(null, { status: 308, headers: { Range: `bytes=0-${badAck ? offset - 2 : offset - 1}` } });
        check(Buffer.concat(chunks).equals(payload), "uploaded bytes differ");
        return Response.json({ id: "mock-file_123" });
      } catch (error) {
        serverErrors.push(String(error));
        return new Response(String(error), { status: 500 });
      }
    },
  });
  const base = `http://127.0.0.1:${server.port}`;
  try {
    await withControlledSumatra(
      EXE,
      async (client, proc) => {
        await client.waitForRenderIdle();
        for (const name of ["CmdGoogleDrive", "CmdSendToZalo"]) {
          const res = await client.request(ControlCommand.TestCommandVisibility, [name]);
          check(res[0] === 0 && String(res[1]).includes("vis=show"), `${name} unavailable with a PDF`);
        }
        const valid = await client.request(ControlCommand.TestDocumentShare, ["client", clientJson]);
        const invalid = await client.request(ControlCommand.TestDocumentShare, ["client", invalidJson]);
        check(valid[0] === 0 && invalid[0] === 1, "desktop vs web OAuth client validation");
        const zalo = await client.request(ControlCommand.TestDocumentShare, ["zalo"]);
        check(zalo[0] === 0 && existsSync(String(zalo[1])), "installed Zalo was not detected");
        const drop = await client.request(ControlCommand.TestDocumentShare, ["drop", upload]);
        check(drop[0] === 0 && drop[1] === upload, "Unicode file clipboard format");

        // Actual menu dispatch and setup cancellation, without launching a browser.
        const frame = Number(
          await waitFor("frame", () => {
            let found = 0;
            enumWindows((hwnd) => {
              if (getWindowPid(hwnd) === proc.pid && getClassName(hwnd) === "SUMATRA_PDF_FRAME") found = hwnd;
              return !found;
            });
            return found;
          }),
        );
        postMessage(frame, WM_COMMAND, cmdId("CmdGoogleDrive"), 0);
        const drive = Number(await waitFor("Drive dialog", () => dialog(proc.pid!, "Google Drive")));
        await waitFor("Drive button", () => {
          let found = 0;
          enumChildWindows(drive, (hwnd) => {
            if (getClassName(hwnd) === "Button" && getControlText(hwnd).startsWith("Sign in and upload")) found = hwnd;
            return !found;
          });
          return found;
        });
        postMessage(drive, 0x0466, 101, 0);
        const setup = Number(await waitFor("OAuth setup", () => dialog(proc.pid!, "Google Drive setup")));
        await waitFor("setup cancel", () => {
          let found = 0;
          enumChildWindows(setup, (hwnd) => {
            if (getClassName(hwnd) === "Button" && getControlText(hwnd) === "Cancel") found = hwnd;
            return !found;
          });
          return found;
        });
        postMessage(setup, 0x0466, 2, 0);
        await waitFor("closed setup", () => !dialog(proc.pid!, "Google Drive setup"));

        const first = client.request(ControlCommand.TestDocumentShare, ["upload", clientJson, upload, base, scratch]);
        const authPath = join(scratch, "auth-url.txt");
        await waitFor("OAuth URL", () => existsSync(authPath) && readFileSync(authPath, "utf8").length > 0);
        auth = new URL(readFileSync(authPath, "utf8"));
        check(auth.searchParams.get("scope") === "https://www.googleapis.com/auth/drive.file", "OAuth scope too broad");
        check(auth.searchParams.get("code_challenge_method") === "S256", "PKCE method");
        const callback = new URL(auth.searchParams.get("redirect_uri")!);
        callback.searchParams.set("state", "wrong-state");
        callback.searchParams.set("code", "wrong-code");
        check((await fetch(callback)).status === 400 && exchanges === 0, "bad state accepted");
        callback.searchParams.set("state", auth.searchParams.get("state")!);
        callback.searchParams.set("code", "mock-code+with spaces");
        check((await fetch(callback)).status === 200, "loopback callback failed");
        const result = await first;
        check(result[0] === 0 && String(result[1]).endsWith("/mock-file_123/view"), `upload failed: ${result}`);
        check(exchanges === 1 && chunks.length === 3 && offset === payload.length, "missing streaming chunks");
        const encrypted = readFileSync(join(scratch, "account.bin"));
        check(
          !encrypted.includes(Buffer.from(refreshToken)) && !encrypted.includes(Buffer.from(clientId)),
          "token stored plaintext",
        );

        const second = await client.request(ControlCommand.TestDocumentShare, [
          "upload",
          clientJson,
          upload,
          base,
          scratch,
        ]);
        check(second[0] === 0 && refreshes === 1 && exchanges === 1, "saved sign-in not reused");
        redirectAttack = true;
        const rejected = await client.request(ControlCommand.TestDocumentShare, [
          "upload",
          clientJson,
          upload,
          base,
          scratch,
        ]);
        check(rejected[0] === 1 && chunks.length === 0, "external upload Location accepted");
        redirectAttack = false;
        badAck = true;
        const badRange = await client.request(ControlCommand.TestDocumentShare, [
          "upload",
          clientJson,
          upload,
          base,
          scratch,
        ]);
        check(badRange[0] === 1 && chunks.length === 1, "invalid resumable offset accepted");
        badAck = false;
        revoked = true;
        const before = uploads;
        const expired = await client.request(ControlCommand.TestDocumentShare, [
          "upload",
          clientJson,
          upload,
          base,
          scratch,
        ]);
        check(
          expired[0] === 1 && uploads === before && String(expired[1]).includes("refresh"),
          "revoked token allowed upload",
        );
        const forget = await client.request(ControlCommand.TestDocumentShare, ["forget", join(scratch, "account.bin")]);
        check(forget[0] === 0 && !existsSync(join(scratch, "account.bin")), "disconnect did not delete token");
        check(serverErrors.length === 0, serverErrors.join("\n"));
      },
      ["-appdata", appdata, pdf],
    );
    console.log(
      "PASS: UI setup/cancel, browser loopback, state, PKCE, encrypted account reuse, 3-chunk integrity, redirect/offset rejection, disconnect, Zalo detection and Unicode attachment. No cloud upload or Zalo message sent.",
    );
  } finally {
    server.stop(true);
  }
}

if (import.meta.main) await runStandalone(testit);
