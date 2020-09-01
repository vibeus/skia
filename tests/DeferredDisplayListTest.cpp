/*
 * Copyright 2017 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkDeferredDisplayList.h"
#include "include/core/SkDeferredDisplayListRecorder.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPromiseImageTexture.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkSurface.h"
#include "include/core/SkSurfaceCharacterization.h"
#include "include/core/SkSurfaceProps.h"
#include "include/core/SkTypes.h"
#include "include/gpu/GrBackendSurface.h"
#include "include/gpu/GrContextThreadSafeProxy.h"
#include "include/gpu/GrDirectContext.h"
#include "include/gpu/GrRecordingContext.h"
#include "include/gpu/GrTypes.h"
#include "include/gpu/gl/GrGLTypes.h"
#include "include/private/GrTypesPriv.h"
#include "src/core/SkDeferredDisplayListPriv.h"
#include "src/gpu/GrCaps.h"
#include "src/gpu/GrContextPriv.h"
#include "src/gpu/GrGpu.h"
#include "src/gpu/GrRecordingContextPriv.h"
#include "src/gpu/GrRenderTargetContext.h"
#include "src/gpu/GrRenderTargetProxy.h"
#include "src/gpu/GrTextureProxy.h"
#include "src/gpu/SkGpuDevice.h"
#include "src/gpu/gl/GrGLDefines.h"
#include "src/image/SkImage_GpuBase.h"
#include "src/image/SkSurface_Gpu.h"
#include "tests/Test.h"
#include "tests/TestUtils.h"
#include "tools/gpu/GrContextFactory.h"

#include <initializer_list>
#include <memory>
#include <utility>

#ifdef SK_VULKAN
#include "src/gpu/vk/GrVkCaps.h"
#endif

class SurfaceParameters {
public:
    static const int kNumParams      = 12;
    static const int kSampleCount    = 5;
    static const int kMipMipCount    = 8;
    static const int kFBO0Count      = 9;
    static const int kProtectedCount = 11;

    SurfaceParameters(GrRecordingContext* rContext)
            : fBackend(rContext->backend())
            , fWidth(64)
            , fHeight(64)
            , fOrigin(kTopLeft_GrSurfaceOrigin)
            , fColorType(kRGBA_8888_SkColorType)
            , fColorSpace(SkColorSpace::MakeSRGB())
            , fSampleCount(1)
            , fSurfaceProps(0x0, kUnknown_SkPixelGeometry)
            , fShouldCreateMipMaps(true)
            , fUsesGLFBO0(false)
            , fIsTextureable(true)
            , fIsProtected(GrProtected::kNo) {
#ifdef SK_VULKAN
        if (GrBackendApi::kVulkan == rContext->backend()) {
            const GrVkCaps* vkCaps = (const GrVkCaps*) rContext->priv().caps();

            fIsProtected = GrProtected(vkCaps->supportsProtectedMemory());
        }
#endif
    }

    int sampleCount() const { return fSampleCount; }

    void setColorType(SkColorType ct) { fColorType = ct; }
    SkColorType colorType() const { return fColorType; }
    void setColorSpace(sk_sp<SkColorSpace> cs) { fColorSpace = std::move(cs); }
    void setTextureable(bool isTextureable) { fIsTextureable = isTextureable; }
    void setShouldCreateMipMaps(bool shouldCreateMipMaps) {
        fShouldCreateMipMaps = shouldCreateMipMaps;
    }

    // Modify the SurfaceParameters in just one way
    void modify(int i) {
        switch (i) {
        case 0:
            fWidth = 63;
            break;
        case 1:
            fHeight = 63;
            break;
        case 2:
            fOrigin = kBottomLeft_GrSurfaceOrigin;
            break;
        case 3:
            fColorType = kRGBA_F16_SkColorType;
            break;
        case 4:
            // This just needs to be a colorSpace different from that returned by MakeSRGB().
            // In this case we just change the gamut.
            fColorSpace = SkColorSpace::MakeRGB(SkNamedTransferFn::kSRGB, SkNamedGamut::kAdobeRGB);
            break;
        case kSampleCount:
            fSampleCount = 4;
            break;
        case 6:
            fSurfaceProps = SkSurfaceProps(0x0, kRGB_H_SkPixelGeometry);
            break;
        case 7:
            fSurfaceProps = SkSurfaceProps(SkSurfaceProps::kUseDeviceIndependentFonts_Flag,
                                           kUnknown_SkPixelGeometry);
            break;
        case 8:
            fShouldCreateMipMaps = false;
            break;
        case 9:
            if (GrBackendApi::kOpenGL == fBackend) {
                fUsesGLFBO0 = true;
                fShouldCreateMipMaps = false; // needs to changed in tandem w/ textureability
                fIsTextureable = false;
            }
            break;
        case 10:
            fShouldCreateMipMaps = false; // needs to changed in tandem w/ textureability
            fIsTextureable = false;
            break;
        case 11:
            fIsProtected = GrProtected::kYes == fIsProtected ? GrProtected::kNo
                                                             : GrProtected::kYes;
            break;
        }
    }

    SkSurfaceCharacterization createCharacterization(GrDirectContext* dContext) const {
        size_t maxResourceBytes = dContext->getResourceCacheLimit();

        if (!dContext->colorTypeSupportedAsSurface(fColorType)) {
            return SkSurfaceCharacterization();
        }

        // Note that Ganesh doesn't make use of the SkImageInfo's alphaType
        SkImageInfo ii = SkImageInfo::Make(fWidth, fHeight, fColorType,
                                           kPremul_SkAlphaType, fColorSpace);

        GrBackendFormat backendFormat = dContext->defaultBackendFormat(fColorType,
                                                                       GrRenderable::kYes);
        if (!backendFormat.isValid()) {
            return SkSurfaceCharacterization();
        }

        SkSurfaceCharacterization c = dContext->threadSafeProxy()->createCharacterization(
                                                maxResourceBytes, ii, backendFormat, fSampleCount,
                                                fOrigin, fSurfaceProps, fShouldCreateMipMaps,
                                                fUsesGLFBO0, fIsTextureable, fIsProtected);
        return c;
    }

    // Create a DDL whose characterization captures the current settings
    sk_sp<SkDeferredDisplayList> createDDL(GrDirectContext* dContext) const {
        SkSurfaceCharacterization c = this->createCharacterization(dContext);
        SkAssertResult(c.isValid());

        SkDeferredDisplayListRecorder r(c);
        SkCanvas* canvas = r.getCanvas();
        if (!canvas) {
            return nullptr;
        }

        canvas->drawRect(SkRect::MakeXYWH(10, 10, 10, 10), SkPaint());
        return r.detach();
    }

    // Create the surface with the current set of parameters
    sk_sp<SkSurface> make(GrDirectContext* dContext, GrBackendTexture* backend) const {
        const SkSurfaceCharacterization c = this->createCharacterization(dContext);

        GrMipmapped mipmapped = !fIsTextureable
                                        ? GrMipmapped::kNo
                                        : GrMipmapped(fShouldCreateMipMaps);

#ifdef SK_GL
        if (fUsesGLFBO0) {
            if (GrBackendApi::kOpenGL != dContext->backend()) {
                return nullptr;
            }

            GrGLFramebufferInfo fboInfo;
            fboInfo.fFBOID = 0;
            fboInfo.fFormat = GR_GL_RGBA8;
            static constexpr int kStencilBits = 8;
            GrBackendRenderTarget backendRT(fWidth, fHeight, 1, kStencilBits, fboInfo);

            if (!backendRT.isValid()) {
                return nullptr;
            }

            sk_sp<SkSurface> result = SkSurface::MakeFromBackendRenderTarget(dContext, backendRT,
                                                                             fOrigin, fColorType,
                                                                             fColorSpace,
                                                                             &fSurfaceProps);
            SkASSERT(result->isCompatible(c));
            return result;
        }
#endif
        CreateBackendTexture(dContext, backend, fWidth, fHeight, fColorType,
                             SkColors::kTransparent, mipmapped, GrRenderable::kYes, fIsProtected);
        if (!backend->isValid()) {
            return nullptr;
        }

        // Even if a characterization couldn't be constructed we want to soldier on to make
        // sure that surface creation will/would've also failed
        SkASSERT(!c.isValid() || c.isCompatible(*backend));

        sk_sp<SkSurface> surface;
        if (!fIsTextureable) {
            // Create a surface w/ the current parameters but make it non-textureable
            surface = SkSurface::MakeFromBackendTextureAsRenderTarget(
                                            dContext, *backend, fOrigin, fSampleCount, fColorType,
                                            fColorSpace, &fSurfaceProps);
        } else {
            surface = SkSurface::MakeFromBackendTexture(
                                            dContext, *backend, fOrigin, fSampleCount, fColorType,
                                            fColorSpace, &fSurfaceProps);
        }

        if (!surface) {
            SkASSERT(!c.isValid());
            this->cleanUpBackEnd(dContext, *backend);
            return nullptr;
        }

        SkASSERT(c.isValid());
        SkASSERT(surface->isCompatible(c));
        return surface;
    }

    void cleanUpBackEnd(GrDirectContext* dContext, const GrBackendTexture& backend) const {
        dContext->deleteBackendTexture(backend);
    }

private:
    GrBackendApi        fBackend;
    int                 fWidth;
    int                 fHeight;
    GrSurfaceOrigin     fOrigin;
    SkColorType         fColorType;
    sk_sp<SkColorSpace> fColorSpace;
    int                 fSampleCount;
    SkSurfaceProps      fSurfaceProps;
    bool                fShouldCreateMipMaps;
    bool                fUsesGLFBO0;
    bool                fIsTextureable;
    GrProtected         fIsProtected;
};

// Test out operator== && operator!=
DEF_GPUTEST_FOR_RENDERING_CONTEXTS(DDLOperatorEqTest, reporter, ctxInfo) {
    auto context = ctxInfo.directContext();

    bool mipmapSupport = context->priv().caps()->mipmapSupport();
    for (int i = 0; i < SurfaceParameters::kNumParams; ++i) {
        SurfaceParameters params1(context);
        params1.modify(i);

        SkSurfaceCharacterization char1 = params1.createCharacterization(context);
        if (!char1.isValid()) {
            continue;  // can happen on some platforms (ChromeOS)
        }

        if (SurfaceParameters::kMipMipCount == i && !mipmapSupport) {
            // If changing the mipmap setting won't result in a different surface characterization,
            // skip this step.
            continue;
        }

        for (int j = 0; j < SurfaceParameters::kNumParams; ++j) {
            SurfaceParameters params2(context);
            params2.modify(j);

            SkSurfaceCharacterization char2 = params2.createCharacterization(context);
            if (!char2.isValid()) {
                continue;  // can happen on some platforms (ChromeOS)
            }

            if (SurfaceParameters::kMipMipCount == j && !mipmapSupport) {
                // If changing the mipmap setting won't result in a different surface
                // characterization, skip this step.
                continue;
            }

            if (i == j) {
                REPORTER_ASSERT(reporter, char1 == char2);
            } else {
                REPORTER_ASSERT(reporter, char1 != char2);
            }

        }
    }

    {
        SurfaceParameters params(context);

        SkSurfaceCharacterization valid = params.createCharacterization(context);
        SkASSERT(valid.isValid());

        SkSurfaceCharacterization inval1, inval2;
        SkASSERT(!inval1.isValid() && !inval2.isValid());

        REPORTER_ASSERT(reporter, inval1 != inval2);
        REPORTER_ASSERT(reporter, valid != inval1);
        REPORTER_ASSERT(reporter, inval1 != valid);
    }
}

////////////////////////////////////////////////////////////////////////////////
// This tests SkSurfaceCharacterization/SkSurface compatibility
void DDLSurfaceCharacterizationTestImpl(GrDirectContext* dContext, skiatest::Reporter* reporter) {
    GrGpu* gpu = dContext->priv().getGpu();
    const GrCaps* caps = dContext->priv().caps();

    // Create a bitmap that we can readback into
    SkImageInfo imageInfo = SkImageInfo::Make(64, 64, kRGBA_8888_SkColorType,
                                              kPremul_SkAlphaType);
    SkBitmap bitmap;
    bitmap.allocPixels(imageInfo);

    sk_sp<SkDeferredDisplayList> ddl;

    // First, create a DDL using the stock SkSurface parameters
    {
        SurfaceParameters params(dContext);

        ddl = params.createDDL(dContext);
        SkAssertResult(ddl);

        // The DDL should draw into an SkSurface created with the same parameters
        GrBackendTexture backend;
        sk_sp<SkSurface> s = params.make(dContext, &backend);
        if (!s) {
            return;
        }

        REPORTER_ASSERT(reporter, s->draw(ddl));
        s->readPixels(imageInfo, bitmap.getPixels(), bitmap.rowBytes(), 0, 0);
        dContext->flushAndSubmit();
        gpu->testingOnly_flushGpuAndSync();
        s = nullptr;
        params.cleanUpBackEnd(dContext, backend);
    }

    // Then, alter each parameter in turn and check that the DDL & surface are incompatible
    for (int i = 0; i < SurfaceParameters::kNumParams; ++i) {
        SurfaceParameters params(dContext);
        params.modify(i);

        if (SurfaceParameters::kProtectedCount == i) {
            if (dContext->backend() != GrBackendApi::kVulkan) {
                // Only the Vulkan backend respects the protected parameter
                continue;
            }
#ifdef SK_VULKAN
            const GrVkCaps* vkCaps = (const GrVkCaps*) dContext->priv().caps();

            // And, even then, only when it is a protected context
            if (!vkCaps->supportsProtectedMemory()) {
                continue;
            }
#endif
        }

        GrBackendTexture backend;
        sk_sp<SkSurface> s = params.make(dContext, &backend);
        if (!s) {
            continue;
        }

        if (SurfaceParameters::kSampleCount == i) {
            int supportedSampleCount = caps->getRenderTargetSampleCount(
                    params.sampleCount(), backend.getBackendFormat());
            if (1 == supportedSampleCount) {
                // If changing the sample count won't result in a different
                // surface characterization, skip this step
                s = nullptr;
                params.cleanUpBackEnd(dContext, backend);
                continue;
            }
        }

        if (SurfaceParameters::kMipMipCount == i && !caps->mipmapSupport()) {
            // If changing the mipmap setting won't result in a different surface characterization,
            // skip this step
            s = nullptr;
            params.cleanUpBackEnd(dContext, backend);
            continue;
        }

        if (SurfaceParameters::kFBO0Count == i && dContext->backend() != GrBackendApi::kOpenGL) {
            // FBO0 only affects the surface characterization when using OpenGL
            s = nullptr;
            params.cleanUpBackEnd(dContext, backend);
            continue;
        }

        REPORTER_ASSERT(reporter, !s->draw(ddl),
                        "DDLSurfaceCharacterizationTest failed on parameter: %d\n", i);

        dContext->flushAndSubmit();
        gpu->testingOnly_flushGpuAndSync();
        s = nullptr;
        params.cleanUpBackEnd(dContext, backend);
    }

    // Next test the compatibility of resource cache parameters
    {
        const SurfaceParameters params(dContext);
        GrBackendTexture backend;

        sk_sp<SkSurface> s = params.make(dContext, &backend);

        size_t maxResourceBytes = dContext->getResourceCacheLimit();

        dContext->setResourceCacheLimit(maxResourceBytes/2);
        REPORTER_ASSERT(reporter, !s->draw(ddl));

        // DDL TODO: once proxies/ops can be de-instantiated we can re-enable these tests.
        // For now, DDLs are drawn once.
#if 0
        // resource limits >= those at characterization time are accepted
        context->setResourceCacheLimits(2*maxResourceCount, maxResourceBytes);
        REPORTER_ASSERT(reporter, s->draw(ddl));
        s->readPixels(imageInfo, bitmap.getPixels(), bitmap.rowBytes(), 0, 0);

        context->setResourceCacheLimits(maxResourceCount, 2*maxResourceBytes);
        REPORTER_ASSERT(reporter, s->draw(ddl));
        s->readPixels(imageInfo, bitmap.getPixels(), bitmap.rowBytes(), 0, 0);

        context->setResourceCacheLimits(maxResourceCount, maxResourceBytes);
        REPORTER_ASSERT(reporter, s->draw(ddl));
        s->readPixels(imageInfo, bitmap.getPixels(), bitmap.rowBytes(), 0, 0);
#endif

        dContext->flushAndSubmit();
        gpu->testingOnly_flushGpuAndSync();
        s = nullptr;
        params.cleanUpBackEnd(dContext, backend);
    }

    // Test that the textureability of the DDL characterization can block a DDL draw
    {
        GrBackendTexture backend;
        SurfaceParameters params(dContext);
        params.setShouldCreateMipMaps(false);
        params.setTextureable(false);

        sk_sp<SkSurface> s = params.make(dContext, &backend);
        if (s) {
            REPORTER_ASSERT(reporter, !s->draw(ddl)); // bc the DDL was made w/ textureability

            dContext->flushAndSubmit();
            gpu->testingOnly_flushGpuAndSync();
            s = nullptr;
            params.cleanUpBackEnd(dContext, backend);
        }
    }

    // Make sure non-GPU-backed surfaces fail characterization
    {
        SkImageInfo ii = SkImageInfo::MakeN32(64, 64, kOpaque_SkAlphaType);

        sk_sp<SkSurface> rasterSurface = SkSurface::MakeRaster(ii);
        SkSurfaceCharacterization c;
        REPORTER_ASSERT(reporter, !rasterSurface->characterize(&c));
    }

    // Exercise the createResized method
    {
        SurfaceParameters params(dContext);
        GrBackendTexture backend;

        sk_sp<SkSurface> s = params.make(dContext, &backend);
        if (!s) {
            return;
        }

        SkSurfaceCharacterization char0;
        SkAssertResult(s->characterize(&char0));

        // Too small
        SkSurfaceCharacterization char1 = char0.createResized(-1, -1);
        REPORTER_ASSERT(reporter, !char1.isValid());

        // Too large
        SkSurfaceCharacterization char2 = char0.createResized(1000000, 32);
        REPORTER_ASSERT(reporter, !char2.isValid());

        // Just right
        SkSurfaceCharacterization char3 = char0.createResized(32, 32);
        REPORTER_ASSERT(reporter, char3.isValid());
        REPORTER_ASSERT(reporter, 32 == char3.width());
        REPORTER_ASSERT(reporter, 32 == char3.height());

        s = nullptr;
        params.cleanUpBackEnd(dContext, backend);
    }

    // Exercise the createColorSpace method
    {
        SurfaceParameters params(dContext);
        GrBackendTexture backend;

        sk_sp<SkSurface> s = params.make(dContext, &backend);
        if (!s) {
            return;
        }

        SkSurfaceCharacterization char0;
        SkAssertResult(s->characterize(&char0));

        // The default params create an sRGB color space
        REPORTER_ASSERT(reporter, char0.colorSpace()->isSRGB());
        REPORTER_ASSERT(reporter, !char0.colorSpace()->gammaIsLinear());

        {
            sk_sp<SkColorSpace> newCS = SkColorSpace::MakeSRGBLinear();

            SkSurfaceCharacterization char1 = char0.createColorSpace(std::move(newCS));
            REPORTER_ASSERT(reporter, char1.isValid());
            REPORTER_ASSERT(reporter, !char1.colorSpace()->isSRGB());
            REPORTER_ASSERT(reporter, char1.colorSpace()->gammaIsLinear());
        }

        {
            SkSurfaceCharacterization char2 = char0.createColorSpace(nullptr);
            REPORTER_ASSERT(reporter, char2.isValid());
            REPORTER_ASSERT(reporter, !char2.colorSpace());
        }

        {
            sk_sp<SkColorSpace> newCS = SkColorSpace::MakeSRGBLinear();

            SkSurfaceCharacterization invalid;
            REPORTER_ASSERT(reporter, !invalid.isValid());
            SkSurfaceCharacterization stillInvalid = invalid.createColorSpace(std::move(newCS));
            REPORTER_ASSERT(reporter, !stillInvalid.isValid());
        }

        s = nullptr;
        params.cleanUpBackEnd(dContext, backend);
    }

    // Exercise the createBackendFormat method
    {
        SurfaceParameters params(dContext);
        GrBackendTexture backend;

        sk_sp<SkSurface> s = params.make(dContext, &backend);
        if (!s) {
            return;
        }

        SkSurfaceCharacterization char0;
        SkAssertResult(s->characterize(&char0));

        // The default params create a renderable RGBA8 surface
        auto originalBackendFormat = dContext->defaultBackendFormat(kRGBA_8888_SkColorType,
                                                                    GrRenderable::kYes);
        REPORTER_ASSERT(reporter, originalBackendFormat.isValid());
        REPORTER_ASSERT(reporter, char0.backendFormat() == originalBackendFormat);

        auto newBackendFormat = dContext->defaultBackendFormat(kRGB_565_SkColorType,
                                                               GrRenderable::kYes);

        if (newBackendFormat.isValid()) {
            SkSurfaceCharacterization char1 = char0.createBackendFormat(kRGB_565_SkColorType,
                                                                        newBackendFormat);
            REPORTER_ASSERT(reporter, char1.isValid());
            REPORTER_ASSERT(reporter, char1.backendFormat() == newBackendFormat);

            SkSurfaceCharacterization invalid;
            REPORTER_ASSERT(reporter, !invalid.isValid());
            auto stillInvalid = invalid.createBackendFormat(kRGB_565_SkColorType,
                                                            newBackendFormat);
            REPORTER_ASSERT(reporter, !stillInvalid.isValid());
        }

        s = nullptr;
        params.cleanUpBackEnd(dContext, backend);
    }

    // Exercise the createFBO0 method
    if (dContext->backend() == GrBackendApi::kOpenGL) {
        SurfaceParameters params(dContext);
        GrBackendTexture backend;

        sk_sp<SkSurface> s = params.make(dContext, &backend);
        if (!s) {
            return;
        }

        SkSurfaceCharacterization char0;
        SkAssertResult(s->characterize(&char0));

        // The default params create a non-FBO0 surface
        REPORTER_ASSERT(reporter, !char0.usesGLFBO0());

        {
            SkSurfaceCharacterization char1 = char0.createFBO0(true);
            REPORTER_ASSERT(reporter, char1.isValid());
            REPORTER_ASSERT(reporter, char1.usesGLFBO0());
        }

        {
            SkSurfaceCharacterization invalid;
            REPORTER_ASSERT(reporter, !invalid.isValid());
            SkSurfaceCharacterization stillInvalid = invalid.createFBO0(true);
            REPORTER_ASSERT(reporter, !stillInvalid.isValid());
        }

        s = nullptr;
        params.cleanUpBackEnd(dContext, backend);
    }
}

#ifdef SK_GL

// Test out the surface compatibility checks regarding FBO0-ness. This test constructs
// two parallel arrays of characterizations and surfaces in the order:
//    FBO0 w/ MSAA, FBO0 w/o MSAA, not-FBO0 w/ MSAA, not-FBO0 w/o MSAA
// and then tries all sixteen combinations to check the expected compatibility.
// Note: this is a GL-only test
DEF_GPUTEST_FOR_GL_RENDERING_CONTEXTS(CharacterizationFBO0nessTest, reporter, ctxInfo) {
    auto context = ctxInfo.directContext();
    const GrCaps* caps = context->priv().caps();
    sk_sp<GrContextThreadSafeProxy> proxy = context->threadSafeProxy();
    const size_t resourceCacheLimit = context->getResourceCacheLimit();

    GrBackendFormat format = GrBackendFormat::MakeGL(GR_GL_RGBA8, GR_GL_TEXTURE_2D);

    int availableSamples = caps->getRenderTargetSampleCount(4, format);
    if (availableSamples <= 1) {
        // This context doesn't support MSAA for RGBA8
        return;
    }

    SkImageInfo ii = SkImageInfo::Make({ 128, 128 }, kRGBA_8888_SkColorType, kPremul_SkAlphaType);

    static constexpr int kStencilBits = 8;
    static constexpr bool kNotMipMapped = false;
    static constexpr bool kNotTextureable = false;
    const SkSurfaceProps surfaceProps(0x0, kRGB_H_SkPixelGeometry);

    // Rows are characterizations and columns are surfaces
    static const bool kExpectedCompatibility[4][4] = {
                    //  FBO0 & MSAA, FBO0 & not-MSAA, not-FBO0 & MSAA, not-FBO0 & not-MSAA
/* FBO0 & MSAA     */ { true,        false,           false,           false },
/* FBO0 & not-MSAA */ { false,       true,            false,           true  },
/* not-FBO0 & MSAA */ { false,       false,           true,            false },
/* not-FBO0 & not- */ { false,       false,           false,           true  }
    };

    SkSurfaceCharacterization characterizations[4];
    sk_sp<SkSurface> surfaces[4];

    int index = 0;
    for (bool isFBO0 : { true, false }) {
        for (int numSamples : { availableSamples, 1 }) {
            characterizations[index] = proxy->createCharacterization(resourceCacheLimit,
                                                                     ii, format, numSamples,
                                                                     kTopLeft_GrSurfaceOrigin,
                                                                     surfaceProps, kNotMipMapped,
                                                                     isFBO0, kNotTextureable);
            SkASSERT(characterizations[index].sampleCount() == numSamples);
            SkASSERT(characterizations[index].usesGLFBO0() == isFBO0);

            GrGLFramebufferInfo fboInfo{ isFBO0 ? 0 : (GrGLuint) 1, GR_GL_RGBA8 };
            GrBackendRenderTarget backendRT(128, 128, numSamples, kStencilBits, fboInfo);
            SkAssertResult(backendRT.isValid());

            surfaces[index] = SkSurface::MakeFromBackendRenderTarget(context, backendRT,
                                                                     kTopLeft_GrSurfaceOrigin,
                                                                     kRGBA_8888_SkColorType,
                                                                     nullptr, &surfaceProps);
            ++index;
        }
    }

    for (int c = 0; c < 4; ++c) {
        for (int s = 0; s < 4; ++s) {
            REPORTER_ASSERT(reporter,
                            kExpectedCompatibility[c][s] ==
                                                 surfaces[s]->isCompatible(characterizations[c]));
        }
    }
}
#endif

