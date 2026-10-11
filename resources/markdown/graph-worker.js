/* Graphviz runs off the document's main thread. The shared Viz engine is local. */
importScripts("viz-global.js");
const ready = Viz.instance();
self.onmessage = async (event) => {
  const { id, source } = event.data;
  try {
    const engine = await ready;
    const svg = engine.renderString(source, { format: "svg" });
    self.postMessage({ id, svg });
  } catch (error) {
    self.postMessage({ id, error: String(error) });
  }
};
