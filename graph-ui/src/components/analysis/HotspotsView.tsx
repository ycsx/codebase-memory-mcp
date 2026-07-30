import { useCallback, useEffect, useMemo, useState } from "react";
import { Flame, LoaderCircle, LocateFixed, RefreshCw, TriangleAlert, X } from "lucide-react";
import { callTool } from "../../api/rpc";
import type { GraphData, GraphNode } from "../../lib/types";
import {
  buildHotspotModel,
  shortPath,
  type ArchitectureHotspot,
  type ImpactRisk,
} from "./analysis";

interface HotspotsViewProps {
  project: string;
  data: GraphData;
  onOpenExplore: (node: GraphNode) => void;
}

interface ArchitectureResult {
  hotspots?: ArchitectureHotspot[];
  total_nodes?: number;
  total_edges?: number;
}

const RISK_STYLE: Record<Exclude<ImpactRisk, "changed">, { tile: string; text: string; label: string }> = {
  critical: {
    tile: "border-rose-400/40 bg-rose-400/[0.07] hover:bg-rose-400/[0.11]",
    text: "text-rose-300",
    label: "严重",
  },
  high: {
    tile: "border-amber-400/35 bg-amber-400/[0.06] hover:bg-amber-400/[0.10]",
    text: "text-amber-300",
    label: "高风险",
  },
  medium: {
    tile: "border-emerald-400/25 bg-emerald-400/[0.045] hover:bg-emerald-400/[0.08]",
    text: "text-emerald-300",
    label: "关注",
  },
};

