// Copyright 2017-2020 The Verible Authors.
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

#include "verible/verilog/formatting/formatter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "verible/common/formatting/basic-format-style.h"
#include "verible/common/formatting/format-token.h"
#include "verible/common/formatting/layout-optimizer.h"
#include "verible/common/formatting/line-wrap-searcher.h"
#include "verible/common/formatting/token-partition-tree.h"
#include "verible/common/formatting/unwrapped-line.h"
#include "verible/common/formatting/verification.h"
#include "verible/common/strings/diff.h"
#include "verible/common/strings/line-column-map.h"
#include "verible/common/strings/position.h"
#include "verible/common/strings/range.h"
#include "verible/common/text/line-terminator.h"
#include "verible/common/text/symbol.h"
#include "verible/common/text/text-structure.h"
#include "verible/common/text/token-info.h"
#include "verible/common/text/tree-utils.h"
#include "verible/common/util/expandable-tree-view.h"
#include "verible/common/util/interval-set.h"
#include "verible/common/util/interval.h"
#include "verible/common/util/iterator-range.h"
#include "verible/common/util/logging.h"
#include "verible/common/util/spacer.h"
#include "verible/common/util/tree-operations.h"
#include "verible/common/util/vector-tree-iterators.h"
#include "verible/common/util/vector-tree.h"
#include "verible/verilog/CST/declaration.h"
#include "verible/verilog/CST/verilog-nonterminals.h"
#include "verible/verilog/analysis/verilog-analyzer.h"
#include "verible/verilog/analysis/verilog-equivalence.h"
#include "verible/verilog/formatting/align.h"
#include "verible/verilog/formatting/comment-controls.h"
#include "verible/verilog/formatting/format-style.h"
#include "verible/verilog/formatting/token-annotator.h"
#include "verible/verilog/formatting/tree-unwrapper.h"
#include "verible/verilog/parser/verilog-token-enum.h"
#include "verible/verilog/preprocessor/verilog-preprocess.h"

namespace verilog {
namespace formatter {
using absl::Status;
using absl::StatusCode;

using verible::ByteOffsetSet;
using verible::ExpandableTreeView;
using verible::LineNumberSet;
using verible::PartitionPolicyEnum;
using verible::TokenPartitionTree;
using verible::TreeViewNodeInfo;
using verible::UnwrappedLine;
using verible::VectorTree;
using verible::VectorTreeLeavesIterator;

using partition_node_type = VectorTree<TreeViewNodeInfo<TokenPartitionTree>>;

// Takes a TextStructureView and FormatStyle, and formats UnwrappedLines.
class Formatter {
 public:
  Formatter(const verible::TextStructureView &text_structure,
            const FormatStyle &style)
      : text_structure_(text_structure), style_(style) {}

  // Formats the source code
  Status Format(const ExecutionControl &);

  Status Format() { return Format(ExecutionControl()); }

  void SelectLines(const LineNumberSet &lines);

  // Outputs all of the FormattedExcerpt lines to stream.
  // If "include_disabled" is false, does not contain the disabled ranges.
  void Emit(bool include_disabled, std::ostream &stream) const;

 private:
  // Contains structural information about the code to format, such as
  // TokenSequence from lexing, and ConcreteSyntaxTree from parsing
  const verible::TextStructureView &text_structure_;

  // The style configuration for the formatter
  FormatStyle style_;

  // Ranges of text where formatter is disabled (by comment directives).
  ByteOffsetSet disabled_ranges_;

