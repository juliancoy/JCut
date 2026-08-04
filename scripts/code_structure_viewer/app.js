"use strict";

const DATA_URL = "code_structure_graph.json";
const canvas = document.querySelector("#graph");
const context = canvas.getContext("2d");
const elements = {
  freshness: document.querySelector("#freshness"),
  metrics: document.querySelector("#metric-grid"),
  viewMode: document.querySelector("#view-mode"),
  layoutMode: document.querySelector("#layout-mode"),
  sizeMetric: document.querySelector("#size-metric"),
  colorMode: document.querySelector("#color-mode"),
  focusDepth: document.querySelector("#focus-depth"),
  search: document.querySelector("#search"),
  edgeFilters: document.querySelector("#edge-filters"),
  edgeStrength: document.querySelector("#edge-strength"),
  edgeStrengthLabel: document.querySelector("#edge-strength-label"),
  fit: document.querySelector("#fit-graph"),
  clear: document.querySelector("#clear-selection"),
  centralityList: document.querySelector("#centrality-list"),
  findingTabs: document.querySelector("#finding-tabs"),
  findingList: document.querySelector("#finding-list"),
  caption: document.querySelector("#graph-caption"),
  status: document.querySelector("#graph-status"),
  empty: document.querySelector("#graph-empty"),
  tooltip: document.querySelector("#graph-tooltip"),
  details: document.querySelector("#details"),
};

const state = {
  payload: null,
  model: null,
  positions: new Map(),
  groupLabels: [],
  visibleNodes: [],
  visibleEdges: [],
  selected: null,
  emphasized: new Set(),
  hovered: null,
  finding: "layers",
  camera: { x: 0, y: 0, scale: 1 },
  dragging: null,
  renderPending: false,
};

const edgeColors = {
  include: "#68a7ff",
  call: "#54d6cf",
  dependency: "#f4b860",
};

function formatNumber(value) {
  return new Intl.NumberFormat().format(value || 0);
}

function basename(path) {
  return path.split("/").pop();
}

function stableHash(text) {
  let hash = 2166136261;
  for (let index = 0; index < text.length; index += 1) {
    hash ^= text.charCodeAt(index);
    hash = Math.imul(hash, 16777619);
  }
  return hash >>> 0;
}

function ownerColor(owner) {
  const hue = stableHash(owner || "unowned") % 360;
  return `hsl(${hue} 58% 61%)`;
}

function layerRank(layer) {
  const match = String(layer || "").match(/L(\d+)/);
  return match ? Number(match[1]) : 99;
}

function edgeKinds(edge) {
  if (edge.kinds) return edge.kinds;
  const kinds = [];
  if (edge.include_edges) kinds.push("include");
  if (edge.call_edges) kinds.push("call");
  if (edge.dependency_edges) kinds.push("dependency");
  return kinds;
}

function edgeWeight(edge) {
  return edge.file_edges || edge.include_count + edge.call_count + edge.dependency_count || 1;
}

function assignPercentiles(nodes, valueKey, percentileKey) {
  const ordered = [...nodes].sort((left, right) =>
    left.centrality[valueKey] - right.centrality[valueKey] || left.id.localeCompare(right.id));
  const denominator = Math.max(1, ordered.length - 1);
  ordered.forEach((node, index) => { node.centrality[percentileKey] = index / denominator; });
}

function annotateCentrality(model) {
  const nodeById = new Map(model.nodes.map((node) => [node.id, node]));
  const outgoing = new Map(model.nodes.map((node) => [node.id, []]));
  const incoming = new Map(model.nodes.map((node) => [node.id, []]));
  model.edges.forEach((edge) => {
    if (!nodeById.has(edge.source) || !nodeById.has(edge.target)) return;
    outgoing.get(edge.source).push(edge);
    incoming.get(edge.target).push(edge);
  });
  const count = Math.max(1, model.nodes.length);
  let ranks = new Map(model.nodes.map((node) => [node.id, 1 / count]));
  const damping = .85;
  for (let iteration = 0; iteration < 40; iteration += 1) {
    const next = new Map(model.nodes.map((node) => [node.id, (1 - damping) / count]));
    let dangling = 0;
    model.nodes.forEach((node) => {
      const edges = outgoing.get(node.id);
      const totalWeight = edges.reduce((total, edge) => total + edge.weight, 0);
      if (!totalWeight) {
        dangling += ranks.get(node.id);
        return;
      }
      edges.forEach((edge) => {
        next.set(edge.target, next.get(edge.target)
          + damping * ranks.get(node.id) * edge.weight / totalWeight);
      });
    });
    if (dangling) {
      const share = damping * dangling / count;
      model.nodes.forEach((node) => next.set(node.id, next.get(node.id) + share));
    }
    ranks = next;
  }
  model.nodes.forEach((node) => {
    const inEdges = incoming.get(node.id);
    const outEdges = outgoing.get(node.id);
    node.centrality = {
      pagerank: ranks.get(node.id),
      fan_in: inEdges.length,
      fan_out: outEdges.length,
      degree: new Set([
        ...inEdges.map((edge) => edge.source),
        ...outEdges.map((edge) => edge.target),
      ]).size,
      weighted_degree: [...inEdges, ...outEdges].reduce((total, edge) => total + edge.weight, 0),
    };
  });
  assignPercentiles(model.nodes, "pagerank", "pagerank_percentile");
  assignPercentiles(model.nodes, "degree", "degree_percentile");
  assignPercentiles(model.nodes, "fan_in", "fan_in_percentile");
  assignPercentiles(model.nodes, "fan_out", "fan_out_percentile");
  ["pagerank", "degree", "fan_in", "fan_out"].forEach((metric) => {
    const maximum = Math.max(...model.nodes.map((node) => node.centrality[metric]), 1e-12);
    model.nodes.forEach((node) => {
      node.centrality[`${metric}_normalized`] = node.centrality[metric] / maximum;
    });
  });
  return model;
}

