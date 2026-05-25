# UEAgentTool 用户指南

更新时间：2026-05-06

## 打开面板

在 Unreal Editor 中打开 UEAgentTool 面板后，先确认底部状态栏显示后端在线。如果后端地址不正确，展开 Settings，修改 Backend Base URL 后点击应用。

## 输出语言切换

顶部工具条和 Settings 都有 `中文 / English` 按钮。默认是中文。

- 选择 `中文` 时，插件会在每次请求里发送 `runtime_options.preferred_output_language = "zh-CN"`。
- 选择 `English` 时，会发送 `runtime_options.preferred_output_language = "en-US"`。
- 切换后插件会把选择保存到本地配置，并尝试同步到当前 backend session。

如果当前结果仍不是预期语言，先看 Debug View 的 `Monitor -> locale`：

- `preferred_output_language`：本轮偏好语言
- `final_output_language`：最终展示语言
- `language_source`：语言来源，例如 `message_override`、`session_preference`、`editor_locale`

## Agent Chat / Project QA

用于普通问答、项目知识问答和上下文咨询。输入问题后点击 `Send`。前端会附带项目名、当前模块、当前文件、选中资产和 session 信息；是否使用项目知识库由后端路由决定。

聊天历史只记录 `Agent Chat / Project QA` 对话。其他工具任务的结果显示在 User View 和 Debug View，不混入聊天上下文。

点击 Debug View 顶部的 `Restore Session` 后，聊天会直接按后端返回的 history 顺序恢复；如果后端已经持久化 assistant 回复，恢复后的顺序应保持 `user -> assistant -> user -> assistant`。

如果询问“知识库有哪些内容”，后端可能返回知识库目录模式。普通界面仍直接展示回答正文；如需确认索引了哪些 knowledge 文件，可在 Debug View 的 Raw Response 中查看 `data.catalog`。

中文问题可以直接询问英文 UE 概念，例如“actor 的生命周期是什么”“增强输入怎么写”。后端会做轻量中英检索扩展，前端不需要额外切换。

Workflow quick actions:
- When Agent Chat returns a multi-step editor workflow, ready steps can appear
  as quick-action buttons in User View.
- Clicking `Create Proposal: ...` sends one ready workflow step to the backend
  and creates one pending Proposal.
- The button does not execute editor writes directly. You still need to review
  the Proposal card and click `Confirm & Execute in UE`.
- If the backend rejects the step because required fields are missing, the
  normal failure message is shown and no editor operation is executed.
- After a confirmed editor operation reports diagnostics, User View can also
  show follow-up quick actions such as `Create Follow-up Proposal:
  connect_blueprint_nodes`. These follow the same rule: they create a pending
  Proposal only, and you must confirm it before any UE edit runs.

## Code Review

1. 切换到 `Code Review`。
2. 在搜索框输入类名、文件名或模块名。
3. 点击 `Refresh Files`，或在搜索框按 Enter。
4. 在文件列表中选择目标 `.h/.cpp/.cs` 文件。
5. 可填写 focus，例如 `Lifecycle`、`Performance`、`API`。
6. 点击 `Analyze Selected File`。

结果显示在 User View，完整请求和调试信息在 Debug View。

如果没有选择文件，面板会阻止提交并在状态栏显示提示。

如果后端请求失败，插件会临时扫描当前工程 `Source/Plugins` 作为兜底，并在状态栏提示来源。如果后端成功返回空列表，状态栏会显示 `scan_diagnostics.empty_reason`，用于判断是路径、查询条件还是扩展名导致为空。

Code Review 的“文件读取范围”会显示后端实际读取的文件路径和读取状态；如果读取失败，或后端返回 `status_hint = read_error`，会作为错误提示展示。

如果后端开启了 LLM 综合审查，Debug View 的 Overview 会显示 `LLM 审查：已完成`；如果未执行，会显示跳过原因，便于判断是配置、JSON 解析还是文件读取问题。

Code Review 结果里如果出现“LLM 分析结果”卡片，它表示后端对规则问题、知识库证据和当前文件做了综合解释。`已跳过` 只代表本次 LLM 分析未执行，规则扫描和文件读取结果仍然可以参考。

