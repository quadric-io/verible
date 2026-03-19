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

}  // namespace
}  // namespace formatter
}  // namespace verilog
