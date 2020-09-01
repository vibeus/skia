/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef SKSL_BOOLLITERAL
#define SKSL_BOOLLITERAL

#include "src/sksl/SkSLContext.h"
#include "src/sksl/ir/SkSLExpression.h"

namespace SkSL {

/**
 * Represents 'true' or 'false'.
 */
struct BoolLiteral : public Expression {
    static constexpr Kind kExpressionKind = kBoolLiteral_Kind;

    BoolLiteral(const Context& context, int offset, bool value)
    : INHERITED(offset, kExpressionKind, *context.fBool_Type)
    , fValue(value) {}

    String description() const override {
        return String(fValue ? "true" : "false");
    }

    bool hasProperty(Property property) const override {
        return false;
    }

    bool isCompileTimeConstant() const override {
        return true;
    }

    bool compareConstant(const Context& context, const Expression& other) const override {
        BoolLiteral& b = (BoolLiteral&) other;
        return fValue == b.fValue;
    }

    int nodeCount() const override {
        return 1;
    }

    std::unique_ptr<Expression> clone() const override {
        return std::unique_ptr<Expression>(new BoolLiteral(fOffset, fValue, &fType));
    }

    const bool fValue;

    typedef Expression INHERITED;

private:
    BoolLiteral(int offset, bool value, const Type* type)
    : INHERITED(offset, kExpressionKind, *type)
    , fValue(value) {}
};

}  // namespace SkSL

#endif