如果该卡片显示“说明”和“原因代码”，前者是给用户看的原因，后者是联调用稳定枚举，例如 `missing_openai_api_key` 或 `json_parse_failed`。

如果结果里出现 `Agent Workflow`、`Fix Draft` 或 `Validation Plan` 相关卡片，它们是后端追加的轻量 Agent 工作流说明、非破坏性修复草稿和验证建议。`Fix Draft` 只是建议，不表示后端已经修改工程；`Validation Plan` 是待执行清单，不表示后端已经完成测试。

## Code Generate

输入要生成的代码需求，保留或修改 `target_type`，点击 `Generate`。`target_type` 默认是 `ue_cpp`，适合 UE C++ 草稿；如果需要其他类型，可以改成 `cpp`、`blueprint`、`tooling` 等后端支持值。

生成结果会显示为“代码草稿”按钮 / Tab。点击某个草稿后，可以查看完整代码、复制代码或复制建议路径。草稿的 `file_path` 只是建议放置位置，不表示磁盘上已经有这个文件。

默认模式只返回虚拟草稿，不会自动写入项目、不会修改源码、不会编译。看到 `write_status=not_written` 或 `is_virtual=true` 是正常状态，表示仍需用户手动复制和应用。

如果需要让后端生成可确认的写入提案，可以勾选 `Create write proposal after generation`。这只会请求后端返回 `write_code_files` proposal，不会直接写盘；必须在结果区或 Debug View 的 `Proposal` 卡片中点击 `Confirm` 后，后端才会按安全策略写入允许路径。点击 `Reject` 会拒绝该提案。

写入提案会显示待写文件路径、状态、大小和 reason。确认后可在 Raw Response、Tools 的 `code_write_result/side_effects` 以及 Artifacts 中查看执行结果。

后端已补充常用 UE 代码知识库和兜底模板。可以测试“角色增强输入代码怎么写”“交互组件 overlap 怎么写”“射线交互组件怎么写”“全局管理器子系统怎么写”等请求，结果应能在草稿里展开查看完整 `.h` / `.cpp` 正文。

如果结果里出现 `Validation Plan`，它用于提示手动放置草稿后应如何验证，例如检查 Build.cs 依赖、编译模块、配置 Enhanced Input 资产、运行 PIE 烟测和复查 Output Log。

需求为空时不会提交请求。

## Logs Analyze

Logs Analyze 现在支持三种常用输入方式：

- 只填写日志来源或文件路径，例如 `Output Log`、`Saved/Logs/RushBa.log` 或 crash context 路径。
- 只粘贴几行 `Error` / `Fatal` / 调用栈片段。
- 同时提供日志文件路径、错误片段和备注。

面板分为三块：

- `Log Source / File`：日志来源或显式文件路径。
- `Error Snippet / Pasted Text`：几行错误片段或较长日志文本。
- `Notes / Attachments`：复现备注或用分号分隔的附件路径。

只要填写了日志文件/source 路径或错误片段，就可以点击 `Analyze Log`。插件不会主动扫描 UE 日志目录，也不会监听实时 Output Log；后端只会只读分析你显式传入的文本或路径。

如果 Logs Analyze 返回 `Validation Plan`，它是排查建议清单，例如确认完整日志窗口、复现步骤、首个 Error/Fatal 和相关模块，不表示后端已经替你执行验证。

Logs Analyze 结果里如果出现“LLM 分析结果”卡片，它表示后端对日志摘要、规则解析和可用知识库证据做了综合解释。未配置 LLM 时，该卡片会显示 `已跳过` 或原因代码，这只是解释层未执行，日志规则分析和验证清单仍可参考。

## Assets Inspect

1. 在 Content Browser 选择一个或多个资产。
2. 切换到 `Assets Inspect`。
3. 面板会显示资产路径、类型、依赖数量和引用数量。
4. 可填写检查备注。
5. 点击 `Inspect Selected Assets`。

如果没有看到选中资产，先重新选择资产或切换面板触发上下文刷新。

未选择资产时不会提交请求。

Assets Inspect 结果里也会显示“LLM 分析结果”卡片，用于解释命名、类型和依赖关系的综合影响。

