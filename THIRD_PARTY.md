# Third-party code

Code copied into this repository from other projects, with its origin and
license. Projects used only as references (read, not copied) are not listed.

## dynarmic

- **Project:** dynarmic
- **URL:** <https://github.com/lioncash/dynarmic>
- **Commit:** `a41c380246d3d9f9874f0f792d234dc0cc17c180` (2024-03-03)
- **License:** 0BSD

  > Permission to use, copy, modify, and/or distribute this software for any
  > purpose with or without fee is hereby granted.

- **Files copied:**

  | Source | Destination | Changes |
  |---|---|---|
  | `src/dynarmic/frontend/A64/decoder/a64.inc` | `FEXCore/Source/Interface/Core/A64Frontend/a64.inc` | none (verbatim) |

- **Also derived:** the decode ordering in
  `FEXCore/Source/Interface/Core/A64Frontend/DecodeTable.cpp` (sort by number
  of fixed bits, bucket by dynarmic's fast-lookup index) follows
  `src/dynarmic/frontend/A64/decoder/a64.h`. dynarmic's A64 translators
  (`src/dynarmic/frontend/A64/translate/impl/`) were consulted for instruction
  semantics; the translators in `A64Frontend/` are written against the FEX IR
  and do not copy them.