  // Set of formatted lines, populated by calling Format().
  std::vector<verible::FormattedExcerpt> formatted_lines_;
};

// TODO(b/148482625): make this public/re-usable for general content comparison.
// Not declared in any header, but also used in formatter_test
extern Status VerifyFormatting(const verible::TextStructureView &text_structure,
                               std::string_view formatted_output,
                               std::string_view filename) {
  // Verify that the formatted output creates the same lexical
  // stream (filtered) as the original.  If any tokens were lost, fall back to
  // printing the original source unformatted.
  // Note: We cannot just Tokenize() and compare because Analyze()
  // performs additional transformations like expanding MacroArgs to
  // expression subtrees.
  const auto reanalyzer = VerilogAnalyzer::AnalyzeAutomaticMode(
      formatted_output, filename, verilog::VerilogPreprocess::Config());
  const auto relex_status = ABSL_DIE_IF_NULL(reanalyzer)->LexStatus();
  const auto reparse_status = reanalyzer->ParseStatus();

  if (!relex_status.ok() || !reparse_status.ok()) {
    const auto &token_errors = reanalyzer->TokenErrorMessages();
    // Only print the first error.
    if (!token_errors.empty()) {
      return absl::DataLossError(
          absl::StrCat("Error lex/parsing-ing formatted output.  "
                       "Please file a bug.\nFirst error: ",
                       token_errors.front()));
    }
  }

  {
    // Filter out only whitespaces and compare.
    // First difference will be printed to cerr for debugging.
    std::ostringstream errstream;
    // Note: text_structure.TokenStream() and reanalyzer->Data().TokenStream()
    // contain already lexed tokens, so this comparison check is repeating the
    // work done by the lexers.
    // Should performance be a concern, we could pass in those tokens to
    // avoid lexing twice, but for now, using plain strings as an interface
    // to comparator functions is simpler and more intuitive.
    // See analysis/verilog_equivalence.cc implementation.
    if (verilog::FormatEquivalent(text_structure.Contents(), formatted_output,
                                  &errstream) != DiffStatus::kEquivalent) {
      return absl::DataLossError(absl::StrCat(
          "Formatted output is lexically different from the input.    "
          "Please file a bug.  Details:\n",
          errstream.str()));
    }
  }

  return absl::OkStatus();
}

static Status ReformatVerilogIncrementally(std::string_view original_text,
                                           std::string_view formatted_text,
                                           std::string_view filename,
                                           const FormatStyle &style,
                                           std::ostream &reformat_stream,
                                           const ExecutionControl &control) {
  // Differences from the first formatting.
  const verible::LineDiffs formatting_diffs(original_text, formatted_text);
  // Added lines will be re-applied to incremental re-formatting.
  LineNumberSet formatted_lines(
      verible::DiffEditsToAddedLineNumbers(formatting_diffs.edits));
  // Even if no line were changed by formatting, need to make sure that
  // reformatting does not accidentally reformat the whole file by
  // adding an out-of-range lines interval.  This effectively disables
  // re-formatting on the whole file unless line ranges are specified.
  formatted_lines.Add(formatting_diffs.after_lines.size() + 1);
  VLOG(1) << "formatted changed lines: " << formatted_lines;
  return FormatVerilog(formatted_text, filename, style, reformat_stream,
                       formatted_lines, control);
}

static Status ReformatVerilog(std::string_view original_text,
                              std::string_view formatted_text,
                              std::string_view filename,
                              const FormatStyle &style,
                              std::ostream &reformat_stream,
                              const LineNumberSet &lines,
                              const ExecutionControl &control) {
  // Disable reformat check to terminate recursion.
  ExecutionControl convergence_control(control);
  convergence_control.verify_convergence = false;

  if (lines.empty()) {
    // format whole file
    return FormatVerilog(formatted_text, filename, style, reformat_stream,
                         lines, convergence_control);
  }
  // reformat incrementally
  return ReformatVerilogIncrementally(original_text, formatted_text, filename,
                                      style, reformat_stream,
                                      convergence_control);
}

static absl::StatusOr<std::unique_ptr<VerilogAnalyzer>> ParseWithStatus(
    std::string_view text, std::string_view filename) {
  std::unique_ptr<VerilogAnalyzer> analyzer =
      VerilogAnalyzer::AnalyzeAutomaticMode(
          text, filename, verilog::VerilogPreprocess::Config());
  {
    // Lex and parse code.  Exit on failure.
    const auto lex_status = ABSL_DIE_IF_NULL(analyzer)->LexStatus();
    const auto parse_status = analyzer->ParseStatus();
    if (!lex_status.ok() || !parse_status.ok()) {
      std::ostringstream errstream;
      constexpr bool with_diagnostic_context = false;
      const std::vector<std::string> syntax_error_messages(
          analyzer->LinterTokenErrorMessages(with_diagnostic_context));
      for (const auto &message : syntax_error_messages) {
        errstream << message << std::endl;
      }
      // Don't bother printing original code
      return absl::InvalidArgumentError(errstream.str());
    }
  }
  return analyzer;
}

absl::Status FormatVerilog(const verible::TextStructureView &text_structure,
                           std::string_view filename, const FormatStyle &style,
                           std::string *formatted_text,
                           const verible::LineNumberSet &lines,
                           const ExecutionControl &control) {
  Formatter fmt(text_structure, style);
  fmt.SelectLines(lines);

  // Format code.
  Status format_status = fmt.Format(control);
  if (!format_status.ok()) {
    if (format_status.code() != StatusCode::kResourceExhausted) {
      // Some more fatal error, halt immediately.
      return format_status;
    }
    // Else allow remainder of this function to execute, and print partially
    // formatted code, but force a non-zero exit status in the end.
  }

  // In any diagnostic mode, proceed no further.
  if (control.AnyStop()) {
    return absl::CancelledError("Halting for diagnostic operation.");
  }

  // Render formatted text to the output buffer.
  std::ostringstream output_buffer;
  fmt.Emit(true, output_buffer);
  *formatted_text = output_buffer.str();

  // For now, unconditionally verify.
  if (Status verify_status =
          VerifyFormatting(text_structure, *formatted_text, filename);
      !verify_status.ok()) {
    return verify_status;
  }

  return format_status;
}

// Replaces QPP inline expressions with stable placeholder identifiers so the
// SV parser processes the surrounding code cleanly.  Two forms are handled:
//
//   Bare-ident:   `ident`          — backtick + identifier + backtick
//   Subscript:    `config['KEY']`  — backtick + content-with-[ + backtick
//
// Both forms are detected by scanning for a closing backtick on the same line.
// The content between backticks determines which form:
//   - Pure identifier chars only → bare-ident
//   - Contains '[' anywhere → subscript (Verilog macros never have closing backtick)
//
// SV compiler directives (`define, `ifdef, ...) have no closing backtick on
// the same line and are left untouched.
//
// Placeholders (__qpp_N__) are valid SV identifiers and format stably, so
// convergence checking operates correctly on the substituted text.
//
// Design note: the lexer also recognises subscript-form inline exprs via the
// QppInlineExpr rule, producing TK_QPP_INLINE_EXPR tokens that are filtered
// from the syntax tree by KeepSyntaxTreeTokens.  That mechanism is retained
// for non-formatter Verible tools (linter, syntax viewer, etc.) which lex QPP
// files directly and cannot easily perform a restore step on structured output.
// The formatter does NOT rely on TK_QPP_INLINE_EXPR; substitution here
// removes the backtick patterns before the lexer runs.  See verilog.lex for
// the full design note.
static std::string SubstituteQppInlineExprs(
    std::string_view text,
    std::vector<std::pair<std::string, std::string>> *subs) {
  std::string result;
  result.reserve(text.size());
  int counter = 0;
  size_t i = 0;
  while (i < text.size()) {
    if (text[i] != '`') {
      result += text[i++];
      continue;
    }
    // Backtick found — scan for a closing backtick on the same line.
    size_t j = i + 1;
    bool has_bracket = false;
    bool has_paren_before_first_bracket = false;
    bool first_char_is_ident =
        (j < text.size() &&
         (text[j] == '_' || (text[j] >= 'A' && text[j] <= 'Z') ||
          (text[j] >= 'a' && text[j] <= 'z')));
    while (j < text.size() && text[j] != '`' && text[j] != '\n') {
      if (text[j] == '[') {
        has_bracket = true;
      } else if (text[j] == '(' && !has_bracket) {
        has_paren_before_first_bracket = true;
      }
      ++j;
    }
    if (j < text.size() && text[j] == '`' && j > i + 1) {
      std::string_view content = text.substr(i + 1, j - i - 1);
      // Classify the content.
      bool is_bare_ident = true;
      for (size_t k = 0; k < content.size(); ++k) {
        char c = content[k];
        bool id_char =
            (c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
             (k > 0 && c >= '0' && c <= '9'));
        if (!id_char) {
          is_bare_ident = false;
          break;
        }
      }
      // Subscript form: expression containing '[' within backticks.
      // Exception: if the expression starts with an identifier and has '('
      // before the first '[', it is a Verilog macro call like
      //   `MACRO(args[i], ..., `nested_qpp`)
      // where the "closing" backtick actually belongs to a nested QPP token
      // inside the macro arguments. QPP Python ternary forms start with '('
      // (not an identifier), so they are still correctly matched.
      bool is_subscript =
          has_bracket && !(first_char_is_ident && has_paren_before_first_bracket);
      if (is_bare_ident || is_subscript) {
        std::string original(text.substr(i, j - i + 1));
        std::string placeholder = absl::StrCat("__qpp_", counter++, "__");
        subs->push_back({placeholder, original});
        result += placeholder;
        i = j + 1;
        continue;
      }
    }
    result += text[i++];
  }
  return result;
}

// Reverses the substitution performed by SubstituteQppInlineExprs,
// replacing each __qpp_N__ placeholder with its original QPP text.
static std::string RestoreQppInlineExprs(
    std::string_view text,
    const std::vector<std::pair<std::string, std::string>> &subs) {
  std::string result(text);
  for (const auto &[placeholder, original] : subs) {
    size_t pos = 0;
    while ((pos = result.find(placeholder, pos)) != std::string::npos) {
      result.replace(pos, placeholder.size(), original);
      pos += original.size();
    }
  }
  return result;
}

// ===== QPP sequential branch formatting =====
//
// When a QPP if/else block is visible to the parser simultaneously, both
// branches contribute tokens to the combined SV token stream.  This causes:
//   - begin/end imbalance   (if one branch opens a block the other doesn't)
//   - duplicate terminators (both branches close a statement with ';')
//
// The sequential approach formats each branch independently:
//   ;if cond:
//     [if-branch SV]    ← formatted using if-masked text
//   ;else:
//     [else-branch SV]  ← formatted using else-masked text
//   ;pass
//
// We produce two masked versions of the file:
//   if-masked   : else-branch lines replaced by blank lines
//   else-masked : if-branch  lines replaced by blank lines
// format each masked text (both are valid SV), then merge by substituting
// the formatted else-branch content from else-masked into if-formatted.
//
// Constraint: each branch must leave the parser at the same depth as the
// other.  This is the natural invariant for well-formed QPP code.  If the
// else-masked text fails to parse, we fall back to single-pass.
//
// Nested QPP blocks (a block whose line range is strictly inside another
// block's range) are not yet handled sequentially — they fall back to
// single-pass as well.

// Line-number bounds of a QPP if/else/pass block (0-based line indices).
struct QppBlockBounds {
  int if_line;    // line index of the ;if directive
  int else_line;  // -1 if the block has no ;else branch
  int pass_line;  // line index of the ;pass directive
};

// Returns true if 'line' is a QPP directive whose first keyword matches 'kw'.
// QPP directive lines have the form: ^;(    )*<kw>[ \t:(#\r]?$
static bool IsQppKw(std::string_view line, std::string_view kw) {
  if (line.empty() || line[0] != ';') return false;
  size_t i = 1;
  while (i < line.size() && line[i] == ' ') ++i;
  if (line.substr(i, kw.size()) != kw) return false;
  i += kw.size();
  return i >= line.size() || line[i] == ' ' || line[i] == '\t' ||
         line[i] == ':' || line[i] == '(' || line[i] == '#' ||
         line[i] == '\r';
}

// Scan 'text' for QPP if/else/pass block boundaries.  Returns one entry per
// matched if/pass pair; inner blocks appear before outer blocks.
static std::vector<QppBlockBounds> ScanQppBlocks(std::string_view text) {
  std::vector<QppBlockBounds> result;
  std::vector<QppBlockBounds> stack;
  int line_no = 0;
  while (!text.empty()) {
    size_t nl = text.find('\n');
    std::string_view line =
        (nl == std::string_view::npos) ? text : text.substr(0, nl);
    if (IsQppKw(line, "if")) {
      stack.push_back({line_no, -1, -1});
    } else if (IsQppKw(line, "else")) {
      if (!stack.empty() && stack.back().else_line == -1)
        stack.back().else_line = line_no;
    } else if (IsQppKw(line, "pass")) {
      if (!stack.empty()) {
        stack.back().pass_line = line_no;
        result.push_back(stack.back());
        stack.pop_back();
      }
    }
    ++line_no;
    text = (nl == std::string_view::npos) ? std::string_view()
                                          : text.substr(nl + 1);
  }
  return result;
}

// Returns true if any block in 'blocks' is strictly nested inside another.
static bool HasNestedQppBlocks(const std::vector<QppBlockBounds> &blocks) {
  for (const auto &outer : blocks)
    for (const auto &inner : blocks)
      if (&outer != &inner && inner.if_line > outer.if_line &&
          inner.if_line < outer.pass_line)
        return true;
  return false;
}

// Split 'text' into lines, keeping the trailing '\n' attached to each line.
static std::vector<std::string> SplitLines(std::string_view text) {
  std::vector<std::string> result;
  while (!text.empty()) {
    size_t nl = text.find('\n');
    if (nl == std::string_view::npos) {
      result.emplace_back(text);
      break;
    }
    result.emplace_back(text.substr(0, nl + 1));
    text = text.substr(nl + 1);
  }
  return result;
}

// Return a copy of 'text' with branch content replaced by bare newlines for
// the non-selected side of each QPP block.  QPP directive lines are kept.
//   keep_if=true  : blank lines between ;else: and ;pass (keep if-branch)
//   keep_if=false : blank lines between ;if and ;else:   (keep else-branch)
static std::string MaskQppBranches(std::string_view text, bool keep_if,
                                   const std::vector<QppBlockBounds> &blocks) {
  std::vector<std::string> lines = SplitLines(text);
  std::vector<bool> blank(lines.size(), false);
  for (const auto &b : blocks) {
    if (keep_if) {
      if (b.else_line >= 0)
        for (int i = b.else_line + 1; i < b.pass_line; ++i) blank[i] = true;
    } else {
      int end = (b.else_line >= 0) ? b.else_line : b.pass_line;
      for (int i = b.if_line + 1; i < end; ++i) blank[i] = true;
    }
  }
  std::string result;
  for (size_t i = 0; i < lines.size(); ++i)
    result += blank[i] ? "\n" : lines[i];
  return result;
}

// Extract the formatted else-branch content (lines between each ;else: and
// ;pass, exclusive of the directive lines) from 'else_fmt'.
// Returns one string per QPP block with an else-branch, in document order.
static std::vector<std::string> ExtractElseSegments(
    const std::string &else_fmt) {
  std::vector<std::string> segments;
  bool in_else = false;
  std::string seg;
  for (const auto &line : SplitLines(else_fmt)) {
    std::string_view lv = line;
    if (!lv.empty() && lv.back() == '\n') lv.remove_suffix(1);
    if (IsQppKw(lv, "else")) {
      in_else = true;
      seg.clear();
    } else if (IsQppKw(lv, "pass") && in_else) {
      segments.push_back(seg);
      in_else = false;
    } else if (in_else) {
      seg += line;
    }
  }
  return segments;
}

// Merge if_formatted (has if-branch content, blank placeholder for else) with
// else-branch segments extracted from else_formatted.  For each QPP block:
//   - if-branch content comes from if_fmt
//   - else-branch content comes from else_segs
//   - QPP directive lines (;if, ;else:, ;pass) come from if_fmt
static std::string MergeQppBranches(const std::string &if_fmt,
                                     const std::vector<std::string> &else_segs) {
  std::string result;
  bool in_else_section = false;
  size_t else_seg_idx = 0;
  for (const auto &line : SplitLines(if_fmt)) {
    std::string_view lv = line;
    if (!lv.empty() && lv.back() == '\n') lv.remove_suffix(1);
    if (IsQppKw(lv, "else")) {
      result += line;  // emit ;else: directive, then switch source
      in_else_section = true;
    } else if (IsQppKw(lv, "pass") && in_else_section) {
      // Emit else-branch content (from else_fmt), then ;pass
      if (else_seg_idx < else_segs.size())
        result += else_segs[else_seg_idx++];
      result += line;
      in_else_section = false;
    } else if (in_else_section) {
      // Skip blank placeholder line in if_fmt
    } else {
      result += line;
    }
  }
  return result;
}

Status FormatVerilog(std::string_view text, std::string_view filename,
                     const FormatStyle &style, std::ostream &formatted_stream,
                     const LineNumberSet &lines,
                     const ExecutionControl &control) {
  // Replace bare-ident QPP inline exprs (`clk`, `hash`, etc.) with stable
  // placeholder identifiers so the SV parser processes them cleanly.
  // Placeholders are restored in the output after formatting.
  std::vector<std::pair<std::string, std::string>> qpp_subs;
  std::string substituted = SubstituteQppInlineExprs(text, &qpp_subs);
  std::string_view effective_text = qpp_subs.empty() ? text : substituted;

  // Scan for QPP if/else/pass blocks and choose the formatting strategy:
  //   sequential : each branch formatted independently (preferred)
  //   single-pass: both branches visible simultaneously (fallback)
  // Sequential is used when there are else-branches and no nested blocks.
  const std::vector<QppBlockBounds> qpp_blocks = ScanQppBlocks(effective_text);
  const bool has_else =
      std::any_of(qpp_blocks.begin(), qpp_blocks.end(),
                  [](const QppBlockBounds &b) { return b.else_line >= 0; });
  const bool use_sequential = has_else && !HasNestedQppBlocks(qpp_blocks);

  std::string formatted_text;
  Status format_status;

  if (use_sequential) {
    // Pass 1: format if-masked text (else-branches replaced by blank lines).
    const std::string if_masked =
        MaskQppBranches(effective_text, /*keep_if=*/true, qpp_blocks);
    const auto if_analyzer = ParseWithStatus(if_masked, filename);
    if (!if_analyzer.ok()) return if_analyzer.status();
    format_status = FormatVerilog(if_analyzer->get()->Data(), filename, style,
                                  &formatted_text, lines, control);

    // Pass 2: format else-masked text, extract else-branch content.
    const std::string else_masked =
        MaskQppBranches(effective_text, /*keep_if=*/false, qpp_blocks);
    const auto else_analyzer = ParseWithStatus(else_masked, filename);
    if (else_analyzer.ok()) {
      std::string else_formatted;
      // Best-effort: ignore status; proceed with whatever was formatted.
      (void)FormatVerilog(else_analyzer->get()->Data(), filename, style,
                          &else_formatted, lines, control);
      // Merge: substitute formatted else-branch content into if-formatted.
      formatted_text =
          MergeQppBranches(formatted_text, ExtractElseSegments(else_formatted));
    }
    // If else-masked fails to parse (unbalanced branch), formatted_text
    // retains the if-only result with blank else placeholders — acceptable
    // degraded output.  The user can fix the unbalanced branch.
  } else {
    // Single-pass: both branches visible simultaneously.
    const auto analyzer = ParseWithStatus(effective_text, filename);
    if (!analyzer.ok()) return analyzer.status();
    format_status = FormatVerilog(analyzer->get()->Data(), filename, style,
                                  &formatted_text, lines, control);
  }

  // Commit formatted text to the output stream, restoring QPP inline exprs.
  if (qpp_subs.empty()) {
    formatted_stream << formatted_text;
  } else {
    formatted_stream << RestoreQppInlineExprs(formatted_text, qpp_subs);
  }
  if (!format_status.ok()) return format_status;

  // When formatting whole-file (no --lines are specified), ensure that
  // the formatting transformation is convergent after one iteration.
  //   format(format(text)) == format(text)
  // For sequential formatting: convergence is verified on the if-masked text
  // (a clean single-branch SV file).  Full merged convergence is a TODO.
  if (control.verify_convergence) {
    // For sequential formatting, verify convergence on the if-masked text
    // (a clean single-branch SV file).  Full merged convergence is a TODO.
    std::string convergence_buf;
    std::string_view cv_text = effective_text;
    if (use_sequential) {
      convergence_buf =
          MaskQppBranches(effective_text, /*keep_if=*/true, qpp_blocks);
      cv_text = convergence_buf;
    }
    std::ostringstream reformat_stream;
    if (auto reformat_status =
            ReformatVerilog(cv_text, formatted_text, filename, style,
                            reformat_stream, lines, control);
        !reformat_status.ok()) {
      return reformat_status;
    }
    const std::string &reformatted_text(reformat_stream.str());
    return verible::ReformatMustMatch(cv_text, lines, formatted_text,
                                      reformatted_text);
  }
  return format_status;
}

absl::Status FormatVerilogRange(const verible::TextStructureView &structure,
                                const FormatStyle &style,
                                std::string *formatted_text,
                                const verible::Interval<int> &line_range,
                                const ExecutionControl &control) {
  if (line_range.empty()) {
    return absl::OkStatus();
  }

  Formatter fmt(structure, style);
  fmt.SelectLines({line_range});

  // Format code.
  Status format_status = fmt.Format(control);
  if (!format_status.ok()) return format_status;

  // In any diagnostic mode, proceed no further.
  if (control.AnyStop()) {
    return absl::CancelledError("Halting for diagnostic operation.");
  }

  std::ostringstream output_buffer;
  fmt.Emit(false, output_buffer);
  *formatted_text = output_buffer.str();

  // The range-format can output a spurious newline in the beginning (#1150).
  // Whitespace handling needs some rework in the formatter, and it is not
  // trivial in the current state to fix at the source.
  // However, we can easily see the effects and mitigate it here: if we see a
  // newline at the beginning of the formatted range, but no newline at the
  // beginning of the original range, erase it in the formatted output.
  // TODO(hzeller): This can go when whitespace handling is revisited.
  //                (Emit(), FormatWhitespaceWithDisabledByteRanges())
  const auto &text_lines = structure.Lines();
  const char unformatted_begin = *text_lines[line_range.min - 1].begin();
  if (!formatted_text->empty() && (*formatted_text)[0] == '\n' &&
      unformatted_begin != '\n') {
    formatted_text->erase(0, 1);  // possibly expensive.
  }

  // We don't have verification tests here as we only have a subset of code
  // so it would be more tricky. Since this output is used in interactive
  // settings such as editors, this is less of an issue.

  return absl::OkStatus();
}

absl::Status FormatVerilogRange(std::string_view full_content,
                                std::string_view filename,
                                const FormatStyle &style,
                                std::string *formatted_text,
                                const verible::Interval<int> &line_range,
                                const ExecutionControl &control) {
  const auto analyzer = ParseWithStatus(full_content, filename);
  if (!analyzer.ok()) return analyzer.status();
  return FormatVerilogRange(analyzer->get()->Data(), style, formatted_text,
                            line_range, control);
}

static verible::Interval<int> DisableByteOffsetRange(
    std::string_view substring, std::string_view superstring) {
  CHECK(!substring.empty());
  auto range = verible::SubstringOffsets(substring, superstring);
  // +1 so that formatting can still occur on the space before the start
  // of the disabled range, for example allowing for indentation adjustments.
  return {range.first + 1, range.second};
}

// Decided at each node in UnwrappedLine partition tree whether or not
// it should be expanded or unexpanded.
static void DeterminePartitionExpansion(
    partition_node_type *node,
    std::vector<verible::PreFormatToken> *preformatted_tokens,
    std::string_view full_text, const ByteOffsetSet &disabled_ranges,
    const FormatStyle &style) {
  auto &node_view = node->Value();
  const UnwrappedLine &uwline = node_view.Value();
  VLOG(3) << "unwrapped line: " << uwline;
  const verible::FormatTokenRange ftoken_range(uwline.TokensRange());
  const auto partition_policy = uwline.PartitionPolicy();

  const auto PreserveSpaces = [&ftoken_range, &full_text,
                               preformatted_tokens]() {
    const ByteOffsetSet new_disable_range{{DisableByteOffsetRange(
        verible::make_string_view_range(ftoken_range.front().Text().begin(),
                                        ftoken_range.back().Text().end()),
        full_text)}};
    verible::PreserveSpacesOnDisabledTokenRanges(preformatted_tokens,
                                                 new_disable_range, full_text);
  };

  // Expand or not, depending on partition policy and other conditions.

  // If this is a leaf partition, there is nothing to expand.
  if (is_leaf(*node)) {
    VLOG(3) << "No children to expand.";
    node_view.Unexpand();
    if (partition_policy == PartitionPolicyEnum::kFitOnLineElseExpand &&
        !style.try_wrap_long_lines &&
        !verible::FitsOnLine(uwline, style).fits) {
      // give-up early and preserve original spacing
      VLOG(3) << "Does not fit (leaf), preserving.";
      PreserveSpaces();
    }
    return;
  }

  // If any children are expanded, then this node must be expanded,
  // regardless of the UnwrappedLine's chosen policy.
  // Thus, this function must be executed with a post-order traversal.
  const auto &children = node->Children();
  if (std::any_of(children.begin(), children.end(),
                  [](const partition_node_type &child) {
                    return child.Value().IsExpanded();
                  })) {
    VLOG(3) << "Child forces parent to expand.";
    node_view.Expand();
    return;
  }

  {
    // If any part of the range is formatting-disabled, expand this partition so
    // that whitespace between subpartitions can be handled accordingly in
    // Formatter::Emit().
    const verible::FormatTokenRange range(uwline.TokensRange());
    const verible::IntervalSet<int> partition_byte_range{
        {range.front().token->left(full_text),
         range.back().token->right(full_text)}};
    // (Same as IntervalSet::Complement without temporary copy.)
    verible::IntervalSet<int> diff(partition_byte_range);
    diff.Difference(disabled_ranges);
    if (partition_byte_range != diff) {
      // Then some sub-interval was removed.
      VLOG(3) << "Partition @bytes " << partition_byte_range
              << " is partially format-disabled, so expand.";
      node_view.Expand();
      return;
    }
  }

  VLOG(3) << "partition policy: " << partition_policy;
  switch (partition_policy) {
    case PartitionPolicyEnum::kUninitialized: {
      LOG(FATAL) << "Got an uninitialized partition policy at: " << uwline;
      break;
    }
    case PartitionPolicyEnum::kAlwaysExpand: {
      if (children.size() > 1) {
        node_view.Expand();
      }
      break;
    }
    case PartitionPolicyEnum::kTabularAlignment: {
      if (uwline.Origin()->Tag().tag ==
          static_cast<int>(NodeEnum::kArgumentList)) {
        // Check whether the whole function call fits on one line. If possible,
        // unexpand and fit into one line. Otherwise expand argument list with
        // tabular alignment.
        auto &node_view_parent = node->Parent()->Value();
        const UnwrappedLine &uwline_parent = node_view_parent.Value();
        if (verible::FitsOnLine(uwline_parent, style).fits) {
          node_view.Unexpand();
          break;
        }
      }

      if (children.size() > 1) {
        node_view.Expand();
      }
      break;
    }

    case PartitionPolicyEnum::kAlreadyFormatted: {
      // All partitions with this policy should be leafs at this point, which
      // are handled above. Just in case - unexpand.
      node_view.Unexpand();
      break;
    }

    case PartitionPolicyEnum::kInline: {
      // All partitions with this policy should be already applied and removed
      // by calls to ApplyAlreadyFormattedPartitionPropertiesToTokens on their
      // parent partitions.
      LOG(FATAL) << "Unreachable. " << partition_policy;
      break;
    }

    // Try to fit kAppendFittingSubPartitions partition into single line.
    // If it doesn't fit expand to grouped nodes.
    case PartitionPolicyEnum::kAppendFittingSubPartitions: {
      // !style.try_wrap_long_lines was already handled above
      if (verible::FitsOnLine(uwline, style).fits) {
        VLOG(3) << "Fits, un-expanding.";
        node_view.Unexpand();
      } else {
        VLOG(3) << "Does not fit, expanding.";
        node_view.Expand();
      }
      break;
    }

    case PartitionPolicyEnum::kFitOnLineElseExpand: {
      if (uwline.Origin() &&
          (uwline.Origin()->Tag().tag ==
               static_cast<int>(NodeEnum::kNetVariableAssignment) ||
           uwline.Origin()->Tag().tag ==
               static_cast<int>(NodeEnum::kBlockItemStatementList) ||
           uwline.Origin()->Tag().tag ==
               static_cast<int>(NodeEnum::kBlockingAssignmentStatement))) {
        // Align unnamed parameters in function call. Example:
        // always_comb begin
        //   value = function_name(8'hA, signal,
        //                         signal_1234);
        // end
        const auto &children_tmp = node->Children();
        auto look_for_arglist = [](const partition_node_type &child) {
          const auto &node_view_child = child.Value();
          const UnwrappedLine &uwline_child = node_view_child.Value();
          return (uwline_child.Origin() &&
                  uwline_child.Origin()->Kind() == verible::SymbolKind::kNode &&
                  verible::SymbolCastToNode(*uwline_child.Origin())
                      .MatchesTag(NodeEnum::kArgumentList));
        };

        // Check if kNetVariableAssignment or kBlockItemStatementList contains
        // kArgumentList node
        if (std::any_of(children_tmp.begin(), children_tmp.end(),
                        look_for_arglist)) {
          node_view.Unexpand();
          break;
        }
      }

      if (verible::FitsOnLine(uwline, style).fits) {
        VLOG(3) << "Fits, un-expanding.";
        node_view.Unexpand();
      } else {
        VLOG(3) << "Does not fit, expanding.";
        node_view.Expand();
      }
      break;
    }

    case PartitionPolicyEnum::kJuxtapositionOrIndentedStack:
    case PartitionPolicyEnum::kJuxtaposition:
    case PartitionPolicyEnum::kStack:
    case PartitionPolicyEnum::kWrap: {
      // The policies are handled (and replaced) in Layout Optimizer.
      LOG(FATAL) << "Unreachable. " << partition_policy;
      break;
    }
  }
}

// Produce a worklist of independently formattable UnwrappedLines.
static std::vector<UnwrappedLine> MakeUnwrappedLinesWorklist(
    const FormatStyle &style, std::string_view full_text,
    const ByteOffsetSet &disabled_ranges,
    const TokenPartitionTree &format_tokens_partitions,
    std::vector<verible::PreFormatToken> *preformatted_tokens) {
  // Initialize a tree view that treats partitions as fully-expanded.
  ExpandableTreeView<TokenPartitionTree> format_tokens_partition_view(
      format_tokens_partitions);

  // For unwrapped lines that fit, don't bother expanding their partitions.
  // Post-order traversal: if a child doesn't 'fit' and needs to be expanded,
  // so must all of its parents (and transitively, ancestors).
  format_tokens_partition_view.ApplyPostOrder(
      [&full_text, &disabled_ranges, &style,
       preformatted_tokens](partition_node_type &node) {
        DeterminePartitionExpansion(&node, preformatted_tokens, full_text,
                                    disabled_ranges, style);
      });

  // Remove trailing blank lines.
  std::vector<UnwrappedLine> unwrapped_lines(
      format_tokens_partition_view.begin(), format_tokens_partition_view.end());
  while (!unwrapped_lines.empty() && unwrapped_lines.back().IsEmpty()) {
    unwrapped_lines.pop_back();
  }
  return unwrapped_lines;
}

static void PrintLargestPartitions(
    std::ostream &stream, const TokenPartitionTree &token_partitions,
    size_t max_partitions, const verible::LineColumnMap &line_column_map,
    std::string_view base_text) {
  stream << "Showing the " << max_partitions
         << " largest (leaf) token partitions:" << std::endl;
  const auto ranked_partitions =
      FindLargestPartitions(token_partitions, max_partitions);
  const verible::Spacer hline(80, '=');
  for (const auto &partition : ranked_partitions) {
    stream << hline << "\n[" << partition->Size() << " tokens";
    if (!partition->IsEmpty()) {
      stream << ", starting at line:col "
             << line_column_map.GetLineColAtOffset(
                    base_text,
                    partition->TokensRange().front().token->left(base_text));
    }
    stream << "]: " << *partition << std::endl;
  }
  stream << hline << std::endl;
}

std::ostream &ExecutionControl::Stream() const {
  return (stream != nullptr) ? *stream : std::cout;
}

void Formatter::SelectLines(const LineNumberSet &lines) {
  disabled_ranges_ = EnabledLinesToDisabledByteRanges(
      lines, text_structure_.GetLineColumnMap());
}

// Given control flags and syntax tree, selectively disable some ranges
// of text from formatting.  This provides an easy way to preserve spacing on
// selected syntax subtrees to reduce formatter harm while allowing
// development to progress.
static void DisableSyntaxBasedRanges(ByteOffsetSet *disabled_ranges,
                                     const verible::Symbol &root,
                                     const FormatStyle &style,
                                     std::string_view full_text) {
  /**
  // Basic template:
  if (!style.controlling_flag) {
    for (const auto& match : FindAllSyntaxTreeNodeTypes(root)) {
      // Refine search into specific subtrees, if applicable.
      // Convert the spanning string_views into byte offset ranges to disable.
      const auto inst_text = verible::StringSpanOfSymbol(*match.match);
      VLOG(4) << "disabled: " << inst_text;
      disabled_ranges->Add(DisableByteOffsetRange(inst_text, full_text));
    }
  }
  **/
}

// Keeps multi-line EOL comments aligned to the same column.
//
// When a line containing nothing else than a single EOL comment follows a line
// containing any tokens and an EOL comment, starting columns (from original,
// unformatted source code) of both comments are compared. If the columns differ
// no more than kMaxColumnDifference, the comment in the comment-only line is
// considered to be a continuation comment. The same check is performed on all
// following comment-only lines. The process of continuation detection ends when
// currently handled line has any tokens other than EOL comment, or when the
// comment's starting column differs too much from the first comment's column.
// All continuation comments are placed in the same column as their starting
// comment's column in formatted output.
class ContinuationCommentAligner {
  // Maximum accepted difference between continuation and starting comments'
  // starting columns
  static constexpr int kMaxColumnDifference = 1;

