# Integration Handover (Next Phase)

## A. MCP Tool Registration (get_flutter_package_map first)
Steps:
1. Add entry to `TOOLS[]` in `src/mcp/mcp.cpp` (name, title, description, inputSchema).
2. Add dispatch branch in `dispatch_tool()`:
   if (strcmp(tool_name, "get_flutter_package_map") == 0) return handle_get_flutter_package_map(srv, args_json);
3. Implement `handle_get_flutter_package_map` returning JSON with packages/deps/signals/violations.
4. Link `cbm_flutter` and `cbm_knowledge` in CMake `cbm_mcp` target (or new target).
5. Build full binary in Debug; run CLI smoke: `code-cortex-mcp cli get_flutter_package_map '{"repo_path":"..."}'`.

## B. Dart Semantic Layer (use official Dart MCP server)
Instead of building a custom analyzer sidecar, configure the official Dart and Flutter MCP server:
```
dart mcp-server
```
in the MCP client config. It provides analyzer diagnostics, symbol resolution, runtime introspection, package management, and testing. Our engine complements it with monorepo awareness, knowledge store, and the 7 new tools.

## C. Knowledge Governance
1. Implement status transitions: draft -> validated -> stale/deprecated.
2. `learn_library_usage` creates draft only; promotion requires source+test+review.
3. Stale detection when recommended API changed.

## D. Remaining tools order
get_flutter_package_map -> get_implementation_guide -> validate_implementation_recipe -> search_approved_examples -> get_feature_context -> get_change_impact -> learn_library_usage.
EOF