function centralityValue(node, metric = elements.sizeMetric?.value || "pagerank") {
  if (metric === "uniform") return .45;
  return node.centrality?.[`${metric}_normalized`] || 0;
}

function centralityRawValue(node, metric = elements.sizeMetric?.value || "pagerank") {
  if (metric === "pagerank") return node.centrality?.pagerank || 0;
  if (metric === "uniform") return 1;
  return node.centrality?.[metric] || 0;
}

function centralityMetricLabel(metric = elements.sizeMetric?.value || "pagerank") {
  return {
    pagerank: "PageRank",
    degree: "degree",
    fan_in: "fan-in",
    fan_out: "fan-out",
    uniform: "uniform",
  }[metric];
}

function normalizeModel(mode) {
  const payload = state.payload;
  if (mode === "file") {
    const nodes = payload.nodes.map((node) => ({
      ...node,
      label: basename(node.path),
      color: ownerColor(node.owner),
      searchText: [
        node.path, node.language, node.owner, node.layer,
        ...(node.targets || []),
        ...node.symbols.slice(0, 400).flatMap((symbol) => [symbol.name, symbol.qualified_name]),
      ].filter(Boolean).join(" ").toLowerCase(),
    }));
    const edges = payload.edges.map((edge, index) => ({
      ...edge,
      id: `file:${index}`,
      kinds: edgeKinds(edge),
      weight: edgeWeight(edge),
    }));
    return annotateCentrality({ mode, nodes, edges, title: "File dependency graph" });
  }

  const rawEdges = mode === "target" ? payload.target_edges : payload.owner_edges;
  const sourceKey = mode === "target" ? "source_target" : "source_owner";
  const targetKey = mode === "target" ? "target_target" : "target_owner";
  const ids = new Set();
  rawEdges.forEach((edge) => {
    ids.add(edge[sourceKey]);
    ids.add(edge[targetKey]);
  });
  const nodes = [...ids].sort().map((id) => ({
    id,
    path: id,
    label: id,
    color: ownerColor(id),
    searchText: id.toLowerCase(),
    metrics: { fan_in: 0, fan_out: 0 },
    aggregate: true,
  }));
  const byId = new Map(nodes.map((node) => [node.id, node]));
  const edges = rawEdges.map((edge, index) => {
    byId.get(edge[sourceKey]).metrics.fan_out += 1;
    byId.get(edge[targetKey]).metrics.fan_in += 1;
    return {
      ...edge,
      id: `${mode}:${index}`,
      source: edge[sourceKey],
      target: edge[targetKey],
      kinds: edgeKinds(edge),
      weight: edgeWeight(edge),
    };
  });
  return annotateCentrality({
    mode,
    nodes,
    edges,
    title: mode === "target" ? "Build target dependency graph" : "Cross-owner dependency graph",
  });
}

