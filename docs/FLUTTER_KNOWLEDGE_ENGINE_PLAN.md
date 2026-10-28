# Plan: Code Cortex MCP → Flutter/Dart Engineering Knowledge Engine

Berdasarkan proposal di thread dan audit repo `tigercosmos/code-cortex-mcp` (v0.23.0, C++23, 155 bahasa Tree-sitter). Prinsip: pertahankan Code Cortex sebagai structural engine, tambahkan 3 lapisan (Flutter/Dart intelligence, engineering knowledge, context retrieval).

## 0. State saat ini (audit)

- Bahasa: C++23, single static binary, SQLite graph lokal (`~/.cache/code-cortex-mcp/`).
- Dart terdaftar di `src/discover/language.cpp` (`.dart`), kategori "good" (75–89%) per klaim upstream — perlu diuji pada monorepo perusahaan.
- Pipeline: `file discovery → tree-sitter extraction → registries → call/type resolution → SQLite`. Pass: definitions, calls, semantic, semantic edges, tests, complexity, gitdiff, githistory, pkgmap, route_nodes, configlink, cross_repo, lsp_cross, enrichment, similarity, artifact, branch_slots.
- Graph schema: Project, Package, Module, File, Class, Function, edges CALLS/IMPORTS/IMPLEMENTS/DATA_FLOWS, dll.
- Incremental & branch-aware sudah ada (one index per checkout/branch, `pipeline_incremental.cpp`).
- Tidak ada Dart analyzer integration, tidak ada pubspec workspace parser, tidak ada knowledge store/recipes.

## 1. Tujuan & non-goals

Tujuan: ketika agent diminta implementasi fitur, ia tahu library mana (internal) yang harus dipakai, arsitektur yang berlaku, contoh yang benar, dan aturan yang tidak boleh dilanggar.

Non-goals: tidak mengganti core graph engine; tidak memindahkan graph ke server DB (tetap SQLite lokal default); tidak auto-publish hasil `learn_library_usage` (status draft).

## 2. Arsitektur high-level

```
repository (Flutter monorepo)
   |
   v
[Discovery & Tree-sitter]  (existing)
   |
   v
[Flutter/Dart Intelligence Adapter]  (NEW)
   - Dart analyzer / Analysis Server (symbol, import, type, generated)
   - pubspec.yaml + workspace parser
   - Flutter conventions (widget composition, state mgmt, navigation, DI)
   - generated code mapping (build_runner, freezed, json_serializable)
   |
   v
[Structural Graph] (SQLite, existing)  +  [Knowledge Store] (NEW, separate, SQLite/PG)
   |
   v
[MCP Tools] existing + 7 new high-level tools
   |
   v
[Context Retrieval Layer] (intent/scope/ranking/budget)
```

Prinsip: pisahkan structural facts (graph) dari knowledge (recipes/ownership/policy) dengan source references. Bedakan fakta statis (pubspec dependency) vs inferensi (arsitektur package) dengan confidence + bukti.

## 3. Roadmap fase (P0/P1)

### Fase 1 — Flutter monorepo discovery (P0)
- Tambah parser workspace/package: pubspec.yaml, dependency_overrides, workspace config, lockfile.
- Package graph: DartPackage, Feature, dependency boundaries, allowed/forbidden deps.
- Ignore rules generated/cache (`.dart_tool`, `build/`, `*.g.dart`, `*.freezed.dart`).
- Targeted reindex per package (reuse `pipeline_incremental.cpp`).
- Uji pada repo nyata sebelum ubah schema besar.

Deliverable: `get_flutter_package_map`, package-aware incremental indexing.

### Fase 2 — Dart analyzer integration (P0)
- Integrasi Dart Analysis Server / analyzer API sebagai enrichment layer terpisah (bisa dinonaktifkan), bukan pengganti Tree-sitter.
- Perkaya resolusi symbol, import alias, generic, extension, interface/impl, cross-package.
- Generated code mapping: hubungkan `.g.dart`/`.freezed.dart` ke source generator.

Deliverable: Dart semantic indexing depth naik; layer enrichment dapat di-toggle.

### Fase 3 — Recipes & library learning (P0)
- Knowledge store schema: library_guide, implementation_recipe, architecture_rule, approved_example, anti_pattern, testing_recipe, decision_record.
- Scope per team/feature, owner, version, validation status, source refs, history.
- Tools: `get_implementation_guide`, `learn_library_usage` (draft), `validate_implementation_recipe`, `search_approved_examples`.
- Mulai 3 use case: local storage, UI component, API integration.

