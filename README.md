# UEAgentTool

UEAgentTool 是一个 Unreal Engine 编辑器插件，用于把 UE 编辑器连接到本地 UEAgentCraft Backend。它提供 Agent Chat、代码审查、代码生成、日志分析、资产检查和编辑器操作确认面板，让 Agent 能读取当前编辑器上下文，并在用户确认后执行安全的 Editor API 操作。

后端仓库：

https://github.com/Nath-Vikky/UEAgentBackend

## 插件定位

UEAgentTool 负责编辑器侧能力：

- 提供 UE 编辑器内的 Agent 面板和用户交互入口。
- 向后端同步 Project Inventory、当前选中资产、当前关卡 Actor、Blueprint Graph、Widget Tree、材质参数等上下文。
- 展示后端返回的回答、分析结果、代码草稿、日志分析和资产检查结果。
- 展示需要确认的 Editor Operation Proposal，并在用户确认后调用真实 UE Editor API 执行。
- 提供可选的本地 TCP 工具发现能力，供后端感知编辑器侧只读工具。

UEAgentTool 不直接负责 LLM 推理、RAG 检索、Agent 路由、评测和长期记录，这些由 UEAgentCraft Backend 负责。

## 核心功能

- `Agent Chat / Project QA`：在编辑器内向 Agent 提问，支持当前项目、当前选中对象和知识库问答。
- `Code Review`：扫描 UE C++ 文件并把选中文件提交给后端审查。
- `Code Generate`：输入需求后接收后端生成的 UE C++ 草稿和建议路径。
- `Logs Analyze`：提交日志文本或日志文件路径，让后端分析错误原因和排查方向。
- `Assets Inspect`：对选中资产执行命名、类型、引用关系和常见设置检查。
- `Project Inventory Sync`：打开面板后自动同步一次项目清单，也支持手动刷新。
- `Editor Operation Proposal`：支持资产、Blueprint、UMG、Material、Level Actor 等操作的预览、确认和执行。
- `MCP/TCP Tool Sensing`：可选开启本地 JSON-RPC line protocol，让后端发现插件侧只读工具。

## 协作架构

```text
Unreal Editor
  -> UEAgentTool Panel
  -> Project Inventory / Active Context Sync
  -> Local HTTP Request
  -> UEAgentCraft Backend Agent Pipeline
  -> Answer / Analysis / Proposal
  -> UEAgentTool Preview and Confirmation
  -> Unreal Editor API Execution
```

典型写操作链路：

```text
用户在 Agent Chat 提出编辑器操作需求
  -> 后端生成 Proposal
  -> 插件展示操作摘要、目标、参数和风险提示
  -> 用户点击确认
  -> 插件调用 UE Editor API 执行
  -> 插件把执行结果回传给后端记录
```

## 环境要求

- Unreal Engine 5.3 项目。
- 已安装并启用本插件。
- UEAgentCraft Backend 本地运行，默认地址为 `http://127.0.0.1:8000`。
- 使用 Rider 或 Unreal Build Tool 编译插件。

## 快速使用

1. 启动后端：

```powershell
.\.venv\Scripts\python.exe -m uvicorn app.main:app --reload --host 127.0.0.1 --port 8000
```

2. 在 UE 项目中启用 `UEAgentTool` 插件。

3. 打开插件面板，确认后端地址为：

```text
http://127.0.0.1:8000
```

4. 等待插件自动同步 Project Inventory，或点击同步按钮手动刷新。

5. 在 Agent Chat、Code Review、Code Generate、Logs Analyze、Assets Inspect 等面板中使用对应功能。

## Project Inventory 与 Active Context

插件会向后端提交编辑器现场信息，让 Agent 能理解项目和当前上下文。

当前同步内容包括：

- 项目名、活动面板和基础编辑器上下文。
- 资产列表、选中资产、资产类型和路径。
- C++ 文件列表和基础符号摘要。
- 当前关卡 Actor、选中 Actor、Actor 位置和基础属性。
- Blueprint Graph 摘要、节点信息和常见事件节点。
- UMG Widget Tree、Widget 类型、文本、可见性和布局信息。
- Material Instance 参数和 focused Material parameter details。

当用户在聊天里说“这个资产”“当前蓝图”“选中的 Actor”“这个 Widget”时，后端会优先使用这些上下文，而不是直接走普通知识库问答。

## 编辑器操作确认

插件侧写操作必须由用户确认后才会执行。当前 Proposal 可覆盖的方向包括：

- 资产重命名、移动、复制和 Redirector 修复。
- Static Mesh 常见设置调整。
- Blueprint 创建、节点添加、节点连接和基础图编辑。
- UMG Widget 创建、属性调整和层级操作。
- Material Instance 参数调整。
- Level Actor 放置、选择、排列和属性调整。

所有写操作都会先展示摘要和参数预览，用户确认后才会调用 Editor API。

## 可选 MCP/TCP 工具服务

插件可以暴露一个本地 JSON-RPC line protocol，用于未来 MCP 风格的工具发现。该功能默认关闭，不替代现有 HTTP Proposal 安全链路。

如需本地调试，可在 UE 项目的 `Config/DefaultEngine.ini` 中开启：

```ini
[UEAgentTool.EditorToolServer]
bEnabled=true
Host=127.0.0.1
Port=8765
```

当前服务支持：

- `initialize`
- `tools/list`
- `tools/call` 的只读工具调用

写入类工具即使在 TCP 工具列表中可见，也必须继续走后端 Proposal 和插件确认按钮。

## 开发说明

源码目录：

- `Source/UEAgentTool/`：插件主要 C++ 代码。
- `Source/UEAgentTool/Private/SAgentRootPanel.cpp`：编辑器面板、请求发送、结果渲染和 Proposal 执行入口。
- `Source/UEAgentTool/Private/AgentEditorToolCatalog.cpp`：编辑器工具目录和工具元数据。
- `Source/UEAgentTool/Private/AgentEditorToolServer.cpp`：可选 TCP 工具服务。
- `Docs/user-guide.md`：插件使用说明。

生成目录 `Binaries/`、`Intermediate/` 不提交到 Git。

## 文档入口

- [用户指南](Docs/user-guide.md)
- [Docs README](Docs/README.md)

## 相关仓库

- UEAgentCraft Backend：https://github.com/Nath-Vikky/UEAgentBackend
