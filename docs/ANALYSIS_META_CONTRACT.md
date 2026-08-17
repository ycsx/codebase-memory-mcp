# 可信分析响应契约

> 状态：W1 候选契约
> Schema：[analysis-meta.schema.json](schemas/analysis-meta.schema.json)
> 适用范围：项目相关的 MCP 原子分析，以及后续 `build_context`、`review_change` 和健康度结果

## 1. 目标

`analysis_meta` 让调用方区分“查到了什么”和“这份结论能信到什么程度”。它必须回答：

1. 查询使用的是哪一代图谱。
2. 图谱是否对应当前源码和 Git 状态。
3. 索引是否记录了已知覆盖缺口。
4. 返回结果是否分页、截断或触及服务上限。
5. 结论是可支持、暂定还是证据不足。

契约不把 best-effort 信号包装成完整性证明。`complete_no_known_gap` 的含义始终是“当前 generation 未记录已知缺口”，不是“动态调用、反射和运行时注册均已被证明不存在”。

## 2. 顶层结构

所有字段固定存在。不适用或无法获得时使用 `null`、`unknown` 或 `not_applicable`，不能静默省略。

| 字段 | 作用 |
|---|---|
| `schema_version` | 契约主版本，v1 固定为整数 `1` |
| `tool` | 产生结果的 MCP 工具名 |
| `profile` | `all`、`analysis`、`scout` 或 `unknown` |
| `project` | 项目名；跨项目聚合或无法确定时为 `null` |
| `graph` | 图谱 generation、索引时间、模式和索引 commit |
| `source` | 当前 Git/源码状态 |
| `freshness` | 图谱与当前源码的一致性判断 |
| `coverage` | 当前 generation 的已知覆盖缺口 |
| `result` | 查询结果的完整性、分页和截断 |
| `confidence` | 证据质量，不是统计概率 |
| `claim` | 调用方能够做出的结论强度 |
| `limitations` | 结构化限制和回退动作 |

## 3. Graph generation

`graph.generation_id` 是一次完整发布的稳定、不透明标识，不能继续直接使用秒级 `projects.indexed_at` 作为最终代际 ID。同一 generation 的 graph、file hashes、coverage 和 metadata 必须一起发布。

兼容阶段允许：

- `graph.generation_id = null`，表示老索引没有稳定代际。
- `graph.indexed_at` 读取现有 `projects.indexed_at`。
- `index_coverage_meta.generation` 只用于判断老 coverage 是否与老项目时间戳匹配。

只有新格式 generation 完成原子发布后，才能写入非空 `generation_id` 和 `generated_at`。

## 4. Freshness

### 4.1 状态

| 状态 | 条件 |
|---|---|
| `current` | generation 已知，并且当前作用域的 commit、工作区或内容证据与索引一致 |
| `stale` | 已发现 commit、工作区或文件内容与索引不一致 |
| `unknown` | 无 generation、无可用源码、无 Git 且未完成内容校验，或探测结果互相矛盾 |

`worktree_state=dirty` 必须使项目级 freshness 至少为 `stale`。无 Git 项目不能仅凭“没有 Git 差异”标记为 `current`；需要完整的内容哈希或明确作用域的 metadata/content 校验。

哈希记录不完整首先是 coverage 证据缺口；如果 Git commit 和工作区状态已经足以证明源码与 generation 一致，不能仅因此把 freshness 降为 `unknown`。只有在 freshness 依赖内容哈希、且没有其他一致性证据时，才将其作为 freshness 的未知原因。

### 4.2 Windows 路径探测

Windows 上的源码根路径和文件探测必须使用统一的 UTF-8 安全文件系统 API。出现“文件系统确认存在，但 freshness 探测为 missing”时：

- `freshness.status` 必须是 `unknown`，不是 `stale` 或 `current`。
- `reason_codes` 包含 `source_probe_inconsistent`。
- 添加 blocking limitation，并要求直接读取源码或重新索引。

## 5. Coverage

`coverage.generation_id` 是生成这份 coverage 记录的代际；
`coverage.generation_match` 表示它是否与 `graph.generation_id` 描述的已发布图谱相同。
旧索引无法比较时，两者均为 `null`，且 coverage 不能被表述为完整。

| 状态 | 条件 |
|---|---|
| `complete_no_known_gap` | coverage generation 匹配、记录完成、哈希记录完整，且当前作用域无已知缺口 |
| `partial` | 存在 `parse_partial`、读取/抽取跳过、排除项或其他明确缺口 |
| `unknown` | metadata 缺失、generation 不匹配、记录截断、查询失败或哈希记录不完整 |

`coverage.signal` 当前默认是 `best_effort`。即使状态为 `complete_no_known_gap`，调用方仍需保留静态分析限制。

