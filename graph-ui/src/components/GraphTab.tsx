import { useEffect, useState, useCallback, useMemo } from "react";
import { Button } from "@/components/ui/button";
import {
  useGraphData,
  clampNodeBudget,
  GRAPH_RENDER_NODE_LIMIT,
  GRAPH_NODE_BUDGET_STEP,
  GRAPH_NODE_BUDGET_MAX,
} from "../hooks/useGraphData";
import { GraphLoader } from "./GraphLoader";
import { DisplaySettingsMenu } from "./DisplaySettingsMenu";
import {
  loadDisplaySettings,
  saveDisplaySettings,
  type DisplaySettings,
} from "../lib/density";
import {
  GraphScene,
  computeCameraTarget,
  type CameraTarget,
  type GraphEdgeSelection,
} from "./GraphScene";
import { Sidebar } from "./Sidebar";
import { FilterPanel } from "./FilterPanel";
import { NodeDetailPanel } from "./NodeDetailPanel";
import { MissedCallout } from "./MissedCallout";
import { ResizeHandle } from "./ResizeHandle";
import { ErrorBoundary } from "./ErrorBoundary";
import type { GraphNode, GraphData, RepoInfo } from "../lib/types";
import { colorForStatus } from "../lib/colors";
import { AnalysisModeBar, type AnalysisMode } from "./analysis/AnalysisModeBar";
import { ImpactView } from "./analysis/ImpactView";
import { HotspotsView } from "./analysis/HotspotsView";
import { ImpactQueryView } from "./analysis/ImpactQueryView";
import { ContextView } from "./analysis/ContextView";
import { ReviewChangeView } from "./analysis/ReviewChangeView";

/* Persist panel widths */
function loadWidth(key: string, fallback: number, min: number, max: number): number {
  try {
    const v = localStorage.getItem(key);
    if (v) return Math.max(min, Math.min(max, parseInt(v, 10)));
  } catch { /* ignore */ }
  return fallback;
}
function saveWidth(key: string, value: number) {
  try { localStorage.setItem(key, String(Math.round(value))); } catch { /* ignore */ }
}

/* Persist the node budget per project */
function budgetKey(project: string): string {
  return `cbm-node-budget:${project}`;
}
function loadNodeBudget(project: string): number {
  try {
    const v = localStorage.getItem(budgetKey(project));
    if (v) return clampNodeBudget(parseInt(v, 10));
  } catch { /* ignore */ }
  return GRAPH_RENDER_NODE_LIMIT;
}
function saveNodeBudget(project: string, value: number) {
  try { localStorage.setItem(budgetKey(project), String(value)); } catch { /* ignore */ }
}

interface GraphTabProps {
  project: string | null;
}

export function formatGraphLimitNotice(data: GraphData | null): string | null {
  if (!data || data.total_nodes <= data.nodes.length) return null;
  return `已显示 ${data.total_nodes.toLocaleString("zh-CN")} 个节点中的 ${data.nodes.length.toLocaleString("zh-CN")} 个（${data.edges.length.toLocaleString("zh-CN")} 条关系）。可提高节点上限或使用筛选条件。`;
}

