# 里程碑验收状态与证据

核对日期：2026-09-20。本轮 W12 部分闭环的代码基线：`14692b6e`，未提交改动另行记录；此前轮次保留原有测试记录。
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
| M4 文档知识图谱 | W10-12 | 文档结构、双向引用、组合工具、提醒、基础覆盖及持久内容版本复核；本地 C、跨项目和浏览器验证通过 | 维护者标注复核、代表性人工黄金集与精确率 >=95%、路由/配置关联、历史删除引用、完整漂移及风险覆盖矩阵 |
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

### W12 部分闭环（2026-09-20，基线 `14692b6e`）

按“完整文档试用、变更提醒、UI”顺序实现，本轮不恢复 HTTP 并发验收：

1. `scripts/eval-document-links-full.py` 复制 923 个 Git tracked 真实文件，
   使用独立 repo/cache 通过 stdio 索引，验证 8 份完整 Markdown 的 63 个候选，
   **63/63 通过**。明确排除 vendor 目录和大于 2 MB 的文件，不使用目标 stub。
   七处重复引用采用更早的证据行号；一份文档的 `limited` 状态明确保留。
   两套评测器合计 **19 项 Python 测试通过**，原 63 条摘录期望不变。
   [完整报告](reports/DOCUMENT_REFERENCES_FULL_2026-09-20.md)记录排除范围及解析限制；
   标注仍待维护者复核，不是 >=95% 人工黄金集验收。
2. `review_change` 增加文件级“建议复核”证据：引用目标所属文件发生变化，
   不断言具体符号一定改变，也不判断文档过期。每条 `review` 保留触发文件、
   目标、来源文档是否同时修改；`review_basis` 保留比较口径、
   当前索引引用来源及 `current/stale/unknown` 新鲜度。
   提醒不改变风险等级或阻断规则，开销纳入证据预算。
   当前索引已清理的历史删除/重命名引用无法恢复，不是持久化漂移跟踪。
3. 代码详情与评审结果展示相关文档、章节及行号证据，支持原文阅读和命中行高亮；
   加入加载、空结果、错误和请求竞争处理。这里只交付最小相关文档 UI，
   此轮未交付代码/文档混合图层、覆盖矩阵、知识健康摘要或漂移队列；
   基础覆盖矩阵的后续实现单独记录如下。

本轮聚焦验证：**206 项 C 测试通过，6 项平台跳过**；
真实本地 stdio 变更提醒 fixture 通过；**93 项前端测试通过，前端构建通过**。
这些计数是本轮选定套件，不替代上面历史套件或全量 CI。
桌面/移动浏览器视觉及交互 QA：**最小 UI 流程通过**。本环境未提供 Browser 技能，
使用普通 Playwright fallback，连接真实本地 console 与 Vite，数据来自隔离 Git fixture。
在 1440x1000 和 390x844 视口验证“评审提醒 -> 定位函数（正确显示无直接引用）
-> 文件详情反查 -> 阅读 Markdown 并高亮第 5 行”。
移动页面无横向溢出，未见框架错误遮罩或应用 console error；
已有 `favicon.ico` 404 和两次 `THREE.Clock` deprecated 单独记录为警告，
不据此宣称所有控制台消息均无错误。
图谱 canvas 为 854x913，像素检查得到 3,741 种颜色和 143,754 个非背景像素；
非空检查及滚轮缩放前后截图变化检查通过。
证据为 `C:/Temp/cbm-w12-qa/browser-result.json`，同目录
`review-desktop.png`、`node-desktop.png`、`review-mobile.png`。
这是本地未纳入 Git 的 QA 证据，不替代跨平台 CI、全部图谱交互或完整 W12 验收。

### 覆盖矩阵与内容版本复核（2026-09-20，本地验证通过）

新增实现与上一轮验收分开记录，上一轮通过结果不自动覆盖本轮改动：

- `get_document_coverage` 提供已索引非 Markdown File 与文档引用状态的基础覆盖查询，
  包含分页；这是当前已索引集合，不是全仓文档充分性或高风险覆盖评分。
