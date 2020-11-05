// Copyright 2019 Google LLC.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.
#ifndef SkPenPathEffect_DEFINED
#define SkPenPathEffect_DEFINED

#include "include/core/SkPathEffect.h"

class SkMatrix;

class SK_API SkPenPathEffect {
public:
    /** return a path effect that strokes with an elipse specified by matrix
    `SkMakePenPathEffect(SkMatrix::MakeScale(W))` strokes with a width of
    `W`.  Ignores SkPaint StrokeWidth parameter.  Has no effect if SkPaint
    style is FillStyle.  Has no effect if matrix is not invertable.

    matrix translate and perspective probably don't do what you expect them to
    do.
    */
    static sk_sp<SkPathEffect> Make(const SkMatrix& matrix);
};

#endif  // SkPenPathEffect_DEFINED
