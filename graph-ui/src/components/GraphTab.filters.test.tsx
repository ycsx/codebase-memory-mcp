/* @vitest-environment jsdom */
import "@testing-library/jest-dom/vitest";
import { fireEvent, render, screen } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
import { GraphTab } from "./GraphTab";
import type { GraphEdgeSelection } from "./GraphScene";
import type { GraphData, GraphNode } from "../lib/types";

/* GraphScene renders a WebGL <Canvas> which jsdom can't run — stub it out. */
vi.mock("./GraphScene", () => ({
  GraphScene: ({
    selectedEdge,
    onNodeClick,
    onEdgeClick,
  }: {
    selectedEdge: GraphEdgeSelection | null;
    onNodeClick: (node: GraphNode) => void;
    onEdgeClick: (selection: GraphEdgeSelection) => void;
  }) => (
    <>
      <button type="button" onClick={() => onNodeClick(SAMPLE.nodes[0])}>
        Select foo
      </button>
      <button
        type="button"
        onClick={() =>
          onEdgeClick({
            scope: "primary",
            edge: SAMPLE.edges[0],
            sourceNode: SAMPLE.nodes[0],
            targetNode: SAMPLE.nodes[1],
          })
        }
      >
        Select edge
      </button>
      <span>
        {selectedEdge ? `Selected edge ${selectedEdge.edge.type}` : "No edge selected"}
      </span>
    </>
  ),
  computeCameraTarget: () => null,
}));

const SAMPLE: GraphData = {
  nodes: [
    { id: 1, x: 0, y: 0, z: 0, label: "Function", name: "foo", size: 1, color: "#fff" },
    { id: 2, x: 1, y: 0, z: 0, label: "Class", name: "Bar", size: 1, color: "#fff" },
  ],
  edges: [{ source: 1, target: 2, type: "CALLS" }],
  total_nodes: 2,
};

function mockLayoutFetch(data: GraphData) {
  const fetchMock = vi.fn(async (input: RequestInfo | URL) => {
    const url = String(input);
    if (url.startsWith("/api/layout")) {
      return new Response(JSON.stringify(data), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      });
    }
    return new Response("{}", { status: 200 });
  });
  vi.stubGlobal("fetch", fetchMock);
  return fetchMock;
}

describe("GraphTab filters", () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it("keeps the filter sidebar visible when all nodes are filtered out", async () => {
    mockLayoutFetch(SAMPLE);

    render(<GraphTab project="demo" />);

    /* Wait for the layout to load — the filter panel header appears. */
    expect(await screen.findByText("筛选")).toBeInTheDocument();

    /* Disable every filter via the "None" shortcut. */
    fireEvent.click(screen.getByRole("button", { name: "全不选" }));

    /* The graph area reports that everything is filtered out… */
    expect(screen.getByText("所有节点均已被筛除")).toBeInTheDocument();

    /* …but the filter sidebar must stay so the user can re-enable filters
       instead of being forced to reset everything. */
    expect(screen.getByText("筛选")).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "全部" })).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "全不选" })).toBeInTheDocument();
  });

  it("keeps filtered-out neighbors available in the detail panel", async () => {
    mockLayoutFetch(SAMPLE);

    render(<GraphTab project="demo" />);

    expect(await screen.findByText("筛选")).toBeInTheDocument();
    fireEvent.click(screen.getByRole("button", { name: "类 1" }));
    expect(screen.getByText(/已从 2 个节点中筛选/)).toBeInTheDocument();

    fireEvent.click(screen.getByRole("button", { name: "Select foo" }));

    expect(screen.getByRole("heading", { name: "foo" })).toBeInTheDocument();
    expect(screen.getByRole("button", { name: /Bar/ })).toBeInTheDocument();
  });

  it("stores an edge selection and highlights both endpoints", async () => {
    mockLayoutFetch(SAMPLE);

    render(<GraphTab project="demo" />);

    expect(await screen.findByText("筛选")).toBeInTheDocument();
    fireEvent.click(screen.getByRole("button", { name: "Select edge" }));

    expect(screen.getByText("Selected edge CALLS")).toBeInTheDocument();
    expect(screen.getByText("已选择 2 个")).toBeInTheDocument();

    fireEvent.click(screen.getByRole("button", { name: "清除选择" }));
    expect(screen.getByText("No edge selected")).toBeInTheDocument();
    expect(screen.queryByText("已选择 2 个")).not.toBeInTheDocument();
  });
});
