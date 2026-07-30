import { FormEvent, useEffect, useMemo, useState } from "react";
import {
  ArrowRight,
  Boxes,
  FileCode2,
  LoaderCircle,
  LocateFixed,
  Route,
  Search,
  ShieldAlert,
  TestTube2,
  TriangleAlert,
} from "lucide-react";
import { callTool } from "../../api/rpc";
import { graphNodeLabel, graphRelationshipLabel } from "../../lib/graphLabels";
import type { GraphData, GraphNode } from "../../lib/types";
import { shortPath } from "./analysis";

interface ImpactQueryViewProps {
  project: string;
  data: GraphData;
  onOpenExplore: (node: GraphNode) => void;
}

type RiskCode = "critical" | "high" | "medium" | "low";
type ImpactFilter = "all" | "direct" | "indirect" | "tests";

export interface ImpactCandidate {
  key: string;
  name: string;
  qualified_name: string;
  label: string;
  file_path: string;
  start_line: number;
  end_line: number;
  target_type: "file" | "symbol";
}

interface ImpactEvidence {
  relationship: string;
  source: string;
  target: string;
  confidence: number;
}

interface ImpactItem {
  id: number;
  name: string;
  qualified_name: string;
  label: string;
  file_path: string;
  start_line: number;
  end_line: number;
  hop: number;
  risk: RiskCode;
  is_test: boolean;
  is_entry_point: boolean;
  evidence?: ImpactEvidence;
}

interface ImpactSummary {
  risk: RiskCode;
  risk_label_zh: string;
  text_zh: string;
  affected_nodes: number;
  direct_count: number;
  indirect_count: number;
  affected_files: number;
  entry_points: number;
  tests: number;
  cross_service: boolean;
}

interface ImpactQueryResult {
  status: "analysis" | "candidates" | "not_found";
  query: string;
  depth?: number;
  truncated?: boolean;
  message_zh?: string;
  candidates?: ImpactCandidate[];
  selected?: ImpactCandidate;
  summary?: ImpactSummary;
  impacts?: ImpactItem[];
  limitations?: string[];
}

const RISK_STYLE: Record<RiskCode, { label: string; text: string; border: string; dot: string }> = {
  critical: {
    label: "严重",
    text: "text-rose-300",
    border: "border-rose-400/40",
    dot: "bg-rose-400",
  },
  high: {
    label: "高",
    text: "text-amber-300",
    border: "border-amber-400/40",
    dot: "bg-amber-400",
  },
  medium: {
    label: "中",
    text: "text-emerald-300",
    border: "border-emerald-400/35",
    dot: "bg-emerald-400",
  },
  low: {
    label: "低",
    text: "text-sky-300",
    border: "border-sky-400/30",
    dot: "bg-sky-400",
  },
};

const FILTERS: Array<{ id: ImpactFilter; label: string }> = [
  { id: "all", label: "全部" },
  { id: "direct", label: "直接" },
  { id: "indirect", label: "间接" },
  { id: "tests", label: "测试" },
];

function graphNodeFor(data: GraphData, qualifiedName: string, filePath: string) {
  return (
    data.nodes.find((node) => qualifiedName && node.qualified_name === qualifiedName) ??
    data.nodes.find((node) => !qualifiedName && filePath && node.file_path === filePath)
  );
}

function metric(label: string, value: number | string, icon: typeof Boxes) {
  const Icon = icon;
  return (
    <div className="min-w-0 px-4 py-3 border-r border-border/20 last:border-r-0">
      <div className="flex items-center gap-1.5 text-[9px] uppercase text-foreground/25">
        <Icon className="w-3 h-3" aria-hidden="true" />
        {label}
      </div>
      <p className="mt-1 text-[17px] font-semibold text-foreground/75 tabular-nums truncate">
        {value}
      </p>
    </div>
  );
}