已存在但过期的 `parse_partial` 证据允许表达为 `freshness=stale` 与 `coverage=partial` 的组合。它说明上一代图谱有明确缺口，但不能冒充当前源码状态。

`coverage.generation_match=false` 必须同时使 `coverage.status=unknown`；它不能被
`parse_partial` 等旧条目覆盖。`coverage.status=complete_no_known_gap` 要求
`generation_match=true`、`recording_status=complete`、`hash_records_complete=true` 且
`details_truncated=false`。任何一个条件未知或不满足，都必须降级为 `unknown` 或
`partial`，不能产生完整性声明。

coverage 代际不匹配不等同于源码过期：如果 graph generation 与当前 Git commit、工作区状态一致，`freshness` 仍可为 `current`，但 coverage 必须保持 `unknown`，高层结论仍不得宣称证据完整。

## 6. Pagination and truncation

所有有上限的工具必须映射到统一 `result`：

| 现有字段 | 统一字段 |
|---|---|
| `has_more` | `result.has_more` |
| `nextCursor`、`next_offset` | 规范化字符串 `result.next_cursor` |
| `total`、`total_results` | `result.total` |
| 当前数组长度 | `result.returned` |
| 工具参数 `limit` | `result.limit` |
| coverage、源码窗口或服务端硬上限 | `result.truncated` 和 `truncation_reasons` |

当结果碰到请求 limit、响应字节预算、coverage 展示上限、源码窗口或服务端行数上限时，`result.status` 不能是 `complete`。
`has_more=true` 必须同时有非空 `next_cursor`，并且响应是 `partial` 和 `truncated`；
`has_more=false` 时 `next_cursor` 必须为 `null`。工具无法判断分页状态时使用
`has_more=null`，不得捏造 continuation cursor。

## 7. Confidence and claim rules

`confidence.level` 只描述已有证据的质量：

- `verified`：当前声明有直接、可回溯且一致的证据。
- `best_effort`：来自静态图谱或不完整探测，仍有已知边界。
- `unknown`：缺少判断所需的证据。

`claim.status` 控制最终文字结论：

- `supported`：可以陈述已发现的正向关系。
- `provisional`：必须带限制说明，不得表述为确定通过或无影响。
- `insufficient`：组合工具应停止给出确定性结论，并返回补救动作。

以下任一条件成立时，`negative_claim_allowed` 必须为 `false`：

1. `freshness.status != current`。
2. `coverage.status != complete_no_known_gap`。
3. `result.status != complete` 或 `result.truncated=true`。
4. 上游查询仍有 `has_more` 或未消费游标。
5. 使用 `scout` profile 进行否定、穷尽、死代码或无影响判断。
6. 涉及反射、动态调用、运行时注册或事件总线，而没有运行时证据。

此外，`source.worktree_state=dirty` 必须有 `worktree_dirty` reason，并将 freshness
降为 `stale`；`source_probe_inconsistent` 必须将 freshness 降为 `unknown`，并附带
`blocking` limitation。`confidence.level=verified` 不能单独解除这些限制；它必须是
当前、未截断证据的质量描述，而不是对未查询范围的证明。

原子工具返回证据和警告；未来组合工具 `build_context`、`review_change` 在证据不足时必须使用 `provisional` 或 `insufficient`，不能把 `unknown` 解释为 `pass`。

## 8. W1 golden scenarios

本契约需要以下独立黄金响应：

1. fresh + complete_no_known_gap。
2. stale + dirty worktree。
3. no Git + freshness unknown。
4. current + partial coverage。
5. complete coverage metadata missing or hash records incomplete -> coverage unknown。
6. result truncated + continuation cursor。
7. coverage generation mismatch。
8. Windows UTF-8 path probe inconsistent。

黄金文件位于 `tests/fixtures/analysis_meta/`。W1 验证 Schema 和状态组合；W2 将同一批样本接入通用 C 响应构建器。

## 9. 当前实现映射与 W2 边界

当前可复用基础：

- `projects.indexed_at` 和 `root_path`。
- `file_hashes` 的 SHA-256、mtime 和 size。
- `index_coverage` 与 `index_coverage_meta`。
- `cbm_git_context_t` 的 branch、head SHA、base SHA 和 worktree 信息。
- `search_graph`、coverage scope 和 MCP tools/list 的分页信号。

W2 才进入生产代码的工作：

- 持久化真正的 `generation_id` 和 `indexed_commit`。
- 增加 Git dirty 状态。
- 构建通用 `analysis_meta` C helper。
- 首先接入 `index_status` 和 `check_index_coverage`。
- 迁移旧索引，并保证失败时继续读取上一代图谱。

本周不直接把未冻结的字段散落接入 16 个工具。