如果这次检查命中了项目级 Inventory，结果区还会出现“项目资产匹配”补充卡片。它表示后端除了当前选中资产，还额外从已提交的 Project Inventory 里命中了相关项目事实。

如果 Assets Inspect 返回 `Validation Plan`，它可能包含重命名建议、Fix Up Redirectors、蓝图编译、StaticMesh 设置或 Reference Viewer 检查项。这些都是人工/编辑器验证提示。

## Editor Operation Proposal

如果后端返回编辑器操作提案，结果区或 Debug View 的 `Proposal` 卡片会显示 `Confirm & Execute in UE`。

当前支持 6 类操作：

- `rename_selected_asset`：重命名单个资产，不移动目录，不自动修复 redirectors。
- `apply_static_mesh_basic_settings`：只修改单个 Static Mesh 的 Nanite、Collision、LOD Group、Lightmap UV、Lightmap Resolution 白名单字段。
- `create_blueprint_asset`：在 `/Game` 下创建普通 Blueprint，不自动打开编辑器，不放入关卡。
- `add_blueprint_variable`：向现有 Blueprint 添加变量，支持常见内置类型和可解析的 `/Script/`、`/Game/` 类型路径。
- `add_blueprint_component`：向现有 Blueprint 的 Simple Construction Script 添加组件；`attach_to` 和 `transform` 属于 best-effort 字段。
- `create_blueprint_event_stub`：在 EventGraph 创建基础事件节点，只支持 `BeginPlay / Tick / ActorBeginOverlap / ActorEndOverlap`，不生成复杂连线或完整逻辑。

点击确认后，插件会先通知后端该 Proposal 已确认，再调用 UE Editor API 执行，最后把执行结果回传后端。点击拒绝只会通知后端取消，不会执行编辑器操作。

这些操作都使用 UE Transaction / Undo 机制，并且只标记资产包 dirty，不自动保存。执行后仍建议在 Content Browser 和 Output Log 中复查结果，需要保留时由用户手动保存资产。

## Debug View

Debug View 用于联调和排错。常用入口：

- `Raw Request`：查看 UE 插件实际发出的请求。
- `Raw Response`：查看后端原始响应。
- `User Projection`：查看 `user_view`。
- `Debug Projection`：查看 `debug_view`。
- `Skill`：查看后端本次使用的固定 Skill、collector/rules/retrieval/projector 信息。
- `Trace`：查看事件回放和 trace links。
- `Artifacts`：查看后端返回的产物和 approval 结果。

Monitor 中会保留 capabilities、knowledge-base status 和 bootstrap 快照。如果要排查知识库导入链路、支持格式、解析依赖或知识域，优先查看 Monitor 的 `knowledge_base_status`。

如果要排查知识库目录、中文检索英文知识文档、Code Generate 命中的 UE 示例、Logs Analyze 输入采集、Logs Analyze LLM 解释层、ReAct 工具选择或 Validation Advisor，可在 Raw Response / Debug Projection 中查看 `data.catalog`、`data.reference_lookup.sources`、`data.input_context.input_mode`、`data.input_context.read_diagnostics`、`data.retrieval_quality_gate`、`data.react_loop`、`data.project_file`、`data.tool_contracts`、`data.self_reflection`、`debug_view.local_search.items`、`debug_view.retrieval`、`step_results` 和 `debug_view.tools`。

Logs Analyze 的弱知识库命中质量门槛只作为 Debug View 诊断。普通用户主结果只展示后端认为可用的引用和结论，不把低质量命中强行当作事实。

Agent Chat / Project QA 如果命中受控 ReAct，只读工具计划、当前文件读取、工具契约和自检结果都放在 Debug View。普通聊天区仍只显示最终回答。当前 `current_file` 是 UE 插件可采集到的工程代码上下文，不一定等同于外部 IDE 中当前打开的源码文件；需要精确审查某个文件时仍建议使用 Code Review 面板。

长期记忆不新增普通用户管理面板。项目名会随请求传给后端，后端可按项目召回少量项目约定；调试时可在 Monitor 的 `memory_summary.long_term_memory` 查看。

Tool Protocol v2 的 `active_context`、`tool_registry_protocol`、`tool_execution_policy`、`side_effects` 会集中出现在 Debug View 的 `Tools` 分区，`active_context` 也会进入 `Intent / Route`。MCP adapter 状态当前只作为 Debug 信息查看，UE 插件仍使用 HTTP，不作为 MCP client。

