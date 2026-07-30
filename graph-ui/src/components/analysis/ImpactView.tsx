import { useCallback, useEffect, useMemo, useState } from "react";
import {
  ArrowRight,
  GitBranch,
  LoaderCircle,
  LocateFixed,
  Play,
  TriangleAlert,
  X,
} from "lucide-react";
import { callTool } from "../../api/rpc";
import type { GraphData, GraphNode } from "../../lib/types";
import { graphNodeLabel } from "../../lib/graphLabels";
import {
  buildImpactModel,
  shortPath,
  type DetectChangesResult,
  type ImpactNode,
  type ImpactRisk,
} from "./analysis";

interface ImpactViewProps {
  project: string;
  data: GraphData;
  onOpenExplore: (node: GraphNode) => void;
}

type ComparisonKind = "working" | "previous" | "custom";

const RISK_STYLES: Record<ImpactRisk, { dot: string; border: string; text: string; label: string }> = {
  changed: {
    dot: "bg-cyan-400",
    border: "border-cyan-400/45",
    text: "text-cyan-300",
    label: "已变更",
  },
  critical: {
    dot: "bg-rose-400",
    border: "border-rose-400/45",
    text: "text-rose-300",
    label: "严重",
  },
  high: {
    dot: "bg-amber-400",
    border: "border-amber-400/45",
    text: "text-amber-300",
    label: "高风险",
  },
  medium: {
    dot: "bg-emerald-400",
    border: "border-emerald-400/40",
    text: "text-emerald-300",
    label: "关注",
  },
};

function comparisonRef(kind: ComparisonKind, customRef: string): string {
  if (kind === "previous") return "HEAD~1";
  if (kind === "custom") return customRef.trim();
  return "HEAD";
}