DEF_GPUTEST_FOR_RENDERING_CONTEXTS(DDLSurfaceCharacterizationTest, reporter, ctxInfo) {
    auto context = ctxInfo.directContext();

    DDLSurfaceCharacterizationTestImpl(context, reporter);
}

// Test that a DDL created w/o textureability can be replayed into both a textureable and
// non-textureable destination. Note that DDLSurfaceCharacterizationTest tests that a
// textureable DDL cannot be played into a non-textureable destination but can be replayed
// into a textureable destination.
DEF_GPUTEST_FOR_RENDERING_CONTEXTS(DDLNonTextureabilityTest, reporter, ctxInfo) {
    auto context = ctxInfo.directContext();
    GrGpu* gpu = context->priv().getGpu();

    // Create a bitmap that we can readback into
    SkImageInfo imageInfo = SkImageInfo::Make(64, 64, kRGBA_8888_SkColorType,
                                              kPremul_SkAlphaType);
    SkBitmap bitmap;
    bitmap.allocPixels(imageInfo);

    for (bool textureability : { true, false }) {
        sk_sp<SkDeferredDisplayList> ddl;

        // First, create a DDL w/o textureability (and thus no mipmaps). TODO: once we have
        // reusable DDLs, move this outside of the loop.
        {
            SurfaceParameters params(context);
            params.setShouldCreateMipMaps(false);
            params.setTextureable(false);

            ddl = params.createDDL(context);
            SkAssertResult(ddl);
        }

        // Then verify it can draw into either flavor of destination
        SurfaceParameters params(context);
        params.setShouldCreateMipMaps(textureability);
        params.setTextureable(textureability);

        GrBackendTexture backend;
        sk_sp<SkSurface> s = params.make(context, &backend);
        if (!s) {
            continue;
        }

        REPORTER_ASSERT(reporter, s->draw(ddl));
        s->readPixels(imageInfo, bitmap.getPixels(), bitmap.rowBytes(), 0, 0);
        context->flushAndSubmit();
        gpu->testingOnly_flushGpuAndSync();
        s = nullptr;
        params.cleanUpBackEnd(context, backend);
    }

}

