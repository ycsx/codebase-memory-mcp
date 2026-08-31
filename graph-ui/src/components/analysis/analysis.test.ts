import { describe, expect, it } from "vitest";
import type { GraphData } from "../../lib/types";
import { buildHotspotModel, buildImpactModel, shortPath } from "./analysis";

const DATA: GraphData = {
  nodes: [
    {
      id: 1,
      x: 0,
      y: 0,
      z: 0,
      label: "Function",
      name: "saveOrder",
      qualified_name: "demo.src.orders.saveOrder",
      file_path: "src/orders.ts",
      size: 1,
      color: "#fff",
    },
    {
      id: 2,
      x: 0,
      y: 0,
      z: 0,
      label: "Function",
      name: "checkout",
      qualified_name: "demo.src.checkout.checkout",
      file_path: "src/checkout.ts",
      size: 1,
      color: "#fff",
    },
    {
      id: 3,
      x: 0,
      y: 0,
      z: 0,
      label: "Route",
      name: "POST /checkout",
      qualified_name: "demo.routes.checkout",
      file_path: "src/routes.ts",
      size: 1,
      color: "#fff",
    },
    {
      id: 4,
      x: 0,
      y: 0,
      z: 0,
      label: "Function",
      name: "unrelated",
      qualified_name: "demo.src.unrelated",
      file_path: "src/unrelated.ts",
      size: 1,
      color: "#fff",
    },
  ],
  edges: [
    { source: 2, target: 1, type: "CALLS" },
    { source: 3, target: 2, type: "CALLS" },
    { source: 4, target: 1, type: "DEFINES" },
  ],
  total_nodes: 4,
};

describe("buildImpactModel", () => {
  it("builds deterministic inbound impact layers with relationship evidence", () => {
    const model = buildImpactModel(
      DATA,
      {
        changed_files: ["src/orders.ts"],
        changed_count: 1,
        impacted_symbols: [
          { name: "saveOrder", label: "Function", file: "src/orders.ts" },
        ],
        depth: 2,
      },
      2,
    );

    expect(model.layers.map((layer) => layer.map((node) => node.name))).toEqual([
      ["saveOrder"],
      ["checkout"],
      ["POST /checkout"],
    ]);
    expect(model.layers[1][0]).toMatchObject({
      risk: "critical",
      evidence: {
        sourceName: "checkout",
        targetName: "saveOrder",
        relationship: "CALLS",
      },
    });
    expect(model.layers[2][0].risk).toBe("high");
    expect(model.links).toHaveLength(2);
  });

  it("keeps unindexed changed symbols visible as stable seed nodes", () => {
    const model = buildImpactModel(
      DATA,
      {
        changed_files: ["src/new-file.ts"],
        changed_count: 1,
        impacted_symbols: [
          { name: "newHandler", label: "Function", file: "src/new-file.ts" },
        ],
        depth: 2,
      },
      2,
    );

    expect(model.layers[0][0]).toMatchObject({
      name: "newHandler",
      file: "src/new-file.ts",
      risk: "changed",
    });
    expect(model.layers[0][0].graphNode).toBeUndefined();
  });

  it("keeps every changed symbol and still expands impact beyond the former 48-node limit", () => {
    const changedNodes = Array.from({ length: 60 }, (_, index) => ({
      id: index + 10,
      x: 0,
      y: 0,
      z: 0,
      label: "Function",
      name: `changed${index}`,
      qualified_name: `demo.src.changed${index}`,
      file_path: `src/changed${index}.ts`,
      size: 1,
      color: "#fff",
    }));
    const caller = {
      id: 100,
      x: 0,
      y: 0,
      z: 0,
      label: "Function",
      name: "caller",
      qualified_name: "demo.src.caller",
      file_path: "src/caller.ts",
      size: 1,
      color: "#fff",
    };
    const graph: GraphData = {
      nodes: [...changedNodes, caller],
      edges: [{ source: caller.id, target: changedNodes[0].id, type: "CALLS" }],
      total_nodes: 61,
    };

    const model = buildImpactModel(
      graph,
      {
        changed_files: changedNodes.map((node) => node.file_path),
        changed_count: changedNodes.length,
        impacted_symbols: changedNodes.map((node) => ({
          name: node.name,
          label: node.label,
          file: node.file_path,
        })),
        depth: 1,
      },
      1,
    );

    expect(model.layers[0]).toHaveLength(60);
    expect(model.layers[1].map((node) => node.name)).toEqual(["caller"]);
    expect(model.totalNodes).toBe(61);
    expect(model.truncated).toBe(false);
  });

  it("does not leak a linked project's same numeric ID into primary impact", () => {
    const graph: GraphData = {
      nodes: [
        { ...DATA.nodes[0], graph_key: "primary:app:1", graph_project: "app" },
      ],
      edges: [],
      total_nodes: 1,
      linked_projects: [
        {
          project: "dependency",
          nodes: [
            { ...DATA.nodes[0], graph_key: "linked:dependency:1", graph_project: "dependency" },
            {
              ...DATA.nodes[1],
              id: 2,
              graph_key: "linked:dependency:2",
              graph_project: "dependency",
            },
          ],
          edges: [
            {
              source: 2,
              target: 1,
              type: "CALLS",
              source_key: "linked:dependency:2",
              target_key: "linked:dependency:1",
            },
          ],
          cross_edges: [],
          offset: { x: 0, y: 0, z: 0 },
        },
      ],
    };

    const model = buildImpactModel(
      graph,
      {
        changed_files: ["src/orders.ts"],
        changed_count: 1,
        impacted_symbols: [{ name: "saveOrder", label: "Function", file: "src/orders.ts" }],
        depth: 1,
      },
      1,
    );

    expect(model.layers[0].map((node) => node.name)).toEqual(["saveOrder"]);
    expect(model.layers[1]).toHaveLength(0);
  });
});

describe("buildHotspotModel", () => {
  it("joins authoritative architecture fan-in to rendered nodes and file risk", () => {
    const model = buildHotspotModel(DATA, [
      { name: "saveOrder", qualified_name: "demo.src.orders.saveOrder", fan_in: 12 },
      { name: "checkout", qualified_name: "demo.src.checkout.checkout", fan_in: 5 },
    ]);

    expect(model.rows.map((row) => row.name)).toEqual(["saveOrder", "checkout"]);
    expect(model.rows[0]).toMatchObject({
      file: "src/orders.ts",
      risk: "critical",
      graphNode: DATA.nodes[0],
    });
    expect(model.rows[1].risk).toBe("high");
    expect(model.files[0]).toMatchObject({
      file: "src/orders.ts",
      fanIn: 12,
      hotspotCount: 1,
    });
  });

  it("uses a Chinese placeholder when a ranked symbol is outside the loaded graph", () => {
    const model = buildHotspotModel(DATA, [
      { name: "remoteHotspot", qualified_name: "demo.src.remoteHotspot", fan_in: 8 },
    ]);

    expect(model.rows[0].file).toBe("当前加载范围内不可见");
    expect(model.rows[0].graphNode).toBeUndefined();
  });
});

describe("shortPath", () => {
  it("normalizes separators and keeps the requested trailing segments", () => {
    expect(shortPath("apps\\api\\src\\orders.ts", 3)).toBe("api/src/orders.ts");
  });
});