function layoutModel() {
  const nodes = state.model.nodes;
  const positions = new Map();
  const layout = elements.layoutMode.value;
  const orderedByCentrality = [...nodes].sort((left, right) =>
    right.centrality.pagerank - left.centrality.pagerank || left.id.localeCompare(right.id));
  state.groupLabels = [];

  if (layout === "centrality" || state.model.mode !== "file" && layout === "layers") {
    if (orderedByCentrality.length) positions.set(orderedByCentrality[0].id, { x: 0, y: 0 });
    let index = 1;
    let ring = 1;
    while (index < orderedByCentrality.length) {
      const radius = ring * 42;
      const capacity = Math.max(6, Math.floor(2 * Math.PI * radius / 34));
      const members = orderedByCentrality.slice(index, index + capacity);
      members.forEach((node, memberIndex) => {
        const angle = 2 * Math.PI * memberIndex / members.length - Math.PI / 2;
        positions.set(node.id, { x: Math.cos(angle) * radius, y: Math.sin(angle) * radius });
      });
      index += members.length;
      ring += 1;
    }
    state.groupLabels.push({ x: 0, y: -24, text: "central core" });
  } else if (layout === "layers") {
    const groups = new Map();
    nodes.forEach((node) => {
      const key = node.layer || "Unassigned";
      if (!groups.has(key)) groups.set(key, []);
      groups.get(key).push(node);
    });
    const orderedGroups = [...groups.entries()].sort((left, right) =>
      layerRank(left[0]) - layerRank(right[0]) || left[0].localeCompare(right[0]));
    orderedGroups.forEach(([layer, members], groupIndex) => {
      members.sort((left, right) =>
        right.centrality.pagerank - left.centrality.pagerank
          || String(left.owner).localeCompare(String(right.owner)) || left.path.localeCompare(right.path));
      state.groupLabels.push({ x: groupIndex * 430 + 110, y: -30, text: layer });
      members.forEach((node, index) => {
        const column = index % 4;
        const row = Math.floor(index / 4);
        positions.set(node.id, {
          x: groupIndex * 430 + column * 72 + (stableHash(node.id) % 17),
          y: row * 43 + (stableHash(`${node.id}:y`) % 11),
          group: layer,
        });
      });
    });
  } else {
    const groups = new Map();
    nodes.forEach((node) => {
      const key = state.model.mode === "file" ? node.owner || "Unowned" : "Relationships";
      if (!groups.has(key)) groups.set(key, []);
      groups.get(key).push(node);
    });
    const orderedGroups = [...groups.entries()].sort((left, right) =>
      right[1].length - left[1].length || left[0].localeCompare(right[0]));
    const groupRadius = Math.max(260, orderedGroups.length * 34);
    orderedGroups.forEach(([group, members], groupIndex) => {
      const groupAngle = Math.PI * 2 * groupIndex / Math.max(1, orderedGroups.length) - Math.PI / 2;
      const center = { x: Math.cos(groupAngle) * groupRadius, y: Math.sin(groupAngle) * groupRadius };
      state.groupLabels.push({ x: center.x, y: center.y - Math.max(34, Math.sqrt(members.length) * 15), text: group });
      members.sort((left, right) => right.centrality.pagerank - left.centrality.pagerank || left.id.localeCompare(right.id));
      members.forEach((node, index) => {
        const angle = index * Math.PI * (3 - Math.sqrt(5));
        const radius = 13 * Math.sqrt(index);
        positions.set(node.id, {
          x: center.x + Math.cos(angle) * radius,
          y: center.y + Math.sin(angle) * radius,
        });
      });
    });
  }
  state.positions = positions;
}

function checkedKinds() {
  return new Set([...elements.edgeFilters.querySelectorAll("input:checked")].map((input) => input.value));
}

function relationshipThreshold() {
  const percentile = Number(elements.edgeStrength.value);
  if (!percentile || !state.model.edges.length) return 1;
  const weights = state.model.edges.map((edge) => edge.weight).sort((left, right) => left - right);
  return weights[Math.min(weights.length - 1, Math.floor(percentile / 100 * weights.length))];
}

function neighborhood(startId, edges, depth) {
  const visited = new Set([startId]);
  let frontier = new Set([startId]);
  for (let step = 0; step < depth; step += 1) {
    const next = new Set();
    edges.forEach((edge) => {
      if (frontier.has(edge.source) && !visited.has(edge.target)) next.add(edge.target);
      if (frontier.has(edge.target) && !visited.has(edge.source)) next.add(edge.source);
    });
    next.forEach((id) => visited.add(id));
    frontier = next;
  }
  return visited;
}

function updateVisibleGraph() {
  const query = elements.search.value.trim().toLowerCase();
  const enabledKinds = checkedKinds();
  const threshold = relationshipThreshold();
  const eligibleEdges = state.model.edges.filter((edge) =>
    edge.weight >= threshold && edge.kinds.some((kind) => enabledKinds.has(kind)));
  let visibleIds = new Set(state.model.nodes.map((node) => node.id));
  if (query) {
    const matches = new Set(
      state.model.nodes.filter((node) => node.searchText.includes(query)).map((node) => node.id),
    );
    visibleIds = new Set(matches);
    eligibleEdges.forEach((edge) => {
      if (matches.has(edge.source)) visibleIds.add(edge.target);
      if (matches.has(edge.target)) visibleIds.add(edge.source);
    });
  }
  if (state.selected && elements.focusDepth.value !== "all") {
    const focused = neighborhood(state.selected.id, eligibleEdges, Number(elements.focusDepth.value));
    visibleIds = new Set([...visibleIds].filter((id) => focused.has(id)));
  }
  state.visibleNodes = state.model.nodes.filter((node) => visibleIds.has(node.id));
  state.visibleEdges = eligibleEdges.filter((edge) =>
    visibleIds.has(edge.source) && visibleIds.has(edge.target));
  elements.empty.hidden = state.visibleNodes.length !== 0;
  elements.edgeStrengthLabel.textContent = Number(elements.edgeStrength.value)
    ? `P${elements.edgeStrength.value} · weight ≥ ${formatNumber(threshold)}` : "all edges";
  elements.status.textContent = `${formatNumber(state.visibleNodes.length)} nodes · ${formatNumber(state.visibleEdges.length)} edges · ${centralityMetricLabel()} sizing`;
  requestRender();
}