static void test_make_render_target(skiatest::Reporter* reporter,
                                    GrDirectContext* dContext,
                                    const SurfaceParameters& params) {
    {
        const SkSurfaceCharacterization c = params.createCharacterization(dContext);

        if (!c.isValid()) {
            GrBackendTexture backend;
            sk_sp<SkSurface> tmp = params.make(dContext, &backend);

            // If we couldn't characterize the surface we shouldn't be able to create it either
            REPORTER_ASSERT(reporter, !tmp);
            if (tmp) {
                tmp = nullptr;
                params.cleanUpBackEnd(dContext, backend);
            }
            return;
        }
    }

    const SkSurfaceCharacterization c = params.createCharacterization(dContext);
    GrBackendTexture backend;

    {
        sk_sp<SkSurface> s = params.make(dContext, &backend);
        REPORTER_ASSERT(reporter, s);
        if (!s) {
            REPORTER_ASSERT(reporter, !c.isValid());
            params.cleanUpBackEnd(dContext, backend);
            return;
        }

        REPORTER_ASSERT(reporter, c.isValid());
        REPORTER_ASSERT(reporter, c.isCompatible(backend));
        REPORTER_ASSERT(reporter, s->isCompatible(c));
        // Note that we're leaving 'backend' live here
    }

    // Make an SkSurface from scratch
    {
        sk_sp<SkSurface> s = SkSurface::MakeRenderTarget(dContext, c, SkBudgeted::kYes);
        REPORTER_ASSERT(reporter, s);
        REPORTER_ASSERT(reporter, s->isCompatible(c));
    }

    // Make an SkSurface that wraps the existing backend texture
    {
        sk_sp<SkSurface> s = SkSurface::MakeFromBackendTexture(dContext, c, backend);
        REPORTER_ASSERT(reporter, s);
        REPORTER_ASSERT(reporter, s->isCompatible(c));
    }

    params.cleanUpBackEnd(dContext, backend);
}

