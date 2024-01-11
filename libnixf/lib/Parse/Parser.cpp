/// \file
/// \brief Parser implementation.
#pragma once

#include "Lexer.h"

#include "nixf/Basic/Diagnostic.h"
#include "nixf/Basic/DiagnosticEngine.h"
#include "nixf/Basic/Range.h"
#include "nixf/Parse/Nodes.h"
#include "nixf/Parse/Parser.h"

#include <cassert>
#include <charconv>
#include <deque>
#include <memory>
#include <stack>
#include <string_view>

namespace {

using namespace nixf;
using namespace nixf::tok;
class RangeBuilder {
  std::stack<const char *> Begins;

public:
  void push(const char *Begin) { Begins.push(Begin); }
  OffsetRange finish(const char *End) {
    assert(!Begins.empty());
    return {Begins.top(), End};
  }
  OffsetRange pop() {
    assert(!Begins.empty());
    const char *Begin = Begins.top();
    Begins.pop();
    return {Begin, Begin};
  }
};

Diagnostic &diagNullExpr(DiagnosticEngine &Diag, const char *Loc,
                         std::string As) {
  Diagnostic &D = Diag.diag(Diagnostic::DK_Expected, OffsetRange(Loc));
  D << ("an expression as " + std::move(As));
  D.fix(Fix::mkInsertion(Loc, " expr"));
  return D;
}

class Parser {
public:
  enum ParserState {
    PS_Expr,
    PS_String,
    PS_IndString,
    PS_Path,
  };

private:
  std::string_view Src;
  Lexer Lex;
  DiagnosticEngine &Diag;

  std::deque<Token> LookAheadBuf;
  std::optional<Token> LastToken;
  std::stack<ParserState> State;
  RangeBuilder RB;

  void pushState(ParserState NewState) {
    assert(LookAheadBuf.empty() &&
           "LookAheadBuf should be empty when pushing state");
    State.push(NewState);
  }

  void popState() {
    assert(LookAheadBuf.empty() &&
           "LookAheadBuf should be empty when popping state");
    State.pop();
  }

  Token peek(std::size_t N = 0) {
    while (N >= LookAheadBuf.size()) {
      switch (State.top()) {
      case PS_Expr:
        LookAheadBuf.emplace_back(Lex.lex());
        break;
      case PS_String:
        LookAheadBuf.emplace_back(Lex.lexString());
        break;
      case PS_IndString:
        LookAheadBuf.emplace_back(Lex.lexIndString());
        break;
      case PS_Path:
        LookAheadBuf.emplace_back(Lex.lexPath());
        break;
      }
    }
    return LookAheadBuf[N];
  }

  void consume() {
    if (LookAheadBuf.empty())
      peek(0);
    popBuf();
  }

  Token popBuf() {
    LastToken = LookAheadBuf.front();
    LookAheadBuf.pop_front();
    return *LastToken;
  }

public:
  Parser(std::string_view Src, DiagnosticEngine &Diag)
      : Src(Src), Lex(Src, Diag), Diag(Diag) {
    pushState(PS_Expr);
  }

  /// \brief Parse interpolable things.
  ///
  /// They are strings, ind-strings, paths, in nix language.
  /// \note This needs context-switching so look-ahead buf should be cleared.
  std::shared_ptr<InterpolatedParts> parseStringParts() {
    std::vector<StringPart> Parts;
    RB.push(Lex.cur());
    while (true) {
      // FIXME: maybe create a stack-based state machine?
      assert(LookAheadBuf.empty()); // We are switching contexts.
      switch (Token Tok = peek(0); Tok.getKind()) {
      case tok_dollar_curly: {
        consume();
        assert(LastToken);
        // interpolation, we need to parse a subtree then.
        if (auto Expr = parseExpr())
          Parts.emplace_back(std::move(Expr));
        else
          diagNullExpr(Diag, LastToken->getEnd(), "interpolation");
        continue;
      }
      case tok_string_part:
      case tok_string_escape:
        // If this is a part of string, just push it.
        consume();
        // TODO: escape and emplace_back
        continue;
      default:
        OffsetRange Range;
        if (LastToken)
          Range = RB.finish(LastToken->getEnd());
        else
          Range = RB.pop();
        return std::make_shared<InterpolatedParts>(Range,
                                                   std::move(Parts)); // TODO!
      }
    }
  }

  std::shared_ptr<Node> parseString() {
    Token Tok = peek();
    assert(Tok.getKind() == tok_dquote);
    // Consume the quote and so make the look-ahead buf empty.
    consume();
    // Switch to string parsing context.
    pushState(PS_String);
    auto Parts = parseStringParts();
    // TODO: string end
    popState();
    return nullptr; // TODO!
  }

  std::shared_ptr<Expr> parseExprSimple() {
    Token Tok = peek();
    switch (Tok.getKind()) {
    case tok_int: {
      consume();
      NixInt N;
      auto [_, Err] = std::from_chars(Tok.getBegin(), Tok.getEnd(), N);
      assert(Err == std::errc());
      return std::make_shared<ExprInt>(Tok.getRange(), N);
    }
    case tok_float: {
      consume();
      // libc++ doesn't support std::from_chars for floating point numbers.
      NixFloat N = std::strtof(std::string(Tok.view()).c_str(), nullptr);
      return std::make_shared<ExprFloat>(Tok.getRange(), N);
    }
    default:
      return nullptr;
    }
  }

  std::shared_ptr<Expr> parseExpr() {
    return parseExprSimple(); // TODO!
  }
  std::shared_ptr<Expr> parse() { return parseExpr(); }
};

} // namespace

namespace nixf {

std::shared_ptr<Node> parse(std::string_view Src, DiagnosticEngine &Diag) {
  Parser P(Src, Diag);
  return P.parse();
}

} // namespace nixf
