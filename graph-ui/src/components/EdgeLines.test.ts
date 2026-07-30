import { describe, expect, it } from "vitest";
import { edgeFromIntersection, edgeIntensityBoost } from "./EdgeLines";
import type { GraphEdge } from "../lib/types";

describe("EdgeLines interaction helpers", () => {
  const edges: GraphEdge[] = [
    { source: 1, target: 2, type: "CALLS" },
    { source: 2, target: 3, type: "CROSS_HTTP_CALLS" },
  ];

  it("gives cross-project HTTP edges a stronger visual intensity", () => {
    expect(edgeIntensityBoost("CROSS_HTTP_CALLS")).toBeGreaterThan(
      edgeIntensityBoost("HTTP_CALLS"),
    );
    expect(edgeIntensityBoost("CALLS")).toBe(1);
  });

  it("maps the THREE line-segment vertex index back to its graph edge", () => {
    expect(edgeFromIntersection(edges, 0)).toBe(edges[0]);
    expect(edgeFromIntersection(edges, 2)).toBe(edges[1]);
    expect(edgeFromIntersection(edges, undefined)).toBeNull();
    expect(edgeFromIntersection(edges, 4)).toBeNull();
  });
});
