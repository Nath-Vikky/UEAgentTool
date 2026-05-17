# UEAgentTool

UEAgentTool is an Unreal Engine editor plugin that connects a local UE workflow to UEAgentBackend. It provides editor-side panels for Agent Chat / Project QA, Code Review, Code Generate, Logs Analyze, and Assets Inspect, plus confirmed editor-operation proposals for safe asset and Blueprint actions.

## Scope

- Local Unreal Editor plugin for personal/editor workflow use.
- Communicates with UEAgentBackend over local HTTP.
- Editor write operations require user confirmation before execution.
- Project Inventory snapshots can be submitted to the backend for project-aware QA.

## Requirements

- Unreal Engine 5.3 project.
- UEAgentBackend running locally, usually at `http://127.0.0.1:8000`.

## Usage

See [Docs/user-guide.md](Docs/user-guide.md) for editor usage notes.

## Development

Build from Rider or Unreal Build Tool inside the owning UE project. Generated folders such as `Binaries/` and `Intermediate/` are intentionally excluded from Git.

## Architecture Notes

- Editor write operations are routed through an internal `FUEAgentEditorToolRegistry`.
- `FUEAgentEditorToolCatalog` centralizes tool metadata such as operation type, category, side-effect level, and required/optional fields.
- `SAgentRootPanel` only binds the current UE Editor executors to catalog definitions, so the existing HTTP Proposal flow stays stable.
- The catalog can build a metadata-only tools list for a future MCP/TCP `tools/list` transport without duplicating UI execution code.
