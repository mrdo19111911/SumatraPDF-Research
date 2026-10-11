// Build pinned, locally served Markdown assets. Runtime rendering needs no CDN.
import { cpSync, mkdirSync, readFileSync, readdirSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";

const root = resolve(import.meta.dir, "..");
const tools = join(root, ".work/markdown-toolchain");
mkdirSync(tools, { recursive: true });
const packages = {
  katex: "0.19.0",
  "@plantuml/core": "1.2026.8",
  "highlight.js": "11.12.0",
  vega: "6.4.0",
  "vega-lite": "6.5.0",
  "vega-embed": "7.3.0",
  "plotly.js-dist-min": "4.1.2",
};
const install = Bun.spawn([process.execPath, "add", ...Object.entries(packages).map(([p, v]) => `${p}@${v}`)], {
  cwd: tools,
  stdout: "inherit",
  stderr: "inherit",
});
if (await install.exited) throw new Error("Markdown dependency install failed");
const assets = join(root, "resources/markdown");
mkdirSync(assets, { recursive: true });
const modules = join(tools, "node_modules");
const copies: Record<string, string> = {
  "katex.min.js": "katex/dist/katex.min.js",
  "katex.min.css": "katex/dist/katex.min.css",
  "plantuml.js": "@plantuml/core/plantuml.js",
  "viz-global.js": "@plantuml/core/viz-global.js",
  "themes.js": "@plantuml/core/themes.js",
  "emoji.js": "@plantuml/core/emoji.js",
  "openiconic.js": "@plantuml/core/openiconic.js",
  "highlight-light.css": "highlight.js/styles/github.min.css",
  "highlight-dark.css": "highlight.js/styles/github-dark.min.css",
  "vega.min.js": "vega/build/vega.min.js",
  "vega-lite.min.js": "vega-lite/build/vega-lite.min.js",
  "vega-embed.min.js": "vega-embed/build/vega-embed.min.js",
  "plotly.min.js": "plotly.js-dist-min/plotly.min.js",
};
for (const [target, source] of Object.entries(copies)) cpSync(join(modules, source), join(assets, target));
mkdirSync(join(assets, "fonts"), { recursive: true });
for (const font of readdirSync(join(modules, "katex/dist/fonts"))) {
  if (font.endsWith(".woff2")) cpSync(join(modules, "katex/dist/fonts", font), join(assets, "fonts", font));
}
const entry = join(tools, "highlight-entry.ts");
writeFileSync(
  entry,
  `import hljs from "highlight.js/lib/common";
import latex from "highlight.js/lib/languages/latex";
hljs.registerLanguage("latex", latex);
window.hljs = hljs;
`,
);
const bundle = await Bun.build({ entrypoints: [entry], target: "browser", format: "iife", minify: true });
if (!bundle.success) throw new Error("Highlight bundle failed");
await Bun.write(join(assets, "highlight.min.js"), bundle.outputs[0]!);
let notices = "Locally bundled Markdown libraries (pinned versions):\n";
for (const [pkg, version] of Object.entries(packages)) {
  notices += `\n===== ${pkg} ${version} =====\n`;
  const dir = join(modules, pkg);
  const name = readdirSync(dir).find((n) => /^licen[sc]e(?:\.|$)/i.test(n));
  if (name) notices += readFileSync(join(dir, name), "utf8") + "\n";
}
writeFileSync(join(assets, "THIRD-PARTY-NOTICES.txt"), notices);
writeFileSync(join(assets, "versions.json"), JSON.stringify(packages, null, 2) + "\n");
console.log("Prepared resources/markdown (rebuild with bun cmd/prepare-markdown.ts)");
