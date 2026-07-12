# C++ Alliance Clang Initiative

## The Problem

Compiler development is in crisis. The pool of contributors has been shrinking for years while the C++ standard accelerates. The gap between what is standardized and what is implemented continues to widen.

In January 2026, eighteen major compiler implementers published P3962, "Implementation reality of WG21 standardization." Their assessment is stark:

> *"Many implementers are volunteers or are only partially funded for standardisation work, and there is often no dedicated staffing to implement all standardized features."*

> *"Full conformance to recent standards remains difficult in practice, with some implementations still working toward C++20 conformance with limited capacity to adopt newer standards."*

> *"Adding new features to the standard necessarily displaces other work, including bug fixes, conformance improvements, performance tuning, and various high-value library enhancements."*

The implementers themselves are calling for help. The gap does not close on its own—it closes when dedicated engineering time is spent on the compilers.

## Our Goals

The initiative has two goals:

1. **Continue the maintenance of Clang.** We contribute to the day-to-day upkeep of Clang—bug fixes, conformance work, and code review—submitted to official upstream Clang through the standard LLVM review process.

2. **Develop an implementation of C++ Profiles.** We are building a working implementation of the C++ Profiles framework proposed in [P3589R2](https://open-std.org/JTC1/SC22/WG21/docs/papers/2025/p3589r2.pdf), carried on this repository's [`profiles-framework`](https://github.com/cppalliance/clang/tree/profiles-framework) branch.

Everything we produce aims for acceptance by the official Clang project; Clang's success is our success.

## Clang Maintenance

Maintenance work happens directly in the upstream workflow: issues are triaged on the LLVM tracker, patches are submitted as pull requests to [llvm/llvm-project](https://github.com/llvm/llvm-project), and every change goes through LLVM's standard review process. The focus is on the areas P3962 identifies as chronically understaffed—bug fixes, conformance improvements, and keeping review queues moving.

## The C++ Profiles Implementation

C++ Profiles let a translation unit opt into additional language restrictions: a *profile* is a named set of rules enforced by the compiler, each formulated to keep the program free of a certain class of problems—for example, use of uninitialized memory. Profiles do not change the meaning of well-formed programs; they reject programs that cannot be shown to be free of the targeted problem.

The [`profiles-framework`](https://github.com/cppalliance/clang/tree/profiles-framework) branch implements:

- **The P3589R2 framework**, gated behind the C++-only `-fprofiles` flag: the `[[profiles::enforce(...)]]`, `[[profiles::suppress(...)]]`, and `[[profiles::require(...)]]` attributes, including their integration with C++ modules, header units, and AST serialization. Profile names are open-ended—standard, implementation-defined, and third-party profiles are all requested through the same syntax.
- **An initial slice of the `std::init` profile**, the initialization profile proposed by Bjarne Stroustrup in [P4222](https://wg21.link/p4222). Its guarantee: no object is read or written before it is initialized, enforced entirely at compile time. Intentionally uninitialized objects are marked with the `[[uninit]]` attribute and verified by local flow analysis; pointers and references to uninitialized memory are marked with `[[ref_to_uninit]]`. The profile currently enforces ten individually suppressible rules covering uninitialized declarations, reads, writes, reference bindings, constructors, and static initialization.
- **Documentation**: a [user-facing guide](https://github.com/cppalliance/clang/blob/profiles-framework/clang/docs/ProfilesFramework.rst) and a [design document](https://github.com/cppalliance/clang/blob/profiles-framework/clang/docs/ProfilesFrameworkInternals.rst) describing the framework's internals.

Prebuilt toolchains (Linux and Windows x86_64) are published as weekly GitHub releases built from the branch, so the implementation can be tried without building Clang from source.

The feature is experimental: attribute spellings, rule names, and diagnostics may change as the proposals evolve. The long-term aim is the same as for all our work—an implementation of sufficient quality to be proposed upstream.

## Team

- **Krystian Stasiowski** — Compiler initiative lead
- **Matheus Izvekov** — Reviewer
- **Vlad Serebrennikov** — Reviewer
- **Coordination**: `#cppa-clang` Slack channel

## References

- [Upstream LLVM Repository](https://github.com/llvm/llvm-project)
- [C++ Alliance Clang Workspace](https://github.com/cppalliance/clang)
- [The `profiles-framework` branch](https://github.com/cppalliance/clang/tree/profiles-framework)
- [P3962: Implementation reality of WG21 standardization](https://wg21.link/p3962)
- [P3589R2: C++ Profiles](https://open-std.org/JTC1/SC22/WG21/docs/papers/2025/p3589r2.pdf)
- [P4222: An initialization profile](https://wg21.link/p4222)
- [C++ Alliance](https://cppalliance.org)
