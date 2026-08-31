import {
  graphEdgeEndpointKey,
  graphNodeKey,
  type GraphData,
  type GraphEdge,
  type GraphNode,
} from "../../lib/types";

export type ImpactRisk = "changed" | "critical" | "high" | "medium";

export interface ChangedSymbol {
  name: string;
  label: string;
  file: string;
}

export interface DetectChangesResult {
  changed_files: string[];
  changed_count: number;
  impacted_symbols: ChangedSymbol[];
  depth: number;
  hint?: string;
}

export interface ImpactEvidence {
  sourceName: string;
  targetName: string;
  relationship: string;
}

export interface ImpactNode {
  key: string;
  graphNode?: GraphNode;
  name: string;
  label: string;
  file: string;
  layer: number;
  risk: ImpactRisk;
  fanIn: number;
  evidence?: ImpactEvidence;
}

export interface ImpactLink {
  source: string;
  target: string;
  relationship: string;
}

export interface ImpactModel {
  layers: ImpactNode[][];
  links: ImpactLink[];
  totalNodes: number;
  truncated: boolean;
}

export interface ArchitectureHotspot {
  name: string;
  qualified_name: string;
  fan_in: number;
}

export interface HotspotRow extends ArchitectureHotspot {
  graphNode?: GraphNode;
  file: string;
  risk: Exclude<ImpactRisk, "changed">;
}

export interface FileHotspot {
  file: string;
  fanIn: number;
  hotspotCount: number;
  nodeCount: number;
  score: number;
  risk: Exclude<ImpactRisk, "changed">;
}

export interface HotspotModel {
  rows: HotspotRow[];
  files: FileHotspot[];
  maxFanIn: number;
}

const STRUCTURAL_LABELS = new Set(["Project", "Folder", "File", "Module"]);
const IMPACT_EDGE_TYPES = new Set([
  "CALLS",
  "IMPORTS",
  "USAGE",
  "DATA_FLOWS",
  "HTTP_CALLS",
  "ASYNC_CALLS",
  "CROSS_HTTP_CALLS",
  "CROSS_ASYNC_CALLS",
  "CROSS_CHANNEL",
  "CROSS_GRPC_CALLS",
  "CROSS_GRAPHQL_CALLS",
  "CROSS_TRPC_CALLS",
  "CROSS_PACKAGE_IMPORTS",
  "CROSS_PROJECT_DEPENDS",
]);

