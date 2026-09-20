# 里程碑验收状态与证据

核对日期：2026-09-20。代码基线：`c09ed61a`，本轮未提交改动另行记录。
周次采用 [20 周原排期](DELIVERY_SCHEDULE.md)，不采用历史“W3-W6 MVP”批次标签。

2026-09-18 范围调整：当前优先开发本地 MCP 功能。用户决定暂缓远程服务验收，
HTTP 高并发问题单独跟踪，不作为文档与代码关联开发的前置阻断。

## 1. 状态总表

| 里程碑 | 原排期 | 已有交付记录 | 尚需关闭的验收项 |
|---|---|---|---|
| M0 可信底座 | W1-2 | 2026-08-18 已记录验收通过 | 持续回归，不因本轮核对重新宣称全量测试通过 |
| M1 上下文编译 | W3-4 | 2026-08-24 已记录功能完成 | 归档当时留待 Linux/CI 的编译、格式和黄金评测证据 |
| M2 PR 评论试点 | W5-6 | CLI/MCP、评审 UI、评论适配、Owner 与规则遥测 | 10 个真实 PR 影子评审、证据可追溯与人工判定 |
| M3 团队远程服务 | W7-9 | 托管 Key、ACL、控制面、Git 索引与部署 MVP | 多主体隔离、目标部署负载/P95、重启恢复、管理与审计验收 |
| M4 文档知识图谱 | W10-12 | Markdown 节点、确定性引用、正反向查询、上下文/影响/评审接入、63 条真实摘录回归 | 摘录标注维护者复核、代表性人工黄金集与精确率 >=95%、路由/配置等关联扩展、漂移与覆盖 UI |
| M5 健康度 | W13-14 | 规划已明确 | 实现、评分证据、真实项目校准与 UI 验收 |
| M6-M8 高级能力 | W15-20 | 规划已明确 | 未在本轮核对中取得整阶段交付证据 |

“实现”“本地测试”“真实试点”“发布部署”是四种独立证据。
模拟 PR、不联网的单元测试、空项目传输压测都不能替代真实项目验收。

## 2. 本轮本地验证

所有测试只使用本机临时数据；本轮不发真实 PR 评论、不修改远端服务、不发布。

| 检查 | 结果 | 证据与限制 |
|---|---|---|
| C 远程/控制面/Watcher/安全套件 | 172 passed，0 failed，3 skipped | `mcp_http httpd watcher security`；本机 Windows，未启用 ASan/UBSan |
| 初次 C 测试 | 132 passed，40 failed，3 skipped | 中文 TEMP 路径导致 Git fixture 创建失败；改用独立 ASCII TEMP 后通过，路径兼容性问题未修复 |
| build_context 黄金评测 | 31/31 通过，退出码 0 | 当前 headless 构建，Windows/MSYS；包含稳定排序、预算、文档/测试及 diff_ref，不替代 Linux/CI |
| PR 评论适配回归 | 26 项通过 | 离线 API mock，不计真实 PR 样本 |
| HTTP 压测工具自测 | 12 项通过 | 本地模拟 HTTP 服务，验证工具自身，不代表生产服务验收 |
| HTTP 多会话传输负载 | 未通过，暂缓跟进 | Windows 20 会话/1000 请求出现访问冲突；未在 stdio 模式复现 |

C 测试命令：

```bash
make -f Makefile.cbm build/c/test-runner CC=clang CXX=clang++ SANITIZE=
./build/c/test-runner.exe mcp_http httpd watcher security
```

运行前必须为 `TEMP`、`TMP`、`TMPDIR` 和 `CBM_CACHE_DIR` 设置独立、可写的临时目录，
并提前创建缓存目录，不能使用真实图谱缓存。
本机本轮日志在 `.tmp/acceptance-20260918-remote/suites-ascii.log`；
初次失败日志在同目录 `suites.log`。这些是未纳入 Git 的本地证据，不是可长期引用的 CI artifact。

