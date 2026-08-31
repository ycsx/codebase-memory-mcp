import { useCallback, useRef, useState } from "react";
import type { GraphData, GraphEdge, GraphNode } from "../lib/types";

export interface LoadProgress {
  receivedBytes: number;
  totalBytes: number | null;
}

interface UseGraphDataResult {
  data: GraphData | null;
  loading: boolean;
  error: string | null;
  progress: LoadProgress;
  fetchOverview: (
    project: string,
    maxNodes?: number,
    graph?: "code" | "missed",
  ) => void;
  fetchDetail: (project: string, centerNode: string) => void;
}

/* Node budget: how many nodes the layout endpoint is asked for. The default
 * keeps first paint fast; the user can raise it in 5k steps up to the hard
 * ceiling (mirrors HARD_MAX_NODES in src/ui/layout3d.c). Edges always follow
 * the budget — the server returns every edge between the loaded nodes. */
export const GRAPH_RENDER_NODE_LIMIT = 5000;
export const GRAPH_NODE_BUDGET_STEP = 5000;
export const GRAPH_NODE_BUDGET_MAX = 10_000_000;

export function clampNodeBudget(value: number): number {
  if (!Number.isFinite(value)) return GRAPH_RENDER_NODE_LIMIT;
  const stepped =
    Math.round(value / GRAPH_NODE_BUDGET_STEP) * GRAPH_NODE_BUDGET_STEP;
  if (stepped < GRAPH_NODE_BUDGET_STEP) return GRAPH_NODE_BUDGET_STEP;
  if (stepped > GRAPH_NODE_BUDGET_MAX) return GRAPH_NODE_BUDGET_MAX;
  return stepped;
}

/** Which graph to lay out: the code graph (default) or the missed graph —
 *  only files the indexer could not fully cover, as their file structure. */
export type GraphVariant = "code" | "missed";

function primaryKey(project: string, id: number): string {
  return `primary:${project}:${id}`;
}

function linkedKey(project: string, id: number): string {
  return `linked:${project}:${id}`;
}

function scopeNodes(nodes: GraphNode[], keyFor: (id: number) => string, project: string): GraphNode[] {
  return nodes.map((node) => ({
    ...node,
    graph_key: keyFor(node.id),
    graph_project: project,
  }));
}

function scopeEdges(edges: GraphEdge[], sourceKey: (id: number) => string, targetKey: (id: number) => string): GraphEdge[] {
  return edges.map((edge) => ({
    ...edge,
    source_key: sourceKey(edge.source),
    target_key: targetKey(edge.target),
  }));
}

/** Attach stable, project-scoped identities to a federated layout response.
 * SQLite node IDs restart at 1 in every project, so numeric IDs cannot be used
 * as React/selection keys once linked projects are rendered together. */
export function scopeGraphData(data: GraphData, project: string): GraphData {
  const primaryNodes = scopeNodes(data.nodes, (id) => primaryKey(project, id), project);
  const scopedLinked = data.linked_projects?.map((linked) => ({
    ...linked,
    nodes: scopeNodes(linked.nodes, (id) => linkedKey(linked.project, id), linked.project),
    edges: scopeEdges(
      linked.edges,
      (id) => linkedKey(linked.project, id),
      (id) => linkedKey(linked.project, id),
    ),
    cross_edges: scopeEdges(
      linked.cross_edges,
      (id) => primaryKey(project, id),
      (id) => linkedKey(linked.project, id),
    ),
  }));
  const scopedMissed = data.missed_graph
    ? {
        ...data.missed_graph,
        nodes: scopeNodes(data.missed_graph.nodes, (id) => `missed:${project}:${id}`, project),
        edges: scopeEdges(
          data.missed_graph.edges,
          (id) => `missed:${project}:${id}`,
          (id) => `missed:${project}:${id}`,
        ),
      }
    : undefined;
  return {
    ...data,
    nodes: primaryNodes,
    edges: scopeEdges(data.edges, (id) => primaryKey(project, id), (id) => primaryKey(project, id)),
    linked_projects: scopedLinked,
    missed_graph: scopedMissed,
  };
}

export async function fetchLayout(
  project: string,
  maxNodes = GRAPH_RENDER_NODE_LIMIT,
  onProgress?: (progress: LoadProgress) => void,
  graph: GraphVariant = "code",
): Promise<GraphData> {
  const params = new URLSearchParams({ project, max_nodes: String(maxNodes) });
  if (graph === "missed") params.set("graph", "missed");
  const res = await fetch(`/api/layout?${params}`);

  if (!res.ok) {
    const body = await res.json().catch(() => ({ error: res.statusText }));
    throw new Error(body.error ?? `HTTP ${res.status}`);
  }

  /* Stream the body when possible so large budgets show live download
   * progress instead of a silent stall. */
  if (!res.body || !onProgress) {
    return res.json();
  }

  const lengthHeader = res.headers.get("content-length");
  const totalBytes = lengthHeader ? parseInt(lengthHeader, 10) || null : null;
  const reader = res.body.getReader();
  const chunks: Uint8Array[] = [];
  let receivedBytes = 0;

  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(value);
    receivedBytes += value.length;
    onProgress({ receivedBytes, totalBytes });
  }

  const merged = new Uint8Array(receivedBytes);
  let offset = 0;
  for (const chunk of chunks) {
    merged.set(chunk, offset);
    offset += chunk.length;
  }
  return JSON.parse(new TextDecoder().decode(merged));
}

const NO_PROGRESS: LoadProgress = { receivedBytes: 0, totalBytes: null };

export function useGraphData(): UseGraphDataResult {
  const [data, setData] = useState<GraphData | null>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [progress, setProgress] = useState<LoadProgress>(NO_PROGRESS);
  const requestSequence = useRef(0);

  const fetchOverview = useCallback(
    async (project: string, maxNodes?: number, graph: GraphVariant = "code") => {
      const requestId = ++requestSequence.current;
      setLoading(true);
      setError(null);
      setProgress(NO_PROGRESS);
      try {
        const result = await fetchLayout(
          project,
          maxNodes,
          (nextProgress) => {
            if (requestSequence.current === requestId) setProgress(nextProgress);
          },
          graph,
        );
        if (requestSequence.current === requestId) setData(scopeGraphData(result, project));
      } catch (e) {
        if (requestSequence.current === requestId) {
          setError(e instanceof Error ? e.message : "Failed to fetch layout");
        }
      } finally {
        if (requestSequence.current === requestId) setLoading(false);
      }
    },
    [],
  );

  const fetchDetail = useCallback(
    async (project: string, _centerNode: string) => {
      const requestId = ++requestSequence.current;
      setLoading(true);
      setError(null);
      setProgress(NO_PROGRESS);
      try {
        /* TODO: detail level with center_node filtering */
        const result = await fetchLayout(project, undefined, (nextProgress) => {
          if (requestSequence.current === requestId) setProgress(nextProgress);
        });
        if (requestSequence.current === requestId) setData(scopeGraphData(result, project));
      } catch (e) {
        if (requestSequence.current === requestId) {
          setError(e instanceof Error ? e.message : "Failed to fetch layout");
        }
      } finally {
        if (requestSequence.current === requestId) setLoading(false);
      }
    },
    [],
  );

  return { data, loading, error, progress, fetchOverview, fetchDetail };
}