function normalizePath(value: string | undefined): string {
  return (value ?? "").replace(/\\/g, "/").replace(/^\.\//, "").toLowerCase();
}

function pathsMatch(nodePath: string | undefined, changedPath: string): boolean {
  const node = normalizePath(nodePath);
  const changed = normalizePath(changedPath);
  return node === changed || node.endsWith(`/${changed}`) || changed.endsWith(`/${node}`);
}

function flattenGraph(data: GraphData): { nodes: GraphNode[]; edges: GraphEdge[] } {
  const nodes = [...data.nodes];
  const edges = [...data.edges];
  for (const linked of data.linked_projects ?? []) {
    nodes.push(...linked.nodes);
    edges.push(...linked.edges, ...linked.cross_edges);
  }
  return { nodes, edges };
}

function riskForLayer(layer: number): ImpactRisk {
  if (layer === 0) return "changed";
  if (layer === 1) return "critical";
  if (layer === 2) return "high";
  return "medium";
}

function riskForRatio(value: number, max: number): Exclude<ImpactRisk, "changed"> {
  const ratio = max > 0 ? value / max : 0;
  if (ratio >= 0.67) return "critical";
  if (ratio >= 0.34) return "high";
  return "medium";
}

function incomingCallCount(node: GraphNode, edges: GraphEdge[]): number {
  if (typeof node.in_calls === "number") return node.in_calls;
  const key = graphNodeKey(node);
  return edges.filter(
    (edge) =>
      edge.type === "CALLS" &&
      (graphEdgeEndpointKey(edge, "target") === key ||
        (!edge.target_key && edge.target === node.id)),
  ).length;
}

export function buildImpactModel(
  data: GraphData,
  changes: DetectChangesResult,
  requestedDepth: number,
  maxNodes = Number.POSITIVE_INFINITY,
): ImpactModel {
  const { nodes, edges } = flattenGraph(data);
  const nodesById = new Map(nodes.map((node) => [graphNodeKey(node), node]));
  const nodesByName = new Map<string, GraphNode[]>();
  for (const node of nodes) {
    const candidates = nodesByName.get(node.name) ?? [];
    candidates.push(node);
    nodesByName.set(node.name, candidates);
  }
  const incoming = new Map<string, GraphEdge[]>();

  for (const edge of edges) {
    if (!IMPACT_EDGE_TYPES.has(edge.type)) continue;
    const list = incoming.get(graphEdgeEndpointKey(edge, "target")) ?? [];
    list.push(edge);
    incoming.set(graphEdgeEndpointKey(edge, "target"), list);
  }

  const layers: ImpactNode[][] = Array.from(
    { length: Math.max(1, requestedDepth + 1) },
    () => [],
  );
  const visited = new Set<string>();
  const queue: Array<{ node: GraphNode; layer: number; key: string }> = [];
  let syntheticIndex = 0;
  let totalNodes = 0;
  let skippedSeeds = false;

  for (const symbol of changes.impacted_symbols ?? []) {
    if (totalNodes >= maxNodes) {
      skippedSeeds = true;
      break;
    }
    const graphNode = (nodesByName.get(symbol.name) ?? []).find(
      (node) => pathsMatch(node.file_path, symbol.file) && !STRUCTURAL_LABELS.has(node.label),
    );

    if (graphNode && !visited.has(graphNodeKey(graphNode))) {
      const key = `node:${graphNodeKey(graphNode)}`;
      visited.add(graphNodeKey(graphNode));
      layers[0].push({
        key,
        graphNode,
        name: graphNode.name,
        label: graphNode.label,
        file: graphNode.file_path ?? symbol.file,
        layer: 0,
        risk: "changed",
        fanIn: incomingCallCount(graphNode, edges),
      });
      queue.push({ node: graphNode, layer: 0, key });
      totalNodes += 1;
    } else if (!graphNode) {
      layers[0].push({
        key: `symbol:${syntheticIndex++}`,
        name: symbol.name,
        label: symbol.label,
        file: symbol.file,
        layer: 0,
        risk: "changed",
        fanIn: 0,
      });
      totalNodes += 1;
    }
  }

  if (layers[0].length === 0) {
    for (const file of changes.changed_files ?? []) {
      if (totalNodes >= maxNodes) {
        skippedSeeds = true;
        break;
      }
      layers[0].push({
        key: `file:${syntheticIndex++}`,
        name: file.split(/[\\/]/).pop() ?? file,
        label: "File",
        file,
        layer: 0,
        risk: "changed",
        fanIn: 0,
      });
      totalNodes += 1;
    }
  }

  const allLinks: ImpactLink[] = [];
  while (queue.length > 0 && totalNodes < maxNodes) {
    const current = queue.shift()!;
    if (current.layer >= requestedDepth) continue;

    const candidateEdges = [...(incoming.get(graphNodeKey(current.node)) ?? [])].sort((a, b) => {
      const aName = nodesById.get(graphEdgeEndpointKey(a, "source"))?.name ?? "";
      const bName = nodesById.get(graphEdgeEndpointKey(b, "source"))?.name ?? "";
      return aName.localeCompare(bName) || a.type.localeCompare(b.type);
    });

    for (const edge of candidateEdges) {
      if (totalNodes >= maxNodes) break;
      const caller = nodesById.get(graphEdgeEndpointKey(edge, "source"));
      if (!caller || STRUCTURAL_LABELS.has(caller.label) || visited.has(graphNodeKey(caller))) continue;

      const layer = current.layer + 1;
      const key = `node:${graphNodeKey(caller)}`;
      visited.add(graphNodeKey(caller));
      layers[layer].push({
        key,
        graphNode: caller,
        name: caller.name,
        label: caller.label,
        file: caller.file_path ?? "Unknown file",
        layer,
        risk: riskForLayer(layer),
        fanIn: incomingCallCount(caller, edges),
        evidence: {
          sourceName: caller.name,
          targetName: current.node.name,
          relationship: edge.type,
        },
      });
      totalNodes += 1;
      allLinks.push({ source: current.key, target: key, relationship: edge.type });
      queue.push({ node: caller, layer, key });
    }
  }

  for (const layer of layers) {
    layer.sort((a, b) => b.fanIn - a.fanIn || a.name.localeCompare(b.name));
  }

  const includedKeys = new Set(layers.flat().map((node) => node.key));
  const links = allLinks.filter(
    (link) => includedKeys.has(link.source) && includedKeys.has(link.target),
  );
  return {
    layers,
    links,
    totalNodes,
    truncated: skippedSeeds || (totalNodes >= maxNodes && queue.length > 0),
  };
}

export function buildHotspotModel(
  data: GraphData,
  architectureHotspots: ArchitectureHotspot[],
): HotspotModel {
  const { nodes, edges } = flattenGraph(data);
  const nodeById = new Map(nodes.map((node) => [graphNodeKey(node), node]));
  const nodeByQualifiedName = new Map<string, GraphNode>();
  for (const node of nodes) {
    if (node.qualified_name && !nodeByQualifiedName.has(node.qualified_name)) {
      /* flattenGraph keeps the primary project first. Preserve that node when
       * two projects expose the same qualified name. */
      nodeByQualifiedName.set(node.qualified_name, node);
    }
  }
  const maxFanIn = Math.max(0, ...architectureHotspots.map((item) => item.fan_in));

  const rows = architectureHotspots
    .map((item) => {
      const graphNode = nodeByQualifiedName.get(item.qualified_name);
      return {
        ...item,
        graphNode,
        file: graphNode?.file_path ?? "当前加载范围内不可见",
        risk: riskForRatio(item.fan_in, maxFanIn),
      } satisfies HotspotRow;
    })
    .sort((a, b) => b.fan_in - a.fan_in || a.qualified_name.localeCompare(b.qualified_name));

  const fileMap = new Map<string, Omit<FileHotspot, "score" | "risk">>();
  const fileKeyFor = (node: GraphNode): string =>
    `${node.graph_project ?? ""}:${node.file_path ?? ""}`;
  for (const node of nodes) {
    if (!node.file_path || STRUCTURAL_LABELS.has(node.label)) continue;
    const fileKey = fileKeyFor(node);
    const current = fileMap.get(fileKey) ?? {
      file: node.file_path,
      fanIn: 0,
      hotspotCount: 0,
      nodeCount: 0,
    };
    current.nodeCount += 1;
    fileMap.set(fileKey, current);
  }

  for (const edge of edges) {
    if (edge.type !== "CALLS") continue;
    const target = nodeById.get(graphEdgeEndpointKey(edge, "target"));
    if (!target?.file_path) continue;
    const current = fileMap.get(fileKeyFor(target));
    if (current) current.fanIn += 1;
  }

  const indexedFanInByFile = new Map<string, number>();
  for (const row of rows) {
    if (!row.graphNode?.file_path) continue;
    const fileKey = fileKeyFor(row.graphNode);
    indexedFanInByFile.set(
      fileKey,
      (indexedFanInByFile.get(fileKey) ?? 0) + row.fan_in,
    );
    const current = fileMap.get(fileKey);
    if (!current) continue;
    current.hotspotCount += 1;
  }
  for (const [fileKey, fanIn] of indexedFanInByFile) {
    const current = fileMap.get(fileKey);
    if (current) current.fanIn = Math.max(current.fanIn, fanIn);
  }

  const scored = [...fileMap.values()]
    .map((file) => ({ ...file, score: file.fanIn * 4 + file.hotspotCount * 3 + Math.sqrt(file.nodeCount) }))
    .filter((file) => file.fanIn > 0 || file.hotspotCount > 0)
    .sort((a, b) => b.score - a.score || a.file.localeCompare(b.file))
    .slice(0, 18);
  const maxScore = Math.max(0, ...scored.map((file) => file.score));
  const files = scored.map((file) => ({
    ...file,
    risk: riskForRatio(file.score, maxScore),
  }));

  return { rows, files, maxFanIn };
}

export function shortPath(path: string, segments = 3): string {
  const parts = path.replace(/\\/g, "/").split("/");
  return parts.slice(-segments).join("/");
}