export function ImpactQueryView({ project, data, onOpenExplore }: ImpactQueryViewProps) {
  const [query, setQuery] = useState("");
  const [depth, setDepth] = useState(2);
  const [result, setResult] = useState<ImpactQueryResult | null>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [filter, setFilter] = useState<ImpactFilter>("all");
  const [selectedKey, setSelectedKey] = useState<string | null>(null);

  useEffect(() => {
    setResult(null);
    setError(null);
    setSelectedKey(null);
  }, [project]);

  const run = async (target?: string) => {
    const normalized = query.trim();
    if (!normalized) return;
    setLoading(true);
    setError(null);
    try {
      const next = await callTool<ImpactQueryResult>("explain_impact", {
        project,
        query: normalized,
        depth,
        ...(target ? { target } : {}),
      });
      setResult(next);
      setFilter("all");
      setSelectedKey(next.impacts?.[0]?.qualified_name ?? null);
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : "影响查询失败。 ");
    } finally {
      setLoading(false);
    }
  };

  const submit = (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault();
    void run();
  };

  const impacts = useMemo(() => {
    const rows = [...(result?.impacts ?? [])].sort(
      (a, b) => a.hop - b.hop || a.file_path.localeCompare(b.file_path) || a.name.localeCompare(b.name),
    );
    if (filter === "direct") return rows.filter((item) => item.hop === 1);
    if (filter === "indirect") return rows.filter((item) => item.hop > 1);
    if (filter === "tests") return rows.filter((item) => item.is_test);
    return rows;
  }, [filter, result]);

  const selected =
    result?.impacts?.find((item) => item.qualified_name === selectedKey) ?? impacts[0] ?? null;
  const selectedGraphNode = selected
    ? graphNodeFor(data, selected.qualified_name, selected.file_path)
    : null;
  const targetGraphNode = result?.selected
    ? graphNodeFor(data, result.selected.qualified_name, result.selected.file_path)
    : null;

  return (
    <div className="h-full min-h-0 flex flex-col bg-[#08151b]">
      <form
        onSubmit={submit}
        className="min-h-14 shrink-0 border-b border-border/25 px-4 py-2 flex flex-wrap items-center gap-2.5"
      >
        <div className="h-8 min-w-[260px] flex-1 max-w-2xl flex items-center rounded-md border border-border/40 bg-[#0b1920] focus-within:border-primary/50">
          <Search className="w-3.5 h-3.5 ml-2.5 text-foreground/25 shrink-0" aria-hidden="true" />
          <input
            aria-label="影响查询目标"
            value={query}
            onChange={(event) => setQuery(event.target.value)}
            placeholder="组件、函数、类、路由或文件路径"
            className="h-full min-w-0 flex-1 bg-transparent px-2.5 text-[11px] text-foreground/75 outline-none placeholder:text-foreground/20"
          />
        </div>
        <label htmlFor="impact-query-depth" className="text-[10px] uppercase text-foreground/30">
          深度
        </label>
        <select
          id="impact-query-depth"
          value={depth}
          onChange={(event) => setDepth(Number(event.target.value))}
          className="h-8 w-16 rounded-md border border-border/40 bg-[#0b1920] px-2 text-[11px] text-foreground/70 outline-none focus:border-primary/50"
        >
          <option value={1}>1</option>
          <option value={2}>2</option>
          <option value={3}>3</option>
          <option value={4}>4</option>
        </select>
        <button
          type="submit"
          disabled={loading || !query.trim()}
          className="h-8 px-3 inline-flex items-center gap-1.5 rounded-md bg-primary/15 text-primary hover:bg-primary/25 text-[11px] font-medium transition-colors disabled:opacity-35"
        >
          {loading ? (
            <LoaderCircle className="w-3.5 h-3.5 animate-spin" aria-hidden="true" />
          ) : (
            <Search className="w-3.5 h-3.5" aria-hidden="true" />
          )}
          查询影响
        </button>
      </form>

      {error ? (
        <div className="flex-1 flex items-center justify-center p-8">
          <div className="max-w-lg text-center">
            <TriangleAlert className="w-5 h-5 mx-auto mb-3 text-amber-400" aria-hidden="true" />
            <p className="text-[12px] text-foreground/65 mb-1">无法完成影响查询</p>
            <p className="text-[11px] text-foreground/30 font-mono break-words">{error}</p>
          </div>
        </div>
      ) : loading && !result ? (
        <div className="flex-1 flex items-center justify-center gap-2 text-[11px] text-foreground/35">
          <LoaderCircle className="w-4 h-4 animate-spin text-primary" aria-hidden="true" />
          正在追踪依赖路径
        </div>
      ) : !result ? (
        <div className="flex-1 flex items-center justify-center p-8">
          <Search className="w-7 h-7 text-foreground/12" aria-hidden="true" />
        </div>
      ) : result.status === "not_found" ? (
        <div className="flex-1 flex items-center justify-center p-8 text-center">
          <div>
            <Search className="w-5 h-5 mx-auto mb-3 text-foreground/18" aria-hidden="true" />
            <p className="text-[12px] text-foreground/55">{result.message_zh ?? "未找到匹配目标。"}</p>
            <p className="mt-1 text-[10px] text-foreground/25 font-mono">{result.query}</p>
          </div>
        </div>
      ) : result.status === "candidates" ? (
        <div className="flex-1 min-h-0 overflow-y-auto">
          <div className="max-w-4xl mx-auto py-5 px-4">
            <div className="h-9 flex items-center justify-between border-b border-border/30">
              <h2 className="text-[11px] font-semibold text-foreground/60">匹配目标</h2>
              <span className="text-[10px] text-foreground/25 tabular-nums">
                {result.candidates?.length ?? 0}
              </span>
            </div>
            <div>
              {(result.candidates ?? []).map((candidate) => (
                <button
                  key={candidate.key}
                  type="button"
                  onClick={() => void run(candidate.key)}
                  disabled={loading}
                  className="w-full min-h-16 px-3 py-2.5 flex items-center gap-3 text-left border-b border-border/15 hover:bg-white/[0.025] disabled:opacity-45"
                >
                  {candidate.target_type === "file" ? (
                    <FileCode2 className="w-4 h-4 text-cyan-300/60 shrink-0" aria-hidden="true" />
                  ) : (
                    <Boxes className="w-4 h-4 text-emerald-300/55 shrink-0" aria-hidden="true" />
                  )}
                  <span className="min-w-0 flex-1">
                    <span className="flex items-center gap-2">
                      <span className="text-[12px] text-foreground/75 truncate">{candidate.name}</span>
                      <span className="text-[9px] text-foreground/30 uppercase shrink-0">
                        {graphNodeLabel(candidate.label)}
                      </span>
                    </span>
                    <span className="mt-1 block text-[10px] font-mono text-foreground/28 truncate">
                      {candidate.qualified_name || candidate.file_path}
                    </span>
                    {candidate.qualified_name && (
                      <span className="mt-0.5 block text-[9px] font-mono text-foreground/20 truncate">
                        {candidate.file_path}
                        {candidate.start_line > 0 ? `:${candidate.start_line}` : ""}
                      </span>
                    )}
                  </span>
                  <ArrowRight className="w-3.5 h-3.5 text-foreground/20 shrink-0" aria-hidden="true" />
                </button>
              ))}
            </div>
          </div>
        </div>
      ) : result.summary && result.selected ? (
        <div className="flex-1 min-h-0 overflow-y-auto">
          <section className={`border-b ${RISK_STYLE[result.summary.risk].border}`}>
            <div className="max-w-[1440px] mx-auto px-5 py-4 flex flex-col md:flex-row md:items-start gap-4">
              <div className="min-w-0 flex-1">
                <div className="flex items-center gap-2 mb-2">
                  <span className={`w-2 h-2 rounded-full ${RISK_STYLE[result.summary.risk].dot}`} />
                  <span className={`text-[10px] font-semibold uppercase ${RISK_STYLE[result.summary.risk].text}`}>
                    {RISK_STYLE[result.summary.risk].label}风险
                  </span>
                  <span className="text-[10px] text-foreground/25">深度 {result.depth}</span>
                </div>
                <h2 className="text-[13px] font-semibold text-foreground/80 break-words">
                  {result.selected.name}
                </h2>
                <p className="mt-1 text-[10px] font-mono text-foreground/28 break-all">
                  {result.selected.qualified_name || result.selected.file_path}
                </p>
                <p className="mt-3 max-w-4xl text-[12px] leading-5 text-foreground/60">
                  {result.summary.text_zh}
                </p>
              </div>
              {targetGraphNode && (
                <button
                  type="button"
                  onClick={() => onOpenExplore(targetGraphNode)}
                  className="h-8 px-3 inline-flex items-center justify-center gap-1.5 rounded-md border border-border/40 text-[11px] text-foreground/55 hover:text-foreground hover:bg-white/[0.04] shrink-0"
                >
                  <LocateFixed className="w-3.5 h-3.5" aria-hidden="true" />
                  在图谱中查看
                </button>
              )}
            </div>
            <div className="max-w-[1440px] mx-auto grid grid-cols-2 md:grid-cols-3 xl:grid-cols-6 border-t border-border/15">
              {metric("直接影响", result.summary.direct_count, ArrowRight)}
              {metric("间接影响", result.summary.indirect_count, Boxes)}
              {metric("文件", result.summary.affected_files, FileCode2)}
              {metric("入口", result.summary.entry_points, Route)}
              {metric("测试", result.summary.tests, TestTube2)}
              {metric("跨服务", result.summary.cross_service ? "是" : "否", ShieldAlert)}
            </div>
          </section>

          <div className="max-w-[1440px] mx-auto min-h-[420px] grid grid-cols-1 lg:grid-cols-[minmax(0,1fr)_320px]">
            <section className="min-w-0 lg:border-r border-border/25" aria-labelledby="impact-result-title">
              <div className="h-11 px-4 flex items-center justify-between gap-3 border-b border-border/20">
                <div className="flex items-center gap-2">
                  <h3 id="impact-result-title" className="text-[10px] uppercase text-foreground/40">
                    影响节点
                  </h3>
                  <span className="text-[10px] text-foreground/22 tabular-nums">{impacts.length}</span>
                </div>
                <div className="inline-flex h-7 items-center rounded-md border border-border/35 bg-black/10 p-0.5">
                  {FILTERS.map((item) => (
                    <button
                      key={item.id}
                      type="button"
                      onClick={() => setFilter(item.id)}
                      aria-pressed={filter === item.id}
                      className={`h-6 px-2.5 rounded text-[9px] transition-colors ${
                        filter === item.id
                          ? "bg-white/[0.08] text-foreground/65"
                          : "text-foreground/25 hover:text-foreground/50"
                      }`}
                    >
                      {item.label}
                    </button>
                  ))}
                </div>
              </div>
              <div>
                {impacts.map((item) => {
                  const active = selected?.qualified_name === item.qualified_name;
                  const style = RISK_STYLE[item.risk];
                  return (
                    <button
                      key={`${item.id}:${item.qualified_name}`}
                      type="button"
                      onClick={() => setSelectedKey(item.qualified_name)}
                      className={`w-full min-h-[62px] px-4 py-2.5 grid grid-cols-[48px_minmax(0,1fr)_100px] items-center gap-3 text-left border-b border-border/15 transition-colors ${
                        active ? "bg-white/[0.035]" : "hover:bg-white/[0.02]"
                      }`}
                    >
                      <span className="text-center">
                        <span className={`block text-[10px] font-semibold ${style.text}`}>+{item.hop}</span>
                        <span className="block mt-0.5 text-[8px] uppercase text-foreground/20">
                          {item.hop === 1 ? "直接" : "间接"}
                        </span>
                      </span>
                      <span className="min-w-0">
                        <span className="flex items-center gap-2 min-w-0">
                          <span className="text-[11px] text-foreground/72 truncate">{item.name}</span>
                          {item.is_entry_point && (
                            <Route className="w-3 h-3 text-rose-300/70 shrink-0" aria-label="入口" />
                          )}
                          {item.is_test && (
                            <TestTube2 className="w-3 h-3 text-emerald-300/70 shrink-0" aria-label="测试" />
                          )}
                        </span>
                        <span className="mt-1 block text-[9px] font-mono text-foreground/24 truncate">
                          {shortPath(item.file_path, 5)}
                          {item.start_line > 0 ? `:${item.start_line}` : ""}
                        </span>
                      </span>
                      <span className="min-w-0 text-right">
                        <span className="block text-[9px] text-foreground/35 truncate">
                          {graphNodeLabel(item.label)}
                        </span>
                        <span className="mt-1 block text-[8px] font-mono text-primary/55 truncate">
                          {item.evidence ? graphRelationshipLabel(item.evidence.relationship) : "依赖路径"}
                        </span>
                      </span>
                    </button>
                  );
                })}
                {impacts.length === 0 && (
                  <p className="py-12 text-center text-[11px] text-foreground/25">
                    当前筛选下没有影响节点。
                  </p>
                )}
              </div>
            </section>

            <aside className="min-h-64 border-t lg:border-t-0 border-border/25">
              <div className="h-11 px-4 flex items-center border-b border-border/20">
                <h3 className="text-[10px] uppercase text-foreground/40">关系证据</h3>
              </div>
              {selected ? (
                <div className="p-4">
                  <div className="flex items-start justify-between gap-3">
                    <div className="min-w-0">
                      <p className="text-[12px] font-semibold text-foreground/75 break-words">
                        {selected.name}
                      </p>
                      <p className="mt-1 text-[9px] text-foreground/28">
                        {graphNodeLabel(selected.label)} · 影响 +{selected.hop}
                      </p>
                    </div>
                    <span className={`text-[9px] uppercase ${RISK_STYLE[selected.risk].text}`}>
                      {RISK_STYLE[selected.risk].label}风险
                    </span>
                  </div>
                  <dl className="mt-4 space-y-3 text-[10px]">
                    <div>
                      <dt className="text-foreground/22 uppercase mb-1">文件</dt>
                      <dd className="font-mono text-foreground/52 break-all leading-4">
                        {selected.file_path}
                        {selected.start_line > 0 ? `:${selected.start_line}` : ""}
                      </dd>
                    </div>
                    <div>
                      <dt className="text-foreground/22 uppercase mb-1">限定名称</dt>
                      <dd className="font-mono text-foreground/42 break-all leading-4">
                        {selected.qualified_name}
                      </dd>
                    </div>
                  </dl>
                  {selected.evidence && (
                    <div className="mt-5 pt-4 border-t border-border/20">
                      <p className="text-[9px] font-mono text-primary/60">
                        {graphRelationshipLabel(selected.evidence.relationship)}
                      </p>
                      <div className="mt-2 grid grid-cols-[minmax(0,1fr)_14px_minmax(0,1fr)] items-center gap-1 text-[10px] text-foreground/50">
                        <span className="truncate" title={selected.evidence.source}>
                          {selected.evidence.source}
                        </span>
                        <ArrowRight className="w-3 h-3 text-foreground/20" aria-hidden="true" />
                        <span className="truncate" title={selected.evidence.target}>
                          {selected.evidence.target}
                        </span>
                      </div>
                      <p className="mt-2 text-[9px] text-foreground/22 tabular-nums">
                        置信度 {Math.round(selected.evidence.confidence * 100)}%
                      </p>
                    </div>
                  )}
                  {selectedGraphNode && (
                    <button
                      type="button"
                      onClick={() => onOpenExplore(selectedGraphNode)}
                      className="mt-5 h-8 px-3 inline-flex items-center gap-1.5 rounded-md border border-border/40 text-[11px] text-foreground/55 hover:text-foreground hover:bg-white/[0.04]"
                    >
                      <LocateFixed className="w-3.5 h-3.5" aria-hidden="true" />
                      在图谱中查看
                    </button>
                  )}
                </div>
              ) : (
                <p className="p-4 text-[11px] text-foreground/25">选择影响节点查看关系证据。</p>
              )}
            </aside>
          </div>

          {(result.limitations?.length ?? 0) > 0 && (
            <section className="max-w-[1440px] mx-auto border-t border-border/20 px-5 py-4">
              <div className="flex items-start gap-2">
                <TriangleAlert className="w-3.5 h-3.5 mt-0.5 text-amber-300/55 shrink-0" aria-hidden="true" />
                <div className="space-y-1">
                  {result.limitations?.map((text) => (
                    <p key={text} className="text-[9px] leading-4 text-foreground/25">
                      {text}
                    </p>
                  ))}
                </div>
              </div>
            </section>
          )}
        </div>
      ) : null}
    </div>
  );
}