export function HotspotsView({ project, data, onOpenExplore }: HotspotsViewProps) {
  const [result, setResult] = useState<ArchitectureResult | null>(null);
  const [selectedFile, setSelectedFile] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const load = useCallback(async () => {
    setLoading(true);
    setError(null);
    try {
      const next = await callTool<ArchitectureResult>("get_architecture", {
        project,
        aspects: ["hotspots"],
        format: "json",
      });
      setResult(next);
    } catch (cause) {
      setResult(null);
      setError(cause instanceof Error ? cause.message : "无法加载架构热点。");
    } finally {
      setLoading(false);
    }
  }, [project]);

  useEffect(() => {
    setSelectedFile(null);
    void load();
  }, [load]);

  const model = useMemo(
    () => buildHotspotModel(data, result?.hotspots ?? []),
    [data, result],
  );
  const visibleRows = selectedFile
    ? model.rows.filter((row) => row.graphNode?.file_path === selectedFile)
    : model.rows;
  const totalFanIn = model.rows.reduce((sum, row) => sum + row.fan_in, 0);
  const mappedCount = model.rows.filter((row) => row.graphNode).length;

  return (
    <div className="h-full min-h-0 bg-[#08151b] overflow-x-hidden overflow-y-auto">
      <div className="min-h-14 px-5 py-2 border-b border-border/25 flex flex-wrap items-center gap-4">
        <div className="flex items-center gap-2 min-w-0">
          <Flame className="w-4 h-4 text-amber-400" aria-hidden="true" />
          <div>
            <h2 className="text-[12px] font-semibold text-foreground/75">架构热点</h2>
            <p className="text-[10px] text-foreground/25">
              符号排名使用完整索引，文件风险使用当前已加载子图
            </p>
          </div>
        </div>
        {result && !loading && (
          <div className="ml-auto flex items-center gap-5 text-[10px] text-foreground/30 tabular-nums">
            <span><strong className="text-foreground/65">{model.rows.length}</strong> 个符号</span>
            <span><strong className="text-foreground/65">{totalFanIn}</strong> 次入站调用</span>
            <span><strong className="text-foreground/65">{mappedCount}</strong> 个已加载</span>
          </div>
        )}
        <button
          type="button"
          onClick={() => void load()}
          disabled={loading}
          title="刷新热点"
          aria-label="刷新热点"
          className="w-8 h-8 inline-flex items-center justify-center rounded-md border border-border/35 text-foreground/35 hover:text-foreground/70 hover:bg-white/[0.04] disabled:opacity-35"
        >
          {loading ? (
            <LoaderCircle className="w-3.5 h-3.5 animate-spin" aria-hidden="true" />
          ) : (
            <RefreshCw className="w-3.5 h-3.5" aria-hidden="true" />
          )}
        </button>
      </div>

      {error ? (
        <div className="h-[calc(100%_-_3.5rem)] flex items-center justify-center p-8">
          <div className="max-w-lg text-center">
            <TriangleAlert className="w-5 h-5 mx-auto mb-3 text-amber-400" aria-hidden="true" />
            <p className="text-[12px] text-foreground/65 mb-1">无法加载热点</p>
            <p className="text-[11px] text-foreground/30 font-mono break-words">{error}</p>
          </div>
        </div>
      ) : loading && !result ? (
        <div className="h-[calc(100%_-_3.5rem)] flex items-center justify-center gap-2 text-[11px] text-foreground/35">
          <LoaderCircle className="w-4 h-4 animate-spin text-primary" aria-hidden="true" />
          正在计算入度
        </div>
      ) : result ? (
        <div className="min-w-0 max-w-[1280px] mx-auto px-5 py-5">
          <section aria-labelledby="risk-map-title">
            <div className="flex items-center justify-between gap-3 mb-3">
              <div>
                <h3 id="risk-map-title" className="text-[11px] font-semibold text-foreground/60">
                  文件风险图
                </h3>
                <p className="mt-0.5 text-[10px] text-foreground/22">
                  按已加载调用关系、热点密度和符号数排序
                </p>
              </div>
              <div className="flex gap-3 text-[9px] uppercase text-foreground/25">
                {(["critical", "high", "medium"] as const).map((risk) => (
                  <span key={risk} className={`inline-flex items-center gap-1 ${RISK_STYLE[risk].text}`}>
                    <span className="w-1.5 h-1.5 rounded-full bg-current" />
                    {RISK_STYLE[risk].label}
                  </span>
                ))}
              </div>
            </div>

            {model.files.length > 0 ? (
              <div className="grid grid-cols-2 md:grid-cols-4 xl:grid-cols-6 auto-rows-[84px] gap-2">
                {model.files.map((file) => {
                  const relative = model.files[0]?.score ? file.score / model.files[0].score : 0;
                  const span = relative >= 0.72 ? 2 : 1;
                  const active = selectedFile === file.file;
                  return (
                    <button
                      key={file.file}
                      type="button"
                      onClick={() => setSelectedFile(active ? null : file.file)}
                      className={`min-w-0 p-3 text-left rounded-md border transition-colors ${RISK_STYLE[file.risk].tile} ${
                        active ? "ring-1 ring-white/40" : ""
                      }`}
                      style={{ gridColumn: `span ${span}` }}
                      aria-pressed={active}
                    >
                      <span className="block text-[10px] font-mono text-foreground/70 truncate" title={file.file}>
                        {shortPath(file.file, span === 2 ? 5 : 3)}
                      </span>
                      <span className="mt-2 flex items-end justify-between gap-2">
                        <span className={`text-[9px] uppercase ${RISK_STYLE[file.risk].text}`}>
                          {RISK_STYLE[file.risk].label}
                        </span>
                        <span className="text-[10px] text-foreground/30 tabular-nums">
                          {file.fanIn} 条调用关系 {"\u00b7"} {file.nodeCount} 个符号
                        </span>
                      </span>
                    </button>
                  );
                })}
              </div>
            ) : (
              <div className="h-28 flex items-center justify-center border-y border-border/15 text-[11px] text-foreground/25">
                当前图谱中未发现调用热点。
              </div>
            )}
          </section>

          <section className="min-w-0 mt-7" aria-labelledby="hotspot-table-title">
            <div className="h-9 flex items-center justify-between gap-3 border-b border-border/30">
              <div className="flex items-center gap-2 min-w-0">
                <h3 id="hotspot-table-title" className="text-[11px] font-semibold text-foreground/60 shrink-0">
                  符号排名
                </h3>
                {selectedFile && (
                  <span className="text-[10px] text-primary/55 font-mono truncate">
                    {shortPath(selectedFile, 5)}
                  </span>
                )}
              </div>
              {selectedFile && (
                <button
                  type="button"
                  onClick={() => setSelectedFile(null)}
                  className="w-7 h-7 inline-flex items-center justify-center rounded text-foreground/30 hover:text-foreground/65 hover:bg-white/[0.04]"
                  title="清除文件筛选"
                  aria-label="清除文件筛选"
                >
                  <X className="w-3.5 h-3.5" aria-hidden="true" />
                </button>
              )}
            </div>

            <div className="min-w-0 max-w-full overflow-x-auto">
              <table className="w-full min-w-[720px] table-fixed text-left">
                <thead>
                  <tr className="h-9 text-[9px] uppercase tracking-wider text-foreground/25 border-b border-border/20">
                    <th className="w-12 px-2 font-medium">排名</th>
                    <th className="w-[34%] px-2 font-medium">符号</th>
                    <th className="px-2 font-medium">文件</th>
                    <th className="w-20 px-2 font-medium text-right">入度</th>
                    <th className="w-24 px-2 font-medium">风险</th>
                    <th className="w-12 px-2"><span className="sr-only">定位</span></th>
                  </tr>
                </thead>
                <tbody>
                  {visibleRows.map((row) => {
                    const rank = model.rows.indexOf(row) + 1;
                    return (
                      <tr key={row.qualified_name} className="h-12 border-b border-border/15 hover:bg-white/[0.02]">
                        <td className="px-2 text-[10px] text-foreground/25 tabular-nums">{rank}</td>
                        <td className="px-2 min-w-0">
                          <p className="text-[11px] text-foreground/72 truncate">{row.name}</p>
                          <p className="text-[9px] text-foreground/20 font-mono truncate" title={row.qualified_name}>
                            {row.qualified_name}
                          </p>
                        </td>
                        <td className="px-2 text-[10px] text-foreground/35 font-mono truncate" title={row.file}>
                          {shortPath(row.file, 4)}
                        </td>
                        <td className="px-2 text-right text-[11px] text-foreground/70 font-mono tabular-nums">
                          {row.fan_in}
                        </td>
                        <td className={`px-2 text-[9px] uppercase ${RISK_STYLE[row.risk].text}`}>
                          {RISK_STYLE[row.risk].label}
                        </td>
                        <td className="px-2 text-right">
                          {row.graphNode && (
                            <button
                              type="button"
                              onClick={() => onOpenExplore(row.graphNode!)}
                              className="w-7 h-7 inline-flex items-center justify-center rounded text-foreground/25 hover:text-primary hover:bg-primary/10"
                              title="在探索中定位"
                              aria-label={`在探索中定位 ${row.name}`}
                            >
                              <LocateFixed className="w-3.5 h-3.5" aria-hidden="true" />
                            </button>
                          )}
                        </td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>
              {visibleRows.length === 0 && (
                <p className="py-8 text-center text-[11px] text-foreground/25">
                  此文件没有进入排名的热点符号。清除文件筛选以查看完整排名。
                </p>
              )}
            </div>
          </section>
        </div>
      ) : null}
    </div>
  );
}
