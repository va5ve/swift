//===--- PropertyUnification.cpp - Rules added w/ building property map ---===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "PropertyMap.h"
#include "RewriteSystem.h"

using namespace swift;
using namespace rewriting;

/// Simplify terms appearing in the substitutions of the last symbol of \p term,
/// which must be a superclass or concrete type symbol.
bool RewriteSystem::simplifySubstitutions(Symbol &symbol,
                                          RewritePath *path) const {
  assert(symbol.hasSubstitutions());

  // Fast path if the type is fully concrete.
  auto substitutions = symbol.getSubstitutions();
  if (substitutions.empty())
    return false;

  // Save the original rewrite path length so that we can reset if if we don't
  // find anything to simplify.
  unsigned oldSize = (path ? path->size() : 0);

  if (path) {
    // The term is at the top of the primary stack. Push all substitutions onto
    // the primary stack.
    path->add(RewriteStep::forDecompose(substitutions.size(),
                                        /*inverse=*/false));

    // Move all substitutions but the first one to the secondary stack.
    for (unsigned i = 1; i < substitutions.size(); ++i)
      path->add(RewriteStep::forShift(/*inverse=*/false));
  }

  // Simplify and collect substitutions.
  SmallVector<Term, 2> newSubstitutions;
  newSubstitutions.reserve(substitutions.size());

  bool first = true;
  bool anyChanged = false;
  for (auto substitution : substitutions) {
    // Move the next substitution from the secondary stack to the primary stack.
    if (!first && path)
      path->add(RewriteStep::forShift(/*inverse=*/true));
    first = false;

    // The current substitution is at the top of the primary stack; simplify it.
    MutableTerm mutTerm(substitution);
    anyChanged |= simplify(mutTerm, path);

    // Record the new substitution.
    newSubstitutions.push_back(Term::get(mutTerm, Context));
  }

  // All simplified substitutions are now on the primary stack. Collect them to
  // produce the new term.
  if (path) {
    path->add(RewriteStep::forDecompose(substitutions.size(),
                                        /*inverse=*/true));
  }

  // If nothing changed, we don't have to rebuild the symbol.
  if (!anyChanged) {
    if (path) {
      // The rewrite path should consist of a Decompose, followed by a number
      // of Shifts, followed by a Compose.
  #ifndef NDEBUG
      for (auto iter = path->begin() + oldSize; iter < path->end(); ++iter) {
        assert(iter->Kind == RewriteStep::Shift ||
               iter->Kind == RewriteStep::Decompose);
      }
  #endif

      path->resize(oldSize);
    }
    return false;
  }

  // Build the new symbol with simplified substitutions.
  symbol = symbol.withConcreteSubstitutions(newSubstitutions, Context);
  return true;
}

/// Simplify substitution terms in superclass, concrete type and concrete
/// conformance symbols.
void RewriteSystem::simplifyLeftHandSideSubstitutions() {
  for (unsigned ruleID = 0, e = Rules.size(); ruleID < e; ++ruleID) {
    auto &rule = getRule(ruleID);
    if (rule.isSubstitutionSimplified())
      continue;

    auto lhs = rule.getLHS();
    auto symbol = lhs.back();
    if (!symbol.hasSubstitutions())
      continue;

    RewritePath path;

    // (1) First, apply the original rule to produce the original lhs.
    path.add(RewriteStep::forRewriteRule(/*startOffset=*/0, /*endOffset=*/0,
                                         ruleID, /*inverse=*/true));

    // (2) Now, simplify the substitutions to get the new lhs.
    if (!simplifySubstitutions(symbol, &path))
      continue;

    // We're either going to add a new rule or record an identity, so
    // mark the old rule as simplified.
    rule.markSubstitutionSimplified();

    MutableTerm newLHS(lhs.begin(), lhs.end() - 1);
    newLHS.add(symbol);

    // Invert the path to get a path from the new lhs to the old rhs.
    path.invert();

    addRule(newLHS, MutableTerm(rule.getRHS()), &path);
  }
}