- `update_document_review` 使用完整来源文件和目标文件的当前字节指纹绑定内容版本。
  用户可确认或重新打开复核；内容恢复为已确认版本时可恢复该版本确认状态。
  状态持久化在独立 `.reviews.sqlite` sidecar，不依赖图谱重建保留，
  但不存储已删除的历史引用边，也不证明文档内容正确或永不过期。
- UI 已实现代码/文档筛选、分页、原文阅读以及使用服务端 token 的确认/重新打开操作。
  这不是完整风险覆盖矩阵、自动漂移判定或用户忽略策略。
- 跨项目试用采用本仓与实际本地 `aikb-web` 的有界副本；
  生成的生命周期 fixture 单独记录，不能冒充真实项目人工标注。

当前验证：前端空闲环境重跑 **103/103 通过，构建通过**。
此前并发运行时为 102 passed / 1 failed，失败为既有 `StatsTab` readOnly 断言波动；
未修改无关代码，空闲重跑通过。

C 聚焦回归最终 **724 passed，0 failed，13 项平台 skipped，退出码 0**，
日志 `.tmp/document-workflow-tests-native.log`。套件为
`store_nodes document_links document_refs context_documents mcp pipeline cli agent_profiles`，
包含全量/增量重索引期间交错确认、取消重试、缓存来源内容变更和 sidecar 删除隔离。
Windows/MSYS Clang 构建通过，日志 `.tmp/document-workflow-build.log`；
本轮显式 `SANITIZE=`，未运行 Windows ASan/UBSan。

初次编译发现测试缺少 `platform.h`，仅修正测试头文件。
随后 make 内运行继承 MSYS TEMP `/c/Users/王骁/...`，导致 fixture 失败：
458 passed、266 failed、13 skipped。原生 PowerShell 下为**同一已编译二进制**
设置 `TEMP`、`TMP`、`TMPDIR=C:/msys64/tmp` 后得到上述 724/13 结果，
没有加入生产绕过来迁就测试。Unicode 生产缓存路径已通过独立跨项目评测验证；
这不宣称修复了 MSYS 测试临时路径兼容性。
sidecar 使用 `.reviews.sqlite` 后缀，以免被图谱数据库扫描误识别。
未恢复网络/远程验收，原 W12 与 M4 整阶段仍未验收。

跨项目实际试用已通过，使用 Unicode 临时根目录 `C:/Temp/cbm-doc-复核`；
证据为 `.tmp/document-workflow-verified.json`：

| 真实本地有界副本 | 文件 | 已索引代码文件 | 文档 | 文档分析 | 引用 |
|---|---:|---:|---:|---|---:|
| 本仓 | 269 | 240 | 29 | 29 ok | 53 |
| aikb-web | 265 | 240 | 25 | 24 ok，1 limited | 2 |

合计 534 个文件、54 份文档，原项目内容哈希保持不变；
`aikb-web` 的 `skills/vue-iview-spec/SKILL.md` 明确保留
`unsupported_multiline_code` 限制，不将其算作完整解析。
独立生成的生命周期 fixture **11 场景通过**，涵盖重启、重索引、代码/文档/共同编辑、
旧版本、删除、重命名和隔离，不计真实人工黄金样本。
Python 组合测试 **30 项：29 通过、1 项 Windows symlink 跳过**；
完整文档候选回归重新运行 **63/63 通过**。

新增工作流浏览器验证通过：因无 Browser 插件，使用普通 Playwright，
连接真实本地 console `9749` 与 Vite `5173`，视口为 1440x1000、390x844。
通过覆盖筛选、引用打开、确认、页面重载后确认保持、重新打开、编辑后旧 token 拒绝、
刷新后确认、再次编辑回到待复核、文档原文阅读与移动无横向溢出流程。
未见框架错误遮罩或应用错误；已有 favicon 404 和两次 `THREE.Clock` 弃用警告单列。
证据为 `C:/Temp/cbm-w12-qa/workflow-browser-result.json`、同目录
`workflow-desktop.png` 与 `workflow-mobile.png`。
浏览器使用本地 UI 服务，不是恢复远程 MCP 验收。

### 后续顺序

1. 保留本轮本地修复与验收工具，归档可重复的测试结果。
2. 保留已通过的 C、跨项目、生命周期及浏览器独立证据，推进维护者标注复核、历史引用保留和后续关系类型。
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
