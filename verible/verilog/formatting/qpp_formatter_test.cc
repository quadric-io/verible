// Copyright 2024 The Verible Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Formatter tests for QPP (Quadric Python Preprocessor) constructs.
//
// QPP embeds Python control flow directly in .sv.qpp files:
//
//   Directive lines:    ^;python_statement   (column-0 semicolon)
//   Inline expressions: `python_expr`        (backtick-delimited)
//
// The formatter must:
//   1. Preserve directive lines verbatim (never reindent them).
//   2. Never insert space adjacent to inline expressions.
//   3. Format SV between directives normally.
//
// Note on inline expressions: QppInlineExpr requires at least one '[' in the
// content (e.g. `config['KEY']`) to distinguish from SV compiler directives.
// Plain backtick-identifier (e.g. `i`) is not a QppInlineExpr.
// Inline expressions are only safe in positions where their removal leaves
// valid SV (e.g. bit-range positions like [`expr`-1:0]).

#include <sstream>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "gtest/gtest.h"
#include "verible/verilog/formatting/format-style.h"
#include "verible/verilog/formatting/formatter.h"

#undef EXPECT_OK
#define EXPECT_OK(value) EXPECT_TRUE((value).ok())

namespace verilog {
namespace formatter {
namespace {

struct FormatterTestCase {
  std::string_view input;
  std::string_view expected;
};

static FormatStyle DefaultStyle() {
  FormatStyle style;
  style.column_limit = 100;
  style.indentation_spaces = 2;
  style.wrap_spaces = 4;
  return style;
}

static void RunFormatterTest(const FormatterTestCase &test_case,
                             const FormatStyle &style) {
  std::ostringstream stream;
  const auto status =
      FormatVerilog(test_case.input, "<qpp-test>", style, stream);
  EXPECT_OK(status);
  EXPECT_EQ(stream.str(), test_case.expected) << "input:\n" << test_case.input;
}

// ---------------------------------------------------------------------------
// QPP inline expressions: `python_expr`
//
// Inline expressions appear inside SV as substitutable values.  The formatter
// must not insert spaces on either side of the backtick-delimited expression.
// Only bit-range positions are safe: removing the inline expr token leaves
// valid SV (e.g. `config['W']`-1:0 → just the -1:0 range remains parseable).
// ---------------------------------------------------------------------------
TEST(QppFormatterTest, InlineExpressionNoSpaceInserted) {
  const FormatStyle style = DefaultStyle();
  static constexpr FormatterTestCase kCases[] = {
      // Logic declaration with inline width expression in bit-range.
      // After substitution, `logic [__qpp_0__-1:0] data;` is valid SV.
      // No spaces inserted around the backtick tokens after restoration.
      {
          "module m;\nlogic [`config['WIDTH']`-1:0] data;\nendmodule\n",
          "module m;\n  logic [`config['WIDTH']`-1:0] data;\nendmodule\n",
      },
      // Both bounds of a bit-range are inline exprs.  Substituting both gives
      // `logic [__qpp_0__-1:__qpp_1__] data;` which is valid SV.
      {
          "module m;\nlogic [`config['W']`-1:`config['LO']`] "
          "data;\nendmodule\n",
          "module m;\n  logic [`config['W']`-1:`config['LO']`] "
          "data;\nendmodule\n",
      },
  };
  for (const auto &tc : kCases) RunFormatterTest(tc, style);
}

// ---------------------------------------------------------------------------
// QPP directive lines: ^;python_statement
//
// Directive lines (;if, ;else:, ;pass, ;for, etc.) must stay at column 0.
// The SV between directives is formatted normally.
// ---------------------------------------------------------------------------
TEST(QppFormatterTest, DirectivesAtColumnZeroSvFormatted) {
  const FormatStyle style = DefaultStyle();
  static constexpr FormatterTestCase kCases[] = {
      // ;if with optional port — directive at col 0, SV indented inside module.
      // Note: [ 7:0] vs [15:0] alignment is the formatter's built-in
      // bit-width alignment.
      {
          "module m;\n"
          ";if (config['FP16']):\n"
          "logic [15:0] fp16_out;\n"
          ";pass\n"
          "logic [7:0] out;\n"
          "endmodule\n",
          "module m;\n"
          ";if (config['FP16']):\n"
          "  logic [15:0] fp16_out;\n"
          ";pass\n"
          "  logic [ 7:0] out;\n"
          "endmodule\n",
      },
      // ;if/;else: choosing between two alternatives — both directives at col
      // 0, single SV item in each branch indented.
      {
          "module m;\n"
          ";if (config['NUM_MACS'] == 16):\n"
          "logic [3:0] sel;\n"
          ";else:\n"
          "logic [1:0] sel;\n"
          ";pass\n"
          "endmodule\n",
          "module m;\n"
          ";if (config['NUM_MACS'] == 16):\n"
          "  logic [3:0] sel;\n"
          ";else:\n"
          "  logic [1:0] sel;\n"
          ";pass\n"
          "endmodule\n",
      },
  };
  for (const auto &tc : kCases) RunFormatterTest(tc, style);
}

// ---------------------------------------------------------------------------
// Nested QPP directives: inner directives indented with spaces after ';'
//
// Real QPP files express nested Python indentation by putting spaces between
// ';' and the Python keyword (e.g. ';    if' at 4-space Python indent).
// The lexer must recognize these as TK_QPP_DIRECTIVE tokens, not plain ';'.
// The formatter must preserve their verbatim content (spaces included).
// ---------------------------------------------------------------------------
TEST(QppFormatterTest, NestedDirectivesPreservedVerbatim) {
  const FormatStyle style = DefaultStyle();
  static constexpr FormatterTestCase kCases[] = {
      // Nested ;if inside ;if — inner directive indented 4 spaces (Python
      // indent level 1).  The formatter must preserve both the outer ';if'
      // and the inner ';    if' exactly as written.
      {
          "module m;\n"
          ";if (config['FP16']):\n"
          "logic [15:0] fp16;\n"
          "logic [1:0] ctrl;\n"
          ";    if (config['EN']):\n"
          "logic [7:0] extra;\n"
          "logic [3:0] mask;\n"
          ";    pass\n"
          ";pass\n"
          "endmodule\n",
          "module m;\n"
          ";if (config['FP16']):\n"
          "  logic [15:0] fp16;\n"
          "  logic [ 1:0] ctrl;\n"
          ";    if (config['EN']):\n"
          "  logic [ 7:0] extra;\n"
          "  logic [ 3:0] mask;\n"
          ";    pass\n"
          ";pass\n"
          "endmodule\n",
      },
  };
  for (const auto &tc : kCases) RunFormatterTest(tc, style);
}

// ---------------------------------------------------------------------------
// QPP ;for loop directive
//
// ;for / ;pass behaves like ;if / ;pass — directive at col 0, SV body
// indented.  This also exercises bare-ident inline exprs inside the loop
// body (data_`i` is a QPP-constructed identifier).
// ---------------------------------------------------------------------------
TEST(QppFormatterTest, ForLoopDirective) {
  const FormatStyle style = DefaultStyle();
  static constexpr FormatterTestCase kCases[] = {
      // ;for with bare-ident in loop body: data_`i` — the `i` is substituted
      // to a placeholder, formatted, then restored.
      {
          "module m;\n"
          ";for i in range(4):\n"
          "logic [7:0] data_`i`;\n"
          ";pass\n"
          "endmodule\n",
          "module m;\n"
          ";for i in range(4):\n"
          "  logic [7:0] data_`i`;\n"
          ";pass\n"
          "endmodule\n",
      },
  };
  for (const auto &tc : kCases) RunFormatterTest(tc, style);
}

// ---------------------------------------------------------------------------
// Empty body between QPP directives
//
// ;if immediately followed by ;pass with no SV between them — the formatter
// must not insert any tokens or spacing between consecutive directives.
// The SV after ;pass is still indented normally.
// ---------------------------------------------------------------------------
TEST(QppFormatterTest, EmptyBodyBetweenDirectives) {
  const FormatStyle style = DefaultStyle();
  static constexpr FormatterTestCase kCases[] = {
      {
          "module m;\n"
          ";if (config['EN']):\n"
          ";pass\n"
          "logic [7:0] out;\n"
          "endmodule\n",
          "module m;\n"
          ";if (config['EN']):\n"
          ";pass\n"
          "  logic [7:0] out;\n"
          "endmodule\n",
      },
  };
  for (const auto &tc : kCases) RunFormatterTest(tc, style);
}

// ---------------------------------------------------------------------------
// Bare-identifier QPP inline expressions: `ident`
//
// Bare-ident forms (`clk`, `hash`, etc.) are not recognized by the QPP lexer
// (which only handles subscript forms like `config['KEY']`).  The formatter
// pre-substitutes them with stable placeholder identifiers, formats, then
// restores the originals in the output.
// ---------------------------------------------------------------------------
TEST(QppFormatterTest, BareIdentInlineExprPreservedInContext) {
  const FormatStyle style = DefaultStyle();
  static constexpr FormatterTestCase kCases[] = {
      // always @(posedge `clk`) — bare-ident in sensitivity list position.
      // Formatter substitutes `clk` with __qpp_0__, formats, then restores.
      {
          "module m;\nalways @(posedge `clk`) begin\nend\nendmodule\n",
          "module m;\n  always @(posedge `clk`) begin\n  end\nendmodule\n",
      },
      // Multiple bare-idents on the same line — each gets its own placeholder.
      {
          "module m;\n"
          "always @(posedge `clk` or negedge `rst_n`) begin\n"
          "end\n"
          "endmodule\n",
          "module m;\n"
          "  always @(posedge `clk` or negedge `rst_n`) begin\n"
          "  end\n"
          "endmodule\n",
      },
      // Port connection: .port(`ident`) — bare-ident as port expression.
      // Each port goes on its own line per Verible's instantiation style.
      {
          "module m;\nsub u_sub (.clk(`clk`), .rst(`rst`));\nendmodule\n",
          "module m;\n"
          "  sub u_sub (\n"
          "      .clk(`clk`),\n"
          "      .rst(`rst`)\n"
          "  );\n"
          "endmodule\n",
      },
      // Parameter list: #(.PARAM(`ident`)) — bare-ident as parameter value.
      {
          "module m;\nsub #(.W(`width`)) u_sub (.clk(clk));\nendmodule\n",
          "module m;\n  sub #(.W(`width`)) u_sub (.clk(clk));\nendmodule\n",
      },
  };
  for (const auto &tc : kCases) RunFormatterTest(tc, style);
}

// ---------------------------------------------------------------------------
// Nested QPP sequential formatting
//
// Outer ;if/;else:/;pass (indent 0) containing inner ;if/;else:/;pass (indent
// 1, four-space Python indent).  Both branches must be preserved in the output.
// The inner else-branch (`logic [3:0] b;`) was lost before the fix because
// ExtractElseSegments/MergeQppBranches used nesting depth instead of Python
// indent level, and MaskQppBranches produced fake blocks when the inner ;if
// was blanked.
// ---------------------------------------------------------------------------
TEST(QppFormatterTest, NestedSequentialBothElseBranchesPreserved) {
  const FormatStyle style = DefaultStyle();
  static constexpr FormatterTestCase kCases[] = {
      // Outer else: `logic [15:0] a;`.  Inner else: `logic [3:0] b;`.
      // Both must appear in the merged output.
      {
          "module m;\n"
          ";if (config['OUTER']):\n"
          "logic [7:0] a;\n"
          ";    if (config['INNER']):\n"
          "logic [7:0] b;\n"
          ";    else:\n"
          "logic [3:0] b;\n"
          ";    pass\n"
          ";else:\n"
          "logic [15:0] a;\n"
          ";pass\n"
          "endmodule\n",
          "module m;\n"
          ";if (config['OUTER']):\n"
          "  logic [7:0] a;\n"
          ";    if (config['INNER']):\n"
          "  logic [7:0] b;\n"
          ";    else:\n"
          "  logic [3:0] b;\n"
          ";    pass\n"
          ";else:\n"
          "  logic [15:0] a;\n"
          ";pass\n"
          "endmodule\n",
      },
  };
  for (const auto &tc : kCases) RunFormatterTest(tc, style);
}

// ---------------------------------------------------------------------------
// ;elif directive
//
// ;elif is treated identically to ;else: in block scanning and merging.
// Each branch must be preserved and the SV in each branch indented normally.
// Every arm is formatted on its own (in its own pass), so declarations are
// not aligned across arms: '[7:0]' is not padded to the width of '[15:0]'.
// ---------------------------------------------------------------------------
TEST(QppFormatterTest, ElifBranchFormatted) {
  const FormatStyle style = DefaultStyle();
  static constexpr FormatterTestCase kCases[] = {
      // ;if / ;elif / ;else: / ;pass — three branches, each with different
      // bit-width so the formatter's alignment is observable.
      {
          "module m;\n"
          ";if (config['W'] == 32):\n"
          "logic [31:0] data;\n"
          ";elif (config['W'] == 16):\n"
          "logic [15:0] data;\n"
          ";else:\n"
          "logic [7:0] data;\n"
          ";pass\n"
          "endmodule\n",
          "module m;\n"
          ";if (config['W'] == 32):\n"
          "  logic [31:0] data;\n"
          ";elif (config['W'] == 16):\n"
          "  logic [15:0] data;\n"
          ";else:\n"
          "  logic [7:0] data;\n"
          ";pass\n"
          "endmodule\n",
      },
  };
  for (const auto &tc : kCases) RunFormatterTest(tc, style);
}

// ---------------------------------------------------------------------------
// Arithmetic QPP inline expressions: `ident-expr`
//
// Expressions like `NUM_GPNPU-1` start with an identifier character, contain
// no brackets, and no spaces.  They are classified as arithmetic inline exprs
// and substituted with a placeholder before parsing, then restored.
// ---------------------------------------------------------------------------
TEST(QppFormatterTest, ArithmeticInlineExprInBitRange) {
  const FormatStyle style = DefaultStyle();
  static constexpr FormatterTestCase kCases[] = {
      // `NUM_GPNPU-1` as the MSB of a bit-range.  After substitution the SV
      // is `logic [__qpp_0__:0] data;` which is valid and formats normally.
      {
          "module m;\n"
          "logic [`NUM_GPNPU-1`:0] data;\n"
          "endmodule\n",
          "module m;\n"
          "  logic [`NUM_GPNPU-1`:0] data;\n"
          "endmodule\n",
      },
  };
  for (const auto &tc : kCases) RunFormatterTest(tc, style);
}

// ---------------------------------------------------------------------------
// Based-literal prefix before a QPP inline expression: 'h`expr`
//
// When a Verilog based-literal prefix ('h, 'd, 'b, 'o) immediately precedes
// a backtick expression, the prefix must be absorbed into the substitution.
// Without this, the placeholder becomes 'h__qpp_0__ which is an invalid SV
// hex literal (underscore is not a valid leading hex digit).
// ---------------------------------------------------------------------------
TEST(QppFormatterTest, HexLiteralPrefixBeforeInlineExpr) {
  const FormatStyle style = DefaultStyle();
  static constexpr FormatterTestCase kCases[] = {
      // 'h`hash_val` — prefix 'h is stripped from the result before
      // substitution and included in the original token, so the placeholder
      // is a plain identifier.
      {
          "module m;\n"
          "localparam P = 'h`hash_val`;\n"
          "endmodule\n",
          "module m;\n"
          "  localparam P = 'h`hash_val`;\n"
          "endmodule\n",
      },
  };
  for (const auto &tc : kCases) RunFormatterTest(tc, style);
}

// ---------------------------------------------------------------------------
// Idempotency: ':' on a continuation line in a bit-range
//
// When a bit-range is too wide to fit on one line the formatter may place ':'
// at the start of a continuation line.  On a second format pass the original
// leading whitespace for that ':' token contains a newline; ExcessSpaces()
// returns 0 in that case, which previously caused the formatter to collapse
// the space before ':' to 0, breaking idempotency.  The fix preserves 1
// space before ':' whenever it appears on a continuation line.
// ---------------------------------------------------------------------------
TEST(QppFormatterTest, BitRangeWrappedLineIsIdempotent) {
  FormatStyle style = DefaultStyle();
  style.column_limit = 60;
  style.wrap_spaces = 4;
  // Regression: a bit-range declaration that just exceeds the column limit
  // wraps the trailing identifier to a continuation line.  On a second format
  // pass the already-formatted output must be left unchanged.
  // Before the ExcessSpaces() fix, ':' in a range context with a preceding
  // newline in its original leading whitespace was incorrectly given 0 spaces,
  // causing format(format(x)) != format(x).
  static constexpr FormatterTestCase kCases[] = {
      // First pass wraps data_output_signal; ':' stays on the same line as
      // the bounds.  Second pass must produce identical output.
      {
          "module m;\n"
          "logic [some_very_long_parameter_name_here - 1 : 0] "
          "data_output_signal;\n"
          "endmodule\n",
          "module m;\n"
          "  logic [some_very_long_parameter_name_here - 1 : 0]\n"
          "      data_output_signal;\n"
          "endmodule\n",
      },
      // Input is already in the wrapped form — must be stable on second pass.
      {
          "module m;\n"
          "  logic [some_very_long_parameter_name_here - 1 : 0]\n"
          "      data_output_signal;\n"
          "endmodule\n",
          "module m;\n"
          "  logic [some_very_long_parameter_name_here - 1 : 0]\n"
          "      data_output_signal;\n"
          "endmodule\n",
      },
  };
  for (const auto &tc : kCases) RunFormatterTest(tc, style);
}


// ---------------------------------------------------------------------------
// Lossless QPP formatting
//
// The formatter may only change whitespace.  These cases assert that
// input == output once all whitespace is removed, covering ;if / ;elif /
// ;else: bodies and the directive lines themselves.  Each case is a pattern
// that used to drop code silently (rc 0):
//   - a block with ;else: inside a ;if that has no ;else: (QPP indent 1)
//     next to a top-level block with ;else: (indent 0): only the first
//     block's indent level was merged, the inner ;else: body was dropped;
//   - an ;if/;elif/;else: chain whose ;elif and ;else: bodies are only valid
//     SV one at a time: both were formatted together, the pass failed, and
//     every ;elif/;else: body in the file (and the ;else: line) was dropped.
// ---------------------------------------------------------------------------
static std::string StripWhitespace(std::string_view text) {
  std::string result;
  for (char c : text) {
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') result += c;
  }
  return result;
}

// Formats 'input' and returns the status; on success asserts that the output
// equals the input modulo whitespace and that formatting is idempotent.
static absl::Status ExpectLosslessFormat(std::string_view input,
                                         std::string *output = nullptr) {
  const FormatStyle style = DefaultStyle();
  std::ostringstream stream;
  const absl::Status status =
      FormatVerilog(input, "<qpp-lossless-test>", style, stream);
  if (!status.ok()) return status;
  const std::string formatted = stream.str();
  EXPECT_EQ(StripWhitespace(formatted), StripWhitespace(input))
      << "input:\n" << input << "\noutput:\n" << formatted;
  std::ostringstream again;
  EXPECT_OK(FormatVerilog(formatted, "<qpp-lossless-test>", style, again));
  EXPECT_EQ(again.str(), formatted) << "not idempotent; input:\n" << input;
  if (output != nullptr) *output = formatted;
  return status;
}

TEST(QppFormatterTest, LosslessMixedIndentElseBodies) {
  // Inner ;    else: body inside a ;if without ;else:, next to a top-level
  // ;if/;else: block.
  static constexpr std::string_view kInput =
      "module m;\n"
      ";if (config['A'] == 'X'):\n"
      "assign a = 1;\n"
      ";else:\n"
      "assign a = 2;\n"
      ";pass\n"
      ";if (config['B'] != 'OFF'):\n"
      ";    if (config['B'] == 'ECC'):\n"
      "assign b = 1;\n"
      ";    else:\n"
      "assign b = 2;\n"
      ";    pass\n"
      ";pass\n"
      "endmodule\n";
  static constexpr std::string_view kExpected =
      "module m;\n"
      ";if (config['A'] == 'X'):\n"
      "  assign a = 1;\n"
      ";else:\n"
      "  assign a = 2;\n"
      ";pass\n"
      ";if (config['B'] != 'OFF'):\n"
      ";    if (config['B'] == 'ECC'):\n"
      "  assign b = 1;\n"
      ";    else:\n"
      "  assign b = 2;\n"
      ";    pass\n"
      ";pass\n"
      "endmodule\n";
  std::string output;
  EXPECT_OK(ExpectLosslessFormat(kInput, &output));
  EXPECT_EQ(output, kExpected);
}

TEST(QppFormatterTest, LosslessElifChainArmsFormattedSeparately) {
  // ;elif and ;else: bodies of the parameter list are not valid SV together
  // (no ',' between them); each arm must be formatted on its own.  The
  // second block's ;else: body must survive as well.
  static constexpr std::string_view kInput =
      "module m #(\n"
      ";if (config['P'] == 'ECC'):\n"
      "  parameter A = 1,\n"
      "  parameter W = 2\n"
      ";elif (config['P'] == 'PAR'):\n"
      "  parameter W = 3\n"
      ";else:\n"
      "  parameter W = 4\n"
      ";pass\n"
      ");\n"
      ";if (config['B'] == 'C'):\n"
      "// customer\n"
      ";else:\n"
      "  assign x = y;\n"
      ";pass\n"
      "endmodule\n";
  EXPECT_OK(ExpectLosslessFormat(kInput));
}

TEST(QppFormatterTest, LosslessIfElifElseBodiesAndDirectives) {
  static constexpr std::string_view kCases[] = {
      // Three statement arms plus a nested block inside the ;elif body and
      // inside the ;else: body, with Python statements and comments.
      "module m;\n"
      ";x = int(config['N'])\n"
      ";if (config['P'] == 'ECC'): # {\n"
      "always_comb begin\n"
      "a = 1;\n"
      "end\n"
      ";elif (config['P'] == 'PAR'): # } {\n"
      ";    if (config['Q']): # {\n"
      "assign b = 1;\n"
      ";    else: # } {\n"
      "assign b = 2;\n"
      ";    pass # }\n"
      "assign a = 2;\n"
      ";else: # } {\n"
      ";    # comment directive inside the else arm\n"
      ";    if (config['R']):\n"
      "assign c = 1;\n"
      ";    elif (config['S']):\n"
      "assign c = 2;\n"
      ";    else:\n"
      "assign c = 3;\n"
      ";    pass\n"
      "assign a = 3;\n"
      ";pass # }\n"
      "endmodule\n",
      // Blocks with different arm counts, an empty ;if body, a block whose
      // ;else: is empty, and a final ;pass without a trailing newline.
      "module m;\n"
      ";if (config['A']):\n"
      ";else:\n"
      "assign a = 1;\n"
      ";pass\n"
      ";if (config['B']):\n"
      "assign b = 1;\n"
      ";else:\n"
      ";pass\n"
      ";if (config['C'] == 1):\n"
      "assign c = 1;\n"
      ";elif (config['C'] == 2):\n"
      "assign c = 2;\n"
      ";elif (config['C'] == 3):\n"
      "\n"
      "assign c = 3;\n"
      "\n"
      ";else:\n"
      "assign c = 4;\n"
      ";pass\n"
      "endmodule\n"
      ";if (config['D']):\n"
      "module d; endmodule\n"
      ";else:\n"
      "module e; endmodule\n"
      ";pass",
      // Nested ;else: inside the ;if body of an outer ;if/;else: block, and
      // an ;if-only container holding an ;if/;else: at indent 1, inside an
      // always_comb (statement context).
      "module m;\n"
      "always_comb begin\n"
      ";if (config['A']):\n"
      ";    if (config['B']):\n"
      "x = 1;\n"
      ";    else:\n"
      "x = 2;\n"
      ";    pass\n"
      ";else:\n"
      "x = 3;\n"
      ";pass\n"
      ";if (config['C']):\n"
      ";    if (config['D']):\n"
      "y = 1;\n"
      ";    else:\n"
      "y = 2;\n"
      ";    pass\n"
      ";pass\n"
      "end\n"
      "endmodule\n",
  };
  for (const auto input : kCases) EXPECT_OK(ExpectLosslessFormat(input));
}

TEST(QppFormatterTest, ArmsOnlyValidTogetherNeverLoseCode) {
  // Neither formatting outcome may be a silent loss: the formatter either
  // succeeds losslessly (single-pass fallback) or reports an error.
  static constexpr std::string_view kCases[] = {
      // Arm 0 alone is a syntax error; arms together parse.
      "module m;\n"
      ";if (config['A']):\n"
      "assign a = b\n"
      ";else:\n"
      ";\n"
      ";pass\n"
      "endmodule\n",
      // No expansion parses at all: must be an error, not rc 0.
      "module m;\n"
      ";if (config['A']):\n"
      "assign a = (b\n"
      ";else:\n"
      "assign a = (c\n"
      ";pass\n"
      "endmodule\n",
  };
  // ExpectLosslessFormat asserts losslessness whenever the status is OK.
  EXPECT_OK(ExpectLosslessFormat(kCases[0]));
  EXPECT_FALSE(ExpectLosslessFormat(kCases[1]).ok());
}

}  // namespace
}  // namespace formatter
}  // namespace verilog