function resizeCanvas() {
  const rect = canvas.getBoundingClientRect();
  const ratio = window.devicePixelRatio || 1;
  canvas.width = Math.max(1, Math.round(rect.width * ratio));
  canvas.height = Math.max(1, Math.round(rect.height * ratio));
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  requestRender();
}

function screenPoint(position) {
  const rect = canvas.getBoundingClientRect();
  return {
    x: (position.x - state.camera.x) * state.camera.scale + rect.width / 2,
    y: (position.y - state.camera.y) * state.camera.scale + rect.height / 2,
  };
}

function worldPoint(clientX, clientY) {
  const rect = canvas.getBoundingClientRect();
  return {
    x: state.camera.x + (clientX - rect.left - rect.width / 2) / state.camera.scale,
    y: state.camera.y + (clientY - rect.top - rect.height / 2) / state.camera.scale,
  };
}

function nodeRadius(node) {
  return 4 + 15 * Math.sqrt(centralityValue(node));
}

function layerColor(layer) {
  const rank = layerRank(layer);
  if (rank === 99) return "#758896";
  return ["#ed6a5a", "#f4b860", "#8acb88", "#54d6cf", "#68a7ff", "#b890e8"][rank % 6];
}

function nodeColor(node) {
  if (elements.colorMode.value === "layer") return layerColor(node.layer);
  if (elements.colorMode.value === "centrality") {
    const score = centralityValue(node, "pagerank");
    return `hsl(${205 - score * 175} ${62 + score * 22}% ${52 + score * 13}%)`;
  }
  return ownerColor(node.owner || node.id);
}

function dominantEdgeKind(edge) {
  if (edge.kinds.includes("call")) return "call";
  if (edge.kinds.includes("include")) return "include";
  return "dependency";
}

function drawArrowHead(source, target, targetRadius, color, alpha) {
  const angle = Math.atan2(target.y - source.y, target.x - source.x);
  const tipX = target.x - Math.cos(angle) * (targetRadius + 2);
  const tipY = target.y - Math.sin(angle) * (targetRadius + 2);
  const size = 4.5;
  context.beginPath();
  context.moveTo(tipX, tipY);
  context.lineTo(tipX - Math.cos(angle - .55) * size, tipY - Math.sin(angle - .55) * size);
  context.lineTo(tipX - Math.cos(angle + .55) * size, tipY - Math.sin(angle + .55) * size);
  context.closePath();
  context.fillStyle = color;
  context.globalAlpha = alpha;
  context.fill();
}

