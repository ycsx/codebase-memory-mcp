import { useEffect, useState } from "react";

export type UiLanguage = "en" | "zh";

export const messages = {
  en: {
    tabs: {
      graph: "Graph",
      projects: "Projects",
      control: "Control",
    },
    common: {
      cancel: "Cancel",
      refresh: "Refresh",
      loading: "Loading...",
      save: "Save",
      saving: "Saving...",
      delete: "Delete",
      noMatches: "No matches",
      dismiss: "Dismiss",
    },
    graph: {
      selectedLabel: "Graph",
      search: "Search...",
      clearSelection: "Clear selection",
      folders: "Folders",
    },
    projects: {
      indexedProjects: "Indexed Projects",
      noIndexedProjects: "No indexed projects",
      indexFirstRepository: "Index your first repository",
      viewGraph: "View Graph",
      nodes: "nodes",
      edges: "edges",
      deleteTitle: "Delete index",
      deleteConfirm: (name: string) => `Delete index for "${name}"?`,
      healthHealthy: "Database healthy",
      healthMissing: "Database missing",
      healthCorrupt: "Database unhealthy",
      healthChecking: "Checking...",
      indexingInProgress: "Indexing in progress",
      indexingComplete: "Indexing complete",
      indexingFailed: "Indexing failed",
      gitRemote: "Git Remote",
      pollEvery: (minutes: number) => `every ${minutes} min`,
      syncNow: "Sync now",
      syncScheduled: "Sync scheduled",
      syncFailed: "Sync failed",
    },
    crossRepo: {
      title: "Cross-Repository Links",
      source: "Source index",
      targets: "Target indexes",
      selectAll: "Select all",
      clear: "Clear",
      run: "Connect repositories",
      running: "Matching APIs...",
      needTwoProjects: "At least two indexed projects are required.",
      selectTarget: "Select at least one target index.",
      complete: "Cross-repository analysis complete",
      failed: "Cross-repository analysis failed",
      totalLinks: "cross-project links",
      projectsScanned: "Projects scanned",
      elapsed: "Elapsed",
      http: "HTTP",
      async: "Async",
      channels: "Channels",
      grpc: "gRPC",
      graphql: "GraphQL",
      trpc: "tRPC",
    },
    index: {
      newIndex: "New Index",
      createIndex: "Create Index",
      localSource: "Local Folder",
      remoteSource: "Git Remote",
      selectRepositoryFolder: "Select Repository Folder",
      instructions: "Navigate to the project root and click \"Index This Folder\".",
      remoteInstructions: "SSH and HTTPS repository URLs use your system OpenSSH configuration, then poll the selected branch for updates.",
      repositoryPath: "Repository path",
      repositoryUrl: "Repository URL",
      repositoryUrlPlaceholder: "git@github.com:company/repository.git or an HTTPS repository URL",
      branch: "Branch",
      pollInterval: "Poll interval",
      minutes: "minutes",
      projectName: "Project ID (optional — permanent, cannot be renamed)",
      projectNamePlaceholder: "Derived from folder name if blank",
      projectNameHelp: "Becomes the database name and query prefix. Leave blank to derive it from the path.",
      filterFolders: "Filter folders",
      noSubdirectories: "No subdirectories",
      indexThisFolder: "Index This Folder",
      cloneAndIndex: "Clone and Index",
      starting: "Starting...",
      browseRoot: (path: string) => `Browse ${path}`,
      indexDirectory: (name: string) => `Index ${name}`,
    },
    adr: {
      title: "Architecture Decision Record",
      lastUpdated: "Last updated",
    },
    control: {
      panel: "Control Panel",
      totalCpu: "Total CPU",
      totalRam: "Total RAM",
      processes: "Processes",
      selfRam: "Self RAM",
      activeProcesses: "Active Processes",
      processLogs: "Process Logs",
      noProcesses: "No processes found",
      noLogs: "No logs yet",
      kill: "Kill",
      thisProcess: "THIS",
      uptime: "Uptime",
      killConfirm: (pid: number) => `Kill process ${pid}?`,
    },
  },
  zh: {
    crossRepo: {
      title: "\u8de8\u9879\u76ee\u5173\u8054",
      source: "\u6e90\u7d22\u5f15",
      targets: "\u76ee\u6807\u7d22\u5f15",
      selectAll: "\u5168\u9009",
      clear: "\u6e05\u7a7a",
      run: "\u5173\u8054\u9879\u76ee",
      running: "\u6b63\u5728\u5339\u914d API...",
      needTwoProjects: "\u81f3\u5c11\u9700\u8981\u4e24\u4e2a\u5df2\u7d22\u5f15\u9879\u76ee\u3002",
      selectTarget: "\u8bf7\u81f3\u5c11\u9009\u62e9\u4e00\u4e2a\u76ee\u6807\u7d22\u5f15\u3002",
      complete: "\u8de8\u9879\u76ee\u5206\u6790\u5b8c\u6210",
      failed: "\u8de8\u9879\u76ee\u5206\u6790\u5931\u8d25",
      totalLinks: "\u6761\u8de8\u9879\u76ee\u5173\u8054",
      projectsScanned: "\u5df2\u626b\u63cf\u9879\u76ee",
      elapsed: "\u8017\u65f6",
      http: "HTTP",
      async: "\u5f02\u6b65",
      channels: "\u901a\u9053",
      grpc: "gRPC",
      graphql: "GraphQL",
      trpc: "tRPC",
    },
    tabs: {
      graph: "图谱",
      projects: "项目",
      control: "控制",
    },
    common: {
      cancel: "取消",
      refresh: "刷新",
      loading: "加载中...",
      save: "保存",
      saving: "保存中...",
      delete: "删除",
      noMatches: "无匹配结果",
      dismiss: "关闭",
    },
    graph: {
      selectedLabel: "图谱",
      search: "搜索...",
      clearSelection: "清除选择",
      folders: "目录",
    },
    projects: {
      indexedProjects: "已索引项目",
      noIndexedProjects: "暂无已索引项目",
      indexFirstRepository: "索引第一个仓库",
      viewGraph: "查看图谱",
      nodes: "节点",
      edges: "边",
      deleteTitle: "删除索引",
      deleteConfirm: (name: string) => `删除 "${name}" 的索引？`,
      healthHealthy: "数据库正常",
      healthMissing: "数据库缺失",
      healthCorrupt: "数据库异常",
      healthChecking: "检查中...",
      indexingInProgress: "正在索引",
      indexingComplete: "索引完成",
      indexingFailed: "索引失败",
      gitRemote: "Git 远程仓库",
      pollEvery: (minutes: number) => `每 ${minutes} 分钟`,
      syncNow: "立即同步",
      syncScheduled: "已安排同步",
      syncFailed: "同步失败",
    },
    index: {
      newIndex: "新建索引",
      createIndex: "创建索引",
      localSource: "本地目录",
      remoteSource: "Git 远程仓库",
      selectRepositoryFolder: "选择仓库目录",
      instructions: "导航到项目根目录，然后点击“索引此目录”。",
      remoteInstructions: "SSH 或 HTTPS 仓库地址均使用系统 OpenSSH 配置，并定时检查所选分支更新。",
      repositoryPath: "仓库路径",
      repositoryUrl: "仓库地址",
      repositoryUrlPlaceholder: "git@github.com:company/repository.git 或 HTTPS 仓库地址",
      branch: "分支",
      pollInterval: "轮询间隔",
      minutes: "分钟",
      projectName: "项目 ID（可选，永久且不可重命名）",
      projectNamePlaceholder: "留空则从路径派生",
      projectNameHelp: "将作为数据库名称与查询前缀；留空则从路径派生。",
      filterFolders: "筛选目录",
      noSubdirectories: "没有子目录",
      indexThisFolder: "索引此目录",
      cloneAndIndex: "克隆并索引",
      starting: "启动中...",
      browseRoot: (path: string) => `浏览 ${path}`,
      indexDirectory: (name: string) => `索引 ${name}`,
    },
    adr: {
      title: "架构决策记录",
      lastUpdated: "最后更新",
    },
    control: {
      panel: "控制面板",
      totalCpu: "总 CPU",
      totalRam: "总内存",
      processes: "进程",
      selfRam: "自身内存",
      activeProcesses: "活动进程",
      processLogs: "进程日志",
      noProcesses: "未找到进程",
      noLogs: "暂无日志",
      kill: "结束",
      thisProcess: "本进程",
      uptime: "运行时间",
      killConfirm: (pid: number) => `结束进程 ${pid}？`,
    },
  },
} as const;

