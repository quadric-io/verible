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
//   1. Preserve directive lines at column 0 (never reindent them).
//   2. Never insert space adjacent to inline expressions.
//   3. Format SV between directives normally — directives are opaque atoms
//      but the SV they surround is fully visible to the parser/formatter.
//
// Unbalanced QPP blocks (;if branch opens 'begin' closed after ;pass) are
// a style violation caught by the QPP lint rule, not the formatter.  The
// UnbalancedBlock test below documents the formatter's best-effort output
// for this case so regressions are visible.  The expected strings are marked
// TODO and must be filled in after the first successful formatter run.

#include "verible/verilog/formatting/formatter.h"

#include <sstream>
#include <string_view>

#include "absl/status/status.h"
#include "gtest/gtest.h"
#include "verible/verilog/formatting/format-style.h"

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
  EXPECT_EQ(stream.str(), test_case.expected)
      << "input:\n"
      << test_case.input;
}

// ---------------------------------------------------------------------------
// QPP inline expressions: `python_expr`
//
// Inline expressions appear inside SV as substitutable values.  The formatter
// must not insert spaces on either side of the backtick-delimited expression.
// ---------------------------------------------------------------------------
TEST(QppFormatterTest, InlineExpressionNoSpaceInserted) {
  const FormatStyle style = DefaultStyle();
  static constexpr FormatterTestCase kCases[] = {
      // Parameter declaration with inline width
      {
          "module m #(parameter int W=`config['W']`);\nendmodule\n",
          "module m #(\n    parameter int W = `config['W']`\n);\nendmodule\n",
      },
      // Logic declaration with inline width expression
      {
          "module m;\nlogic [`config['WIDTH']`-1:0] data;\nendmodule\n",
          "module m;\n  logic [`config['WIDTH']`-1:0] data;\nendmodule\n",
      },
      // Assign with inline value
      {
          "module m;\nassign foo=`config['FOO']`;\nendmodule\n",
          "module m;\n  assign foo = `config['FOO']`;\nendmodule\n",
      },
  };
  for (const auto &tc : kCases) RunFormatterTest(tc, style);
}

// ---------------------------------------------------------------------------
// QPP directive lines: ^;python_statement
//
// Directive lines (;if, ;else:, ;pass, ;for, etc.) must stay at column 0.
// The SV between directives is fully visible to the formatter and is
// indented/spaced normally.
// ---------------------------------------------------------------------------
TEST(QppFormatterTest, DirectivesAtColumnZeroSvFormatted) {
  const FormatStyle style = DefaultStyle();
  static constexpr FormatterTestCase kCases[] = {
      // ;for loop generating repeated declarations — SV body gets indented
      {
          "module m;\n"
          ";for i in range(N):\n"
          "logic sig`i`;\n"
          ";pass\n"
          "endmodule\n",
          "module m;\n"
          ";for i in range(N):\n"
          "  logic sig`i`;\n"
          ";pass\n"
          "endmodule\n",
      },
      // ;if with optional port — port declaration gets indented inside module
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
          "  logic [7:0] out;\n"
          "endmodule\n",
      },
      // ;if/;else: choosing between two alternatives — both branches formatted
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
// Unbalanced QPP blocks (style violation — caught by lint, not formatter)
//
// When a ;if branch opens a 'begin' that is only closed after ;pass, the
// parser sees unmatched begin/end and enters error recovery.  The formatter
// still produces output but it may be poorly indented around the violation.
//
// These tests document the actual formatter output so regressions are
// visible.  The expected strings below are stubs — fill them in after the
// first successful `bazel test` run by capturing the actual output.
//
// DO NOT "fix" these expected outputs to look pretty.  They intentionally
// show degraded output so readers understand what they get if the lint rule
// is bypassed.
// ---------------------------------------------------------------------------
TEST(QppFormatterTest, UnbalancedBlockDegradedOutput) {
  const FormatStyle style = DefaultStyle();
  static constexpr FormatterTestCase kCases[] = {
      // begin opened inside ;if, closed after ;pass
      // TODO: replace expected string with output captured from formatter run
      {
          "module m;\n"
          "always_comb begin : blk\n"
          ";if (config['FP16']):\n"
          "    fp_result = fp_add(a, b);\n"
          "    collapse = 1'b0;\n"
          ";pass\n"
          "    result = a + b;\n"
          "end\n"
          "endmodule\n",
          "",  // TODO: fill in after first build
      },
      // ;if/;else split: each branch opens 'if (...) begin' without 'end'
      // TODO: replace expected string with output captured from formatter run
      {
          "module m;\n"
          "always_comb begin\n"
          ";if (config['FP16']):\n"
          "    if (wbFrEX1) begin\n"
          "        asyncEX__rfWrEnValid = 1'b1;\n"
          ";else:\n"
          "    if (wbFrEX1 || pmAccsumEn) begin\n"
          "        asyncEX__rfWrEnValid = 1'b1;\n"
          ";pass\n"
          "    end\n"
          "end\n"
          "endmodule\n",
          "",  // TODO: fill in after first build
      },
  };
  for (const auto &tc : kCases) RunFormatterTest(tc, style);
}

}  // namespace
}  // namespace formatter
}  // namespace verilog
