# Verible

Verible is an open-source SystemVerilog parser, linter, and formatter.
This repo is Quadric's fork (`quadric-io/verible`).

## Repo structure

The parts that matter for formatter work:

```
verilog/formatting/
  tree-unwrapper.cc        # CST → partition tree; controls line-break boundaries
  token-annotator.cc       # inter-token spacing and break decisions
  formatter.cc             # top-level entry point
  formatter_test.cc        # golden input/output test cases (add here for new behavior)
  token-annotator_test.cc  # SpacingOptions tests per token-pair

verilog/parser/            # lexer/parser; token types like MacroCallCloseToEndLine
verilog/analysis/          # CST node enums (NodeEnum::kCast, kMacroCall, etc.)
```

## Build and test

```bash
# build the formatter
bazel build //verilog/tools/formatter:verilog_format

# run all formatting tests (fast — run before every push)
bazel test //verilog/formatting/...

# run specific test binaries
bazel test //verilog/formatting:formatter_test
bazel test //verilog/formatting:token_annotator_test
```

## Key architectural concepts

**Partition boundaries vs. spacing rules — understand this before touching the formatter:**

- `tree-unwrapper.cc` walks the CST and calls either `VisitIndentedSection()` (creates a
  new partition, which forces a line break before it) or `TraverseChildren()` (stays in the
  same partition). Getting this wrong is the most common cause of unexpected line breaks.
  `BreakDecisionBetween` rules in token-annotator are powerless across partition boundaries.

- `token-annotator.cc` `BreakDecisionBetween()` sets `SpacingOptions` between adjacent tokens
  *within* a partition. `kMustAppend` prevents a break; `kMustWrap` forces one. These only
  fire after partition structure is already resolved by tree-unwrapper.

- `MacroCallCloseToEndLine` is a special lexer token for `)` at end-of-line in a macro call.
  In `` `MACRO(args)'(expr) `` the `)` is *not* at EOL so it gets the regular `)` token —
  distinguish these when writing token-pair rules.

## QPP formatter (Quadric-specific)

The sequential QPP formatter is on branch `thomas-format-sequential` in quadric-io/verible.
It formats each QPP `if/else` branch as an independent SV file. Key entry point:
`verilog/tools/formatter/verilog_format_main.cc` — look for `MaskQppBranches`,
`MergeQppBranches`, `SubstituteQppInlineExprs`.

## Known fix: `` `MACRO(args)'(expr) `` cast-width line break

`kMacroCall` used as casting type was always calling `VisitIndentedSection()`, creating a
partition boundary that forced a line break before `'`. Fix committed on
`thomas-format-sequential`:

- `tree-unwrapper.cc`: detect `Context().DirectParentIs(NodeEnum::kCast)` and call
  `TraverseChildren()` instead of `VisitIndentedSection()`.
- `token-annotator.cc`: belt-and-suspenders `kMustAppend` rule for `)` /
  `MacroCallCloseToEndLine` followed by `'`.