`make -f Makefile.cbm cbm CC=clang CXX=clang++ SANITIZE=` 本轮构建成功。
随后运行 `bash scripts/eval-build-context.sh "$PWD/build/c/codebase-memory-mcp.exe"`，
在独立 ASCII 临时目录创建 fixture/cache，31 条黄金评测通过；
日志为 `.tmp/acceptance-20260918-remote/build-context.log`。
首次选用 `C:/Windows/Temp` 被系统目录索引防护拒绝，此次仅有工具输出、未留存独立日志；
最终改用非系统临时目录通过，没有修改安全防护以迁就测试。

Python 回归命令（只使用标准库）：

```bash
python -m unittest discover -s tests -p test_review_change_comment.py -v
python -m unittest discover -s tests -p test_eval_remote_mcp.py -v
```

本轮修复了评论适配器的 GitLab API 地址重复拼接、只查第一页导致的去重遗漏，
以及将失败工具输出误当评审结果的问题。评论新增源码/图关系证据展示，
缺少行号或完整路径时显式说明；长内容有截断标记，不构造未经核实的远程链接。

## 3. M2 真实 PR 影子评审

当前没有归档在本台账中的真实 PR 样本，因此已验收样本数为 **0/10**；
这不代表外部环境绝对没有做过评审，只代表尚无可核验记录。

每个样本必须记录：

- 项目、平台、PR/MR 编号、base/head commit、实际比较基准和采样时间。
- 工具版本、graph generation、freshness、coverage、截断与限制。
- 原始 `review_change` JSON、评论预览，以及真实发布时的评论 ID/地址。
- 规则 ID、命中证据、CODEOWNERS 来源和人工 confirm/ignore 判定及理由。
- 同一 commit 重试的评论 ID，证明是更新而不是重复新增。
- 样本结论：通过、有条件通过、失败；失败的跟踪项与复测结果。

建议选择覆盖公共 API、路由/配置、跨模块调用、测试/文档缺口和低覆盖/过期状态的 PR，
但必须保留真实样本属性，不能为了凑分类改写评审结果。

先在已索引的目标项目上输出只读预览：

```bash
python scripts/review-change-comment.py \
  --binary /path/to/codebase-memory-mcp \
  --root /path/to/repository --project PROJECT_ID \
  --since BASE_REF --dry-run
```

`BASE_REF` 应为实际 PR 比较基准，不使用默认 `HEAD` 冒充 PR base。
用相同参数调用 `cli --json review_change` 保存原始结果。
在 `--dry-run` 下追加 `--rule-action RULE_ID=confirm` 或 `RULE_ID=ignore`、
`--telemetry-file /path/to/review-telemetry.jsonl` 可记录人工判断；不要自动代替评审人判定。
真实发布评论需要用户选定仓库和 PR、配置最小权限凭据，且首轮保持评论模式，不启用合并阻断。
不把 Token、源码中的凭据或未脱敏私有代码纳入公开样本。

## 4. M3 验收边界

### 本地回归与团队试点

现有 `scripts/soak-test.sh` 是 stdio 耐久测试，不能作为 HTTP 20 会话负载的验收证据。
控制面索引任务历史仍在进程内，任务持久化/重启恢复需要单独实现与测试。

新增的传输验收入口：

```bash
# 在进程环境中提供临时测试凭据，不把 Token 写入命令或报告。
python scripts/eval-remote-mcp.py \
  --url http://127.0.0.1:9766/mcp \
  --token-env CBM_REMOTE_MCP_TOKEN --sessions 20 --requests 1000
```

脚本仅在显式配置凭据后运行，默认只允许字面量 loopback 地址，
远端需 `--allow-remote` 且使用 HTTPS。禁用系统代理、重定向和隐式重试。
1000 次仅计 `tools/list` 与 `tools/call list_projects` 的混合工作负载；
initialize、initialized 通知和 DELETE 会话清理另外记录。
报告包含会话唯一性、请求数、错误数、跳过数和工作负载 P95；
初始化、工作负载或清理失败都会使结果失败。
退出码为 0 表示传输测试通过，1 表示测试失败，2 表示配置错误。
这不是完整工具语义、跨主体 ACL、真实图查询或团队运行验收。

