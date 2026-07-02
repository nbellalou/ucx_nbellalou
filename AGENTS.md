# Agent Guide for UCX

This file is for coding agents working in this repository. It summarizes the
project shape and points to the source-of-truth docs that should be followed
when changing code.

## Agentic File Budget

- Keep this root `AGENTS.md` under about 120 lines.
- Keep subtree `AGENTS.md` files under about 100 lines and focused on local
  ownership, commands, and pitfalls.
- Put repeatable workflows, review procedures, and extended examples in
  agent skills or regular docs, then link to them from the relevant guide.

## Agent Skills

Repository skills live under `.agents/skills/<skill-name>/SKILL.md`. Use each
skill's frontmatter description to decide when it applies. Keep `.agents` as
the canonical location and add tool-specific adapters only when a tool requires
them.

## Universal Work Rules

- For any file you touch, read the nearest `AGENTS.md` and each parent guide up
  to this root guide before editing.
- Keep changes scoped to the requested behavior; use `REVIEW.md` for PR split
  expectations.
- Prefer existing local patterns before adding new abstractions.
- Do not duplicate rules across `AGENTS.md` files, skills, and `REVIEW.md`;
  link to the source of truth instead.
- Do not duplicate code or similar implementation patterns; use helper
  functions or shared utilities when appropriate.
- Do not commit generated build output, generated docs, local install
  directories, or editor/tool scratch files.
- Commit messages usually follow `COMPONENT/SUBCOMPONENT: Imperative message`,
  for example `UCP/CORE: Fix endpoint flush completion`.

## Project Map

UCX is a C communication framework with C++ unit tests. Use the nearest
subtree guide when one exists for local ownership, commands, and pitfalls:

- `src/AGENTS.md` for runtime source code.
- `test/AGENTS.md` for test code.

Other top-level areas:

- `bindings`: Go and Java bindings over UCX APIs.
- `buildlib`: build, packaging, and CI helper scripts.
- `config`: autotools helpers and m4 feature checks.
- `debian`: Debian packaging metadata and scripts.
- `docs`: Sphinx, Doxygen, style, and user documentation.
- `examples`: small programs that demonstrate public APIs.

## Source-of-Truth Docs

Follow these project docs instead of duplicating their contents:

- `docs/CodeStyle.md` for C/C++ formatting and naming.
- `docs/LoggingStyle.md` for log levels and message style.
- `docs/OptimizationStyle.md` for performance-sensitive changes.
- `REVIEW.md` for UCX pull-request review checks and comment style.

## Cursor Cloud specific instructions

Build system is autotools; standard commands live in `README.md` and
`.agents/skills/ucx-development/SKILL.md`. The startup update script only
installs system build/lint packages — you must still generate and build the
tree yourself:

- One-time per checkout: `./autogen.sh`, then out-of-source
  `mkdir -p build-devel && cd build-devel && ../contrib/configure-devel --prefix=$PWD/install`.
  `autogen.sh` tries a git submodule init that fails without network; it warns
  and continues, which is fine.
- Incremental rebuilds: `make -j$(nproc)` from `build-devel`.

Non-obvious caveats:

- No RDMA/CUDA/ROCm hardware here. Only `self`, `tcp`, `sysv`, `posix`, `cma`
  transports are available (check `build-devel/src/tools/info/ucx_info -d`).
  Many gtest variants (e.g. `gga`, IB/mlx5, cuda) therefore `[ SKIP ]` — that
  is expected, not a failure.
- `make -C test/gtest test` runs a huge, slow matrix. For quick checks run the
  binary directly: `build-devel/test/gtest/gtest --gtest_filter=...`.
- Smoke test the stack with the examples over loopback, e.g. run
  `build-devel/examples/ucp_hello_world -p 13337` and then
  `build-devel/examples/ucp_hello_world -n 127.0.0.1 -p 13337`.
- Lint is diff-based (`git clang-format`, `codespell`, ctags). `codespell` is
  clean on a source-only checkout; run from repo root and it will flag
  generated files under `build-devel/` — ignore those.
