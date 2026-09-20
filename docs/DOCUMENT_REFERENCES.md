# 文档引用使用指引

核对日期：2026-09-20。本文对应当前源码实现，不代表这些改动已发布到 Release。
此能力在本地 MCP stdio 和 CLI 中可用，不要求远程 HTTP 服务。

## 启用与更新

1. 使用包含文档引用改动的构建。源码构建方式见 [INSTALL.md](../INSTALL.md)；
   输出为 `build/c/codebase-memory-mcp`，Windows 为 `.exe`。
2. 确认客户端配置指向该程序，重启或重新连接 MCP。重编译不会替换已运行的进程。
3. 调用 `list_projects` 获取项目名，再对旧项目执行一次 `index_repository`；
   没有文件改动的旧索引也会补建引用。不要删除真实缓存来升级。
4. 使用 `index_status` 和 `check_index_coverage` 检查文档及目标代码是否被索引。
   后续常规增量更新会维护引用，不必每次查询重新索引。

仓库内的 Agent 指令模板和 CLI 帮助也已同步。已有客户端指令文件不会因修改源码
自动更新；需要更新时先用 `codebase-memory-mcp install --dry-run` 查看计划，
核对后再按[安装指引](../INSTALL.md)应用。查询新工具本身不要求重新安装客户端。

```bash
codebase-memory-mcp cli list_projects
codebase-memory-mcp cli index_repository '{"repo_path":"/absolute/path/to/repo","name":"example","mode":"moderate"}'
```

请替换示例项目名和路径。目录被 Git 忽略、`.cbmignore` 或其他发现规则排除时，
该目录中的文档/代码不会自动成为引用目标；检查[忽略规则](cbmignore.md)。

## 支持的引用

| Markdown 写法 | 解析方式 |
|---|---|
| `[实现](../src/core.py)` | 相对当前文档定位已索引文件 |
| 行内代码 `` `src/core.py` `` | 尝试文档相对路径和仓库相对路径；两个不同目标产生歧义时跳过 |
| 行内代码 `` `example.src.core.compute` `` | 精确匹配图谱完整 qualified name；先用 `search_graph` 核对 |

最近的包含该行的章节拥有引用，否则归属整个文档。
同一来源节点到同一目标保留首条证据，不返回每一次重复出现。
引用边类型是 `REFERENCES`，自动生成边标记 `producer=document_links`。
删除、重命名和修改后的增量索引会清理旧自动引用，同时保留其他来源的关系。

这是保守的 Markdown 子集。图片、外部链接、HTML 注释、围栏/缩进代码块和引用块
不产生引用。复杂链接、链接标题、reference-style links、URL 编码路径、短符号名、
路由和配置键关联尚未支持；跨行代码跨度会报告受限状态。
没有关联不等于文档无关，也不等于代码没有文档。

## 正向与反向查询

读取文档及章节：

```bash
codebase-memory-mcp cli get_document '{"project":"example","path":"docs/guide.md"}'
```

也可使用 `name`，但路径更明确。新增响应字段：

- `references[]`：`source_qualified_name`、目标 `target` 和证据 `properties`。
- `references_truncated`：最多保留 100 条，按文档行号和目标等字段稳定排序。
- `reference_analysis`：文档处理状态、原因、支持范围和版本。

从代码反查文档：

```bash
codebase-memory-mcp cli get_related_documents '{"project":"example","target":"file:src/core.py","limit":20}'
codebase-memory-mcp cli get_related_documents '{"project":"example","target":"example.src.core.compute","limit":20}'
```

`target` 必须是准确 QN 或 `file:仓库相对路径`，不按短名称猜测。
文件查询聚合该文件及其中符号的引用；精确符号查询只返回该符号的直接引用，
不会自动带上仅引用其所在文件的文档。不存在的目标返回工具错误。
`limit` 默认 20，允许 1-100。

每条结果包含文档/章节 `source`、代码 `target`、`document_qualified_name` 和
原始证据 `properties`。证据包含 `source` 匹配方式、`matched_text`、
`document_span.start_line/end_line`、置信度及目标 QN。
这里的置信度描述确定性匹配，不表示文档内容一定正确或仍然适用。