////////////////////////////////////////////////////////////////////////////////
// This tests the SkSurface::MakeRenderTarget variants that take an SkSurfaceCharacterization.
// In particular, the SkSurface, backendTexture and SkSurfaceCharacterization
// should always be compatible.
void DDLMakeRenderTargetTestImpl(GrDirectContext* dContext, skiatest::Reporter* reporter) {
    for (int i = 0; i < SurfaceParameters::kNumParams; ++i) {

        if (SurfaceParameters::kFBO0Count == i) {
            // MakeRenderTarget doesn't support FBO0
            continue;
        }

        if (SurfaceParameters::kProtectedCount == i) {
            if (dContext->backend() != GrBackendApi::kVulkan) {
                // Only the Vulkan backend respects the protected parameter
                continue;
            }
#ifdef SK_VULKAN
            const GrVkCaps* vkCaps = (const GrVkCaps*) dContext->priv().caps();

            // And, even then, only when it is a protected context
            if (!vkCaps->supportsProtectedMemory()) {
                continue;
            }
#endif
        }


        SurfaceParameters params(dContext);
        params.modify(i);

        if (!dContext->priv().caps()->mipmapSupport()) {
            params.setShouldCreateMipMaps(false);
        }

        test_make_render_target(reporter, dContext, params);
    }
}