/// Similar to RewriteSystem::simplifySubstitutions(), but also replaces type
/// parameters with concrete types and builds a type difference describing
/// the transformation.
///
/// Returns None if the concrete type symbol cannot be simplified further.
///
/// Otherwise returns an index which can be passed to
/// RewriteSystem::getTypeDifference().
Optional<unsigned>
PropertyMap::concretelySimplifySubstitutions(Term baseTerm, Symbol symbol,
                                             RewritePath *path) const {
  assert(symbol.hasSubstitutions());

  // Fast path if the type is fully concrete.
  auto substitutions = symbol.getSubstitutions();
  if (substitutions.empty())
    return None;

  // Save the original rewrite path length so that we can reset if if we don't
  // find anything to simplify.
  unsigned oldSize = (path ? path->size() : 0);

  if (path) {
    // The term is at the top of the primary stack. Push all substitutions onto
    // the primary stack.
    path->add(RewriteStep::forDecompose(substitutions.size(),
                                        /*inverse=*/false));

    // Move all substitutions but the first one to the secondary stack.
    for (unsigned i = 1; i < substitutions.size(); ++i)
      path->add(RewriteStep::forShift(/*inverse=*/false));
  }

  // Simplify and collect substitutions.
  llvm::SmallVector<std::pair<unsigned, Term>, 1> sameTypes;
  llvm::SmallVector<std::pair<unsigned, Symbol>, 1> concreteTypes;

  for (unsigned index : indices(substitutions)) {
    // Move the next substitution from the secondary stack to the primary stack.
    if (index != 0 && path)
      path->add(RewriteStep::forShift(/*inverse=*/true));

    auto term = symbol.getSubstitutions()[index];
    MutableTerm mutTerm(term);

    // Note that it's of course possible that the term both requires
    // simplification, and the simplified term has a concrete type.
    //
    // This isn't handled with our current representation of
    // TypeDifference, but that should be fine since the caller
    // has to iterate until fixed point anyway.
    //
    // This should be rare in practice.
    if (System.simplify(mutTerm, path)) {
      // Record a mapping from this substitution to the simplified term.
      sameTypes.emplace_back(index, Term::get(mutTerm, Context));
    } else {
      auto *props = lookUpProperties(mutTerm);

      if (props && props->ConcreteType) {
        // The property map entry might apply to a suffix of the substitution
        // term, so prepend the appropriate prefix to its own substitutions.
        auto prefix = props->getPrefixAfterStrippingKey(mutTerm);
        auto concreteSymbol =
          props->ConcreteType->prependPrefixToConcreteSubstitutions(
              prefix, Context);

        // Record a mapping from this substitution to the concrete type.
        concreteTypes.emplace_back(index, concreteSymbol);

        // If U.V is the substitution term and V is the property map key,
        // apply the rewrite step U.(V => V.[concrete: C]) followed by
        // prepending the prefix U to each substitution in the concrete type
        // symbol if |U| > 0.
        if (path) {
          path->add(RewriteStep::forRewriteRule(/*startOffset=*/prefix.size(),
                                                /*endOffset=*/0,
                                                /*ruleID=*/*props->ConcreteTypeRule,
                                                /*inverse=*/true));

          if (!prefix.empty()) {
            path->add(RewriteStep::forPrefixSubstitutions(/*length=*/prefix.size(),
                                                          /*endOffset=*/0,
                                                          /*inverse=*/false));
          }
        }
      }
    }
  }

  // If nothing changed, we don't have to build the type difference.
  if (sameTypes.empty() && concreteTypes.empty()) {
    if (path) {
      // The rewrite path should consist of a Decompose, followed by a number
      // of Shifts, followed by a Compose.
  #ifndef NDEBUG
      for (auto iter = path->begin() + oldSize; iter < path->end(); ++iter) {
        assert(iter->Kind == RewriteStep::Shift ||
               iter->Kind == RewriteStep::Decompose);
      }
  #endif

      path->resize(oldSize);
    }
    return None;
  }

  auto difference = buildTypeDifference(baseTerm, symbol,
                                        sameTypes, concreteTypes,
                                        Context);
  assert(difference.LHS != difference.RHS);

  unsigned differenceID = System.recordTypeDifference(difference);

  // All simplified substitutions are now on the primary stack. Collect them to
  // produce the new term.
  if (path) {
    path->add(RewriteStep::forDecomposeConcrete(differenceID,
                                                /*inverse=*/true));
  }

  return differenceID;
}

void PropertyMap::concretelySimplifyLeftHandSideSubstitutions() const {
  for (unsigned ruleID = 0, e = System.getRules().size(); ruleID < e; ++ruleID) {
    auto &rule = System.getRule(ruleID);
    if (rule.isLHSSimplified() ||
        rule.isRHSSimplified() ||
        rule.isSubstitutionSimplified())
      continue;

    auto optSymbol = rule.isPropertyRule();
    if (!optSymbol || !optSymbol->hasSubstitutions())
      continue;

    auto symbol = *optSymbol;

    RewritePath path;

    auto differenceID = concretelySimplifySubstitutions(
        rule.getRHS(), symbol, &path);
    if (!differenceID)
      continue;

    rule.markSubstitutionSimplified();

    auto difference = System.getTypeDifference(*differenceID);
    assert(difference.LHS == symbol);

    // If the original rule is (T.[concrete: C] => T) and [concrete: C'] is
    // the simplified symbol, then difference.LHS == [concrete: C] and
    // difference.RHS == [concrete: C'], and the rewrite path we just
    // built takes T.[concrete: C] to T.[concrete: C'].
    //
    // We want a path from T.[concrete: C'] to T, so invert the path to get
    // a path from T.[concrete: C'] to T.[concrete: C], and add a final step
    // applying the original rule (T.[concrete: C] => T).
    path.invert();
    path.add(RewriteStep::forRewriteRule(/*startOffset=*/0,
                                         /*endOffset=*/0,
                                         /*ruleID=*/ruleID,
                                         /*inverted=*/false));
    MutableTerm rhs(rule.getRHS());
    MutableTerm lhs(rhs);
    lhs.add(difference.RHS);

    System.addRule(lhs, rhs, &path);
  }
}