顶层返回 `total`、`returned`、`truncated`、`status`、`reason`、`scope` 和文档状态计数。
排序按来源路径、证据行、目标等字段确定；没有分页游标，截断时应缩小目标或提高限额。
反向状态聚合项目内文档处理情况，某份不相关文档受限也可能使整体状态为 `limited`。

## 接入开发任务

| 工具 | 文档开关默认值 | 文档输出与限额 |
|---|---|---|
| `build_context` | `include_docs:false` | `documentation_references` 对象；最多 8 条、24 个候选目标，与代码证据共享估算预算 |
| `explain_impact` | `include_docs:false` | `related_documentation` 对象；`document_limit` 默认 20，限制 1-100 |
| `review_change` | `include_docs:true` | 成功读取变更后返回 `related_documentation`；从变更文件及文件内符号查找，最多 20 条，并按估算剩余预算裁剪 |

```bash
codebase-memory-mcp cli build_context '{"project":"example","task":"修改 compute 并核对说明","target":"example.src.core.compute","include_docs":true,"token_budget":4000}'
codebase-memory-mcp cli explain_impact '{"project":"example","query":"src/core.py","target":"file:src/core.py","include_docs":true,"document_limit":20}'
codebase-memory-mcp cli review_change '{"project":"example","since":"HEAD","include_docs":true,"token_budget":4000}'
```

`review_change` 的 Git 比较范围沿用原有规则；评审提交差异时传入实际基准 ref，
不要把 `HEAD` 当成所有 PR 的通用基准。
原有 `documentation` 字符串数组继续保留。`build_context` 关闭文档时，
`documentation_references.status` 为 `not_requested`；另外两个工具关闭时不附该对象。
文档引用与调用影响分开返回，不增加依赖影响数量，也不会直接判定文档漂移或过期。

检查 `truncated`、`budget_truncated`，以及上下文的 `targets_truncated`。
估算 token 不是特定模型 tokenizer 的硬性响应长度保证；文档证据被裁剪时，
仍可用反向查询单独读取。

## 状态与排错

| 状态或现象 | 含义与操作 |
|---|---|
| 工具不可见 | 确认二进制包含改动，重启 MCP 连接；客户端可能缓存工具列表 |
| `unknown / reindex_required` | 旧文档缺少引用分析版本，执行普通索引补建 |
| `unknown / no_indexed_documents` | 未索引到 Document；检查路径、忽略规则和索引覆盖 |
| `limited` | 查阅具体原因、状态计数和源文档；读取失败等暂时问题恢复后重新索引 |
| `ok` 且没有结果 | 只表示声明子集已处理；检查完整 QN、文件路径、歧义与不支持语法 |
| `truncated:true` | 数量或预算截断，不可据此声称没有其他相关文档 |

当前没有独立的“关闭 Markdown 引用索引”配置；查询的 `include_docs:false`
只是关闭本次组合工具的文档输出。不要将规划中的开关或关系类型当成现有参数。
远程档位和源码读取权限见 [REMOTE_MCP.md](REMOTE_MCP.md)，不作为本地使用前提。

## 回归与边界

```bash
python scripts/eval-document-links.py --check-sources
python scripts/eval-document-links.py build/c/codebase-memory-mcp
python -m unittest discover -s tests -p test_eval_document_links.py
bash scripts/eval-build-context.sh ./build/c/codebase-memory-mcp
```

Windows 使用 `.exe`；文档评测可加 `--temp-root C:/msys64/tmp` 指定可写的非系统
ASCII 临时目录。评测创建独立缓存，不修改真实项目索引。

63 个候选来自 8 份真实文档的单行摘录（33 正例、30 反例），以最小目标文件桩测试引用。
它不验证完整原文块上下文，也不是代表性全仓 precision/recall；标注仍是
`maintainer_review_pending`。源码指引修改后要核对出处，不能静默改变预期标签。
新增功能、回归结果及未完成项见 [MILESTONE_ACCEPTANCE.md](MILESTONE_ACCEPTANCE.md)；
阶段方案与尚未实现的关系/UI 见 [DOCUMENT_KNOWLEDGE_GRAPH_PLAN.md](DOCUMENT_KNOWLEDGE_GRAPH_PLAN.md)。