function render() {
  state.renderPending = false;
  const rect = canvas.getBoundingClientRect();
  context.clearRect(0, 0, rect.width, rect.height);
  if (!state.model) return;
  const position = state.positions;
  const emphasized = state.emphasized;
  const selectedId = state.selected?.id;
  const nodeById = new Map(state.model.nodes.map((node) => [node.id, node]));
  const topCentral = new Set([...state.model.nodes]
    .sort((left, right) => right.centrality.pagerank - left.centrality.pagerank)
    .slice(0, 4).map((node) => node.id));

  state.groupLabels.forEach((label) => {
    const point = screenPoint(label);
    context.globalAlpha = .72;
    context.fillStyle = "#8fa6b5";
    context.font = "700 11px ui-monospace, monospace";
    context.textAlign = "center";
    context.fillText(label.text, point.x, point.y);
  });
  context.textAlign = "start";

  state.visibleEdges.forEach((edge) => {
    const sourcePosition = position.get(edge.source);
    const targetPosition = position.get(edge.target);
    if (!sourcePosition || !targetPosition) return;
    const source = screenPoint(sourcePosition);
    const target = screenPoint(targetPosition);
    const active = selectedId && (edge.source === selectedId || edge.target === selectedId)
      || emphasized.has(edge.source) && emphasized.has(edge.target);
    context.beginPath();
    context.moveTo(source.x, source.y);
    context.lineTo(target.x, target.y);
    const color = edgeColors[dominantEdgeKind(edge)];
    const alpha = active ? .9 : Math.min(.30, .05 + Math.log2(edge.weight + 1) * .028);
    context.strokeStyle = color;
    context.globalAlpha = alpha;
    context.lineWidth = active ? 1.8 : Math.min(1.5, .45 + Math.log2(edge.weight + 1) * .12);
    context.stroke();
    if (active || state.visibleEdges.length < 900 || state.camera.scale > .55) {
      const targetNode = nodeById.get(edge.target);
      drawArrowHead(source, target, nodeRadius(targetNode) * Math.max(.55, Math.min(1.15, state.camera.scale)), color, alpha);
    }
  });
  context.globalAlpha = 1;

  const showLabels = state.camera.scale > .95 && state.visibleNodes.length < 450;
  state.visibleNodes.forEach((node) => {
    const world = position.get(node.id);
    if (!world) return;
    const point = screenPoint(world);
    const radius = nodeRadius(node) * Math.max(.55, Math.min(1.15, state.camera.scale));
    const selected = selectedId === node.id;
    const highlighted = selected || state.hovered === node.id || emphasized.has(node.id);
    context.beginPath();
    context.arc(point.x, point.y, radius, 0, Math.PI * 2);
    context.fillStyle = nodeColor(node);
    context.globalAlpha = selected ? 1 : highlighted ? .95 : .72;
    context.fill();
    if (highlighted) {
      context.strokeStyle = selected ? "#ffffff" : "#f4b860";
      context.lineWidth = 2;
      context.stroke();
    }
    if (showLabels || highlighted || topCentral.has(node.id)) {
      context.globalAlpha = highlighted ? 1 : .74;
      context.font = `${highlighted ? 700 : 500} 10px ui-monospace, monospace`;
      context.fillStyle = "#dce8ef";
      context.fillText(node.label, point.x + radius + 4, point.y + 3);
    }
  });
  context.globalAlpha = 1;
}

function requestRender() {
  if (state.renderPending) return;
  state.renderPending = true;
  requestAnimationFrame(render);
}

function fitGraph() {
  const visible = state.visibleNodes.length ? state.visibleNodes : state.model.nodes;
  if (!visible.length) return;
  const points = visible.map((node) => state.positions.get(node.id)).filter(Boolean);
  const minX = Math.min(...points.map((point) => point.x));
  const maxX = Math.max(...points.map((point) => point.x));
  const minY = Math.min(...points.map((point) => point.y));
  const maxY = Math.max(...points.map((point) => point.y));
  const rect = canvas.getBoundingClientRect();
  state.camera.x = (minX + maxX) / 2;
  state.camera.y = (minY + maxY) / 2;
  state.camera.scale = Math.max(.08, Math.min(2.2,
    Math.min((rect.width - 90) / Math.max(80, maxX - minX),
      (rect.height - 90) / Math.max(80, maxY - minY))));
  requestRender();
}

function hitTest(clientX, clientY) {
  const rect = canvas.getBoundingClientRect();
  const x = clientX - rect.left;
  const y = clientY - rect.top;
  let best = null;
  let bestDistance = Infinity;
  state.visibleNodes.forEach((node) => {
    const point = screenPoint(state.positions.get(node.id));
    const distance = Math.hypot(point.x - x, point.y - y);
    if (distance < Math.max(10, nodeRadius(node) * state.camera.scale + 4) && distance < bestDistance) {
      best = node;
      bestDistance = distance;
    }
  });
  return best;
}

function createElement(tag, className, text) {
  const element = document.createElement(tag);
  if (className) element.className = className;
  if (text !== undefined) element.textContent = text;
  return element;
}

function addBadges(container, values) {
  values.filter((value) => value?.text).forEach((value) => {
    container.append(createElement("span", `badge ${value.kind || ""}`, value.text));
  });
}

function addMetricGrid(container, metrics) {
  const grid = createElement("div", "detail-grid");
  metrics.forEach(([label, value]) => {
    const box = document.createElement("div");
    box.append(createElement("strong", "", formatNumber(value)));
    box.append(createElement("span", "", label));
    grid.append(box);
  });
  container.append(grid);
}

function addList(container, title, values, formatter = (value) => String(value)) {
  if (!values?.length) return;
  container.append(createElement("h3", "", title));
  const list = createElement("ul", "detail-list");
  values.forEach((value) => list.append(createElement("li", "", formatter(value))));
  container.append(list);
}