DEF_GPUTEST_FOR_RENDERING_CONTEXTS(DDLMakeRenderTargetTest, reporter, ctxInfo) {
    auto context = ctxInfo.directContext();

    DDLMakeRenderTargetTestImpl(context, reporter);
}

////////////////////////////////////////////////////////////////////////////////
static constexpr int kSize = 8;

struct TextureReleaseChecker {
    TextureReleaseChecker() : fReleaseCount(0) {}
    int fReleaseCount;
    static void Release(void* self) {
        static_cast<TextureReleaseChecker*>(self)->fReleaseCount++;
    }
};

enum class DDLStage { kMakeImage, kDrawImage, kDetach, kDrawDDL };

// This tests the ability to create and use wrapped textures in a DDL world
DEF_GPUTEST_FOR_RENDERING_CONTEXTS(DDLWrapBackendTest, reporter, ctxInfo) {
    auto dContext = ctxInfo.directContext();

    GrBackendTexture backendTex;
    CreateBackendTexture(dContext, &backendTex, kSize, kSize, kRGBA_8888_SkColorType,
            SkColors::kTransparent, GrMipmapped::kNo, GrRenderable::kNo, GrProtected::kNo);
    if (!backendTex.isValid()) {
        return;
    }

    SurfaceParameters params(dContext);
    GrBackendTexture backend;

    sk_sp<SkSurface> s = params.make(dContext, &backend);
    if (!s) {
        dContext->deleteBackendTexture(backendTex);
        return;
    }

    SkSurfaceCharacterization c;
    SkAssertResult(s->characterize(&c));

    SkDeferredDisplayListRecorder recorder(c);

    SkCanvas* canvas = recorder.getCanvas();
    SkASSERT(canvas);

    auto rContext = canvas->recordingContext();
    if (!rContext) {
        s = nullptr;
        params.cleanUpBackEnd(dContext, backend);
        dContext->deleteBackendTexture(backendTex);
        return;
    }

    // Wrapped Backend Textures are not supported in DDL
    TextureReleaseChecker releaseChecker;
    sk_sp<SkImage> image =
            SkImage::MakeFromTexture(rContext, backendTex, kTopLeft_GrSurfaceOrigin,
                                     kRGBA_8888_SkColorType, kPremul_SkAlphaType, nullptr,
                                     TextureReleaseChecker::Release, &releaseChecker);
    REPORTER_ASSERT(reporter, !image);

    dContext->deleteBackendTexture(backendTex);

    s = nullptr;
    params.cleanUpBackEnd(dContext, backend);
}

