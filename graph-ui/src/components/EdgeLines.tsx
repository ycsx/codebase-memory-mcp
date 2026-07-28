import { useEffect, useMemo } from "react";
import * as THREE from "three";
import type { GraphNode, GraphEdge } from "../lib/types";
import { edgeIntensityScale } from "../lib/density";

interface EdgeLinesProps {
  nodes: GraphNode[];
  edges: GraphEdge[];
  highlightedIds: Set<number> | null;
  opacity?: number;
  /* User edge-brightness multiplier (see DisplaySettings). Layered on top of
   * the automatic density scale. */
  brightness?: number;
  /* When set, edge.target is looked up in this array instead of `nodes`.
   * Used for cross-galaxy edges where source lives in the primary graph
   * and target lives in a linked project's offset-adjusted nodes. */
  targetNodes?: GraphNode[];
  /* Exact edge selection is scoped by GraphScene. Non-selected layers receive
   * selectionActive=true with selectedEdge=null, which hides their edges. */
  selectedEdge?: GraphEdge | null;
  selectionActive?: boolean;
  onEdgeClick?: (edge: GraphEdge) => void;
}

function getClusterKey(fp?: string): string {
  if (!fp) return "";
  const parts = fp.split("/");
  return parts.slice(0, Math.min(2, parts.length)).join("/");
}

/* Edge type → color (matches the filter panel) */
const EDGE_TYPE_COLORS: Record<string, string> = {
  CALLS: "#1DA27E",
  IMPORTS: "#3b82f6",
  DEFINES: "#a855f7",
  DEFINES_METHOD: "#a855f7",
  CONTAINS_FILE: "#22c55e",
  CONTAINS_FOLDER: "#22c55e",
  CONTAINS_PACKAGE: "#22c55e",
  HANDLES: "#eab308",
  IMPLEMENTS: "#f97316",
  HTTP_CALLS: "#e11d48",
  ASYNC_CALLS: "#ec4899",
  GRPC_CALLS: "#f59e0b",
  GRAPHQL_CALLS: "#e879f9",
  TRPC_CALLS: "#a78bfa",
  CROSS_HTTP_CALLS: "#fb923c",
  CROSS_ASYNC_CALLS: "#fb7185",
  CROSS_GRPC_CALLS: "#fbbf24",
  CROSS_GRAPHQL_CALLS: "#f0abfc",
  CROSS_TRPC_CALLS: "#c4b5fd",
  CROSS_CHANNEL: "#fdba74",
  MEMBER_OF: "#64748b",
  TESTS_FILE: "#06b6d4",
};

const DEFAULT_EDGE_COLOR = "#1C8585";
const CROSS_HTTP_INTENSITY_BOOST = 1.8;

export function edgeIntensityBoost(type: string): number {
  return type === "CROSS_HTTP_CALLS" ? CROSS_HTTP_INTENSITY_BOOST : 1;
}

/* THREE.LineSegments reports the start vertex of the hit segment: 0, 2, 4… */
export function edgeFromIntersection(
  renderedEdges: GraphEdge[],
  vertexIndex: number | undefined,
): GraphEdge | null {
  if (vertexIndex === undefined || vertexIndex < 0) return null;
  return renderedEdges[Math.floor(vertexIndex / 2)] ?? null;
}