 public:
  ContinuationCommentAligner(const verible::LineColumnMap &line_column_map,
                             const std::string_view base_text)
      : line_column_map_(line_column_map), base_text_(base_text) {}

  // Takes the next line that has to be formatted and a vector of already
  // formatted lines.
  //
  // Continuation comment lines are formatted and appended to
  // already_formatted_lines. In case of all other lines neither the line nor
  // already_formatted_lines are modified.
  // Return value informs whether the line has been formatted and added
  // to already_formatted_lines.
  bool HandleLine(
      const UnwrappedLine &uwline,
      std::vector<verible::FormattedExcerpt> *already_formatted_lines) {
    VLOG(4) << __FUNCTION__ << ": " << uwline;

    if (already_formatted_lines->empty()) {
      VLOG(4) << "Not a continuation comment line: first line";
      return false;
    }

    if (uwline.Size() != 1 || uwline.TokensRange().back().TokenEnum() !=
                                  verilog_tokentype::TK_EOL_COMMENT) {
      VLOG(4) << "Not a continuation comment line: "
              << "does not consist of a single EOL comment.";
      formatted_column_ = kInvalidColumn;
      original_column_ = kInvalidColumn;
      return false;
    }

    const auto &previous_line = already_formatted_lines->back();
    VLOG(4) << __FUNCTION__ << ": previous line: " << previous_line;
    if (original_column_ == kInvalidColumn) {
      if (previous_line.Tokens().size() <= 1) {
        VLOG(4) << "Not a continuation comment line: "
                << "too few tokens in previous line.";
        return false;
      }
      const auto *previous_comment = previous_line.Tokens().back().token;
      if (previous_comment->token_enum() != verilog_tokentype::TK_EOL_COMMENT) {
        VLOG(4) << "Not a continuation comment line: "
                << "no EOL comment in previous line.";
        return false;
      }
      original_column_ = GetTokenColumn(previous_comment);
    }

    const auto *comment = uwline.TokensRange().back().token;
    const int comment_column = GetTokenColumn(comment);

    VLOG(4) << "Original column: " << original_column_ << " vs. "
            << comment_column;

    if (std::abs(original_column_ - comment_column) > kMaxColumnDifference) {
      VLOG(4) << "Not a continuation comment line: "
              << "starting column difference is too big";
      original_column_ = kInvalidColumn;
      return false;
    }

    VLOG(4) << "Continuation comment line - finalizing formatting";
    if (formatted_column_ == kInvalidColumn) {
      formatted_column_ = CalculateEolCommentColumn(previous_line);
    }
    UnwrappedLine aligned_uwline(uwline);
    aligned_uwline.SetIndentationSpaces(formatted_column_);
    already_formatted_lines->emplace_back(aligned_uwline);

    return true;
  }

