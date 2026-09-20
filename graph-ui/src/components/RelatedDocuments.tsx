import { useEffect, useRef, useState } from "react";
import { BookOpen, RefreshCw, X } from "lucide-react";
import { callTool } from "../api/rpc";
import { cn } from "../lib/utils";
import { Button } from "./ui/button";
import { Badge } from "./ui/badge";
import { ScrollArea } from "./ui/scroll-area";
import { DocumentReviewAction, type DocumentReviewState } from "./DocumentReviewAction";

interface ReferenceNode {
  qualified_name?: string;
  name?: string;
  file_path?: string;
  start_line?: number;
}

export interface DocumentReference {
  source: ReferenceNode;
  target: ReferenceNode;
  document_qualified_name?: string;
  properties?: {
    matched_text?: string;
    source?: string;
    document_span?: { start_line?: number; end_line?: number };
  };
  review?: DocumentReviewState & {
    status?: string;
    reason?: string;
    changed_file?: string;
    target_qualified_name?: string;
    document_changed?: boolean;
    message_zh?: string;
  };
}

export interface RelatedDocumentsResult {
  references?: DocumentReference[];
  total?: number;
  returned?: number;
  status?: string;
  reason?: string;
  truncated?: boolean;
  budget_truncated?: boolean;
  review_basis?: {
    changed_files_source?: string;
    requested_ref?: string;
    comparison?: string;
    reference_snapshot?: string;
    freshness?: string;
    limitation?: string;
  };
}

interface Snippet {
  source?: string;
  start_line?: number;
  end_line?: number;
}

export function DocumentSource({ project, reference, onClose }: {
  project: string;
  reference: DocumentReference;
  onClose: () => void;
}) {
  const [snippet, setSnippet] = useState<Snippet | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [attempt, setAttempt] = useState(0);
  const highlightedLine = useRef<HTMLSpanElement>(null);
  const qualifiedName = reference.source.qualified_name;
  const line = reference.properties?.document_span?.start_line ?? reference.source.start_line;
  const lastLine = reference.properties?.document_span?.end_line ?? line;

  useEffect(() => {
    const controller = new AbortController();
    setSnippet(null);
    setError(null);
    if (!qualifiedName) {
      setError("文档缺少准确名称，无法读取原文。");
      return () => controller.abort();
    }
    callTool<Snippet>("get_code_snippet", { project, qualified_name: qualifiedName }, controller.signal)
      .then((value) => {
        if (!controller.signal.aborted) setSnippet(value);
      })
      .catch((cause) => {
        if (!controller.signal.aborted) setError(cause instanceof Error ? cause.message : "文档原文读取失败。");
      });
    return () => controller.abort();
  }, [project, qualifiedName, attempt]);

  useEffect(() => {
    highlightedLine.current?.scrollIntoView?.({ block: "nearest" });
  }, [snippet, line]);

  const start = snippet?.start_line ?? reference.source.start_line ?? 1;
  const lines = snippet?.source?.split("\n");
  const lineAvailable = line === undefined || (lines && line >= start && line < start + lines.length);
  const evidenceText = line !== undefined && lines
    ? lines.slice(Math.max(0, line - start), Math.max(0, (lastLine ?? line) - start + 1)).join("\n")
    : "";
  const evidenceMismatch = Boolean(lines && lineAvailable && reference.properties?.matched_text &&
    !evidenceText.includes(reference.properties.matched_text));
  return (
    <div className="mt-2 flex min-w-0 flex-col gap-2" aria-label="文档原文">
      <div className="flex items-start justify-between gap-2">
        <p className="min-w-0 break-all font-mono text-[10px] text-muted-foreground">
          {reference.source.file_path}{line ? `:${line}` : ""}
        </p>
        <Button type="button" variant="ghost" size="icon-xs" aria-label="关闭文档原文" title="关闭文档原文" onClick={onClose}><X /></Button>
      </div>
      {error ? <div role="alert" className="flex flex-col items-start gap-2">
        <p className="break-all text-[11px] text-destructive">{error}</p>
        <Button type="button" variant="outline" size="xs" onClick={() => setAttempt((value) => value + 1)}><RefreshCw data-icon="inline-start" />重试读取</Button>
      </div> : !snippet ? <p role="status" className="text-[11px] text-muted-foreground">正在读取文档原文...</p> : lines ? <>
        {!lineAvailable && <p className="text-[11px] text-muted-foreground">当前原文范围不包含证据行，引用可能需要重新索引。</p>}
        {evidenceMismatch && <p className="text-[11px] text-muted-foreground">当前原文与索引证据不一致，建议重新索引。</p>}
        <ScrollArea className="h-64 min-w-0 rounded border border-border">
          <pre className="p-2 font-mono text-[10px] leading-5 whitespace-pre-wrap break-all">
            {lines.map((text, index) => {
              const number = start + index;
              const selected = !evidenceMismatch && line !== undefined && number >= line && number <= (lastLine ?? line);
              return <span key={number} ref={number === line ? highlightedLine : undefined} data-source-line={number} data-highlighted={selected || undefined} className={cn("block", selected && "bg-accent text-accent-foreground")}>
                <span className="mr-3 inline-block w-8 select-none text-right text-muted-foreground">{number}</span>{text || "\u00a0"}
              </span>;
            })}
          </pre>
        </ScrollArea>
      </> : <p className="text-[11px] text-muted-foreground">没有可读取的文档原文。</p>}
    </div>
  );
}