static sk_sp<SkPromiseImageTexture> dummy_fulfill_proc(void*) {
    SkASSERT(0);
    return nullptr;
}
static void dummy_release_proc(void*) { SkASSERT(0); }
static void dummy_done_proc(void*) {}

////////////////////////////////////////////////////////////////////////////////
// Test out the behavior of an invalid DDLRecorder
DEF_GPUTEST_FOR_RENDERING_CONTEXTS(DDLInvalidRecorder, reporter, ctxInfo) {
    auto dContext = ctxInfo.directContext();

    {
        SkImageInfo ii = SkImageInfo::MakeN32Premul(32, 32);
        sk_sp<SkSurface> s = SkSurface::MakeRenderTarget(dContext, SkBudgeted::kNo, ii);

        SkSurfaceCharacterization characterization;
        SkAssertResult(s->characterize(&characterization));

        // never calling getCanvas means the backing surface is never allocated
        SkDeferredDisplayListRecorder recorder(characterization);
    }

    {
        SkSurfaceCharacterization invalid;

        SkDeferredDisplayListRecorder recorder(invalid);

        const SkSurfaceCharacterization c = recorder.characterization();
        REPORTER_ASSERT(reporter, !c.isValid());
        REPORTER_ASSERT(reporter, !recorder.getCanvas());
        REPORTER_ASSERT(reporter, !recorder.detach());

        GrBackendFormat format = dContext->defaultBackendFormat(kRGBA_8888_SkColorType,
                                                                GrRenderable::kNo);
        SkASSERT(format.isValid());

        sk_sp<SkImage> image = recorder.makePromiseTexture(
                format, 32, 32, GrMipmapped::kNo,
                kTopLeft_GrSurfaceOrigin,
                kRGBA_8888_SkColorType,
                kPremul_SkAlphaType, nullptr,
                dummy_fulfill_proc,
                dummy_release_proc,
                dummy_done_proc,
                nullptr,
                SkDeferredDisplayListRecorder::PromiseImageApiVersion::kNew);
        REPORTER_ASSERT(reporter, !image);
    }
}