| 门槛 | 所需证据 | 当前结论 |
|---|---|---|
| 多会话传输 | 20 独立会话、1000 混合只读请求、错误数、延迟与清理结果 | 本轮工具与结果见后续记录 |
| 多主体权限 | A/B 主体不同项目 ACL、工具列表裁剪、改参越权、会话交叉复用、撤销/轮换 | 本地安全回归不能代替目标部署上的完整矩阵 |
| 查询 P95 <1 秒 | 基准机器配置、真实已索引项目规模、查询集合、并发、P95、错误率 | 空项目 list_projects 不计此项 |
| 索引生命周期 | 重复提交、查询可用性、失败保留旧 generation、重试、重启后的任务状态 | 本地/Git 冲突已有回归；重启恢复尚未验收 |
| 审计与凭据 | principal/项目/工具/状态可关联，日志不含 Token，Key 仅创建时显示 | 需目标部署样本与检查记录 |
| 管理体验 | 项目、任务、generation、覆盖、授权和指标的实际操作记录 | 不以健康探针存在替代完整管理 UI |

完成这些证据前，M3 保持“部分交付、待整体验收”，不标记团队试点完成。

## 5. 下一步顺序

### 本地功能进展（2026-09-18）

已实现确定性文档引用的首个本地闭环：

- 索引后处理生成文档/章节到已索引文件或完整 QN 符号的 `REFERENCES`。
- 记录匹配来源、文档行号、匹配文本和目标 QN，不猜测同名短符号。
- `get_document` 返回引用证据、稳定排序、100 条上限及可见处理状态。
- 增量重建清理旧自动引用，保留其他 producer 的关系；旧索引无改动时也可补建。
- HTML 注释和代码文本不生成引用；暂时读取失败在普通重索引时重试。

