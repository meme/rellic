/*
 * Copyright (c) 2018 Trail of Bits, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "rellic/AST/ExprCombine.h"
#include "rellic/AST/InferenceRule.h"
#include "rellic/AST/Util.h"

namespace rellic {

namespace {

using namespace clang::ast_matchers;

static const auto zero_int_lit = integerLiteral(equals(0));

// Matches `(&base)[0]` and subs it for `base`
class ArraySubscriptAddrOfRule : public InferenceRule {
 public:
  ArraySubscriptAddrOfRule()
      : InferenceRule(arraySubscriptExpr(
            stmt().bind("sub"),
            hasBase(parenExpr(has(unaryOperator(stmt().bind("base"))))),
            hasIndex(zero_int_lit))) {}

  void run(const MatchFinder::MatchResult &result) {
    auto op = result.Nodes.getNodeAs<clang::UnaryOperator>("base");
    if (op->getOpcode() == clang::UO_AddrOf) {
      match = result.Nodes.getNodeAs<clang::ArraySubscriptExpr>("sub");
    }
  }

  clang::Stmt *GetOrCreateSubstitution(clang::ASTContext &ctx,
                                       clang::Stmt *stmt) {
    auto sub = clang::cast<clang::ArraySubscriptExpr>(stmt);
    CHECK(sub == match) << "Substituted ArraySubscriptExpr is not the matched "
                           "ArraySubscriptExpr!";
    auto paren = clang::cast<clang::ParenExpr>(sub->getBase());
    auto addr_of = clang::cast<clang::UnaryOperator>(paren->getSubExpr());
    return addr_of->getSubExpr();
  }
};

// Matches `&base[0]` and subs it for `base`
class AddrOfArraySubscriptRule : public InferenceRule {
 public:
  AddrOfArraySubscriptRule()
      : InferenceRule(
            unaryOperator(stmt().bind("addr_of"), hasOperatorName("&"),
                          has(arraySubscriptExpr(hasIndex(zero_int_lit))))) {}

  void run(const MatchFinder::MatchResult &result) {
    match = result.Nodes.getNodeAs<clang::UnaryOperator>("addr_of");
  }

  clang::Stmt *GetOrCreateSubstitution(clang::ASTContext &ctx,
                                       clang::Stmt *stmt) {
    auto addr_of = clang::cast<clang::UnaryOperator>(stmt);
    CHECK(addr_of == match)
        << "Substituted UnaryOperator is not the matched UnaryOperator!";
    auto sub = clang::cast<clang::ArraySubscriptExpr>(addr_of->getSubExpr());
    return sub->getBase();
  }
};

// Matches `*&expr` and subs it for `expr`
class DerefAddrOfRule : public InferenceRule {
 public:
  DerefAddrOfRule()
      : InferenceRule(unaryOperator(
            stmt().bind("deref"), hasOperatorName("*"),
            has(ignoringParenCasts(unaryOperator(hasOperatorName("&")))))) {}

  void run(const MatchFinder::MatchResult &result) {
    match = result.Nodes.getNodeAs<clang::UnaryOperator>("deref");
  }

  clang::Stmt *GetOrCreateSubstitution(clang::ASTContext &ctx,
                                       clang::Stmt *stmt) {
    auto deref = clang::cast<clang::UnaryOperator>(stmt);
    CHECK(deref == match)
        << "Substituted UnaryOperator is not the matched UnaryOperator!";
    auto subexpr = deref->getSubExpr()->IgnoreParenCasts();
    auto addr_of = clang::cast<clang::UnaryOperator>(subexpr);
    return addr_of->getSubExpr();
  }
};

// Matches `!(comp)` and subs it for `negcomp`
class NegComparisonRule : public InferenceRule {
 public:
  NegComparisonRule()
      : InferenceRule(unaryOperator(
            stmt().bind("not"), hasOperatorName("!"),
            has(ignoringParenCasts(binaryOperator(stmt().bind("binop")))))) {}

  void run(const MatchFinder::MatchResult &result) {
    auto binop = result.Nodes.getNodeAs<clang::BinaryOperator>("binop");
    if (binop->isComparisonOp()) {
      match = result.Nodes.getNodeAs<clang::UnaryOperator>("not");
    }
  }

  clang::Stmt *GetOrCreateSubstitution(clang::ASTContext &ctx,
                                       clang::Stmt *stmt) {
    auto op = clang::cast<clang::UnaryOperator>(stmt);
    CHECK(op == match)
        << "Substituted UnaryOperator is not the matched UnaryOperator!";
    auto subexpr = op->getSubExpr()->IgnoreParenCasts();
    auto binop = clang::cast<clang::BinaryOperator>(subexpr);
    auto opc = clang::BinaryOperator::negateComparisonOp(binop->getOpcode());
    return CreateBinaryOperator(ctx, opc, binop->getLHS(), binop->getRHS(),
                                binop->getType());
  }
};

// Matches `(a)` and subs it for `a`
class ParenDeclRefExprStripRule : public InferenceRule {
 public:
  ParenDeclRefExprStripRule()
      : InferenceRule(
            parenExpr(stmt().bind("paren"),
                      has(ignoringImpCasts(declRefExpr(to(varDecl())))))) {}

  void run(const MatchFinder::MatchResult &result) {
    match = result.Nodes.getNodeAs<clang::ParenExpr>("paren");
  }

  clang::Stmt *GetOrCreateSubstitution(clang::ASTContext &ctx,
                                       clang::Stmt *stmt) {
    auto paren = clang::cast<clang::ParenExpr>(stmt);
    CHECK(paren == match)
        << "Substituted ParenExpr is not the matched ParenExpr!";
    return paren->getSubExpr();
  }
};

// Matches `(&expr)->field` and subs it for `expr.field`
class MemberExprAddrOfRule : public InferenceRule {
 public:
  MemberExprAddrOfRule()
      : InferenceRule(memberExpr(
            stmt().bind("arrow"), isArrow(),
            has(expr(stmt().bind("base"), ignoringParenCasts(unaryOperator(
                                              hasOperatorName("&"))))))) {}

  void run(const MatchFinder::MatchResult &result) {
    auto arrow = result.Nodes.getNodeAs<clang::MemberExpr>("arrow");
    if (result.Nodes.getNodeAs<clang::Expr>("base") == arrow->getBase()) {
      match = arrow;
    }
  }

  clang::Stmt *GetOrCreateSubstitution(clang::ASTContext &ctx,
                                       clang::Stmt *stmt) {
    auto arrow = clang::cast<clang::MemberExpr>(stmt);
    CHECK(arrow == match)
        << "Substituted MemberExpr is not the matched MemberExpr!";
    auto base = arrow->getBase()->IgnoreParenCasts();
    auto addr_of = clang::cast<clang::UnaryOperator>(base);
    return CreateMemberExpr(ctx, addr_of->getSubExpr(), arrow->getMemberDecl(),
                            arrow->getType());
  }
};

}  // namespace

char ExprCombine::ID = 0;

ExprCombine::ExprCombine(clang::ASTContext &ctx,
                         rellic::IRToASTVisitor &ast_gen)
    : ModulePass(ExprCombine::ID), ast_ctx(&ctx), ast_gen(&ast_gen) {}

bool ExprCombine::VisitParenExpr(clang::ParenExpr *paren) {
  // DLOG(INFO) << "VisitParenExpr";
  std::vector<InferenceRule *> rules({new ParenDeclRefExprStripRule});
  auto sub = ApplyFirstMatchingRule(*ast_ctx, paren, rules);
  if (sub != paren) {
    substitutions[paren] = sub;
  }

  delete rules.back();

  return true;
}

bool ExprCombine::VisitArraySubscriptExpr(clang::ArraySubscriptExpr *expr) {
  // DLOG(INFO) << "VisitArraySubscriptExpr";
  std::vector<InferenceRule *> rules({new ArraySubscriptAddrOfRule});
  auto sub = ApplyFirstMatchingRule(*ast_ctx, expr, rules);
  if (sub != expr) {
    substitutions[expr] = sub;
  }

  delete rules.back();

  return true;
}

bool ExprCombine::VisitUnaryOperator(clang::UnaryOperator *op) {
  // DLOG(INFO) << "VisitUnaryOperator";
  std::vector<InferenceRule *> rules;

  rules.push_back(new NegComparisonRule);
  rules.push_back(new DerefAddrOfRule);
  rules.push_back(new AddrOfArraySubscriptRule);

  auto sub = ApplyFirstMatchingRule(*ast_ctx, op, rules);
  if (sub != op) {
    substitutions[op] = sub;
  }

  for (auto rule : rules) {
    delete rule;
  }

  return true;
}

bool ExprCombine::VisitMemberExpr(clang::MemberExpr *expr) {
  // DLOG(INFO) << "VisitArraySubscriptExpr";
  std::vector<InferenceRule *> rules({new MemberExprAddrOfRule});
  auto sub = ApplyFirstMatchingRule(*ast_ctx, expr, rules);
  if (sub != expr) {
    substitutions[expr] = sub;
  }

  delete rules.back();

  return true;
}

bool ExprCombine::runOnModule(llvm::Module &module) {
  LOG(INFO) << "Rule-based statement simplification";
  Initialize();
  
  TraverseDecl(ast_ctx->getTranslationUnitDecl());
  return changed;
}

llvm::ModulePass *createExprCombinePass(clang::ASTContext &ctx,
                                        rellic::IRToASTVisitor &gen) {
  return new ExprCombine(ctx, gen);
}
}  // namespace rellic