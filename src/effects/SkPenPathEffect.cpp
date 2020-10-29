// Copyright 2019 Google LLC.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include "./SkPenPathEffectImpl.h"
#include "include/effects/SkPenPathEffect.h"
#include "include/core/SkStrokeRec.h"
#include "src/core/SkReadBuffer.h"

SkPenPE::SkPenPE(const SkMatrix& matrix) : fMat(matrix) {}

SkPenPE::~SkPenPE() = default;

SkRect SkPenPE::onComputeFastBounds(const SkRect& src) const {
    return src.makeOutset(fabsf(fMat[0]) + fabsf(fMat[1]) + fabsf(fMat[2]),
                          fabsf(fMat[3]) + fabsf(fMat[4]) + fabsf(fMat[5]));
}

bool SkPenPE::onFilterPath(SkPath* dst,
                                   const SkPath& src,
                                   SkStrokeRec* strokeRec,
                                   const SkRect* cullRect) const {
    SkASSERT(strokeRec);
    SkMatrix inverse;
    *dst = src;
    if (strokeRec->getStyle() != SkStrokeRec::kFill_Style && fMat.invert(&inverse)) {
        dst->transform(inverse);
        strokeRec->setStrokeStyle(1, strokeRec->getStyle() == SkStrokeRec::kStrokeAndFill_Style);
        strokeRec->applyToPath(dst, *dst);
        dst->transform(fMat);
        *strokeRec = SkStrokeRec(SkStrokeRec::kFill_InitStyle);
    }
    return true;
}

void SkPenPE::flatten(SkWriteBuffer& buffer) const { buffer.writeMatrix(fMat); }

sk_sp<SkFlattenable> SkPenPE::CreateProc(SkReadBuffer& buffer) {
    SkMatrix matrix;
    buffer.readMatrix(&matrix);
    return buffer.isValid() ? sk_make_sp<SkPenPE>(matrix) : nullptr;
}

sk_sp<SkPathEffect> SkPenPathEffect::Make(const SkMatrix& matrix) {
    return sk_make_sp<SkPenPE>(matrix);
}
