# Agent Rules

1. **Portable, dependency-free C99**:
   Keep self-contained: do not add external source or package dependencies. Any approved third-party source must be vendored with its license under `ext/`.

2. **Modular design and minimal interfaces**:
   Maintain clear module boundaries: give each source module one focused responsibility and expose the smallest practical interface.

3. **Symmetry and consistency**:
   Preserve symmetry and consistency in names, APIs, collection shapes, and spatial composition so related concepts remain easy to compare and understand.

4. **Deterministic simulation**:
   Keep simulation and app behavior deterministic for identical seeds, inputs, and time steps; do not introduce hidden global state.

5. **Serializable state changes**:
   Keep state changes serializable through commands, and preserve backward compatibility for public APIs and saved YAML whenever practical.

6. **Correctness, app flow, and visual tests**:
   Add or update focused correctness, app flow, and visual E2E coverage for every behavior change and bug fix.

7. **Appropriate test execution**:
   Run the relevant `make.py --test` aspects after each change; run the full suite for cross-cutting, rendering, release, or public API changes.

8. **Safety and failure handling**:
   Treat compiler warnings, test failures, leaks, out-of-bounds access, and unchecked allocation or I/O failures as defects; handle failure paths explicitly.

9. **Documentation and generated artifacts**:
   Keep documentation, CLI help, examples, generated screenshots, and the feature set in sync; refresh generated artifacts with their documented commands rather than
   editing them manually.

10. **Small, readable, focused changes**:
    Prefer small, readable changes that follow existing C style, units, ownership rules, and naming conventions; avoid duplication, speculative abstractions, and unrelated refactoring.
