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
//   Directive lines:       ^;python_statement   (column-0 semicolon)
//   Inline expressions:    `python_expr`        (backtick-delimited)
//   Opaque blocks:         ^;if ... ^;pass      (entire block = one token)
//
// The formatter must:
//   1. Preserve directive lines at column 0 (never reindent them).
//   2. Never insert space adjacent to inline expressions.
//   3. Preserve opaque blocks (;if...;pass) verbatim — interior SV is not
//      reformatted, which is intentional since branches may contain
//      unbalanced begin/end across the if/else split.

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
// Non-block directives (;for, standalone ;pass, ;else: etc.) must be:
//   - preserved at column 0
//   - not re-indented even when they appear inside indented SV blocks
// ---------------------------------------------------------------------------
TEST(QppFormatterTest, DirectivePreservedAtColumnZero) {
  const FormatStyle style = DefaultStyle();
  static constexpr FormatterTestCase kCases[] = {
      // ;for loop generating repeated port declarations (self-contained SV)
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
  };
  for (const auto &tc : kCases) RunFormatterTest(tc, style);
}

// ---------------------------------------------------------------------------
// QPP opaque blocks: ;if ... ;pass
//
// The entire ;if...;pass block is emitted as a single TK_QPP_BLOCK token.
// The interior is NOT reformatted.  Both well-formed and unbalanced cases
// must be preserved verbatim.
// ---------------------------------------------------------------------------
TEST(QppFormatterTest, WellFormedBlockPreservedVerbatim) {
  const FormatStyle style = DefaultStyle();

  // Well-formed: each branch contains a complete, balanced SV statement.
  // The formatter must not touch the interior even though it is valid SV.
  static constexpr FormatterTestCase kCases[] = {
      // Simple optional port
      {
          "module m (\n"
          "  input clk,\n"
          ";if (config['FP16']):\n"
          "  input logic isFpMode,\n"
          ";pass\n"
          "  input logic [7:0] data\n"
          ");\n"
          "endmodule\n",
          "module m (\n"
          "  input clk,\n"
          ";if (config['FP16']):\n"
          "  input logic isFpMode,\n"
          ";pass\n"
          "  input logic [7:0] data\n"
          ");\n"
          "endmodule\n",
      },
      // Optional logic declarations
      {
          "module m;\n"
          ";if (config['FP16']):\n"
          "logic [15:0] fp16_out;\n"
          "logic nextIsFpMode;\n"
          ";pass\n"
          "logic [7:0] out;\n"
          "endmodule\n",
          "module m;\n"
          ";if (config['FP16']):\n"
          "logic [15:0] fp16_out;\n"
          "logic nextIsFpMode;\n"
          ";pass\n"
          "  logic [7:0] out;\n"
          "endmodule\n",
      },
      // if/else choosing between two alternatives
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
          "logic [3:0] sel;\n"
          ";else:\n"
          "logic [1:0] sel;\n"
          ";pass\n"
          "endmodule\n",
      },
  };
  for (const auto &tc : kCases) RunFormatterTest(tc, style);
}

TEST(QppFormatterTest, UnbalancedBlockPreservedVerbatim) {
  const FormatStyle style = DefaultStyle();

  // Unbalanced: the ;if branch opens a begin that is closed after ;pass.
  // This is the primary motivation for TK_QPP_BLOCK — without opaque
  // accumulation the parser would see an unmatched 'begin' and lose state.
  // The formatter must reproduce the block exactly as written.
  static constexpr FormatterTestCase kCases[] = {
      // begin opened inside ;if, closed after ;pass
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
          "module m;\n"
          "  always_comb begin : blk\n"
          ";if (config['FP16']):\n"
          "    fp_result = fp_add(a, b);\n"
          "    collapse = 1'b0;\n"
          ";pass\n"
          "    result = a + b;\n"
          "  end\n"
          "endmodule\n",
      },
      // ;if/;else split: each branch opens 'if (...) begin' without 'end'
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
          "module m;\n"
          "  always_comb begin\n"
          ";if (config['FP16']):\n"
          "    if (wbFrEX1) begin\n"
          "        asyncEX__rfWrEnValid = 1'b1;\n"
          ";else:\n"
          "    if (wbFrEX1 || pmAccsumEn) begin\n"
          "        asyncEX__rfWrEnValid = 1'b1;\n"
          ";pass\n"
          "    end\n"
          "  end\n"
          "endmodule\n",
      },
  };
  for (const auto &tc : kCases) RunFormatterTest(tc, style);
}

}  // namespace
}  // namespace formatter
}  // namespace verilog
