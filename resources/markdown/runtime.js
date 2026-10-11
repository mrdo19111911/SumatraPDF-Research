/* Markdown enhancement bootstrap. Libraries are bundled locally and loaded on demand. */
(function () {
  const base = "https://sumatrapdf.markdown/_sumatra_md/";
  const started = performance.now();
  const stats = (window.__sumatraMarkdownStats = { loaded: [], errors: [], done: false, milliseconds: 0 });
  const scripts = new Map();
  const background = getComputedStyle(document.body).backgroundColor.match(/[\d.]+/g) || [255, 255, 255];
  const dark = +background[0] * 0.2126 + +background[1] * 0.7152 + +background[2] * 0.0722 < 140;
  function script(name) {
    if (!scripts.has(name))
      scripts.set(
        name,
        new Promise((resolve, reject) => {
          const node = document.createElement("script");
          node.src = name === "mermaid.min.js" ? "https://sumatrapdf.markdown/mermaid.min.js" : base + name;
          node.onload = () => {
            stats.loaded.push(name);
            resolve();
          };
          node.onerror = () => reject(new Error("Cannot load " + name));
          document.head.appendChild(node);
        }),
      );
    return scripts.get(name);
  }
  function stylesheet(name) {
    const node = document.createElement("link");
    node.rel = "stylesheet";
    node.href = base + name;
    document.head.appendChild(node);
  }
  const style = document.createElement("style");
  style.textContent = `.sumatra-math.display{display:block;overflow-x:auto;text-align:center;margin:1em 0}
    .sumatra-diagram{display:block!important;overflow:auto;margin:1em 0}.sumatra-diagram svg{max-width:100%;height:auto}
    .sumatra-note{border-left:4px solid #4488cc;padding:.7em 1em;margin:1em 0;background:rgba(100,150,200,.08)}
    .sumatra-note-title{font-weight:600;margin-bottom:.4em}.sumatra-note.warning,.sumatra-note.caution{border-color:#cc8833}
    .footnotes{font-size:.9em;border-top:1px solid #8885;padding-top:1em}.sumatra-error{color:#c54}
    .sumatra-render-source{font-size:.85em;opacity:.8}pre code.hljs{background:transparent;padding:0}`;
  document.head.appendChild(style);
  function fail(node, error) {
    stats.errors.push(String(error));
    const text = document.createElement("div");
    text.className = "sumatra-error";
    text.textContent = "Could not render: " + (error.message || error);
    node.before(text); // Preserve the source when a single diagram is invalid.
  }
  function svgElement(source) {
    const svg = new DOMParser().parseFromString(source, "image/svg+xml").documentElement;
    if (svg.localName !== "svg") throw new Error("Renderer did not return SVG");
    svg.querySelectorAll("script,foreignObject").forEach((node) => node.remove());
    for (const node of [svg, ...svg.querySelectorAll("*")])
      for (const attr of [...node.attributes]) {
        if (/^on/i.test(attr.name) || (/href$/i.test(attr.name) && /^\s*javascript:/i.test(attr.value)))
          node.removeAttribute(attr.name);
      }
    return document.importNode(svg, true);
  }
  function targetFor(pre) {
    const target = document.createElement("div");
    target.className = "sumatra-diagram";
    pre.before(target);
    return target;
  }
  function notes() {
    document.querySelectorAll("blockquote").forEach((node) => {
      const paragraph = node.querySelector(":scope > p");
      const first = paragraph && paragraph.firstChild;
      if (!first || first.nodeType !== Node.TEXT_NODE) return;
      const match = /^\[!(NOTE|TIP|IMPORTANT|WARNING|CAUTION)\]\s*/i.exec(first.textContent);
      if (!match) return;
      const kind = match[1].toLowerCase();
      first.textContent = first.textContent.slice(match[0].length);
      node.classList.add("sumatra-note", kind);
      const title = document.createElement("div");
      title.className = "sumatra-note-title";
      title.textContent = match[1];
      node.prepend(title);
    });
  }
  let graphWorker;
  let graphId = 0;
  const graphRequests = new Map();
  function graph(source) {
    if (!graphWorker) {
      graphWorker = new Worker(base + "graph-worker.js");
      stats.loaded.push("graph-worker.js");
      graphWorker.onmessage = (event) => {
        const pending = graphRequests.get(event.data.id);
        if (!pending) return;
        clearTimeout(pending.timer);
        graphRequests.delete(event.data.id);
        if (event.data.error) pending.reject(new Error(event.data.error));
        else pending.resolve(event.data.svg);
      };
      graphWorker.onerror = (event) => {
        for (const pending of graphRequests.values()) {
          clearTimeout(pending.timer);
          pending.reject(new Error(event.message));
        }
        graphRequests.clear();
      };
    }
    return new Promise((resolve, reject) => {
      const id = ++graphId;
      const timer = setTimeout(() => {
        graphRequests.delete(id);
        reject(new Error("Graphviz render timed out"));
      }, 30000);
      graphRequests.set(id, { resolve, reject, timer });
      graphWorker.postMessage({ id, source });
    });
  }
  let plantFrame,
    plantReady,
    plantQueue = Promise.resolve(),
    plantId = 0;
  const plantRequests = new Map();
  function plant(source) {
    if (!plantFrame) {
      plantFrame = document.createElement("iframe");
      plantFrame.hidden = true;
      plantFrame.src = base + "plantuml-frame.html";
      stats.loaded.push("plantuml-frame.html");
      plantReady = new Promise((resolve) => {
        window.addEventListener("message", (event) => {
          if (event.origin !== location.origin || event.source !== plantFrame.contentWindow) return;
          if (event.data.type === "plantuml-ready") resolve();
          const pending = plantRequests.get(event.data.id);
          if (!pending) return;
          clearTimeout(pending.timer);
          plantRequests.delete(event.data.id);
          if (event.data.error) pending.reject(new Error(event.data.error));
          else pending.resolve(event.data.svg);
        });
      });
      document.body.appendChild(plantFrame);
    }
    const task = plantQueue
      .then(() => plantReady)
      .then(
        () =>
          new Promise((resolve, reject) => {
            const id = ++plantId;
            const timer = setTimeout(() => {
              plantRequests.delete(id);
              reject(new Error("PlantUML render timed out"));
            }, 30000);
            plantRequests.set(id, { resolve, reject, timer });
            plantFrame.contentWindow.postMessage({ type: "plantuml-render", id, source }, location.origin);
          }),
      );
    plantQueue = task.catch(() => {}); // TeaVM shares state: serialize diagrams in one lightweight frame.
    return task;
  }
  async function diagrams() {
    const tasks = [];
    for (const code of document.querySelectorAll("pre > code")) {
      const match = /\blanguage-([\w-]+)/.exec(code.className);
      const language = match ? match[1].toLowerCase() : "";
      if (
        !["dot", "graphviz", "plantuml", "puml", "vega", "vega-lite", "vegalite", "plotly", "math"].includes(language)
      )
        continue;
      const pre = code.parentElement,
        source = code.textContent;
      tasks.push(
        (async () => {
          const target = targetFor(pre);
          try {
            if (language === "dot" || language === "graphviz") target.appendChild(svgElement(await graph(source)));
            else if (language === "plantuml" || language === "puml")
              target.appendChild(svgElement(await plant(source)));
            else if (language === "math") {
              stylesheet("katex.min.css");
              await script("katex.min.js");
              window.katex.render(source, target, { displayMode: true, throwOnError: false, trust: false });
            } else if (language === "plotly") {
              const spec = JSON.parse(source);
              await script("plotly.min.js");
              await window.Plotly.newPlot(target, spec.data || [], spec.layout || {}, {
                responsive: true,
                displaylogo: false,
                ...(spec.config || {}),
              });
            } else {
              const spec = JSON.parse(source);
              await script("vega.min.js");
              await script("vega-lite.min.js");
              await script("vega-embed.min.js");
              await window.vegaEmbed(target, spec, {
                actions: false,
                renderer: "svg",
                mode: language === "vega" ? "vega" : "vega-lite",
              });
            }
            target.dataset.renderer = language;
            pre.remove();
          } catch (error) {
            target.remove();
            fail(pre, error);
          }
        })(),
      );
    }
    await Promise.all(tasks);
  }
  async function enhance() {
    notes();
    const math = [...document.querySelectorAll(".sumatra-math")];
    const formulas = math.length
      ? (async () => {
          stylesheet("katex.min.css");
          await script("katex.min.js");
          const macros = {};
          for (const node of math)
            window.katex.render(node.textContent, node, {
              displayMode: node.classList.contains("display"),
              throwOnError: false,
              trust: false,
              macros,
            });
        })()
      : Promise.resolve();
    const mermaid = [...document.querySelectorAll("pre.mermaid")];
    const flow = mermaid.length
      ? (async () => {
          await script("mermaid.min.js");
          window.mermaid.initialize({ startOnLoad: false, securityLevel: "strict", theme: dark ? "dark" : "default" });
          for (const node of mermaid) {
            try {
              await window.mermaid.run({ nodes: [node] });
            } catch (error) {
              fail(node, error);
            }
          }
        })()
      : Promise.resolve();
    const rendering = diagrams();
    const ordinaryCode = [...document.querySelectorAll("pre > code")].filter(
      (node) => !/language-(dot|graphviz|plantuml|puml|vega|vega-lite|vegalite|plotly|math)\b/.test(node.className),
    );
    const highlight = ordinaryCode.length
      ? (async () => {
          stylesheet(dark ? "highlight-dark.css" : "highlight-light.css");
          await script("highlight.min.js");
          ordinaryCode.forEach((node) => window.hljs.highlightElement(node));
        })()
      : Promise.resolve();
    const results = await Promise.allSettled([formulas, flow, rendering, highlight]);
    results
      .filter((result) => result.status === "rejected")
      .forEach((result) => stats.errors.push(String(result.reason)));
    stats.milliseconds = performance.now() - started;
    stats.done = true;
  }
  window.__sumatraMarkdownReady = enhance();
})();