DEF_GPUTEST_FOR_RENDERING_CONTEXTS(DDLCreateCharacterizationFailures, reporter, ctxInfo) {
    auto dContext = ctxInfo.directContext();
    size_t maxResourceBytes = dContext->getResourceCacheLimit();
    auto proxy = dContext->threadSafeProxy().get();

    auto check = [proxy, reporter, maxResourceBytes](const GrBackendFormat& backendFormat,
                                                     int width, int height,
                                                     SkColorType ct, bool willUseGLFBO0,
                                                     GrProtected prot) {
        const SkSurfaceProps surfaceProps(0x0, kRGB_H_SkPixelGeometry);

        SkImageInfo ii = SkImageInfo::Make(width, height, ct,
                                           kPremul_SkAlphaType, nullptr);

        SkSurfaceCharacterization c = proxy->createCharacterization(
                                                maxResourceBytes, ii, backendFormat, 1,
                                                kBottomLeft_GrSurfaceOrigin, surfaceProps, false,
                                                willUseGLFBO0, true, prot);
        REPORTER_ASSERT(reporter, !c.isValid());
    };

    GrBackendFormat goodBackendFormat = dContext->defaultBackendFormat(kRGBA_8888_SkColorType,
                                                                       GrRenderable::kYes);
    SkASSERT(goodBackendFormat.isValid());

    GrBackendFormat badBackendFormat;
    SkASSERT(!badBackendFormat.isValid());

    SkColorType kGoodCT = kRGBA_8888_SkColorType;
    SkColorType kBadCT = kUnknown_SkColorType;

    static const bool kGoodUseFBO0 = false;
    static const bool kBadUseFBO0 = true;

    int goodWidth = 64;
    int goodHeight = 64;
    int badWidths[] = { 0, 1048576 };
    int badHeights[] = { 0, 1048576 };

    check(goodBackendFormat, goodWidth, badHeights[0], kGoodCT, kGoodUseFBO0, GrProtected::kNo);
    check(goodBackendFormat, goodWidth, badHeights[1], kGoodCT, kGoodUseFBO0, GrProtected::kNo);
    check(goodBackendFormat, badWidths[0], goodHeight, kGoodCT, kGoodUseFBO0, GrProtected::kNo);
    check(goodBackendFormat, badWidths[1], goodHeight, kGoodCT, kGoodUseFBO0, GrProtected::kNo);
    check(badBackendFormat, goodWidth, goodHeight, kGoodCT, kGoodUseFBO0, GrProtected::kNo);
    check(goodBackendFormat, goodWidth, goodHeight, kBadCT, kGoodUseFBO0, GrProtected::kNo);
    check(goodBackendFormat, goodWidth, goodHeight, kGoodCT, kBadUseFBO0, GrProtected::kNo);
    if (dContext->backend() == GrBackendApi::kVulkan) {
        check(goodBackendFormat, goodWidth, goodHeight, kGoodCT, kGoodUseFBO0, GrProtected::kYes);
    }
}

////////////////////////////////////////////////////////////////////////////////
// Ensure that flushing while DDL recording doesn't cause a crash
DEF_GPUTEST_FOR_RENDERING_CONTEXTS(DDLFlushWhileRecording, reporter, ctxInfo) {
    auto direct = ctxInfo.directContext();

    SkImageInfo ii = SkImageInfo::MakeN32Premul(32, 32);
    sk_sp<SkSurface> s = SkSurface::MakeRenderTarget(direct, SkBudgeted::kNo, ii);

    SkSurfaceCharacterization characterization;
    SkAssertResult(s->characterize(&characterization));

    SkDeferredDisplayListRecorder recorder(characterization);
    SkCanvas* canvas = recorder.getCanvas();

    // CONTEXT TODO: once getGrContext goes away this test should be deleted since this
    // situation won't be possible.
    canvas->getGrContext()->flushAndSubmit();
}

////////////////////////////////////////////////////////////////////////////////
// Test that flushing a DDL via SkSurface::flush works

struct FulfillInfo {
    sk_sp<SkPromiseImageTexture> fTex;
    bool fFulfilled = false;
    bool fReleased  = false;
    bool fDone      = false;
};

static sk_sp<SkPromiseImageTexture> tracking_fulfill_proc(void* context) {
    FulfillInfo* info = (FulfillInfo*) context;
    info->fFulfilled = true;
    return info->fTex;
}

static void tracking_release_proc(void* context) {
    FulfillInfo* info = (FulfillInfo*) context;
    info->fReleased = true;
}

static void tracking_done_proc(void* context) {
    FulfillInfo* info = (FulfillInfo*) context;
    info->fDone = true;
}