export function ImpactView({ project, data, onOpenExplore }: ImpactViewProps) {
  const [comparison, setComparison] = useState<ComparisonKind>("working");
  const [customRef, setCustomRef] = useState("");
  const [depth, setDepth] = useState(2);
  const [changes, setChanges] = useState<DetectChangesResult | null>(null);
  const [changedFileFilter, setChangedFileFilter] = useState<string | null>(null);
  const [selected, setSelected] = useState<ImpactNode | null>(null);
  const [analyzedRef, setAnalyzedRef] = useState("HEAD");
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const runAnalysis = useCallback(async () => {
    const since = comparisonRef(comparison, customRef);
    if (!since) {
      setError("请输入要比较的 Git ref。");
      return;
    }
    setLoading(true);
    setError(null);
    try {
      const result = await callTool<DetectChangesResult>("detect_changes", {
        project,
        since,
        scope: "impact",
        depth,
      });
      if (result.hint) throw new Error(result.hint);
      const symbols = [...new Map(
        (result.impacted_symbols ?? []).map((symbol) => [
          `${symbol.file}\u0000${symbol.label}\u0000${symbol.name}`,
          symbol,
        ]),
      ).values()];
      setChanges({
        ...result,
        changed_files: [...new Set(result.changed_files ?? [])],
        impacted_symbols: symbols,
      });
      setChangedFileFilter(null);
      setAnalyzedRef(since);
    } catch (cause) {
      setChanges(null);
      setError(cause instanceof Error ? cause.message : "变更检测失败。");
    } finally {
      setLoading(false);
    }
  }, [comparison, customRef, depth, project]);

  useEffect(() => {
    void runAnalysis();
  }, [project]); // The initial comparison is deliberately HEAD, never an assumed branch name.

  const displayedChanges = useMemo(() => {
    if (!changes || !changedFileFilter) return changes;
    return {
      ...changes,
      changed_files: [changedFileFilter],
      impacted_symbols: changes.impacted_symbols.filter(
        (symbol) => symbol.file === changedFileFilter,
      ),
    };
  }, [changedFileFilter, changes]);

  const model = useMemo(
    () => (displayedChanges ? buildImpactModel(data, displayedChanges, depth) : null),
    [data, depth, displayedChanges],
  );

  useEffect(() => {
    setSelected(model?.layers.flat()[0] ?? null);
  }, [model]);

  const positions = useMemo(() => {
    if (!model) return new Map<string, { x: number; y: number }>();
    const next = new Map<string, { x: number; y: number }>();
    model.layers.forEach((layer, layerIndex) => {
      layer.forEach((node, index) => {
        next.set(node.key, { x: 28 + layerIndex * 280, y: 52 + index * 86 });
      });
    });
    return next;
  }, [model]);

  const canvasWidth = Math.max(620, (model?.layers.length ?? 1) * 280 + 20);
  const canvasHeight = Math.max(
    330,
    90 + Math.max(1, ...(model?.layers.map((layer) => layer.length) ?? [1])) * 86,
  );
  const symbolCounts = useMemo(() => {
    const counts = new Map<string, number>();
    for (const symbol of changes?.impacted_symbols ?? []) {
      counts.set(symbol.file, (counts.get(symbol.file) ?? 0) + 1);
    }
    return counts;
  }, [changes]);

  return (
    <div className="h-full flex flex-col bg-[#08151b]">
      <div className="min-h-14 shrink-0 border-b border-border/25 px-4 py-2 flex flex-wrap items-center gap-2.5">
        <GitBranch className="w-4 h-4 text-foreground/35" aria-hidden="true" />
        <label htmlFor="impact-comparison" className="text-[10px] uppercase text-foreground/30">
          比较
        </label>
        <select
          id="impact-comparison"
          value={comparison}
          onChange={(event) => setComparison(event.target.value as ComparisonKind)}
          className="h-8 min-w-40 rounded-md border border-border/40 bg-[#0b1920] px-2.5 text-[11px] text-foreground/70 outline-none focus:border-primary/50"
        >
          <option value="working">工作区 (HEAD)</option>
          <option value="previous">上一个提交 (HEAD~1)</option>
          <option value="custom">自定义 ref</option>
        </select>
        {comparison === "custom" && (
          <input
            aria-label="自定义 Git ref"
            value={customRef}
            onChange={(event) => setCustomRef(event.target.value)}
            placeholder="origin/develop 或 v1.2.0"
            className="h-8 w-52 rounded-md border border-border/40 bg-[#0b1920] px-2.5 text-[11px] font-mono text-foreground/75 outline-none placeholder:text-foreground/20 focus:border-primary/50"
          />
        )}
        <label htmlFor="impact-depth" className="ml-1 text-[10px] uppercase text-foreground/30">
          深度
        </label>
        <select
          id="impact-depth"
          value={depth}
          onChange={(event) => setDepth(Number(event.target.value))}
          className="h-8 w-16 rounded-md border border-border/40 bg-[#0b1920] px-2 text-[11px] text-foreground/70 outline-none focus:border-primary/50"
        >
          <option value={1}>1</option>
          <option value={2}>2</option>
          <option value={3}>3</option>
        </select>
        <button
          type="button"
          onClick={() => void runAnalysis()}
          disabled={loading || (comparison === "custom" && !customRef.trim())}
          className="h-8 px-3 inline-flex items-center gap-1.5 rounded-md bg-primary/15 text-primary hover:bg-primary/25 text-[11px] font-medium transition-colors disabled:opacity-35"
        >
          {loading ? (
            <LoaderCircle className="w-3.5 h-3.5 animate-spin" aria-hidden="true" />
          ) : (
            <Play className="w-3.5 h-3.5" aria-hidden="true" />
          )}
          分析
        </button>
        {changes && !loading && (
          <div className="ml-auto flex items-center gap-3 text-[10px] text-foreground/30 tabular-nums">
            <span>{changes.changed_files.length} 个文件</span>
            <span>{changes.impacted_symbols.length} 个符号</span>
            {model && <span>{model.totalNodes} 个影响节点</span>}
            <span className="font-mono text-foreground/45">与 {analyzedRef} 比较</span>
          </div>
        )}
      </div>

      {error ? (
        <div className="flex-1 flex items-center justify-center p-8">
          <div className="max-w-lg text-center">
            <TriangleAlert className="w-5 h-5 mx-auto mb-3 text-amber-400" aria-hidden="true" />
            <p className="text-[12px] text-foreground/65 mb-1">无法与此 ref 比较</p>
            <p className="text-[11px] text-foreground/30 font-mono break-words">{error}</p>
          </div>
        </div>
      ) : loading && !changes ? (
        <div className="flex-1 flex items-center justify-center gap-2 text-[11px] text-foreground/35">
          <LoaderCircle className="w-4 h-4 animate-spin text-primary" aria-hidden="true" />
          正在追踪变更符号
        </div>
      ) : changes && model ? (
        <div className="flex-1 min-h-0 overflow-auto">
          <div className="min-h-full grid grid-cols-1 lg:h-full lg:min-w-[940px] lg:grid-cols-[220px_minmax(500px,1fr)_280px]">
            <aside className="min-h-0 max-h-44 border-b border-border/25 flex flex-col lg:max-h-none lg:border-b-0 lg:border-r">
              <div className="h-10 shrink-0 px-3 flex items-center justify-between border-b border-border/20">
                <h2 className="text-[10px] uppercase tracking-wider text-foreground/40">
                  变更文件
                </h2>
                <span className="flex items-center gap-1 text-[10px] text-foreground/25 tabular-nums">
                  {changes.changed_files.length}
                  {changedFileFilter && (
                    <button
                      type="button"
                      onClick={() => setChangedFileFilter(null)}
                      className="w-6 h-6 inline-flex items-center justify-center rounded text-foreground/30 hover:text-foreground/70 hover:bg-white/[0.04]"
                      title="清除文件筛选"
                      aria-label="清除文件筛选"
                    >
                      <X className="w-3.5 h-3.5" aria-hidden="true" />
                    </button>
                  )}
                </span>
              </div>
              <div className="min-h-0 overflow-y-auto py-1">
                {changes.changed_files.map((file) => {
                  const active = changedFileFilter === file;
                  return (
                  <button
                    key={file}
                    type="button"
                    onClick={() => setChangedFileFilter(active ? null : file)}
                    aria-pressed={active}
                    className={`w-full px-3 py-2.5 border-b border-border/10 text-left transition-colors ${
                      active ? "bg-primary/[0.08]" : "hover:bg-white/[0.025]"
                    }`}
                  >
                    <p className="text-[11px] text-foreground/70 font-mono break-all leading-4">
                      {shortPath(file, 4)}
                    </p>
                    <p className="mt-1 text-[10px] text-foreground/25">
                      已索引 {symbolCounts.get(file) ?? 0} 个符号
                    </p>
                  </button>
                  );
                })}
                {changes.changed_files.length === 0 && (
                  <p className="px-3 py-5 text-[11px] text-foreground/25 leading-5">
                    未发现相对于 {analyzedRef} 的变更。
                  </p>
                )}
              </div>
            </aside>

            <section className="min-w-0 min-h-[430px] overflow-auto relative lg:min-h-0" aria-label="影响图">
              {model.totalNodes > 0 ? (
                <div className="relative" style={{ width: canvasWidth, height: canvasHeight }}>
                  <svg
                    className="absolute inset-0 pointer-events-none"
                    width={canvasWidth}
                    height={canvasHeight}
                    aria-hidden="true"
                  >
                    <defs>
                      <marker id="impact-arrow" viewBox="0 0 8 8" refX="7" refY="4" markerWidth="5" markerHeight="5" orient="auto-start-reverse">
                        <path d="M 0 0 L 8 4 L 0 8 z" fill="rgba(148,163,184,.36)" />
                      </marker>
                    </defs>
                    {model.links.map((link) => {
                      const source = positions.get(link.source);
                      const target = positions.get(link.target);
                      if (!source || !target) return null;
                      const x1 = source.x + 220;
                      const y1 = source.y + 33;
                      const x2 = target.x;
                      const y2 = target.y + 33;
                      const bend = Math.max(32, (x2 - x1) * 0.48);
                      return (
                        <path
                          key={`${link.source}:${link.target}`}
                          d={`M ${x1} ${y1} C ${x1 + bend} ${y1}, ${x2 - bend} ${y2}, ${x2} ${y2}`}
                          fill="none"
                          stroke="rgba(148,163,184,.26)"
                          strokeWidth="1.2"
                          markerEnd="url(#impact-arrow)"
                        />
                      );
                    })}
                  </svg>

                  {model.layers.map((layer, layerIndex) => (
                    <div key={layerIndex}>
                      <div
                        className="absolute top-4 flex items-center gap-2 text-[10px] uppercase tracking-wider text-foreground/30"
                        style={{ left: 28 + layerIndex * 280 }}
                      >
                        <span>{layerIndex === 0 ? "已变更" : `影响 +${layerIndex}`}</span>
                        <span className="text-foreground/18 tabular-nums">{layer.length}</span>
                      </div>
                      {layer.map((node) => {
                        const position = positions.get(node.key)!;
                        const style = RISK_STYLES[node.risk];
                        return (
                          <button
                            key={node.key}
                            type="button"
                            onClick={() => setSelected(node)}
                            className={`absolute w-[220px] h-[66px] px-3 text-left rounded-md border bg-[#0d2028] hover:bg-[#112831] transition-colors overflow-hidden ${
                              selected?.key === node.key ? style.border : "border-border/30"
                            }`}
                            style={{ left: position.x, top: position.y }}
                          >
                            <span className={`absolute left-0 top-0 bottom-0 w-0.5 ${style.dot}`} />
                            <span className="flex items-center justify-between gap-2">
                              <span className="text-[11px] font-medium text-foreground/80 truncate">
                                {node.name}
                              </span>
                              <span className={`text-[9px] uppercase ${style.text}`}>{style.label}</span>
                            </span>
                            <span className="mt-1 block text-[10px] text-foreground/25 font-mono truncate">
                              {shortPath(node.file)}
                            </span>
                            <span className="mt-0.5 block text-[9px] text-foreground/20">
                              {graphNodeLabel(node.label)} {node.fanIn > 0 ? `\u00b7 ${node.fanIn} 次入站调用` : ""}
                            </span>
                          </button>
                        );
                      })}
                    </div>
                  ))}
                </div>
              ) : (
                <div className="h-full flex items-center justify-center text-[11px] text-foreground/25">
                  当前图谱中没有变更符号。
                </div>
              )}
            </section>

            <aside className="min-h-56 border-t border-border/25 overflow-y-auto lg:min-h-0 lg:border-t-0 lg:border-l">
              <div className="h-10 px-3 flex items-center border-b border-border/20">
                <h2 className="text-[10px] uppercase tracking-wider text-foreground/40">证据</h2>
              </div>
              {selected ? (
                <div className="p-4">
                  <div className="flex items-start justify-between gap-3 mb-4">
                    <div className="min-w-0">
                      <p className="text-[13px] font-semibold text-foreground/85 break-words">
                        {selected.name}
                      </p>
                      <p className="text-[10px] text-foreground/30 mt-1">{graphNodeLabel(selected.label)}</p>
                    </div>
                    <span className={`text-[9px] uppercase ${RISK_STYLES[selected.risk].text}`}>
                      {RISK_STYLES[selected.risk].label}
                    </span>
                  </div>

                  <dl className="space-y-3 text-[10px]">
                    <div>
                      <dt className="text-foreground/25 uppercase mb-1">文件</dt>
                      <dd className="font-mono text-foreground/60 break-all leading-4">{selected.file}</dd>
                    </div>
                    {selected.graphNode?.qualified_name && (
                      <div>
                        <dt className="text-foreground/25 uppercase mb-1">限定名称</dt>
                        <dd className="font-mono text-foreground/45 break-all leading-4">
                          {selected.graphNode.qualified_name}
                        </dd>
                      </div>
                    )}
                    <div className="grid grid-cols-2 gap-3">
                      <div>
                        <dt className="text-foreground/25 uppercase mb-1">跳数</dt>
                        <dd className="text-foreground/65 tabular-nums">{selected.layer}</dd>
                      </div>
                      <div>
                        <dt className="text-foreground/25 uppercase mb-1">入度</dt>
                        <dd className="text-foreground/65 tabular-nums">{selected.fanIn}</dd>
                      </div>
                    </div>
                  </dl>

                  {selected.evidence && (
                    <div className="mt-5 pt-4 border-t border-border/20">
                      <p className="text-[10px] uppercase text-foreground/25 mb-2">关系证据</p>
                      <div className="flex items-center gap-2 text-[10px] text-foreground/55 min-w-0">
                        <span className="truncate">{selected.evidence.sourceName}</span>
                        <ArrowRight className="w-3 h-3 shrink-0 text-foreground/25" aria-hidden="true" />
                        <span className="truncate">{selected.evidence.targetName}</span>
                      </div>
                      <p className="mt-1.5 font-mono text-[9px] text-primary/60">
                        {selected.evidence.relationship}
                      </p>
                    </div>
                  )}

                  {selected.graphNode && (
                    <button
                      type="button"
                      onClick={() => onOpenExplore(selected.graphNode!)}
                      className="mt-5 h-8 px-3 inline-flex items-center gap-1.5 rounded-md border border-border/40 text-[11px] text-foreground/55 hover:text-foreground hover:bg-white/[0.04]"
                    >
                      <LocateFixed className="w-3.5 h-3.5" aria-hidden="true" />
                      在探索中定位
                    </button>
                  )}
                </div>
              ) : (
                <p className="p-4 text-[11px] text-foreground/25">选择节点以查看其证据。</p>
              )}
            </aside>
          </div>
        </div>
      ) : null}
    </div>
  );
}
