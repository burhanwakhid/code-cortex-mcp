# Knowledge Store Design (Flutter Monorepo Code Intelligence)

## Scope
Separate from structural graph (SQLite Code Cortex). Stores engineering knowledge with source refs.

## Storage
- Default local SQLite: `~/.cache/code-cortex-mcp/knowledge.db`
- Optional shared: PostgreSQL (enterprise hardening).

## Tables
- `knowledge_item(id, type, title, scope, owner, version, status, created_at, updated_at)`
  - type: library_guide, implementation_recipe, architecture_rule, approved_example, anti_pattern, testing_recipe, decision_record
  - status: draft | validated | stale | deprecated
- `knowledge_source(item_id, graph_node_id, file_path, line_start, line_end, confidence, is_static_fact)`
- `knowledge_scope(item_id, team, feature, package)`
- `knowledge_history(item_id, changed_at, change_type, note)`

## Validation
- `validate_implementation_recipe`: check source refs exist, API symbols still present (via graph), status transitions.
- `learn_library_usage`: creates draft items only; requires source + test + review to promote to validated.

## Retrieval
- Rank by intent match, scope (feature/team), dependency proximity, recipe validity, example quality.
- Context budget (bytes/tokens) + continuation handle.

## MCP tools (planned)
- get_implementation_guide(intent, feature?)
- learn_library_usage(library, package?) -> draft
- validate_implementation_recipe(id)
- search_approved_examples(intent, feature?)
EOF