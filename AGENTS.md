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

Build/test/run commands live in the `ucx-development` skill and `README.md`;
follow those. Notes specific to this VM:

- No RDMA/InfiniBand or GPU (CUDA/ROCm) hardware here. Only `self`, `tcp`,
  shared-memory (`sysv`/`posix`/`cma`) transports are available; tests and
  perftests needing IB/GPU will skip or be unavailable. This is expected.
- Build once per fresh session: `./autogen.sh`, then out-of-source
  `build-devel` via `contrib/configure-devel` and `make -j$(nproc)`. The full
  devel build (including gtest) takes a couple of minutes.
- Run/verify: `build-devel/src/tools/info/ucx_info -d` lists transports; a
  loopback `ucx_perftest` (server `-c 0`, client `localhost -t tag_lat -c 1`)
  exercises the UCP data path. The perftest server exits after one client
  run, so restart it between runs.
- `make -C test/gtest test` runs the whole suite and is slow; for focused
  checks run `build-devel/test/gtest/gtest` with a `GTEST_FILTER`.
- Lint: the `-Werror` build is the primary check; run `codespell` (uses the
  repo `.codespellrc`) for spelling. Exclude build dirs (e.g. `build-devel`)
  when scanning so generated `libtool`/`configure` files are not flagged.