### Fase 4 — Enterprise hardening (P1)
- Git sync, recipe invalidation, permission, audit log, shared knowledge store (PostgreSQL opsional).
- Context budget & retrieval ranking (intent, scope, dependency proximity, recipe validity, example quality).
- Branch/worktree awareness untuk knowledge (reuse branch slots).
- Benchmark regresi (correctness, efficiency, implementation quality, knowledge quality).

## 4. Perubahan graph schema (bertahap)

Tambah node (sebagai metadata/edge dulu bila cukup): FlutterApp, DartPackage, Feature, GeneratedArtifact, TestSuite, ImplementationRecipe.
Tambah edge: DEPENDS_ON_PACKAGE, OWNS_FEATURE, USES_LIBRARY, TESTED_BY, GENERATED_FROM, FOLLOWS_RECIPE.
Aturan: jangan buat node baru jika info sudah cukup diwakili. Setiap inferensi harus punya confidence + evidence.

## 5. MCP tools baru

| Tool | Perilaku |
|------|----------|
| get_flutter_package_map | Package & dependency relevan |
| get_feature_context | Arsitektur, state mgmt, DI, UI, test suatu feature |
| get_implementation_guide | Recipe untuk intent (storage/API/UI) |
| learn_library_usage | Analisis custom library → draft recipe |
| validate_implementation_recipe | Cek source ref, API, status validasi |
| get_change_impact | Graph impact + package/feature dependents |
| search_approved_examples | Contoh implementasi approved |

Pertahankan tools asli (inspect_symbol, search_graph, trace_path, query_graph, get_code_snippet, get_architecture, get_graph_schema, search_code, index_*).

## 6. Info Flutter yang harus dikenali

State management (Provider/BLoC/Riverpod/internal), DI (registration, scope, constructor, generated), navigation (route, args, deep link, antar-feature), UI (custom widgets, design system, theme), data (API client, model, repository, cache, local storage), code generation (freezed, json_serializable, DI generator, build_runner), testing (widget/viewmodel/repository → unit/widget test + fixture), platform integration (method channels, native modules).

Adapter generik: pahami struktur umum, izinkan perusahaan menambah adapter/aturan internal.

## 7. Performa & context

- Package-aware incremental indexing (pubspec + dep graph + lockfile + generated artifacts).
- Pisahkan structural graph & knowledge store.
- Context budget/token limit + cara melanjutkan pencarian jika terpotong.
- Generated code awareness: jangan jadikan generated file contoh utama.
- Branch/worktree awareness.

## 8. Benchmark & validasi

Buat benchmark dari task nyata developer. Jalankan baseline Code Cortex asli vs fork, agent/model sama. Ukur:
- Correctness: akurasi symbol resolution, package dependency, source ref.
- Efficiency: indexing time, peak RAM, retrieval time, total token/task.
- Implementation quality: test pass rate, kesesuaian library internal, jumlah koreksi.
- Knowledge quality: % recipe valid, stale recipe terdeteksi, success rate penggunaan recipe.

Jangan anggap klaim performa upstream otomatis berlaku pada monorepo Flutter — ukur dulu.

## 9. Langkah teknis pertama

1. Audit struktur repo Code Cortex yang akan di-fork (selesai: clone di /root/code-cortex-mcp).
2. Susun design document untuk Flutter monorepo adapter (file ini adalah draft).
3. Sebelum modifikasi core indexing: prototype parser pubspec/workspace + package graph di layer terpisah, uji pada 1 monorepo nyata.
4. Tambah Dart analyzer enrichment sebagai optional layer.
5. Implement knowledge store + 3 recipe use case.
6. Benchmark baseline vs fork.

## 10. Risiko & mitigasi

- Dart analyzer integration berat/versi: buat adapter opsional, fallback Tree-sitter.
- Schema migration: tambah metadata/edge bertahap, jangan ganti total.
- Knowledge staleness: validation + invalidation + source refs.
- Performa monorepo: package-aware incremental, context budget.
- Klaim upstream (155 bahasa, speed): verifikasi sendiri pada repo nyata.

## Referensi

- Repo: https://github.com/tigercosmos/code-cortex-mcp
- Dart analyzer & Analysis Server API, pub workspace.
- Flutter docs: pemisahan UI/data, repository pattern, DI, testing.
- Dart AI agent skills distribution (opsi ekspor kompatibel).