function showFileDetails(node) {
  const container = elements.details;
  container.replaceChildren();
  container.append(createElement("div", "path", node.path));
  const badges = createElement("div", "badges");
  const parser = node.parser;
  addBadges(badges, [
    { text: node.language },
    { text: node.owner || "unowned" },
    { text: node.layer || "no layer" },
    { text: parser.used },
    { text: parser.compilation_status, kind: parser.compilation_status === "succeeded" ? "good" : "warn" },
    { text: `${parser.confidence.level} confidence`, kind: parser.confidence.level === "high" ? "good" : "warn" },
  ]);
  container.append(badges);
  addMetricGrid(container, [
    ["PageRank ppm", Math.round(node.centrality.pagerank * 1_000_000)],
    ["centrality percentile", Math.round(node.centrality.pagerank_percentile * 100)],
    ["graph degree", node.centrality.degree],
    ["weighted degree", node.centrality.weighted_degree],
    ["lines", node.lines], ["symbols", node.symbol_count],
    ["fan-in", node.metrics.fan_in], ["fan-out", node.metrics.fan_out],
    ["include in", node.metrics.include_fan_in], ["include out", node.metrics.include_fan_out],
    ["call in", node.metrics.call_fan_in], ["call out", node.metrics.call_fan_out],
  ]);
  if (node.targets?.length) addList(container, "Build targets", node.targets);
  const incoming = state.model.edges.filter((edge) => edge.target === node.id)
    .sort((left, right) => right.weight - left.weight).slice(0, 40);
  const outgoing = state.model.edges.filter((edge) => edge.source === node.id)
    .sort((left, right) => right.weight - left.weight).slice(0, 40);
  addList(container, "Incoming relationships", incoming,
    (edge) => `${edge.source} → this · ${edge.kinds.join("/")} · weight ${formatNumber(edge.weight)}`);
  addList(container, "Outgoing relationships", outgoing,
    (edge) => `this → ${edge.target} · ${edge.kinds.join("/")} · weight ${formatNumber(edge.weight)}`);
  if (parser.fallback_reason) addList(container, "Fallback reason", [parser.fallback_reason]);
  addList(container, "Diagnostics", parser.diagnostics);
  addList(container, "Unmatched constructs", parser.unmatched,
    (item) => `${item.kind}${item.line ? ` at line ${item.line}` : ""}`);
  addList(container, "Suspicious constructs", parser.suspicious,
    (item) => `${item.kind}${item.line ? ` at line ${item.line}` : ""}${item.detail ? ` — ${item.detail}` : ""}`);
  container.append(createElement("h3", "", `Symbols (${formatNumber(node.symbols.length)})`));
  const symbols = createElement("ul", "detail-list symbol-list");
  [...node.symbols]
    .sort((left, right) => (right.end_line - right.line) - (left.end_line - left.line) || left.line - right.line)
    .slice(0, 250)
    .forEach((symbol) => {
      const item = document.createElement("li");
      const line = createElement("div", "symbol-line");
      line.append(createElement("code", "", symbol.qualified_name || symbol.name));
      line.append(createElement("small", "", `${symbol.kind} · ${symbol.line}–${symbol.end_line}`));
      item.append(line);
      if (symbol.signature || symbol.type) item.append(createElement("small", "", symbol.signature || symbol.type));
      symbols.append(item);
    });
  container.append(symbols);
}

function showAggregateDetails(node) {
  const container = elements.details;
  container.replaceChildren();
  container.append(createElement("div", "path", node.id));
  const modeLabel = state.model.mode === "target" ? "build target" : "owner";
  const badges = createElement("div", "badges");
  addBadges(badges, [{ text: modeLabel }]);
  container.append(badges);
  const connected = state.model.edges.filter((edge) => edge.source === node.id || edge.target === node.id);
  addMetricGrid(container, [
    ["PageRank ppm", Math.round(node.centrality.pagerank * 1_000_000)],
    ["centrality percentile", Math.round(node.centrality.pagerank_percentile * 100)],
    ["incoming groups", node.metrics.fan_in],
    ["outgoing groups", node.metrics.fan_out],
    ["file edges", connected.reduce((total, edge) => total + (edge.file_edges || 0), 0)],
    ["relationships", connected.length],
  ]);
  addList(container, "Relationships", connected.sort((left, right) => right.weight - left.weight).slice(0, 100),
    (edge) => `${edge.source} → ${edge.target} · ${formatNumber(edge.weight)} file edges`);
}

function selectNode(node, center = false) {
  state.selected = node;
  state.emphasized = new Set([node.id]);
  if (center) {
    const position = state.positions.get(node.id);
    if (position) {
      state.camera.x = position.x;
      state.camera.y = position.y;
      state.camera.scale = Math.max(state.camera.scale, .8);
    }
  }
  if (state.model.mode === "file") showFileDetails(node);
  else showAggregateDetails(node);
  updateVisibleGraph();
  if (elements.focusDepth.value !== "all") fitGraph();
  else requestRender();
}

function clearSelection(refresh = true) {
  state.selected = null;
  state.emphasized.clear();
  elements.tooltip.hidden = true;
  elements.details.replaceChildren(createElement("p", "empty-copy",
    "Select a node or finding to inspect parser health, graph metrics, symbols, and evidence."));
  if (refresh && state.model) {
    updateVisibleGraph();
    fitGraph();
  } else {
    requestRender();
  }
}