 private:
  int GetTokenColumn(const verible::TokenInfo *token) {
    CHECK_NOTNULL(token);
    const int column =
        line_column_map_.GetLineColAtOffset(base_text_, token->left(base_text_))
            .column;
    CHECK_GE(column, 0);
    return column;
  }

  static void AdjustColumnUsingTokenSpacing(
      const verible::FormattedToken &token, int *column) {
    switch (token.before.action) {
      case verible::SpacingDecision::kPreserve: {
        if (token.before.preserved_space_start !=
            verible::string_view_null_iterator()) {
          *column += token.OriginalLeadingSpaces().length();
        } else {
          *column += token.before.spaces;
        }
        break;
      }
      case verible::SpacingDecision::kWrap:
        *column = 0;
        ABSL_FALLTHROUGH_INTENDED;
      case verible::SpacingDecision::kAlign:
      case verible::SpacingDecision::kAppend:
        *column += token.before.spaces;
        break;
    }
  }

  static int CalculateEolCommentColumn(const verible::FormattedExcerpt &line) {
    int column = 0;
    const auto &front = line.Tokens().front();

    if (front.before.action != verible::SpacingDecision::kPreserve) {
      column += line.IndentationSpaces();
    }
    if (front.before.action == verible::SpacingDecision::kAlign) {
      column += front.before.spaces;
    }
    column += front.token->text().length();

    for (const auto &ftoken : verible::make_range(line.Tokens().begin() + 1,
                                                  line.Tokens().end() - 1)) {
      AdjustColumnUsingTokenSpacing(ftoken, &column);
      column += ftoken.token->text().length();
    }
    AdjustColumnUsingTokenSpacing(line.Tokens().back(), &column);

    CHECK_GE(column, 0);
    return column;
  }

