# C++ preferences

1. Use one return statement per function, at the end. Void functions may omit it.
2. Use explicit types; `auto` is allowed only for iterator types.
3. Use lambdas only when necessary and keep them short. Use named functions for long logic.
4. Prefer project containers, including `HeapVector`, over standard-library equivalents where suitable.
5. Format C++ with the project's `.clang-format`.
6. Reuse existing code or extract shared logic to reduce duplication.
7. Do not throw exceptions for error handling. Use status results, logs and assertions as appropriate, and stop invalid execution paths explicitly.
8. Keep tests focused on observable behavior, important invariants, and concrete regressions. Extend an existing test when it covers the same behavior. Add a case or test at another layer only when it can catch a distinct failure. Skip trivial checks and assertions that merely restate the implementation.