const findingDefinitions = {
  layers: { label: "Layers", key: "layer_direction_violations" },
  cycles: { label: "Cycles", key: "include_cycles" },
  reachability: { label: "Reachability", key: "dead_or_unreachable" },
  duplicates: { label: "Duplicates", key: "duplicate_responsibility" },
};

function findingText(type, finding) {
  if (type === "layers") return [`${finding.source} → ${finding.target}`, `${finding.source_layer} → ${finding.target_layer} · ${finding.kinds.join(", ")}`];
  if (type === "cycles") return [finding.files.join(" → "), `${finding.edge_count} include edges`];
  if (type === "reachability") return [finding.path, `${finding.status} · ${finding.confidence}`];
  return [finding.files.join(" ↔ "), `similarity ${finding.max_similarity}`];
}

function findingNodeIds(type, finding) {
  if (type === "layers") return [finding.source, finding.target];
  if (type === "cycles" || type === "duplicates") return finding.files;
  return [finding.path];
}

function renderFindings() {
  elements.findingTabs.replaceChildren();
  Object.entries(findingDefinitions).forEach(([type, definition]) => {
    const count = state.payload.findings[definition.key].length;
    const button = createElement("button", type === state.finding ? "active" : "", `${definition.label} · ${count}`);
    button.type = "button";
    button.setAttribute("role", "tab");
    button.setAttribute("aria-selected", String(type === state.finding));
    button.addEventListener("click", () => { state.finding = type; renderFindings(); });
    elements.findingTabs.append(button);
  });
  elements.findingList.replaceChildren();
  const definition = findingDefinitions[state.finding];
  const findings = state.payload.findings[definition.key];
  if (!findings.length) {
    elements.findingList.append(createElement("p", "empty-copy", "No findings in this queue."));
    return;
  }
  findings.forEach((finding) => {
    const [primary, secondary] = findingText(state.finding, finding);
    const button = createElement("button", "finding-item", primary);
    button.type = "button";
    button.append(createElement("small", "", secondary));
    button.addEventListener("click", () => {
      if (state.model.mode !== "file") setMode("file");
      elements.search.value = "";
      updateVisibleGraph();
      const ids = findingNodeIds(state.finding, finding);
      state.emphasized = new Set(ids);
      const node = state.model.nodes.find((candidate) => candidate.id === ids[0]);
      if (node) selectNode(node, true);
      state.emphasized = new Set(ids);
      requestRender();
    });
    elements.findingList.append(button);
  });
}

function renderOverview() {
  const summary = state.payload.graph_summary;
  const metrics = [
    [state.payload.summary.files, "files"],
    [summary.resolved_file_edges, "file edges"],
    [summary.symbol_calls_resolved, "resolved calls"],
    [summary.include_cycles, "include cycles"],
    [summary.layer_direction_violations, "layer candidates"],
    [summary.dead_or_unreachable_implementations, "reachability"],
  ];
  const template = document.querySelector("#metric-template");
  elements.metrics.replaceChildren();
  metrics.forEach(([value, label]) => {
    const fragment = template.content.cloneNode(true);
    fragment.querySelector("strong").textContent = formatNumber(value);
    fragment.querySelector("span").textContent = label;
    elements.metrics.append(fragment);
  });
}

function renderCentralityList() {
  const metric = elements.sizeMetric.value;
  const nodes = [...state.model.nodes]
    .sort((left, right) => centralityRawValue(right, metric) - centralityRawValue(left, metric)
      || left.id.localeCompare(right.id))
    .slice(0, 10);
  elements.centralityList.replaceChildren();
  nodes.forEach((node, index) => {
    const button = createElement("button", "centrality-item");
    button.type = "button";
    button.style.setProperty("--score", `${Math.round(centralityValue(node, metric) * 100)}%`);
    button.append(createElement("span", "centrality-rank", `#${index + 1}`));
    button.append(createElement("span", "centrality-name", node.label));
    const raw = centralityRawValue(node, metric);
    const score = metric === "pagerank" ? `${Math.round(raw * 1_000_000)} ppm` : formatNumber(raw);
    button.append(createElement("span", "centrality-score", score));
    button.title = `${node.id} · ${centralityMetricLabel(metric)} ${score}`;
    button.addEventListener("click", () => selectNode(node, true));
    elements.centralityList.append(button);
  });
}

function setMode(mode) {
  state.model = normalizeModel(mode);
  elements.viewMode.value = mode;
  elements.caption.textContent = state.model.title;
  clearSelection(false);
  layoutModel();
  updateVisibleGraph();
  renderCentralityList();
  fitGraph();
}

