import { useEffect, useState } from "react";
import { BookOpen, ChevronLeft, ChevronRight, RefreshCw, Search } from "lucide-react";
import { callTool } from "../../api/rpc";
import { DocumentSource, RelatedDocumentsPanel } from "../RelatedDocuments";
import { Button } from "../ui/button";
import { Badge } from "../ui/badge";

interface CoverageItem {
  file_path: string;
  qualified_name?: string;
  reference_count?: number;
  status: string;
  reasons?: string[];
}
interface CoverageResult {
  total: number;
  offset: number;
  returned: number;
  has_more: boolean;
  next_offset?: number;
  items: CoverageItem[];
  summary?: {
    indexed_code_files: number;
    referenced_code_files: number;
    indexed_documents: number;
    limited_documents: number;
    unknown_documents: number;
  };
  limitation?: string;
}
const PAGE_SIZE = 25;
const LABELS: Record<string, string> = {
  referenced: "已被引用", not_referenced: "未被引用", ok: "已分析", limited: "分析受限", unknown: "状态未知",
};

export function DocumentCoverageView({ project }: { project: string }) {
  // Remount on project changes so page/filter state cannot leak between repositories.
  return <ProjectCoverage key={project} project={project} />;
}

function ProjectCoverage({ project }: { project: string }) {
  const [view, setView] = useState("code");
  const [status, setStatus] = useState("");
  const [offset, setOffset] = useState(0);
  const [attempt, setAttempt] = useState(0);
  const [result, setResult] = useState<CoverageResult | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [selected, setSelected] = useState<string | null>(null);
  const [document, setDocument] = useState<CoverageItem | null>(null);
  useEffect(() => {
    const controller = new AbortController();
    setResult(null); setError(null); setSelected(null); setDocument(null);
    callTool<CoverageResult>("get_document_coverage", {
      project, view, offset, limit: PAGE_SIZE, ...(status ? { status } : {}),
    }, controller.signal).then((next) => {
      if (!controller.signal.aborted) setResult(next);
    }).catch((cause) => {
      if (!controller.signal.aborted) setError(cause instanceof Error ? cause.message : "文档覆盖加载失败。");
    });
    return () => controller.abort();
  }, [project, view, status, offset, attempt]);

  return <section aria-label="文档覆盖" className="flex h-full min-h-0 min-w-0 flex-col overflow-y-auto bg-background">
    <div className="flex flex-wrap items-center gap-3 border-b border-border p-4">
      <h2 className="text-sm font-medium">文档覆盖</h2>
      <label className="flex items-center gap-2 text-xs text-muted-foreground">范围
        <select aria-label="覆盖范围" value={view} onChange={(event) => { setView(event.target.value); setStatus(""); setOffset(0); }} className="h-8 rounded border border-border bg-background px-2 text-foreground">
          <option value="code">代码文件</option><option value="documents">文档</option>
        </select>
      </label>
      <label className="flex items-center gap-2 text-xs text-muted-foreground">状态
        <select aria-label="覆盖状态" value={status} onChange={(event) => { setStatus(event.target.value); setOffset(0); }} className="h-8 rounded border border-border bg-background px-2 text-foreground">
          <option value="">全部</option>
          {(view === "code" ? ["referenced", "not_referenced"] : ["ok", "limited", "unknown"]).map((value) => <option key={value} value={value}>{LABELS[value]}</option>)}
        </select>
      </label>
      <Button type="button" variant="ghost" size="icon-sm" title="刷新覆盖" aria-label="刷新覆盖" onClick={() => { setOffset(0); setAttempt((value) => value + 1); }}><RefreshCw /></Button>
    </div>
    <div className="flex min-w-0 flex-col gap-3 p-4">
      <p className="text-xs text-muted-foreground">未被引用不等于缺少文档；这里只统计当前索引中的明确引用。</p>
      {error ? <div role="alert" className="flex flex-col items-start gap-2">
        <p className="break-all text-xs text-destructive">{error}</p>
        <Button type="button" size="sm" variant="outline" onClick={() => { setOffset(0); setAttempt((value) => value + 1); }}><RefreshCw data-icon="inline-start" />重试覆盖</Button>
      </div> : !result ? <p role="status" className="text-xs text-muted-foreground">正在加载文档覆盖...</p> : <>
        {result.summary && <dl className="grid grid-cols-2 gap-3 border-b border-border pb-3 text-xs sm:grid-cols-5">
          {[
            ["代码文件", result.summary.indexed_code_files], ["已被引用", result.summary.referenced_code_files],
            ["文档", result.summary.indexed_documents], ["分析受限", result.summary.limited_documents], ["状态未知", result.summary.unknown_documents],
          ].map(([label, count]) => <div key={label}><dt className="text-muted-foreground">{label}</dt><dd className="mt-1 font-mono text-lg">{count}</dd></div>)}
        </dl>}
        {result.items.length === 0 ? <p className="text-xs text-muted-foreground">没有符合条件的已索引文件。</p> : <table className="w-full table-fixed text-left text-xs" aria-label="文档覆盖矩阵">
          <thead><tr className="border-b border-border text-muted-foreground"><th className="w-[55%] py-2 font-medium">文件</th><th className="py-2 font-medium">状态</th><th className="w-14 py-2 text-right font-medium">{view === "code" ? "引用" : "详情"}</th></tr></thead>
          <tbody>{result.items.map((item) => <tr key={item.file_path} className="border-b border-border align-top">
            <td className="py-3 pr-3 font-mono break-all">{item.file_path}{item.reasons?.length ? <p className="mt-1 text-[10px] text-muted-foreground">{item.reasons.join(" · ")}</p> : null}</td>
            <td className="py-3 pr-1"><Badge variant="outline">{LABELS[item.status] ?? item.status}</Badge></td>
            <td className="py-2 text-right">{view === "code" ? <Button type="button" variant="ghost" size="xs" aria-label={`查看引用 ${item.file_path}`} onClick={() => setSelected(item.file_path)}><Search data-icon="inline-start" />{item.reference_count ?? 0}</Button> : <Button type="button" variant="ghost" size="icon-xs" title="阅读文档原文" aria-label={`阅读文档 ${item.file_path}`} disabled={!item.qualified_name} onClick={() => setDocument(item)}><BookOpen /></Button>}</td>
          </tr>)}</tbody>
        </table>}
        <div className="flex flex-wrap items-center justify-between gap-2">
          <span className="text-xs text-muted-foreground">{result.total ? `${result.offset + 1}–${result.offset + result.returned}` : "0"} / {result.total}</span>
          <div className="flex items-center gap-2">
            <Button type="button" variant="outline" size="icon-sm" title="上一页" aria-label="上一页" disabled={offset === 0} onClick={() => setOffset((value) => Math.max(0, value - PAGE_SIZE))}><ChevronLeft /></Button>
            <Button type="button" variant="outline" size="icon-sm" title="下一页" aria-label="下一页" disabled={!result.has_more} onClick={() => setOffset(result.next_offset ?? offset + PAGE_SIZE)}><ChevronRight /></Button>
          </div>
        </div>
        {result.limitation && <p className="break-words text-[10px] text-muted-foreground">{result.limitation}</p>}
        {selected && <div className="min-w-0 border-t border-border pt-3"><p className="mb-3 break-all font-mono text-xs">{selected}</p><RelatedDocumentsPanel key={selected} project={project} target={`file:${selected}`} /></div>}
        {document && <div className="min-w-0 border-t border-border pt-3"><DocumentSource key={document.qualified_name} project={project} reference={{ source: { qualified_name: document.qualified_name, file_path: document.file_path }, target: {} }} onClose={() => setDocument(null)} /></div>}
      </>}
    </div>
  </section>;
}
