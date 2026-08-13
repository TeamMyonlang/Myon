# Known Issues (TODO tracker)

This is a running TODO/known-issue list for larger or lower-priority bugs found
while hardening `module external.*`. Items here are intentionally **deferred**
(not fixed in the current change) because they are either large refactors,
non-critical, or out of the current scope. Fixed items are listed at the bottom
for context.

Priority legend: 🔴 large/important · 🟡 medium · 🟢 minor

---

## Deferred — module system

### 🔴 MVM backend cannot load external modules
`src/mvm_compiler.c` — `compile_stmt(STMT_MODULE)`.
Previously external module declarations were a **silent no-op** in the MVM
(`.myc`) compiler, so any `.myon` using `module external.*` compiled to a
program that referenced undefined symbols — a silent `.myon` vs `.myc`
divergence (violates the mvm_spec §2 equivalence guarantee).

- **Done now:** the compiler now *rejects* external imports with a clear
  `unsupported(...)` error instead of silently miscompiling.
- **TODO:** actually implement loading. Sketch:
  1. In `mvm_compile_program`, walk `STMT_MODULE` decls; for `external.*` load
     + lex + parse the file (reuse `resolve_module_file` semantics from the
     interpreter; base dir from `source_path`).
  2. Detect circular imports (loading flag), matching spec §14.5.
  3. Register the module's top-level structs via `register_struct`, then splice
     its top-level functions/statements into `<main>` before the main program's
     statements (mirrors the tree-walk "dump into global" model for unaliased).
  4. For **aliased** modules, compile each top-level function into a `<main>`
     global under a synthesized unique name and record an
     `(alias, member) -> global slot` map; then intercept `m.square(...)` in
     `compile_call` (EXPR_MEMBER whose target is the alias ident) and
     `m.square` in `compile_expr` to emit a `LOAD_GLOBAL slot`.
  5. Retain module ASTs/tokens/source for the module's lifetime and free them
     in `mvm_compile_program`'s cleanup + error paths.

### 🟡 Aliased-module struct definitions are still global
`src/interpreter.c` — `prescan()` / `register_struct()`.
Functions/variables of an aliased module now live in a per-module namespace
(`m.foo`), but **structs** defined in a module are still registered globally
(`find_struct` searches `it->structs`). So two modules defining a struct of the
same name still collide, and a module's struct is reachable unqualified.
- **TODO:** namespace struct registration per module (qualified `m.Point`), or
  at least error on cross-module struct-name collisions.

### 🟡 Nested aliased imports are not scoped to the importing module
`src/interpreter.c` — `load_external_module()`.
When module A does `module external.util.b as bb`, the alias `bb` is registered
in the interpreter-global `it->modules` list, not scoped to A. So `bb.*` is
visible to the top-level program too, and alias names can clash across modules.
- **TODO:** track per-module alias tables (module-local module lists).

### 🟢 External module member errors report the importing statement's line
Circular-import / load errors surface at the importing statement's line, which
for the entry program can be the top-of-file `system` line in some paths. Minor
diagnostic-quality issue.

---

## Deferred — larger bugs spotted in big files (not yet fixed)

### 🟡 Module path length capped at 1024 bytes
`src/interpreter.c` — `resolve_module_file()`.
The new resolver bounds-checks and errors on overflow (previously the old loader
did `file[len++] = ...` with **no bound check at all** against a fixed 512-byte
buffer — a latent stack buffer overflow for deep/long dotted paths). The hard
cap remains at 1024 bytes; deeper trees still error out cleanly. Acceptable for
now; a fully dynamic buffer would remove the cap.

### 🟡 Module AST retention is coarse
`src/interpreter.c`.
Loaded module programs are retained for the whole interpreter lifetime (freed
only at `interp_free` / end of `interpret`). This is a deliberate
leak-avoidance tradeoff (function values reference the AST); a production impl
would refcount/track per module and free earlier.

---

## Fixed in the current change (for reference)

- 🔴 **`as` alias was non-functional.** External module symbols were dumped flat
  into the global scope, so `module external.util.math as m` then `m.square()`
  failed and only bare `square()` worked (with silent cross-module override on
  name clash). Aliased modules now load into a private namespace env and
  `m.square()` / `m.square` (first-class value) resolve against it; two modules
  can now define the same function name without colliding. Unaliased imports
  keep the flat/global model for backward compatibility.
- 🔴 **Import paths resolved against the process CWD, not the script.** Running
  `myon /elsewhere/main.myon` failed to find its sibling modules. Imports now
  resolve relative to the importing script's own directory
  (`interpret_set_script_path` in `main.c` → `it->script_dir` →
  `resolve_module_file`). stdin/REPL fall back to CWD.
- 🔴 **MVM silently ignored external imports** → now rejected with a clear error
  (full support tracked above).
- 🟡 **Unchecked path buffer in the old loader** → replaced with a
  bounds-checked `resolve_module_file`.
- 🟢 **Module AST double-handling** → module programs are now retained via
  `interp_retain_program` and freed exactly once (both the persistent and
  one-shot interpreter paths), instead of being leaked ad hoc.
