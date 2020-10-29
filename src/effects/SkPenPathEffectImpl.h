// Copyright 2019 Google LLC.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.
#ifndef SkPenPathEffectImpl_DEFINED
#define SkPenPathEffectImpl_DEFINED

#include "include/core/SkPathEffect.h"
#include "include/core/SkMatrix.h"

struct SkPenPE : public SkPathEffect {
public:
    SkPenPE(const SkMatrix&);
    ~SkPenPE() override;

protected:
    SkRect onComputeFastBounds(const SkRect& src) const override;
    bool onFilterPath(SkPath*, const SkPath&, SkStrokeRec*, const SkRect*) const override;
    void flatten(SkWriteBuffer&) const override;

private:
    SK_FLATTENABLE_HOOKS(SkPenPE)

    SkMatrix fMat;

    typedef SkPathEffect INHERITED;
};
#endif  // SkPenPathEffectImpl_DEFINED