DEF_GPUTEST_FOR_RENDERING_CONTEXTS(DDLSkSurfaceFlush, reporter, ctxInfo) {
    auto context = ctxInfo.directContext();

    SkImageInfo ii = SkImageInfo::Make(32, 32, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    sk_sp<SkSurface> s = SkSurface::MakeRenderTarget(context, SkBudgeted::kNo, ii);

    SkSurfaceCharacterization characterization;
    SkAssertResult(s->characterize(&characterization));

    GrBackendTexture backendTexture;

    if (!CreateBackendTexture(context, &backendTexture, ii, SkColors::kCyan, GrMipmapped::kNo,
                              GrRenderable::kNo)) {
        REPORTER_ASSERT(reporter, false);
        return;
    }

    FulfillInfo fulfillInfo;
    fulfillInfo.fTex = SkPromiseImageTexture::Make(backendTexture);

    sk_sp<SkDeferredDisplayList> ddl;

    {
        SkDeferredDisplayListRecorder recorder(characterization);

        GrBackendFormat format = context->defaultBackendFormat(kRGBA_8888_SkColorType,
                                                               GrRenderable::kNo);
        SkASSERT(format.isValid());

        sk_sp<SkImage> promiseImage = recorder.makePromiseTexture(
                format, 32, 32, GrMipmapped::kNo,
                kTopLeft_GrSurfaceOrigin,
                kRGBA_8888_SkColorType,
                kPremul_SkAlphaType, nullptr,
                tracking_fulfill_proc,
                tracking_release_proc,
                tracking_done_proc,
                &fulfillInfo,
                SkDeferredDisplayListRecorder::PromiseImageApiVersion::kNew);

        SkCanvas* canvas = recorder.getCanvas();

        canvas->clear(SK_ColorRED);
        canvas->drawImage(promiseImage, 0, 0);
        ddl = recorder.detach();
    }

    context->flushAndSubmit();

    s->draw(ddl);

    GrFlushInfo flushInfo;
    s->flush(SkSurface::BackendSurfaceAccess::kPresent, flushInfo);
    context->submit();

    REPORTER_ASSERT(reporter, fulfillInfo.fFulfilled);
    REPORTER_ASSERT(reporter, fulfillInfo.fReleased);

    if (GrBackendApi::kVulkan == context->backend() ||
        GrBackendApi::kMetal  == context->backend()) {
        // In order to receive the done callback with Vulkan we need to perform the equivalent
        // of a glFinish
        s->flush();
        context->submit(true);
    }

    REPORTER_ASSERT(reporter, fulfillInfo.fDone);

    REPORTER_ASSERT(reporter, fulfillInfo.fTex->unique());
    fulfillInfo.fTex.reset();

    DeleteBackendTexture(context, backendTexture);
}

////////////////////////////////////////////////////////////////////////////////
// Ensure that reusing a single DDLRecorder to create multiple DDLs works cleanly
DEF_GPUTEST_FOR_RENDERING_CONTEXTS(DDLMultipleDDLs, reporter, ctxInfo) {
    auto context = ctxInfo.directContext();

    SkImageInfo ii = SkImageInfo::MakeN32Premul(32, 32);
    sk_sp<SkSurface> s = SkSurface::MakeRenderTarget(context, SkBudgeted::kNo, ii);

    SkBitmap bitmap;
    bitmap.allocPixels(ii);

    SkSurfaceCharacterization characterization;
    SkAssertResult(s->characterize(&characterization));

    SkDeferredDisplayListRecorder recorder(characterization);

    SkCanvas* canvas1 = recorder.getCanvas();

    canvas1->clear(SK_ColorRED);

    canvas1->save();
    canvas1->clipRect(SkRect::MakeXYWH(8, 8, 16, 16));

    sk_sp<SkDeferredDisplayList> ddl1 = recorder.detach();

    SkCanvas* canvas2 = recorder.getCanvas();

    SkPaint p;
    p.setColor(SK_ColorGREEN);
    canvas2->drawRect(SkRect::MakeWH(32, 32), p);

    sk_sp<SkDeferredDisplayList> ddl2 = recorder.detach();

    REPORTER_ASSERT(reporter, ddl1->priv().lazyProxyData());
    REPORTER_ASSERT(reporter, ddl2->priv().lazyProxyData());

    // The lazy proxy data being different ensures that the SkSurface, SkCanvas and backing-
    // lazy proxy are all different between the two DDLs
    REPORTER_ASSERT(reporter, ddl1->priv().lazyProxyData() != ddl2->priv().lazyProxyData());

    s->draw(ddl1);
    s->draw(ddl2);

    // Make sure the clipRect from DDL1 didn't percolate into DDL2
    s->readPixels(ii, bitmap.getPixels(), bitmap.rowBytes(), 0, 0);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            REPORTER_ASSERT(reporter, bitmap.getColor(x, y) == SK_ColorGREEN);
            if (bitmap.getColor(x, y) != SK_ColorGREEN) {
                return; // we only really need to report the error once
            }
        }
    }
}

#ifdef SK_GL
////////////////////////////////////////////////////////////////////////////////
// Check that the texture-specific flags (i.e., for external & rectangle textures) work
// for promise images. As such, this is a GL-only test.
DEF_GPUTEST_FOR_GL_RENDERING_CONTEXTS(DDLTextureFlagsTest, reporter, ctxInfo) {
    auto context = ctxInfo.directContext();

    SkImageInfo ii = SkImageInfo::MakeN32Premul(32, 32);
    sk_sp<SkSurface> s = SkSurface::MakeRenderTarget(context, SkBudgeted::kNo, ii);

    SkSurfaceCharacterization characterization;
    SkAssertResult(s->characterize(&characterization));

    SkDeferredDisplayListRecorder recorder(characterization);

    for (GrGLenum target : { GR_GL_TEXTURE_EXTERNAL, GR_GL_TEXTURE_RECTANGLE, GR_GL_TEXTURE_2D } ) {
        for (auto mipMapped : { GrMipmapped::kNo, GrMipmapped::kYes }) {
            GrBackendFormat format = GrBackendFormat::MakeGL(GR_GL_RGBA8, target);

            sk_sp<SkImage> image = recorder.makePromiseTexture(
                    format, 32, 32, mipMapped,
                    kTopLeft_GrSurfaceOrigin,
                    kRGBA_8888_SkColorType,
                    kPremul_SkAlphaType, nullptr,
                    dummy_fulfill_proc,
                    dummy_release_proc,
                    dummy_done_proc,
                    nullptr,
                    SkDeferredDisplayListRecorder::PromiseImageApiVersion::kNew);
            if (GR_GL_TEXTURE_2D != target && mipMapped == GrMipmapped::kYes) {
                REPORTER_ASSERT(reporter, !image);
                continue;
            }
            REPORTER_ASSERT(reporter, image);

            GrTextureProxy* backingProxy = ((SkImage_GpuBase*) image.get())->peekProxy();

            REPORTER_ASSERT(reporter, backingProxy->mipmapped() == mipMapped);
            if (GR_GL_TEXTURE_2D == target) {
                REPORTER_ASSERT(reporter, !backingProxy->hasRestrictedSampling());
            } else {
                REPORTER_ASSERT(reporter, backingProxy->hasRestrictedSampling());
            }
        }
    }
}
#endif  // SK_GL

////////////////////////////////////////////////////////////////////////////////
// Test colorType and pixelConfig compatibility.
DEF_GPUTEST_FOR_GL_RENDERING_CONTEXTS(DDLCompatibilityTest, reporter, ctxInfo) {
    auto context = ctxInfo.directContext();

    for (int ct = 0; ct <= kLastEnum_SkColorType; ++ct) {
        SkColorType colorType = static_cast<SkColorType>(ct);

        SurfaceParameters params(context);
        params.setColorType(colorType);
        params.setColorSpace(nullptr);

        if (!context->priv().caps()->mipmapSupport()) {
            params.setShouldCreateMipMaps(false);
        }

        test_make_render_target(reporter, context, params);
    }

}