export type UiMessages = (typeof messages)[UiLanguage];

export function detectLanguage(acceptLanguage?: string | null, override?: string | null): UiLanguage {
  if (override === "zh" || override === "en") return override;
  if (!acceptLanguage) return "en";
  const normalized = acceptLanguage.toLowerCase();
  return normalized.includes("zh-cn") || normalized.includes("zh") ? "zh" : "en";
}

let cachedLanguage: UiLanguage = "en";
let languageLoaded = false;
let languageRequest: Promise<UiLanguage> | null = null;
const languageListeners = new Set<(lang: UiLanguage) => void>();

function loadUiLanguage(): Promise<UiLanguage> {
  if (languageLoaded) return Promise.resolve(cachedLanguage);
  if (languageRequest) return languageRequest;

  languageRequest = fetch("/api/ui-config")
    .then((r) => r.json())
    .then((data) => detectLanguage(null, data?.lang))
    .catch(() => detectLanguage(navigator.language))
    .then((lang) => {
      cachedLanguage = lang;
      languageLoaded = true;
      for (const listener of languageListeners) listener(lang);
      return lang;
    })
    .finally(() => {
      languageRequest = null;
    });

  return languageRequest;
}

export function useUiMessages(): UiMessages {
  const [lang, setLang] = useState<UiLanguage>(cachedLanguage);

  useEffect(() => {
    let cancelled = false;
    languageListeners.add(setLang);
    void loadUiLanguage().then((nextLang) => {
      if (!cancelled) setLang(nextLang);
    });
    return () => {
      cancelled = true;
      languageListeners.delete(setLang);
    };
  }, []);

  return messages[lang];
}
