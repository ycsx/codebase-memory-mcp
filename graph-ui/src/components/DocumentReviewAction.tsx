import { useEffect, useRef, useState } from "react";
import { Check, RefreshCw, Undo2 } from "lucide-react";
import { callTool } from "../api/rpc";
import { Badge } from "./ui/badge";
import { Button } from "./ui/button";

export interface DocumentReviewState {
  state?: "pending" | "confirmed" | "unavailable";
  token?: string | null;
  reviewed_at?: string | null;
}

export function DocumentReviewAction({ project, source, target, review, onRefresh }: {
  project: string;
  source?: string;
  target?: string;
  review: DocumentReviewState;
  onRefresh?: () => void;
}) {
  const [current, setCurrent] = useState(review);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const request = useRef<AbortController | null>(null);
  useEffect(() => {
    setCurrent(review); setError(null); setBusy(false);
    return () => request.current?.abort();
  }, [project, source, target, review]);

  async function update() {
    if (!current.token || !source || !target || busy || error) return;
    const controller = new AbortController();
    request.current = controller;
    setBusy(true);
    try {
      const next = await callTool<DocumentReviewState>("update_document_review", {
        project, source_qualified_name: source, target_qualified_name: target,
        token: current.token, action: current.state === "confirmed" ? "reopen" : "confirm",
      }, controller.signal);
      if (!controller.signal.aborted) setCurrent(next);
    } catch (cause) {
      if (!controller.signal.aborted) setError(cause instanceof Error ? cause.message : "复核状态保存失败。");
    } finally {
      if (!controller.signal.aborted) setBusy(false);
    }
  }

  if (!current.state) return null;
  return <div className="flex flex-col gap-1">
    <div className="flex flex-wrap items-center gap-2">
      <Badge variant={current.state === "confirmed" ? "outline" : "secondary"}>
        {current.state === "confirmed" ? "已确认" : current.state === "pending" ? "待复核" : "暂不可确认"}
      </Badge>
      {current.token && source && target && current.state !== "unavailable" && <Button type="button" size="xs" variant="outline" disabled={busy || Boolean(error)} onClick={update}>
        {current.state === "confirmed" ? <Undo2 data-icon="inline-start" /> : <Check data-icon="inline-start" />}
        {busy ? "正在保存..." : current.state === "confirmed" ? "重新复核" : "确认已复核"}
      </Button>}
      {current.reviewed_at && <span className="break-all text-[10px] text-muted-foreground">确认时间：{current.reviewed_at}</span>}
    </div>
    {error && <div className="flex flex-col items-start gap-1" role="alert">
      <p className="break-all text-[10px] text-destructive">{error}</p>
      <p className="text-[10px] text-muted-foreground">请刷新引用后核对当前内容，保存失败不代表已经确认。</p>
      {onRefresh && <Button type="button" size="xs" variant="outline" onClick={onRefresh}><RefreshCw data-icon="inline-start" />刷新引用</Button>}
    </div>}
  </div>;
}
