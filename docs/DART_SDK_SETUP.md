# Dart SDK Setup Handover (for Dart Analyzer integration)

## Accounts/keys
No external account required. Dart SDK is free.

## Install (Linux)
Option A (apt):
```
sudo apt-get update
sudo apt-get install -y dart
```
Option B (download):
```
wget https://storage.googleapis.com/dart-archive/channels/stable/release/latest/sdk/dartsdk-linux-x64-release.zip
unzip dartsdk-linux-x64-release.zip -d /opt
export PATH=/opt/dart-sdk/bin:$PATH
```

## Verify
```
dart --version
dart analyze --version
```

## Usage for adapter
- Use `dart analyze` / Analysis Server (`dart language-server` or `dart analyze --machine`) for semantic resolution.
- Integration should be optional sidecar process with timeout; fallback to Tree-sitter if unavailable.
- Generated code: run `dart run build_runner build` before analysis for `.g.dart`/`.freezed.dart`.

## Official Dart MCP Server (recommended for semantic layer)
Instead of building a custom analyzer sidecar, use the official Dart and Flutter MCP server:
```
dart mcp-server
```
Configure in MCP client config:
```json
{
  "mcpServers": {
    "dart": {
      "command": "dart",
      "args": ["mcp-server"]
    }
  }
}
```
Capabilities: static analysis & diagnostics, symbol resolution, runtime introspection, package management, testing & formatting.
Requires Dart 3.9+.
```

## Commands to test on a Flutter monorepo
```
cd <repo>
dart pub get
dart analyze
dart run build_runner build --delete-conflicting-outputs
```