function showTooltip(node, event) {
  if (!node) {
    elements.tooltip.hidden = true;
    return;
  }
  const panelRect = canvas.parentElement.getBoundingClientRect();
  elements.tooltip.replaceChildren(
    createElement("strong", "", node.id),
    createElement("span", "", `PageRank ${Math.round(node.centrality.pagerank * 1_000_000)} ppm · degree ${node.centrality.degree} · in ${node.centrality.fan_in} · out ${node.centrality.fan_out}`),
  );
  elements.tooltip.hidden = false;
  const left = Math.min(panelRect.width - 315, event.clientX - panelRect.left + 14);
  const top = Math.min(panelRect.height - 70, event.clientY - panelRect.top + 14);
  elements.tooltip.style.left = `${Math.max(8, left)}px`;
  elements.tooltip.style.top = `${Math.max(52, top)}px`;
}

function bindEvents() {
  elements.viewMode.addEventListener("change", () => setMode(elements.viewMode.value));
  elements.layoutMode.addEventListener("change", () => {
    layoutModel();
    fitGraph();
  });
  elements.sizeMetric.addEventListener("change", () => {
    renderCentralityList();
    updateVisibleGraph();
  });
  elements.colorMode.addEventListener("change", requestRender);
  elements.focusDepth.addEventListener("change", () => {
    updateVisibleGraph();
    fitGraph();
  });
  elements.search.addEventListener("input", updateVisibleGraph);
  elements.edgeFilters.addEventListener("change", updateVisibleGraph);
  elements.edgeStrength.addEventListener("input", updateVisibleGraph);
  elements.fit.addEventListener("click", fitGraph);
  elements.clear.addEventListener("click", clearSelection);
  new ResizeObserver(resizeCanvas).observe(canvas);

  canvas.addEventListener("pointerdown", (event) => {
    canvas.setPointerCapture(event.pointerId);
    state.dragging = {
      startX: event.clientX,
      startY: event.clientY,
      cameraX: state.camera.x,
      cameraY: state.camera.y,
      moved: false,
    };
    elements.tooltip.hidden = true;
    canvas.classList.add("dragging");
  });
  canvas.addEventListener("pointermove", (event) => {
    if (state.dragging) {
      const dx = event.clientX - state.dragging.startX;
      const dy = event.clientY - state.dragging.startY;
      if (Math.hypot(dx, dy) > 3) state.dragging.moved = true;
      state.camera.x = state.dragging.cameraX - dx / state.camera.scale;
      state.camera.y = state.dragging.cameraY - dy / state.camera.scale;
      requestRender();
    } else {
      const hovered = hitTest(event.clientX, event.clientY);
      state.hovered = hovered?.id || null;
      canvas.style.cursor = hovered ? "pointer" : "grab";
      showTooltip(hovered, event);
      requestRender();
    }
  });
  canvas.addEventListener("pointerleave", () => {
    state.hovered = null;
    elements.tooltip.hidden = true;
    requestRender();
  });
  canvas.addEventListener("pointerup", (event) => {
    if (state.dragging && !state.dragging.moved) {
      const node = hitTest(event.clientX, event.clientY);
      if (node) selectNode(node);
    }
    state.dragging = null;
    canvas.classList.remove("dragging");
  });
  canvas.addEventListener("wheel", (event) => {
    event.preventDefault();
    const before = worldPoint(event.clientX, event.clientY);
    state.camera.scale = Math.max(.05, Math.min(5, state.camera.scale * Math.exp(-event.deltaY * .0012)));
    const after = worldPoint(event.clientX, event.clientY);
    state.camera.x += before.x - after.x;
    state.camera.y += before.y - after.y;
    requestRender();
  }, { passive: false });
}

async function start() {
  try {
    const response = await fetch(DATA_URL);
    if (!response.ok) throw new Error(`${response.status} ${response.statusText}`);
    const payload = await response.json();
    if (payload.schema !== "jcut_code_structure_viewer_v1") {
      throw new Error(`unsupported viewer schema: ${payload.schema}`);
    }
    state.payload = payload;
    const generated = new Date(payload.generated_at).toLocaleString();
    const commit = payload.freshness.git_commit ? payload.freshness.git_commit.slice(0, 10) : "no commit";
    elements.freshness.textContent = `${generated} · ${commit}${payload.freshness.git_dirty ? " · dirty workspace" : ""}`;
    elements.freshness.classList.add("current");
    renderOverview();
    renderFindings();
    bindEvents();
    setMode("file");
    document.body.dataset.viewerReady = "true";
    document.body.dataset.nodeCount = String(state.model.nodes.length);
    window.__codeStructureViewerReady = true;
  } catch (error) {
    const message = location.protocol === "file:"
      ? "This viewer must be served over HTTP. Run: python3 scripts/serve_code_structure.py"
      : `Unable to load ${DATA_URL}: ${error.message}`;
    document.querySelector(".app-shell").replaceChildren(createElement("div", "error-banner", message));
    window.__codeStructureViewerError = message;
  }
}

start();
