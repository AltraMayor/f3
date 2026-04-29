---
trigger: model_decision
description: After generating or editing code
---

# F3 Coding Style

## 1. General Principles
- **Language**: C (C17 standard as specified in the Makefile).
- **Line Length**: Maximum 80 columns. The only exception is for literal strings to facilitate grep-ability of output messages.
- **Portability**: Uses POSIX features and includes OS-specific blocks (e.g., `__APPLE__`, `__OpenBSD__`, `__CYGWIN__`) where necessary.

## 2. Formatting and Indentation
- **Indentation**: Uses **Tabs** for indentation.
- **Braces**: K&R style. Opening braces `{` are on the same line as the statement (`if`, `while`, `for`, `switch`, `struct`, and function definitions).
- **Whitespace**:
    - Space after keywords (`if`, `while`, `for`, `switch`, `do`).
    - No space between function name and the opening parenthesis `(`.
    - Pointers: `char *ptr` (space before the asterisk, not after).
    - Alignment: Struct member assignments and function parameters are often aligned using tabs when spanning multiple lines.

## 3. Naming Conventions
- **Files**: Lowercase with `snake_case` (e.g., `f3read.c`, `libutils.h`).
- **Variables and Functions**: `snake_case` (e.g., `check_chunk`, `bytes_read`).
- **Types (structs, enums, typedefs)**: `snake_case` (e.g., `struct block_stats`, `enum block_state`).
- **Macros and Constants**: `UPPER_CASE` (e.g., `SECTOR_ORDER`, `UNUSED`).
- **Enum Members**: Lowercase with a short prefix (e.g., `bs_unknown`, `bs_good`).

## 4. Functions and Variables
- **Scope**: Internal helper functions are marked `static`. Small, performance-critical functions are marked `static inline`.
- **Declarations**: Variables are declared at the beginning of their scope.
- **Initialization**: Structs are often initialized using designated initializers or `memset`.

## 5. Macros
- **Safety**: Multi-line macros use the `do { ... } while (0)` idiom.
- **Parentheses**: Arguments and the final expression are wrapped in parentheses to prevent precedence issues.
- **Alignment**: Backslashes `\` in multi-line macros are aligned.

## 6. Error Handling and Correctness
- **Assertions**: Heavy use of `assert()` for internal state verification and "can't happen" cases.
- **Error Propagation**: Uses `goto out` patterns for resource cleanup or direct returns of error codes (e.g., `errno`).
- **Safety**: Prefers `snprintf` over `sprintf`.

## 7. Headers and Includes
- **Include Guards**: Format `HEADER_FILENAME_H` (e.g., `#ifndef HEADER_LIBUTILS_H`).
- **Order**: Standard library headers followed by local project headers.
- **Documentation**: Comments in headers are often aligned using tabs for readability.

## 8. Comments
- **Style**: Uses C-style comments `/* ... */`.
- **Placement**: Comments are placed above the code they describe or as trailing comments aligned with tabs.