export function EdgeLines({
  nodes,
  edges,
  highlightedIds,
  opacity = 1.0,
  brightness = 1.0,
  targetNodes,
  selectedEdge = null,
  selectionActive = false,
  onEdgeClick,
}: EdgeLinesProps) {
  const { geometry, renderedEdges } = useMemo(() => {
    /* Shrink per-edge glow as the edge count grows so the additively-blended
     * center doesn't saturate to white; the user multiplier rides on top. */
    const densityScale = edgeIntensityScale(edges.length) * brightness;
    const srcMap = new Map<number, number>();
    for (let i = 0; i < nodes.length; i++) {
      srcMap.set(nodes[i].id, i);
    }
    const tgtArr = targetNodes ?? nodes;
    const tgtMap = targetNodes ? new Map<number, number>() : srcMap;
    if (targetNodes) {
      for (let i = 0; i < targetNodes.length; i++) {
        tgtMap.set(targetNodes[i].id, i);
      }
    }

    const hasHighlight = highlightedIds && highlightedIds.size > 0;
    const positions = new Float32Array(edges.length * 6);
    const colors = new Float32Array(edges.length * 6);
    const visibleEdges: GraphEdge[] = [];
    let validCount = 0;

    for (const edge of edges) {
      const si = srcMap.get(edge.source);
      const ti = tgtMap.get(edge.target);
      if (si === undefined || ti === undefined) continue;

      const s = nodes[si];
      const t = tgtArr[ti];
      const isSelectedEdge = selectionActive && edge === selectedEdge;
      if (selectionActive && !isSelectedEdge) continue;

      const sHL = !hasHighlight || highlightedIds.has(s.id);
      const tHL = !hasHighlight || highlightedIds.has(t.id);
      if (hasHighlight && !sHL && !tHL) continue;

      const sameCluster =
        getClusterKey(s.file_path) === getClusterKey(t.file_path);

      /* Intensity based on cluster membership and highlight.
       * With additive blending + dark background, these glow nicely. */
      let intensity = sameCluster ? 0.25 : 0.06;
      if (isSelectedEdge) {
        intensity = Math.min(2, 1.2 * edgeIntensityBoost(edge.type));
      } else if (hasHighlight) {
        /* A selection stays at full strength (never density-scaled) so it
         * pops against the dimmed rest; only the un-selected bulk is scaled. */
        intensity = sHL && tHL ? 0.5 : 0.04 * densityScale;
      } else {
        intensity *= densityScale;
      }
      if (!isSelectedEdge) intensity *= edgeIntensityBoost(edge.type);

      const off = validCount * 6;
      positions[off] = s.x;
      positions[off + 1] = s.y;
      positions[off + 2] = s.z;
      positions[off + 3] = t.x;
      positions[off + 4] = t.y;
      positions[off + 5] = t.z;

      /* Color from edge TYPE (correlates with edge type filter) */
      const edgeColor = new THREE.Color(
        EDGE_TYPE_COLORS[edge.type] ?? DEFAULT_EDGE_COLOR,
      );
      colors[off] = edgeColor.r * intensity;
      colors[off + 1] = edgeColor.g * intensity;
      colors[off + 2] = edgeColor.b * intensity;
      colors[off + 3] = edgeColor.r * intensity;
      colors[off + 4] = edgeColor.g * intensity;
      colors[off + 5] = edgeColor.b * intensity;
      visibleEdges.push(edge);
      validCount++;
    }

    const geo = new THREE.BufferGeometry();
    geo.setAttribute(
      "position",
      new THREE.BufferAttribute(positions.slice(0, validCount * 6), 3),
    );
    geo.setAttribute(
      "color",
      new THREE.BufferAttribute(colors.slice(0, validCount * 6), 3),
    );
    return { geometry: geo, renderedEdges: visibleEdges };
  }, [
    nodes,
    edges,
    highlightedIds,
    targetNodes,
    brightness,
    selectedEdge,
    selectionActive,
  ]);

  useEffect(() => () => geometry.dispose(), [geometry]);

  return (
    <lineSegments
      geometry={geometry}
      onClick={
        onEdgeClick
          ? (event) => {
              const edge = edgeFromIntersection(renderedEdges, event.index);
              if (!edge) return;
              event.stopPropagation();
              onEdgeClick(edge);
            }
          : undefined
      }
    >
      <lineBasicMaterial
        vertexColors
        transparent
        opacity={opacity}
        blending={THREE.AdditiveBlending}
        depthWrite={false}
        toneMapped={false}
      />
    </lineSegments>
  );
}
