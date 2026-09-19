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

Build/test/run commands are the standard ones in `README.md` and the
`.agents/skills/ucx-development/SKILL.md` skill; use those as the source of
truth. Notes specific to this cloud VM:

- The autotools toolchain (`autoconf`, `automake`, `libtool`, `m4`) is
  installed by the startup update script; the source tree still needs
  `./autogen.sh` once before configuring.
- Build out-of-source in `build-devel/` (already present in the VM snapshot):
  `cd build-devel && ../contrib/configure-devel --prefix=$PWD/install && make -j$(nproc)`.
  Re-run `../contrib/configure-devel` only when `configure.ac`/`Makefile.am`
  change; otherwise just re-run `make`.
- No RDMA/InfiniBand/CUDA/ROCm hardware is available here. Only the `tcp`,
  `self`, `posix`, `sysv`, and `cma` transports come up; gtest prints many
  `... is not available` lines for IB/GPU transports, which is expected, not a
  failure. Verify with `build-devel/src/tools/info/ucx_info -d`.
- Run the gtest binary directly for control, e.g.
  `build-devel/test/gtest/gtest --gtest_filter='test_string.*:test_ucp_context.*'`.
  The `make -C test/gtest test` target wraps the binary and applies its own
  launch/filter logic, so a `GTEST_FILTER` you export may not behave as
  expected. The full suite is very large (15k+ cases); filter to the layer you
  changed.
- Lint is codespell (see `buildlib/tools/codestyle.sh`), installed via
  `pip3 install codespell` into `~/.local/bin`; the `-Werror` build itself is
  the primary correctness gate.
- Smoke test end to end without hardware:
  `cd build-devel/examples && UCX_TLS=tcp,self,sm ./ucp_hello_world -p 13337 &`
  then `UCX_TLS=tcp,self,sm ./ucp_hello_world -n 127.0.0.1 -p 13337`; look for
  `UCP TEST SUCCESS`.