export function GraphTab({ project }: GraphTabProps) {
  const { data, loading, error, progress, fetchOverview } = useGraphData();
  const [analysisMode, setAnalysisMode] = useState<AnalysisMode>("explore");
  const [highlightedIds, setHighlightedIds] = useState<Set<number> | null>(null);
  const [selectedPath, setSelectedPath] = useState<string | null>(null);
  const [selectedNode, setSelectedNode] = useState<GraphNode | null>(null);
  const [selectedEdge, setSelectedEdge] = useState<GraphEdgeSelection | null>(null);
  const [cameraTarget, setCameraTarget] = useState<CameraTarget | null>(null);
  const [repoInfo, setRepoInfo] = useState<RepoInfo | null>(null);
  const [showLabels, setShowLabels] = useState(true);
  const [display, setDisplay] = useState<DisplaySettings>(() =>
    loadDisplaySettings(),
  );
  const updateDisplay = useCallback((next: DisplaySettings) => {
    setDisplay(next);
    saveDisplaySettings(next);
  }, []);
  const [leftWidth, setLeftWidth] = useState(() =>
    loadWidth("cbm-left-w", 260, 150, 500),
  );
  const [rightWidth, setRightWidth] = useState(() =>
    loadWidth("cbm-right-w", 320, 240, 760),
  );
  const [detailExpanded, setDetailExpanded] = useState(false);
  const limitNotice = formatGraphLimitNotice(data);

  /* Node budget — keyed to its project so switching projects re-reads the
   * persisted value and triggers exactly one fetch. */
  const [budget, setBudget] = useState<{ project: string | null; value: number }>(
    { project: null, value: GRAPH_RENDER_NODE_LIMIT },
  );
  const [budgetDraft, setBudgetDraft] = useState(String(GRAPH_RENDER_NODE_LIMIT));

  const commitBudget = useCallback(() => {
    const parsed = clampNodeBudget(parseInt(budgetDraft, 10));
    setBudgetDraft(String(parsed));
    if (project && parsed !== budget.value) {
      saveNodeBudget(project, parsed);
      setBudget({ project, value: parsed });
    }
  }, [budgetDraft, project, budget.value]);

  /* Filter state — all enabled by default */
  const [enabledLabels, setEnabledLabels] = useState<Set<string>>(new Set());
  const [enabledEdgeTypes, setEnabledEdgeTypes] = useState<Set<string>>(new Set());

  /* Missed skeleton (#963): the file structure of files the indexer could
   * not fully cover, shown as a white satellite cluster beside the code
   * galaxy. Toggle only hides/shows it — the data rides along with every
   * code-graph layout. */
  const [showMissedSkeleton, setShowMissedSkeleton] = useState(true);

  /* Dead-code view: recolor by status + status-based filters */
  const [deadCodeView, setDeadCodeView] = useState(false);
  const [showOnlyDead, setShowOnlyDead] = useState(false);
  const [hideEntryPoints, setHideEntryPoints] = useState(false);
  const [hideTests, setHideTests] = useState(false);

  /* Initialize filters when data loads */
  useEffect(() => {
    if (!data) return;
    const labels = new Set(data.nodes.map((n) => n.label));
    const types = new Set(data.edges.map((e) => e.type));
    for (const lp of data.linked_projects ?? []) {
      for (const n of lp.nodes) labels.add(n.label);
      for (const e of lp.edges) types.add(e.type);
      for (const e of lp.cross_edges) types.add(e.type);
    }
    setEnabledLabels(labels);
    setEnabledEdgeTypes(types);
  }, [data]);

  /* Compute filtered data */
  const filteredData: GraphData | null = useMemo(() => {
    if (!data) return null;

    /* Status-based filters (dead-code view) */
    const statusOk = (n: GraphNode) => {
      if (showOnlyDead && n.status !== "dead") return false;
      if (hideEntryPoints && n.status === "entry") return false;
      if (hideTests && n.status === "test") return false;
      return true;
    };
    /* Recolor by status when the dead-code view is on */
    const paint = (n: GraphNode): GraphNode =>
      deadCodeView ? { ...n, color: colorForStatus(n.status) } : n;
    const keep = (n: GraphNode) => enabledLabels.has(n.label) && statusOk(n);

    const nodes = data.nodes.filter(keep).map(paint);
    const nodeIds = new Set(nodes.map((n) => n.id));
    const edges = data.edges.filter(
      (e) =>
        enabledEdgeTypes.has(e.type) &&
        nodeIds.has(e.source) &&
        nodeIds.has(e.target),
    );

    const linked_projects = data.linked_projects?.map((lp) => {
      const lpNodes = lp.nodes.filter(keep).map(paint);
      const lpIds = new Set(lpNodes.map((n) => n.id));
      const lpEdges = lp.edges.filter(
        (e) =>
          enabledEdgeTypes.has(e.type) && lpIds.has(e.source) && lpIds.has(e.target),
      );
      const crossEdges = lp.cross_edges.filter(
        (e) =>
          enabledEdgeTypes.has(e.type) && nodeIds.has(e.source) && lpIds.has(e.target),
      );
      return { ...lp, nodes: lpNodes, edges: lpEdges, cross_edges: crossEdges };
    });

    return { nodes, edges, total_nodes: data.total_nodes, linked_projects };
  }, [
    data,
    enabledLabels,
    enabledEdgeTypes,
    deadCodeView,
    showOnlyDead,
    hideEntryPoints,
    hideTests,
  ]);

  /* Detail inspection is independent of visual filters. A filtered-out
   * neighbor should still be available in the selected node's relationship
   * list, including linked-project and cross-project edges. */
  const detailGraph = useMemo(() => {
    if (!data) {
      return { nodes: [] as GraphNode[], edges: [] as GraphData["edges"] };
    }
    const nodes = [...data.nodes];
    const edges = [...data.edges];
    for (const linked of data.linked_projects ?? []) {
      nodes.push(...linked.nodes);
      edges.push(...linked.edges, ...linked.cross_edges);
    }
    return { nodes, edges };
  }, [data]);

  /* Re-read the persisted budget when the project changes… */
  useEffect(() => {
    if (project) {
      const value = loadNodeBudget(project);
      setBudget({ project, value });
      setBudgetDraft(String(value));
    }
  }, [project]);

  /* …and fetch only once budget and project agree (one fetch per change). */
  useEffect(() => {
    if (project && budget.project === project) {
      fetchOverview(project, budget.value);
      setHighlightedIds(null);
      setSelectedPath(null);
      setSelectedNode(null);
      setSelectedEdge(null);
      setDetailExpanded(false);
    }
  }, [project, budget, fetchOverview]);

  /* Missed skeleton: offset into place and paint white — a ghost of the
   * files the graph could not fully cover, sitting beside the galaxy. */
  const missedSkeleton = useMemo(() => {
    const mg = data?.missed_graph;
    if (!mg || mg.nodes.length === 0) return null;
    const nodes = mg.nodes.map((n) => ({
      ...n,
      x: n.x + mg.offset.x,
      y: n.y + mg.offset.y,
      z: n.z + mg.offset.z,
      color: "#e9eef5",
    }));
    return { nodes, edges: mg.edges, ids: new Set(nodes.map((n) => n.id)) };
  }, [data]);

  /* Overview framing: both clusters (galaxy + skeleton) in one shot. */
  const overviewTarget = useMemo(() => {
    if (!data) return null;
    const all = missedSkeleton ? [...data.nodes, ...missedSkeleton.nodes] : data.nodes;
    return computeCameraTarget(all, new Set(all.map((n) => n.id)));
  }, [data, missedSkeleton]);

  /* Auto-frame the full graph on load. When a missed skeleton exists, the
   * same overview target includes both clusters. */
  useEffect(() => {
    if (overviewTarget) {
      setCameraTarget(overviewTarget);
    }
  }, [overviewTarget]);

  /* Clicking empty space while the skeleton has focus flies back to the
   * overview (the galaxy may be entirely off-screen at that point, so there
   * is no code node to click). No-op during normal galaxy exploration. */
  const handleBackgroundClick = useCallback(() => {
    if (selectedEdge) {
      setSelectedEdge(null);
      setHighlightedIds(null);
      setSelectedPath(null);
      setCameraTarget(null);
      return;
    }
    if (selectedNode && missedSkeleton?.ids.has(selectedNode.id) && overviewTarget) {
      setSelectedNode(null);
      setHighlightedIds(null);
      setSelectedPath(null);
      setCameraTarget(overviewTarget);
    }
  }, [selectedEdge, selectedNode, missedSkeleton, overviewTarget]);

  /* Fetch git remote metadata for GitHub deep-links */
  useEffect(() => {
    if (!project) {
      setRepoInfo(null);
      return;
    }
    let cancelled = false;
    fetch(`/api/repo-info?project=${encodeURIComponent(project)}`)
      .then((r) => (r.ok ? r.json() : null))
      .then((d) => {
        if (!cancelled && d && !d.error) setRepoInfo(d as RepoInfo);
      })
      .catch(() => {});
    return () => {
      cancelled = true;
    };
  }, [project]);

  const handleSelectPath = useCallback(
    (path: string, nodeIds: Set<number>) => {
      if (!filteredData || !path || nodeIds.size === 0) {
        setSelectedEdge(null);
        setHighlightedIds(null);
        setSelectedPath(null);
        setCameraTarget(null);
        return;
      }
      setSelectedEdge(null);
      setSelectedPath(path);
      setHighlightedIds(nodeIds);
      setCameraTarget(computeCameraTarget(filteredData.nodes, nodeIds));
    },
    [filteredData],
  );

  const handleNodeClick = useCallback(
    (node: GraphNode) => {
      if (!filteredData) return;
      setSelectedEdge(null);

      /* Clicking the missed skeleton re-centers the camera on that whole
       * cluster (it's small — the natural focus unit is the skeleton, not a
       * single node); clicking any code node flies back to the code galaxy
       * via the normal per-node focus below. */
      if (missedSkeleton?.ids.has(node.id)) {
        setSelectedNode(node);
        setHighlightedIds(null);
        setSelectedPath(node.file_path ?? null);
        setCameraTarget(computeCameraTarget(missedSkeleton.nodes, missedSkeleton.ids));
        return;
      }

      setSelectedNode(node);

      /* Highlight the node and its direct connections */
      const connectedIds = new Set([node.id]);
      for (const edge of filteredData.edges) {
        if (edge.source === node.id) connectedIds.add(edge.target);
        if (edge.target === node.id) connectedIds.add(edge.source);
      }
      setHighlightedIds(connectedIds);
      setSelectedPath(node.file_path ?? null);
      setCameraTarget(computeCameraTarget(filteredData.nodes, connectedIds));
    },
    [filteredData, missedSkeleton],
  );

  const handleEdgeClick = useCallback((selection: GraphEdgeSelection) => {
    const endpointIds = new Set([
      selection.sourceNode.id,
      selection.targetNode.id,
    ]);
    setSelectedEdge(selection);
    setSelectedNode(null);
    setDetailExpanded(false);
    setSelectedPath(null);
    setHighlightedIds(endpointIds);
    setCameraTarget(
      computeCameraTarget(
        [selection.sourceNode, selection.targetNode],
        endpointIds,
      ),
    );
  }, []);

  const handleNavigateToNode = useCallback(
    (node: GraphNode) => {
      handleNodeClick(node);
    },
    [handleNodeClick],
  );

  const handleOpenExplore = useCallback(
    (node: GraphNode) => {
      setAnalysisMode("explore");
      handleNodeClick(node);
    },
    [handleNodeClick],
  );

  const closeDetailPanel = useCallback(() => {
    setSelectedNode(null);
    setSelectedEdge(null);
    setHighlightedIds(null);
    setSelectedPath(null);
    setDetailExpanded(false);
  }, []);

  const toggleLabel = useCallback((label: string) => {
    setEnabledLabels((prev) => {
      const next = new Set(prev);
      if (next.has(label)) next.delete(label);
      else next.add(label);
      return next;
    });
  }, []);

  const toggleEdgeType = useCallback((type: string) => {
    setEnabledEdgeTypes((prev) => {
      const next = new Set(prev);
      if (next.has(type)) next.delete(type);
      else next.add(type);
      return next;
    });
  }, []);

  const enableAll = useCallback(() => {
    if (!data) return;
    const labels = new Set(data.nodes.map((n) => n.label));
    const types = new Set(data.edges.map((e) => e.type));
    for (const lp of data.linked_projects ?? []) {
      for (const n of lp.nodes) labels.add(n.label);
      for (const e of lp.edges) types.add(e.type);
      for (const e of lp.cross_edges) types.add(e.type);
    }
    setEnabledLabels(labels);
    setEnabledEdgeTypes(types);
  }, [data]);

  const disableAll = useCallback(() => {
    setEnabledLabels(new Set());
    setEnabledEdgeTypes(new Set());
  }, []);

  if (!project) {
    return (
      <div className="flex items-center justify-center h-full">
        <p className="text-white/30 text-sm">
          请先在“项目”页签中选择一个项目
        </p>
      </div>
    );
  }

  if (loading) {
    return (
      <div className="flex items-center justify-center h-full">
        <GraphLoader nodeBudget={budget.value} progress={progress} />
      </div>
    );
  }

  if (error) {
    return (
      <div className="flex items-center justify-center h-full">
        <div className="text-center p-8">
          <p className="text-red-400 text-sm mb-2">{error}</p>
          <Button variant="outline" size="sm" onClick={() => fetchOverview(project)}>
            重试
          </Button>
        </div>
      </div>
    );
  }

  /* No data, or the project genuinely has no nodes — there are no filters to
     interact with, so show a plain full-screen message. The "all filtered out"
     case is handled inside the layout below so the filter sidebar stays put. */
  if (!data || !filteredData || data.nodes.length === 0) {
    return (
      <div className="flex items-center justify-center h-full">
        <p className="text-white/30 text-sm">该项目中没有节点</p>
      </div>
    );
  }

  return (
    <div className="h-full flex flex-col">
      <AnalysisModeBar mode={analysisMode} onChange={setAnalysisMode} />
      <div className="flex-1 min-h-0">
        {analysisMode === "query" ? (
          <ImpactQueryView project={project} data={data} onOpenExplore={handleOpenExplore} />
        ) : analysisMode === "review" ? (
          <ReviewChangeView project={project} data={data} onOpenExplore={handleOpenExplore} />
        ) : analysisMode === "context" ? (
          <ContextView project={project} data={data} onOpenExplore={handleOpenExplore} />
        ) : analysisMode === "impact" ? (
          <ImpactView project={project} data={data} onOpenExplore={handleOpenExplore} />
        ) : analysisMode === "hotspots" ? (
          <HotspotsView project={project} data={data} onOpenExplore={handleOpenExplore} />
        ) : (
          <div className="h-full flex">
      {/* Left sidebar — resizable */}
      <div
        className="hidden md:flex border-r border-border/30 flex-col h-full bg-[#0b1920]/90 backdrop-blur-md shrink-0"
        style={{ width: leftWidth }}
      >
        <FilterPanel
          data={data}
          enabledLabels={enabledLabels}
          enabledEdgeTypes={enabledEdgeTypes}
          showLabels={showLabels}
          onToggleLabel={toggleLabel}
          onToggleEdgeType={toggleEdgeType}
          onToggleShowLabels={() => setShowLabels((v) => !v)}
          onEnableAll={enableAll}
          onDisableAll={disableAll}
          deadCodeView={deadCodeView}
          showOnlyDead={showOnlyDead}
          hideEntryPoints={hideEntryPoints}
          hideTests={hideTests}
          onToggleDeadCodeView={() => setDeadCodeView((v) => !v)}
          onToggleShowOnlyDead={() => setShowOnlyDead((v) => !v)}
          onToggleHideEntryPoints={() => setHideEntryPoints((v) => !v)}
          onToggleHideTests={() => setHideTests((v) => !v)}
          missedView={showMissedSkeleton}
          missedCount={data?.missed_graph?.nodes.filter((n) => n.label === "File").length ?? 0}
          onToggleMissedView={() => setShowMissedSkeleton((v) => !v)}
        />
        <Sidebar
          nodes={filteredData.nodes}
          onSelectPath={handleSelectPath}
          selectedPath={selectedPath}
        />
      </div>
      <div className="hidden md:block">
        <ResizeHandle
          side="left"
          onResize={(d) => {
            setLeftWidth((w) => {
              const nw = Math.max(150, Math.min(500, w + d));
              saveWidth("cbm-left-w", nw);
              return nw;
            });
          }}
        />
      </div>

      {/* Graph area */}
      <div className="flex-1 relative overflow-hidden">
        {filteredData.nodes.length === 0 ? (
          <div className="flex items-center justify-center h-full">
            <div className="text-center">
              <p className="text-white/30 text-sm mb-3">所有节点均已被筛除</p>
              <Button size="sm" onClick={enableAll}>
                重置筛选
              </Button>
            </div>
          </div>
        ) : (
          <>
            <ErrorBoundary>
              <GraphScene
                data={filteredData}
                missed={showMissedSkeleton ? missedSkeleton : null}
                highlightedIds={highlightedIds}
                cameraTarget={cameraTarget}
                showLabels={showLabels}
                display={display}
                selectedEdge={selectedEdge}
                onNodeClick={handleNodeClick}
                onEdgeClick={handleEdgeClick}
                onBackgroundClick={handleBackgroundClick}
              />
            </ErrorBoundary>

            {/* HUD */}
            <div className="absolute top-4 left-4 right-20 sm:right-auto sm:max-w-[65%] text-[11px] text-white/30 pointer-events-none font-mono">
              <p>
                {filteredData.nodes.length.toLocaleString("zh-CN")} 个节点 /{" "}
                {filteredData.edges.length.toLocaleString("zh-CN")} 条关系
              </p>
              {data.nodes.length > filteredData.nodes.length && (
                <p className="text-white/25 mt-0.5">
                  已从 {data.nodes.length.toLocaleString("zh-CN")} 个节点中筛选
                </p>
              )}
              {limitNotice && (
                <p className="text-amber-300/80 mt-0.5">{limitNotice}</p>
              )}
              {highlightedIds && highlightedIds.size > 0 && (
                <p className="text-cyan-400/50 mt-0.5">
                  已选择 {highlightedIds.size} 个
                </p>
              )}
            </div>

            <div className="absolute top-4 right-4 flex flex-wrap justify-end gap-2 items-center max-w-[calc(100%_-_2rem)]">
              {highlightedIds && (
                <Button
                  size="sm"
                  onClick={() => {
                    setSelectedEdge(null);
                    setHighlightedIds(null);
                    setSelectedPath(null);
                    setSelectedNode(null);
                    setCameraTarget(null);
                  }}
                >
                  清除选择
                </Button>
              )}
              <div className="hidden sm:flex items-center gap-1.5 h-8 px-2 rounded-md border border-border/50 bg-[#0b1920]/80 backdrop-blur-sm">
                <label
                  htmlFor="node-budget"
                  className="text-[10px] uppercase tracking-wider text-white/40"
                >
                  节点上限
                </label>
                <input
                  id="node-budget"
                  type="number"
                  min={GRAPH_NODE_BUDGET_STEP}
                  max={GRAPH_NODE_BUDGET_MAX}
                  step={GRAPH_NODE_BUDGET_STEP}
                  value={budgetDraft}
                  onChange={(e) => setBudgetDraft(e.target.value)}
                  onBlur={commitBudget}
                  onKeyDown={(e) => {
                    if (e.key === "Enter") {
                      e.currentTarget.blur();
                    }
                  }}
                  className="w-24 bg-transparent text-right text-xs font-mono text-cyan-200/90 outline-none [appearance:textfield] [&::-webkit-outer-spin-button]:appearance-none [&::-webkit-inner-spin-button]:appearance-none"
                  aria-label="节点上限：要加载的节点数量"
                  title="要加载的节点数量（每次调整 5,000 个，节点之间的关系会自动加载）"
                />
              </div>
              <div className="hidden sm:block">
                <DisplaySettingsMenu settings={display} onChange={updateDisplay} />
              </div>
              <Button
                variant="outline"
                size="sm"
                onClick={() => {
                  setHighlightedIds(null);
                  setSelectedPath(null);
                  setSelectedNode(null);
                  setCameraTarget(null);
                  fetchOverview(project, budget.value);
                }}
              >
                刷新
              </Button>
            </div>
          </>
        )}
      </div>

      {/* Right detail panel — resizable */}
      {selectedNode && filteredData && (
        <>
          <div className="hidden md:block">
            <ResizeHandle
              side="right"
              onResize={(d) => {
                setDetailExpanded(false);
                setRightWidth((w) => {
                  const nw = Math.max(240, Math.min(760, w + d));
                  saveWidth("cbm-right-w", nw);
                  return nw;
                });
              }}
            />
          </div>
          <div
            className={`fixed inset-x-2 top-24 bottom-2 z-30 border border-border bg-background shadow-2xl overflow-hidden md:static md:inset-auto md:z-auto md:border-y-0 md:border-r-0 md:border-l md:shadow-none md:shrink-0 md:h-full ${
              detailExpanded ? "transition-[width] duration-200" : ""
            }`}
            style={{
              width: detailExpanded
                ? "min(860px, calc(100vw - 16px))"
                : `min(${rightWidth}px, calc(100vw - 16px))`,
              maxWidth: "calc(100vw - 16px)",
              maxHeight: "100%",
            }}
          >
            {missedSkeleton?.ids.has(selectedNode.id) ? (
              /* Skeleton node: the standard panel (code snippet, callers) is
               * meaningless for a not-fully-indexed file — show the coverage
               * callout with its report-the-edge-case actions instead. */
              <MissedCallout
                node={selectedNode}
                project={project}
                onClose={closeDetailPanel}
              />
            ) : (
              <NodeDetailPanel
                node={selectedNode}
                allNodes={detailGraph.nodes}
                allEdges={detailGraph.edges}
                project={project}
                repoInfo={repoInfo}
                expanded={detailExpanded}
                onToggleExpanded={() => setDetailExpanded((value) => !value)}
                onClose={closeDetailPanel}
                onNavigate={handleNavigateToNode}
              />
            )}
          </div>
        </>
      )}
          </div>
        )}
      </div>
    </div>
  );
}