  const verible::LineColumnMap &line_column_map_;
  const std::string_view base_text_;

  // Used when the most recenly handled line can't have a continuation comment.
  static constexpr int kInvalidColumn = -1;

  // Starting column of current comment group in original source code.
  int original_column_ = kInvalidColumn;
  // Starting column of current comment group in formatted source code.
  int formatted_column_ = kInvalidColumn;
};

Status Formatter::Format(const ExecutionControl &control) {
  const std::string_view full_text(text_structure_.Contents());
  const auto &token_stream(text_structure_.TokenStream());

  // Initialize auxiliary data needed for TreeUnwrapper.
  UnwrapperData unwrapper_data(token_stream);

  // Partition input token stream into hierarchical set of UnwrappedLines.
  TreeUnwrapper tree_unwrapper(text_structure_, style_,
                               unwrapper_data.preformatted_tokens);

  const TokenPartitionTree *format_tokens_partitions = nullptr;
  // TODO(fangism): The following block could be parallelized because
  // full-partitioning does not depend on format annotations.
  {
    // Annotate inter-token information between all adjacent PreFormatTokens.
    // This must be done before any decisions about ExpandableTreeView
    // can be made because they depend on minimum-spacing, and must-break.
    AnnotateFormattingInformation(style_, text_structure_,
                                  &unwrapper_data.preformatted_tokens);

    // Determine ranges of disabling the formatter, based on comment controls.
    disabled_ranges_.Union(DisableFormattingRanges(full_text, token_stream));

    // Find disabled formatting ranges for specific syntax tree node types.
    // These are typically temporary workarounds for sections that users
    // habitually prefer to format themselves.
    if (const auto &root = text_structure_.SyntaxTree()) {
      DisableSyntaxBasedRanges(&disabled_ranges_, *root, style_, full_text);
    }

    // QPP directive lines (;if, ;else:, ;pass) must stay at column 0.
    // Setting kPreserve here causes FormattedExcerpt::FormattedText to skip
    // IndentationSpaces() for the front token of any QPP directive partition.
    for (auto &ftoken : unwrapper_data.preformatted_tokens) {
      if (verilog_tokentype(ftoken.token->token_enum()) ==
          verilog_tokentype::TK_QPP_DIRECTIVE) {
        ftoken.before.break_decision = verible::SpacingOptions::kPreserve;
      }
    }

    // Disable formatting ranges.
    verible::PreserveSpacesOnDisabledTokenRanges(
        &unwrapper_data.preformatted_tokens, disabled_ranges_, full_text);

    // Partition PreFormatTokens into candidate unwrapped lines.
    format_tokens_partitions = tree_unwrapper.Unwrap();
  }

  {
    // For debugging only: identify largest leaf partitions, and stop.
    if (control.show_token_partition_tree) {
      control.Stream() << "Full token partition tree:\n"
                       << verible::TokenPartitionTreePrinter(
                              *format_tokens_partitions,
                              control.show_inter_token_info)
                       << std::endl;
    }
    if (control.show_largest_token_partitions != 0) {
      PrintLargestPartitions(control.Stream(), *format_tokens_partitions,
                             control.show_largest_token_partitions,
                             text_structure_.GetLineColumnMap(), full_text);
    }
    if (control.AnyStop()) {
      return absl::OkStatus();
    }
  }

  {  // In this pass, perform additional modifications to the partitions and
     // spacings.
    tree_unwrapper.ApplyPreOrder([&](TokenPartitionTree &node) {
      const auto &uwline = node.Value();
      const auto partition_policy = uwline.PartitionPolicy();

      switch (partition_policy) {
        case PartitionPolicyEnum::kAppendFittingSubPartitions:
          // Reshape partition tree with kAppendFittingSubPartitions policy
          verible::ReshapeFittingSubpartitions(style_, &node);
          break;
        case PartitionPolicyEnum::kJuxtaposition:
        case PartitionPolicyEnum::kStack:
        case PartitionPolicyEnum::kWrap:
        case PartitionPolicyEnum::kJuxtapositionOrIndentedStack:
          verible::OptimizeTokenPartitionTree(style_, &node);
          break;
        case PartitionPolicyEnum::kTabularAlignment:
          // TODO(b/145170750): Adjust inter-token spacing to achieve alignment,
          // but leave partitioning intact.
          // This relies on inter-token spacing having already been annotated.
          TabularAlignTokenPartitions(style_, full_text, disabled_ranges_,
                                      &node);
          break;
        default:
          break;
      }
    });
  }

  // Apply token spacing from partitions to tokens. This is permanent, so it
  // must be done after all reshaping is done.
  {
    auto *root = tree_unwrapper.CurrentTokenPartition();
    auto node_iter = VectorTreeLeavesIterator(&LeftmostDescendant(*root));
    const auto end = ++VectorTreeLeavesIterator(&RightmostDescendant(*root));

    // Iterate over leaves. kAlreadyFormatted partitions are either leaves
    // themselves or parents of leaf partitions with kInline policy.
    for (; node_iter != end; ++node_iter) {
      const auto partition_policy = node_iter->Value().PartitionPolicy();

      if (partition_policy == PartitionPolicyEnum::kAlreadyFormatted) {
        verible::ApplyAlreadyFormattedPartitionPropertiesToTokens(
            &(*node_iter), &unwrapper_data.preformatted_tokens);
      } else if (partition_policy == PartitionPolicyEnum::kInline) {
        auto *parent = node_iter->Parent();
        CHECK_NOTNULL(parent);
        CHECK_EQ(parent->Value().PartitionPolicy(),
                 PartitionPolicyEnum::kAlreadyFormatted);
        // This removes the node pointed to by node_iter (and all other
        // siblings)
        verible::ApplyAlreadyFormattedPartitionPropertiesToTokens(
            parent, &unwrapper_data.preformatted_tokens);
        // Move to the parent which is now a leaf
        node_iter = verible::VectorTreeLeavesIterator(parent);
      }
    }
  }

  // Re-apply kPreserve for QPP directive tokens.  The
  // ApplyAlreadyFormattedPartitionPropertiesToTokens pass above sets
  // kMustWrap on the first token of every kAlreadyFormatted partition, which
  // overrides the kPreserve we set earlier for QPP directives that happen to
  // fall inside an alignment group range (e.g. ';if' between two aligned
  // non-blocking assignments).  Re-applying here ensures they stay at column 0
  // regardless of whether they went through the kAlreadyFormatted path or the
  // SearchLineWraps path.
  for (auto &ftoken : unwrapper_data.preformatted_tokens) {
    if (verilog_tokentype(ftoken.token->token_enum()) ==
        verilog_tokentype::TK_QPP_DIRECTIVE) {
      ftoken.before.break_decision = verible::SpacingOptions::kPreserve;
    }
  }

  // Produce sequence of independently operable UnwrappedLines.
  const auto unwrapped_lines = MakeUnwrappedLinesWorklist(
      style_, full_text, disabled_ranges_, *format_tokens_partitions,
      &unwrapper_data.preformatted_tokens);

  // For each UnwrappedLine: minimize total penalty of wrap/break decisions.
  // TODO(fangism): This could be parallelized if results are written
  // to their own 'slots'.
  std::vector<const UnwrappedLine *> partially_formatted_lines;
  formatted_lines_.reserve(unwrapped_lines.size());
  ContinuationCommentAligner continuation_comment_aligner(
      text_structure_.GetLineColumnMap(), text_structure_.Contents());
  for (const auto &uwline : unwrapped_lines) {
    // TODO(fangism): Use different formatting strategies depending on
    // uwline.PartitionPolicy().
    if (continuation_comment_aligner.HandleLine(uwline, &formatted_lines_)) {
    } else if (uwline.PartitionPolicy() ==
               PartitionPolicyEnum::kAlreadyFormatted) {
      // For partitions that were successfully aligned, do not search
      // line-wrapping, but instead accept the adjusted padded spacing.
      formatted_lines_.emplace_back(uwline);
    } else {
      // In other case, default to searching for optimal line wrapping.
      const auto optimal_solutions =
          verible::SearchLineWraps(uwline, style_, control.max_search_states);
      if (control.show_equally_optimal_wrappings &&
          optimal_solutions.size() > 1) {
        verible::DisplayEquallyOptimalWrappings(control.Stream(), uwline,
                                                optimal_solutions);
      }
      // Arbitrarily choose the first solution, if there are multiple.
      formatted_lines_.push_back(optimal_solutions.front());
      if (!formatted_lines_.back().CompletedFormatting()) {
        // Copy over any lines that did not finish wrap searching.
        partially_formatted_lines.push_back(&uwline);
      }
    }
  }

  // Report any unwrapped lines that failed to complete wrap searching.
  if (!partially_formatted_lines.empty()) {
    std::ostringstream err_stream;
    err_stream << "*** Some token partitions failed to complete within the "
                  "search limit:"
               << std::endl;
    for (const auto *line : partially_formatted_lines) {
      err_stream << *line << std::endl;
    }
    err_stream << "*** end of partially formatted partition list" << std::endl;
    // Treat search state limit like a limited resource.
    return absl::ResourceExhaustedError(err_stream.str());
  }

  return absl::OkStatus();
}

// From options, extract the line terminator style. If 'auto' was chosen,
// attempt to determine from text.
static verible::LineTerminatorStyle DetermineOutputLineTerminator(
    verible::LineTerminatorOptionStyle from_options, std::string_view text) {
  static constexpr int32_t kCountAtMost = 100;  // sufficient stats
  switch (from_options) {
    case verible::LineTerminatorOptionStyle::kCRLF:
      return verible::LineTerminatorStyle::kCRLF;
    case verible::LineTerminatorOptionStyle::kLF:
      return verible::LineTerminatorStyle::kLF;
    case verible::LineTerminatorOptionStyle::kAuto:
      return verible::GuessLineTerminator(text, kCountAtMost);
  }
  return verible::LineTerminatorStyle::kLF;
}

void Formatter::Emit(bool include_disabled, std::ostream &stream) const {
  const std::string_view full_text(text_structure_.Contents());
  std::function<bool(const verible::TokenInfo &)> include_token_p;
  if (include_disabled) {
    include_token_p = [](const verible::TokenInfo &) { return true; };
  } else {
    include_token_p = [this, &full_text](const verible::TokenInfo &tok) {
      return !disabled_ranges_.Contains(tok.left(full_text));
    };
  }

  const verible::LineTerminatorStyle out_terminator =
      DetermineOutputLineTerminator(style_.line_terminator, full_text);
  int position = 0;  // tracks with the position in the original full_text
  for (const verible::FormattedExcerpt &line : formatted_lines_) {
    // TODO(fangism): The handling of preserved spaces before tokens is messy:
    // some of it is handled here, some of it is inside FormattedToken.
    // TODO(mglb): Test empty line handling when this method becomes testable.
    const auto front_offset =
        line.Tokens().empty() ? position
                              : line.Tokens().front().token->left(full_text);
    const std::string_view leading_whitespace(
        full_text.substr(position, front_offset - position));

    if (!line.Tokens().empty()) {
      const auto &front_token = line.Tokens().front();
      // When leading_whitespace is empty and the front token has kPreserve
      // spacing with a valid preserved_space_start, emit OriginalLeadingSpaces()
      // directly instead of going through FormatWhitespaceWithDisabledByteRanges.
      // FormatWhitespaceWithDisabledByteRanges inserts a spurious newline when
      // leading_whitespace is empty and the position is not in disabled_ranges_
      // (this fires for consecutive child partitions of a preserved
      // kFitOnLineElseExpand partition), causing non-convergent formatting.
      if (leading_whitespace.empty() &&
          front_token.before.action == verible::SpacingDecision::kPreserve &&
          front_token.before.preserved_space_start !=
              verible::string_view_null_iterator()) {
        stream << front_token.OriginalLeadingSpaces();
      } else {
        FormatWhitespaceWithDisabledByteRanges(full_text, leading_whitespace,
                                               disabled_ranges_, include_disabled,
                                               stream, out_terminator);
      }
      // When front of first token is format-disabled, the previous call will
      // already cover the space up to the front token, in which case,
      // the left-indentation for this line should be suppressed to avoid
      // being printed twice.
      line.FormattedText(stream, !disabled_ranges_.Contains(front_offset),
                         include_token_p);
      position = line.Tokens().back().token->right(full_text);
    } else {
      FormatWhitespaceWithDisabledByteRanges(full_text, leading_whitespace,
                                             disabled_ranges_, include_disabled,
                                             stream, out_terminator);
    }
  }

  // Handle trailing spaces after last token.
  const std::string_view trailing_whitespace(full_text.substr(position));
  FormatWhitespaceWithDisabledByteRanges(full_text, trailing_whitespace,
                                         disabled_ranges_, include_disabled,
                                         stream, out_terminator);
}

}  // namespace formatter
}  // namespace verilog