本地 `document_links graph_buffer mcp` 套件：**243 passed，6 skipped**；
6 项均为 Windows 不适用的 POSIX/fork 测试。
日志为 `.tmp/document-links-tests.log`。
新增引用后的 `build_context` 黄金回归 **31/31 通过**，
日志为 `.tmp/document-links-build-context.log`。
Windows 本地生产二进制构建成功；本轮修改范围的 Cppcheck 检查退出码为 0，
日志为 `.tmp/document-links-cppcheck.log`。
本轮不是 50-100 条真实人工标注黄金集，
不据此宣称完成整个 W11 或达到真实关系精确率门槛。
支持语法和使用方式见[文档专项 9.5](DOCUMENT_KNOWLEDGE_GRAPH_PLAN.md#95-当前可用的本地引用功能)。

### 本地功能进展（2026-09-20）

本轮继续本地 stdio/MCP 功能开发，不恢复暂缓的 HTTP 验收：

1. 新增真实文档摘录黄金集：8 个仓库 Markdown 文件、63 个候选引用
   （33 正例、30 反例），来源路径/行号/原文 token 可核对。
   实际 stdio 回归 63/63 通过，评测器自身 11 项单测通过。
   fixture 使用真实摘录行及最小 File stub，不包含完整原文块上下文；
   `maintainer_review_pending` 标明维护者待复核，不据此报告全仓 precision/recall。
2. 新增 `get_related_documents`：接受完整 QN 或 `file:仓库相对路径`，
   文件查询包含文件内符号；`limit` 默认 20、上限 100。
   返回 `source`、`target`、`properties` 证据及数量、截断和分析状态，
   不按短名猜测，不把空结果解释为完整覆盖。
3. 开发工作流接入：`build_context` 在 `include_docs:true` 时返回
   `documentation_references` 对象，最多 8 条引用、24 个代码目标，
   共享原有估算 token 预算；`explain_impact` 默认不包含文档，
   可用 `include_docs:true`、`document_limit:20` 返回 `related_documentation`；
   `review_change` 默认包含文档，按 `changed_paths` 查询同形状关联结果并计入估算预算。

可重复命令：

```bash
python scripts/eval-document-links.py --check-sources
python scripts/eval-document-links.py build/c/codebase-memory-mcp
python -m unittest discover -s tests -p test_eval_document_links.py
```

本机 Windows 使用 `build/c/codebase-memory-mcp.exe`，追加
`--temp-root C:/msys64/tmp`，使用独立临时 cache；不使用真实用户索引。
详细契约、参数与黄金集限制见[文档专项 9.5](DOCUMENT_KNOWLEDGE_GRAPH_PLAN.md#95-当前可用的本地引用功能)。
本轮 Windows/MSYS Clang 构建成功（未启用 ASan/UBSan），
`document_links document_refs context_documents graph_buffer mcp` 套件
**255 passed，0 failed，6 skipped**；跳过项为 Windows 不适用的 POSIX/fork 测试。
日志为 `.tmp/document-query-tests.log`；构建记录为 `.tmp/document-query-build.log`
和修复测试夹具后的 `.tmp/document-query-rebuild.log`。
新增 `document_refs` 测试初次因未创建项目而触发外键约束失败；
修复夹具项目初始化及重复边设置后整轮重跑通过，生产查询模块未因此修改。

新生产二进制的上下文黄金回归 **31/31 通过**，
日志为 `.tmp/document-query-build-context.log`；
文档摘录回归 **63/63 通过**，日志为 `.tmp/document-query-golden.log`。
`src/context/document_refs.c` 和 `src/context/build_context.c` 的 Cppcheck 检查退出码为 0，
日志为 `.tmp/document-query-cppcheck.log`；修改 C 文件的 clang-format 检查通过。
这些是本地隔离回归证据，不替代 Linux/CI、维护者标注复核或全仓关系质量验收。

### 文档与指引同步（2026-09-20）

- 新增[文档引用使用指引](DOCUMENT_REFERENCES.md)，统一启用、参数、响应、限制与排错说明。
- 同步 README、安装/贡献指南、npm/PyPI 说明、路线图、排期、文档专项及响应契约。
- 同步 CLI 帮助、安装器生成的 Skill/Agent 指令和只读子 Agent 模板；
  源码注册工具总数为 20，远程 analysis/scout 档位分别为 16/10（仍受权限裁剪）。
- 原文变动后重新核对黄金集来源行号，保留 63 条样例的候选文本与预期标签。
  来源检查、11 项 Python 单测及 63/63 stdio 回归通过，
  日志为 `.tmp/document-docs-golden.log`。
- 本地构建及实际 `--help` 检查通过，CLI 套件 **209 passed，0 failed，7 skipped**
  （Windows 不适用的 POSIX 测试）；日志为 `.tmp/document-docs-cli-tests.log`。
  首轮发现厂商模板测试的旧工具数量断言，修正契约并重跑通过。
- 本地 Markdown 链接目标、代码块配对和 `git diff --check` 通过。

未发布 Release，未覆盖用户现有客户端指令或配置，未恢复 HTTP 验收。
更新现有客户端指引前先查看 `install --dry-run` 计划。

### 后续顺序

1. 保留本轮本地修复与验收工具，归档可重复的测试结果。
2. 完成摘录标注维护者复核与代表性质量评估，再推进相关知识视图与后续关系类型。
3. M2 的 10 个真实 PR 影子评审和 M3 远程服务验收暂缓，不阻断本地功能开发。
4. 恢复团队远程服务交付时，再关闭 Windows HTTP 并发、权限、负载、任务恢复与审计门槛。

本轮 HTTP 诊断记录：默认 16 workers 和 32 workers 对照均在 20 会话/1000 请求中失败；
Windows WER 记录 `0xc0000005`，偏移映射到 `_mi_theap_default_set`。
4 会话/100 请求通过；32 workers 下仅诊断设置 `CBM_MI_THREAD_DONE=0` 后，
20 会话/1000 请求通过，P95 381.210 ms。这支持线程退出回收路径关联，
但不是生产修复，也不证明 stdio 模式存在相同故障。
没有修改回收默认值、生产 C 代码或 vendored 分配器。
原始结果位于 `.tmp/acceptance-20260918-remote/http-soak*.json`。

每次状态推进必须补测试命令、版本、环境、结果和证据位置，不仅修改“已完成”标签。
