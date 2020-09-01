/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef SKSL_WHILESTATEMENT
#define SKSL_WHILESTATEMENT

#include "src/sksl/ir/SkSLExpression.h"
#include "src/sksl/ir/SkSLStatement.h"

namespace SkSL {

/**
 * A 'while' loop.
 */
struct WhileStatement : public Statement {
    static constexpr Kind kStatementKind = kWhile_Kind;

    WhileStatement(int offset, std::unique_ptr<Expression> test,
                   std::unique_ptr<Statement> statement)
    : INHERITED(offset, kStatementKind)
    , fTest(std::move(test))
    , fStatement(std::move(statement)) {}

    int nodeCount() const override {
        return 1 + fTest->nodeCount() + fStatement->nodeCount();
    }

    std::unique_ptr<Statement> clone() const override {
        return std::unique_ptr<Statement>(new WhileStatement(fOffset, fTest->clone(),
                                                             fStatement->clone()));
    }

    String description() const override {
        return "while (" + fTest->description() + ") " + fStatement->description();
    }

    std::unique_ptr<Expression> fTest;
    std::unique_ptr<Statement> fStatement;

    typedef Statement INHERITED;
};

}  // namespace SkSL

#endif