Eval Report API 当前没有普通用户面板；如需确认后端是否暴露评测能力，可在 Monitor 的 `capabilities_snapshot.evaluation` 查看。

Editor Operation Bridge 的能力查询结果在 Monitor 的 `editor_operation_capabilities`。执行后的结果可在 Raw Response、Proposal、Tools 的 `side_effects` 和后端记录的 operation result 中排查。

打开插件面板时，UEAgentTool 会自动静默提交一次 Project Inventory；如果后端当时未启动，自动提交会跳过，用户仍可在 Debug View 顶部点击 `Submit Inventory` 手动重试。`Submit Inventory` 会扫描当前工程的 Asset Registry 资产和 `Source/Plugins` 代码摘要，并提交到 `POST /api/v1/project-inventory/snapshot`。这用于让 Project QA 回答项目事实问题，例如工程里有哪些资产、某类资产设置、某个 Blueprint 有哪些组件/变量/函数/图表、某模块有哪些代码文件。插件不会解析 `.uasset` 二进制，也不会修改资产。

Inventory snapshot 会尽量补充 Blueprint 的 `parent_class/components/variables/functions/graphs/interfaces/editor_flags`，Static Mesh 的 `nanite_enabled/lod_count/collision_complexity/lightmap_resolution`，以及代码文件的 `classes/symbols/modified_at`。

当前版本还会把编辑器当前 World 中已加载关卡的 Actor 摘要提交到 `level_actors[]`，包括 `actor_label/actor_class/level_name/blueprint_path/transform/components/tags/mobility`；同时会把 Material Instance 摘要提交到 `material_instances[]`，包括 `parent_material/scalar_parameters/vector_parameters/texture_parameters/static_switch_parameters`。因此 Agent Chat / Project QA 可以在提交快照后回答“当前关卡有哪些 Actor”“某个材质实例有哪些参数”这类项目事实问题。插件仍然不会解析 `.uasset` 二进制，也不会修改资产。

提交成功后结果区会显示快照摘要和“UE 侧扫描诊断”。摘要来自后端 `snapshot.summary`，扫描诊断来自 `snapshot.scan_diagnostics`。

如果想验证聊天恢复链路，先发起几轮 Agent Chat，再点击 `Restore Session`。恢复后的聊天顺序应与后端 history 保持一致，而不是按本地时间重新排序。

## Blueprint Graph Result Details

Blueprint graph operations now report richer result data back to the backend:

- `created_nodes[]` and `linked_nodes[]` include stable `node_id`, `node_name`, `node_class`, node title, position, and role.
- `linked_pins[]` includes source/target pin metadata such as `pin_id`, `pin_name`, direction, and pin type.
- `linked_pin_summaries[]` contains display-friendly strings.
- `add_blueprint_node_template` also reports `created_node_id` and, when an entry event is used, `entry_node_id`.

These fields help the backend create safer follow-up Proposals, for example a pin-connection repair suggestion. They do not execute follow-up edits automatically.

When the backend returns `editor_operation_result_summary` or `editor_operation_follow_ups` blocks, User View now expands the most useful fields into readable bullet points: execution state, Blueprint path, graph name, created node count, linked pin count, compile status, diagnostic flags, repair advice, and ready follow-up candidate counts. The full JSON is still available in Debug View.

## Highlights Window

User View 不再直接把 summary、issues、recommendations 等高亮信息塞进主聊天区域。任务完成后，主面板只显示一条结果摘要，点击“打开高亮”可在独立窗口查看完整高亮内容。

Assets Inspect 的命名问题会优先显示 severity、reason 和 suggestion。

“LLM 分析结果”卡片会放在摘要后方，便于先看综合判断，再看具体问题和建议。

前端会把 stable block 标签、状态和严重级别显示为中文；如果原因、建议或摘要正文仍出现英文，通常表示后端 `user_view` 自然语言没有按中文输出，需要按交接文档继续调整。

切到英文模式后，这些 stable block 标签和派生状态文案也会切到英文；真正的正文摘要、原因和建议仍以后端返回为准。