export function RelatedDocuments({ project, result, title = "相关文档", onRefresh }: {
  project: string;
  result: RelatedDocumentsResult;
  title?: string;
  onRefresh?: () => void;
}) {
  const [selected, setSelected] = useState<DocumentReference | null>(null);
  useEffect(() => setSelected(null), [project, result]);
  const references = result.references ?? [];
  const basis = result.review_basis;
  return (
    <section className="flex min-w-0 flex-col gap-2" aria-label={title}>
      <div className="flex flex-wrap items-center gap-2">
        <h3 className="text-[11px] font-medium text-foreground">{title}</h3>
        <Badge variant="outline">{result.returned ?? references.length}/{result.total ?? references.length}</Badge>
        {result.status === "limited" && <Badge variant="secondary">分析受限</Badge>}
        {result.status === "unknown" && <Badge variant="secondary">分析状态未知</Badge>}
      </div>
      {(result.status === "limited" || result.status === "unknown") && <p className="break-all text-[10px] text-muted-foreground">{result.reason ?? "引用分析未完成"}</p>}
      {(result.truncated || result.budget_truncated) && <p className="text-[10px] text-muted-foreground">{result.budget_truncated ? "文档证据已按预算截断" : "文档引用已按数量截断"}，可能仍有其他相关文档。</p>}
      {basis && <div className="flex flex-col gap-1 text-[10px] text-muted-foreground" aria-label="文档复核依据">
        <p className="break-all">比较基准：{basis.requested_ref ?? "未知"} · {basis.comparison ?? basis.changed_files_source ?? "未知"}</p>
        <p className="break-all">引用快照：{basis.reference_snapshot ?? "未知"} · 新鲜度：{basis.freshness ?? "未知"}</p>
        {basis.limitation && <p className="break-words">{basis.limitation}</p>}
      </div>}
      {references.length === 0 && <p className="text-[11px] text-muted-foreground">未发现已索引的文档引用。</p>}
      <div className="flex min-w-0 flex-col divide-y divide-border">
        {references.map((reference, index) => {
          const line = reference.properties?.document_span?.start_line;
          return <article key={`${reference.source.qualified_name}-${reference.target.qualified_name}-${index}`} className="flex min-w-0 flex-col gap-1.5 py-2">
            <div className="flex flex-wrap items-start gap-2">
              <p className="min-w-0 flex-1 break-words text-[11px]">{reference.source.name ?? reference.source.file_path ?? reference.source.qualified_name}</p>
              {reference.review && (reference.review.reason || !reference.review.state) && <Badge variant="secondary">建议复核</Badge>}
              <Button type="button" variant="ghost" size="icon-xs" aria-label={`阅读文档 ${reference.source.file_path ?? reference.source.name} 第 ${line ?? reference.source.start_line ?? 1} 行`} title="阅读文档原文" disabled={!reference.source.qualified_name} onClick={() => setSelected(reference)}><BookOpen /></Button>
            </div>
            <p className="break-all font-mono text-[10px] text-muted-foreground">{reference.source.file_path}{line ? `:${line}` : ""}</p>
            <p className="break-all font-mono text-[10px] text-muted-foreground">{reference.properties?.matched_text ?? reference.target.qualified_name}</p>
            <p className="break-all font-mono text-[10px] text-muted-foreground">引用目标：{reference.target.qualified_name ?? reference.target.file_path}</p>
            {reference.review && (reference.review.reason || !reference.review.state) && <div className="flex flex-col gap-1 text-[10px] text-muted-foreground">
              <p>{reference.review.message_zh ?? "引用的代码文件发生变更，建议复核文档；这不表示文档已经过期。"}</p>
              <p className="break-all">变更文件：{reference.review.changed_file}</p>
              <p>{reference.review.document_changed ? "文档也在本次变更中，仍需核对内容。" : "文档未在本次变更文件中。"}</p>
            </div>}
            {reference.review && <DocumentReviewAction project={project} source={reference.source.qualified_name} target={reference.target.qualified_name} review={reference.review} onRefresh={onRefresh} />}
            {selected === reference && <DocumentSource key={`${project}:${reference.source.qualified_name}:${line}`} project={project} reference={reference} onClose={() => setSelected(null)} />}
          </article>;
        })}
      </div>
    </section>
  );
}

export function RelatedDocumentsPanel({ project, target }: { project: string; target: string }) {
  const [result, setResult] = useState<RelatedDocumentsResult | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [attempt, setAttempt] = useState(0);
  useEffect(() => {
    const controller = new AbortController();
    setResult(null);
    setError(null);
    callTool<RelatedDocumentsResult>("get_related_documents", { project, target, limit: 20 }, controller.signal)
      .then((value) => { if (!controller.signal.aborted) setResult(value); })
      .catch((cause) => { if (!controller.signal.aborted) setError(cause instanceof Error ? cause.message : "文档引用加载失败。"); });
    return () => controller.abort();
  }, [project, target, attempt]);
  if (error) return <section aria-label="相关文档" className="flex flex-col items-start gap-2">
    <p role="alert" className="break-all text-[11px] text-destructive">{error}</p>
    <Button type="button" variant="outline" size="xs" onClick={() => setAttempt((value) => value + 1)}><RefreshCw data-icon="inline-start" />重试文档引用</Button>
  </section>;
  if (!result) return <p role="status" className="text-[11px] text-muted-foreground">正在加载相关文档...</p>;
  return <RelatedDocuments key={`${project}:${target}`} project={project} result={result} onRefresh={() => setAttempt((value) => value + 1)} />;
}
