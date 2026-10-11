import { dlopen, FFIType, ptr } from "bun:ffi";
import { existsSync, mkdirSync, readFileSync, readdirSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { ControlCommand, withControlledSumatra } from "./control";
import { EXE, runStandalone } from "./util";

export async function testit(): Promise<void> {
  const source = readFileSync("src/PaperLibrary.cpp", "utf8");
  const year = source.match(/static int CitationYear\(Str raw\) \{[\s\S]*?\n\}/)?.[0];
  const eligible = source.match(/static bool ArxivEligible\(Str raw, Str id\) \{[\s\S]*?\n\}/)?.[0];
  if (!year || !eligible) throw new Error("Missing historical-citation routing policy");
  const constants = source
    .match(/constexpr int k(?:ArxivFirstYear|EarliestCitationYear|MinCandidateTitle) = \d+;/g)
    ?.join("\n");
  if (!constants) throw new Error("Missing citation year bounds");
  const root = resolve(".work/citation-routing");
  mkdirSync(root, { recursive: true });
  const file = join(root, "policy.cpp");
  const object = join(root, "policy.obj");
  const dll = join(root, `policy-${Date.now()}.dll`);
  writeFileSync(
    file,
    `using u8 = unsigned char;\nstruct Str { const char* s; int len; Str():s(nullptr),len(0){} Str(const char* p,int n):s(p),len(n){} };\nint len(Str s) { return s.len; }\n${constants}\n${year}\n${eligible}\nextern "C" __declspec(dllexport) int route(const char* s, int n, int id) { return ArxivEligible({s,n},{s,id}); }\n`,
  );
  let compiler = process.env.CITATION_MSVC_BIN ? join(process.env.CITATION_MSVC_BIN, "cl.exe") : Bun.which("cl.exe");
  if (!compiler) {
    const tools = join(
      process.env["ProgramFiles(x86)"] || "C:/Program Files (x86)",
      "Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC",
    );
    if (existsSync(tools)) {
      for (const version of readdirSync(tools).sort().reverse()) {
        const file = join(tools, version, "bin/Hostx64/x64/cl.exe");
        if (existsSync(file)) {
          compiler = file;
          break;
        }
      }
    }
  }
  if (!compiler) {
    console.log("SKIP native algorithm checks: install Visual Studio C++ tools or set CITATION_MSVC_BIN");
    return;
  }
  const bin = dirname(compiler);
  for (const command of [
    [join(bin, "cl.exe"), "/nologo", "/c", "/GS-", `/Fo${object}`, file],
    [join(bin, "link.exe"), "/nologo", "/DLL", "/NOENTRY", "/NODEFAULTLIB", `/OUT:${dll}`, object],
  ]) {
    const result = Bun.spawnSync(command, { stdout: "pipe", stderr: "pipe" });
    if (result.exitCode) throw new Error(result.stdout.toString() + result.stderr.toString());
  }
  const lib = dlopen(dll, {
    route: { args: [FFIType.ptr, FFIType.i32, FFIType.i32], returns: FFIType.i32 },
  });
  const cases: [string, number, boolean][] = [
    ["Poussin, Sur la fonction zeta, 59 (1899), 1-74. doi:10.3406/marb.1899.2449.", 0, false],
    ["Vinogradov, A new estimate, Mat. 22 (1958), 161-164.", 0, false],
    ["Weinberger, Exponents, Acta Arith. 22 (1973), 117-124.", 0, false],
    ["Vinogradov, Selected Works, Springer, Berlin, 1985.", 0, false],
    ["Titchmarsh, A divisor problem, 54 (1930), correction, 57 (1933), 478-479.", 0, false],
    ["Author, Contemporary paper, 160 (2004), 781-793.", 0, true],
    ["Author, Paper, 1991.", 0, true],
    ["Author, No publication date. doi:10.1980/example.1234.", 0, true],
    ["Author, Pagination only, 1980-1984.", 0, true],
    ["Author, Old paper, 1930. arXiv:2401.12345.", 10, true],
    ["Author, On a conjecture (1890), Journal (2024).", 0, true],
  ];
  try {
    for (const [raw, id, expected] of cases) {
      const buffer = Buffer.from(raw);
      const actual = Boolean(lib.symbols.route(ptr(buffer), buffer.length, id));
      if (actual !== expected) throw new Error(`Wrong arXiv route: ${raw}; expected ${expected}, got ${actual}`);
    }
  } finally {
    lib.close();
  }
  await withControlledSumatra(EXE, async (client) => {
    const titles: [string, string, number, string][] = [
      ["A. Author, A considerably longer title, Journal, 2019.", ",", 0, "A considerably longer title"],
      [
        "A. Author, Real paper title on metric spaces, A journal name substantially longer than the title, 2019.",
        ",",
        0,
        "Real paper title on metric spaces",
      ],
      [
        "A. Author, Real paper title on metric spaces, A journal name substantially longer than the title, 2019.",
        ",",
        1,
        "",
      ],
      ["Author, Equal title A on geometry, Equal title B on geometry, 2019.", ",", 0, "Equal title A on geometry"],
      ["Author, Equal title A on geometry, Equal title B on geometry, 2019.", ",", 1, "Equal title B on geometry"],
      ["A. Author, PRIMES is in P, doi:10.1000/verylongidentifier.", ",", 0, ""],
      ["Author, 16 : 1950158, 2019.", ",", 0, ""],
      ["A. Author. A paper without commas. Journal of Mathematics.", ",", 0, ""],
      [
        "De Leon M. & Lainz Valcazar M. Singular Lagrangians and precontact Hamiltonian systems. International Journal of Geometric Methods in Modern Physics, 16 : 1950158, 2019.",
        ".",
        0,
        "Singular Lagrangians and precontact Hamiltonian systems",
      ],
    ];
    for (const [raw, separator, rank, expected] of titles) {
      const result = await client.request(ControlCommand.TestResearchTools, [
        "library-title",
        raw,
        separator,
        String(rank),
      ]);
      if (result[0] !== 0) throw new Error(`Title probe failed: ${result}`);
      const actual = String(result[1]);
      if (actual !== expected) throw new Error(`Wrong title rank: ${raw}; expected ${expected}, got ${actual}`);
    }
  });
  console.log(`PASS ${cases.length} native routing cases and application title ranking`);
}

if (import.meta.main) await runStandalone(testit);
