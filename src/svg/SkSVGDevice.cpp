/*
 * Copyright 2015 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/svg/SkSVGDevice.h"

#include "include/core/SkBitmap.h"
#include "include/core/SkBlendMode.h"
#include "include/core/SkClipOp.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkData.h"
#include "include/core/SkDataTable.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPathEffect.h"
#include "include/core/SkPathTypes.h"
#include "include/core/SkPathUtils.h"
#include "include/core/SkPoint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"
#include "include/core/SkScalar.h"
#include "include/core/SkShader.h"
#include "include/core/SkSize.h"
#include "include/core/SkSpan.h"
#include "include/core/SkStream.h"
#include "include/core/SkString.h"
#include "include/core/SkSurfaceProps.h"
#include "include/core/SkTileMode.h"
#include "include/core/SkTypeface.h"
#include "include/encode/SkPngEncoder.h"
#include "include/private/base/SkDebug.h"
#include "include/private/base/SkNoncopyable.h"
#include "include/private/base/SkTPin.h"
#include "include/private/base/SkTemplates.h"
#include "include/private/base/SkTo.h"
#include "include/svg/SkSVGCanvas.h"
#include "src/base/SkBase64.h"
#include "src/base/SkTLazy.h"
#include "src/core/SkAnnotationKeys.h"
#include "src/core/SkClipStack.h"
#include "src/core/SkDevice.h"
#include "src/core/SkFontPriv.h"
#include "src/core/SkTHash.h"
#include "src/image/SkImage_Base.h"
#include "src/shaders/SkColorShader.h"
#include "src/shaders/SkShaderBase.h"
#include "src/text/GlyphRun.h"
#include "src/xml/SkXMLWriter.h"

#include <cstring>
#include <memory>
#include <string>
#include <utility>

using namespace skia_private;

class SkBlender;
class SkMesh;
class SkVertices;
struct SkSamplingOptions;

namespace {

static SkString svg_color(SkColor color) {
    // https://www.w3.org/TR/css-color-3/#html4
    auto named_color = [](SkColor c) -> const char* {
        switch (c & 0xffffff) {
        case 0x000000: return "black";
        case 0x000080: return "navy";
        case 0x0000ff: return "blue";
        case 0x008000: return "green";
        case 0x008080: return "teal";
        case 0x00ff00: return "lime";
        case 0x00ffff: return "aqua";
        case 0x800000: return "maroon";
        case 0x800080: return "purple";
        case 0x808000: return "olive";
        case 0x808080: return "gray";
        case 0xc0c0c0: return "silver";
        case 0xff0000: return "red";
        case 0xff00ff: return "fuchsia";
        case 0xffff00: return "yellow";
        case 0xffffff: return "white";
        default: break;
        }

        return nullptr;
    };

    if (const auto* nc = named_color(color)) {
        return SkString(nc);
    }

    uint8_t r = SkColorGetR(color);
    uint8_t g = SkColorGetG(color);
    uint8_t b = SkColorGetB(color);

    // Some users care about every byte here, so we'll use hex colors with single-digit channels
    // when possible.
    uint8_t rh = r >> 4;
    uint8_t rl = r & 0xf;
    uint8_t gh = g >> 4;
    uint8_t gl = g & 0xf;
    uint8_t bh = b >> 4;
    uint8_t bl = b & 0xf;
    if ((rh == rl) && (gh == gl) && (bh == bl)) {
        return SkStringPrintf("#%1X%1X%1X", rh, gh, bh);
    }

    return SkStringPrintf("#%02X%02X%02X", r, g, b);
}

static SkScalar svg_opacity(SkColor color) {
    return SkIntToScalar(SkColorGetA(color)) / SK_AlphaOPAQUE;
}

// Keep in sync with SkPaint::Cap
static const char* cap_map[]  = {
    nullptr,    // kButt_Cap (default)
    "round", // kRound_Cap
    "square" // kSquare_Cap
};
static_assert(std::size(cap_map) == SkPaint::kCapCount, "missing_cap_map_entry");

static const char* svg_cap(SkPaint::Cap cap) {
    SkASSERT(static_cast<size_t>(cap) < std::size(cap_map));
    return cap_map[cap];
}

// Keep in sync with SkPaint::Join
static const char* join_map[] = {
    nullptr,    // kMiter_Join (default)
    "round", // kRound_Join
    "bevel"  // kBevel_Join
};
static_assert(std::size(join_map) == SkPaint::kJoinCount, "missing_join_map_entry");

static const char* svg_join(SkPaint::Join join) {
    SkASSERT(join < std::size(join_map));
    return join_map[join];
}

static SkString svg_transform(const SkMatrix& t) {
    SkASSERT(!t.isIdentity());

    SkString tstr;
    switch (t.getType()) {
    case SkMatrix::kPerspective_Mask:
        // TODO: handle perspective matrices?
        break;
    case SkMatrix::kTranslate_Mask:
        tstr.printf("translate(%g %g)", t.getTranslateX(), t.getTranslateY());
        break;
    case SkMatrix::kScale_Mask:
        tstr.printf("scale(%g %g)", t.getScaleX(), t.getScaleY());
        break;
    default:
        // http://www.w3.org/TR/SVG/coords.html#TransformMatrixDefined
        //    | a c e |
        //    | b d f |
        //    | 0 0 1 |
        tstr.printf("matrix(%g %g %g %g %g %g)",
                    t.getScaleX(),     t.getSkewY(),
                    t.getSkewX(),      t.getScaleY(),
                    t.getTranslateX(), t.getTranslateY());
        break;
    }

    return tstr;
}

struct Resources {
    Resources(const SkPaint& paint)
        : fPaintServer(svg_color(paint.getColor())) {}

    SkString fPaintServer;
    SkString fColorFilter;
};

// Determine if the paint requires us to reset the viewport.
// Currently, we do this whenever the paint shader calls
// for a repeating image.
bool RequiresViewportReset(const SkPaint& paint) {
  SkShader* shader = paint.getShader();
  if (!shader)
    return false;

  SkTileMode xy[2];
  SkImage* image = shader->isAImage(nullptr, xy);

  if (!image)
    return false;

  for (int i = 0; i < 2; i++) {
    if (xy[i] == SkTileMode::kRepeat)
      return true;
  }
  return false;
}

void AddPath(const sktext::GlyphRun& glyphRun, const SkPoint& offset, SkPathBuilder* builder) {
    struct Rec {
        SkPathBuilder* fBuilder;
        const SkPoint  fOffset;
        const SkPoint* fPos;
    } rec = { builder, offset, glyphRun.positions().data() };

    glyphRun.font().getPaths(glyphRun.glyphsIDs(),
            [](const SkPath* path, const SkMatrix& mx, void* ctx) {
                Rec* rec = reinterpret_cast<Rec*>(ctx);
                if (path) {
                    SkMatrix total = mx;
                    total.postTranslate(rec->fPos->fX + rec->fOffset.fX,
                                        rec->fPos->fY + rec->fOffset.fY);
                    rec->fBuilder->addPath(*path, total);
                } else {
                    // TODO: this is going to drop color emojis.
                }
                rec->fPos += 1; // move to the next glyph's position
            }, &rec);
}

}  // namespace

// For now all this does is serve unique serial IDs, but it will eventually evolve to track
// and deduplicate resources.
class SkSVGDevice::ResourceBucket : ::SkNoncopyable {
public:
    ResourceBucket()
            : fGradientCount(0)
            , fPathCount(0)
            , fImageCount(0)
            , fPatternCount(0)
            , fColorFilterCount(0) {}

    SkString addGradient() {
        return SkStringPrintf("gradient_%u", fGradientCount++);
    }

    SkString addPath() {
        return SkStringPrintf("path_%u", fPathCount++);
    }

    SkString addImage() {
        return SkStringPrintf("img_%u", fImageCount++);
    }

    SkString addColorFilter() { return SkStringPrintf("cfilter_%u", fColorFilterCount++); }

    SkString addPattern() {
      return SkStringPrintf("pattern_%u", fPatternCount++);
    }

private:
    uint32_t fGradientCount;
    uint32_t fPathCount;
    uint32_t fImageCount;
    uint32_t fPatternCount;
    uint32_t fColorFilterCount;
};

struct SkSVGDevice::MxCp {
    const SkMatrix* fMatrix;
    const SkClipStack*  fClipStack;

    MxCp(const SkMatrix* mx, const SkClipStack* cs) : fMatrix(mx), fClipStack(cs) {}
    MxCp(SkSVGDevice* device) : fMatrix(&device->localToDevice()), fClipStack(&device->cs()) {}
};

class SkSVGDevice::AutoElement : ::SkNoncopyable {
public:
    AutoElement(const char name[], SkXMLWriter* writer)
        : fWriter(writer)
        , fResourceBucket(nullptr) {
        fWriter->startElement(name);
    }

    AutoElement(const char name[], const std::unique_ptr<SkXMLWriter>& writer)
        : AutoElement(name, writer.get()) {}

    AutoElement(const char name[], SkSVGDevice* svgdev,
                ResourceBucket* bucket, const MxCp& mc, const SkPaint& paint)
        : fWriter(svgdev->fWriter.get())
        , fResourceBucket(bucket) {

        svgdev->syncClipStack(*mc.fClipStack);
        Resources res = this->addResources(mc, paint);

        fWriter->startElement(name);

        this->addPaint(paint, res);

        if (!mc.fMatrix->isIdentity()) {
            this->addAttribute("transform", svg_transform(*mc.fMatrix));
        }
    }

    ~AutoElement() {
        fWriter->endElement();
    }

    void addAttribute(const char name[], const char val[]) {
        fWriter->addAttribute(name, val);
    }

    void addAttribute(const char name[], const SkString& val) {
        fWriter->addAttribute(name, val.c_str());
    }

    void addAttribute(const char name[], int32_t val) {
        fWriter->addS32Attribute(name, val);
    }

    void addAttribute(const char name[], SkScalar val) {
        fWriter->addScalarAttribute(name, val);
    }

    void addText(const SkString& text) {
        fWriter->addText(text.c_str(), text.size());
    }

    void addRectAttributes(const SkRect&);
    void addPathAttributes(const SkPath&, SkParsePath::PathEncoding);
    void addTextAttributes(const SkFont&);

private:
    Resources addResources(const MxCp&, const SkPaint& paint);
    void addShaderResources(const SkPaint& paint, Resources* resources);
    void addGradientShaderResources(const SkShader* shader, const SkPaint& paint,
                                    Resources* resources);
    void addColorFilterResources(const SkColorFilter& cf, Resources* resources);
    void addImageShaderResources(const SkShader* shader, const SkPaint& paint,
                                 Resources* resources);

    void addPatternDef(const SkBitmap& bm);

    void addPaint(const SkPaint& paint, const Resources& resources);

    SkString addGradientDef(SkShaderBase::GradientType,
                            const SkShaderBase::GradientInfo& info,
                            const SkShader* shader,
                            const SkMatrix& localMatrix);

    SkXMLWriter*               fWriter;
    ResourceBucket*            fResourceBucket;
};

void SkSVGDevice::AutoElement::addPaint(const SkPaint& paint, const Resources& resources) {
    // Path effects are applied to all vector graphics (rects, rrects, ovals,
    // paths etc).  This should only happen when a path effect is attached to
    // non-vector graphics (text, image) or a new vector graphics primitive is
    //added that is not handled by base drawPath() routine.
    if (paint.getPathEffect() != nullptr) {
        SkDebugf("Unsupported path effect in addPaint.");
    }
    SkPaint::Style style = paint.getStyle();
    if (style == SkPaint::kFill_Style || style == SkPaint::kStrokeAndFill_Style) {
        static constexpr char kDefaultFill[] = "black";
        if (!resources.fPaintServer.equals(kDefaultFill)) {
            this->addAttribute("fill", resources.fPaintServer);
        }
        if (SK_AlphaOPAQUE != SkColorGetA(paint.getColor())) {
            this->addAttribute("fill-opacity", svg_opacity(paint.getColor()));
        }
    } else {
        SkASSERT(style == SkPaint::kStroke_Style);
        this->addAttribute("fill", "none");
    }

    if (!resources.fColorFilter.isEmpty()) {
        this->addAttribute("filter", resources.fColorFilter.c_str());
    }

    if (style == SkPaint::kStroke_Style || style == SkPaint::kStrokeAndFill_Style) {
        this->addAttribute("stroke", resources.fPaintServer);

        SkScalar strokeWidth = paint.getStrokeWidth();
        if (strokeWidth == 0) {
            // Hairline stroke
            strokeWidth = 1;
            this->addAttribute("vector-effect", "non-scaling-stroke");
        }
        this->addAttribute("stroke-width", strokeWidth);

        if (const char* cap = svg_cap(paint.getStrokeCap())) {
            this->addAttribute("stroke-linecap", cap);
        }

        if (const char* join = svg_join(paint.getStrokeJoin())) {
            this->addAttribute("stroke-linejoin", join);
        }

        if (paint.getStrokeJoin() == SkPaint::kMiter_Join) {
            this->addAttribute("stroke-miterlimit", paint.getStrokeMiter());
        }

        if (SK_AlphaOPAQUE != SkColorGetA(paint.getColor())) {
            this->addAttribute("stroke-opacity", svg_opacity(paint.getColor()));
        }
    } else {
        SkASSERT(style == SkPaint::kFill_Style);
        // SVG default stroke value is "none".
    }
}

Resources SkSVGDevice::AutoElement::addResources(const MxCp& mc, const SkPaint& paint) {
    Resources resources(paint);

    if (paint.getShader()) {
        AutoElement defs("defs", fWriter);

        this->addShaderResources(paint, &resources);
    }

    if (const SkColorFilter* cf = paint.getColorFilter()) {
        // TODO: Implement skia color filters for blend modes other than SrcIn
        SkBlendMode mode;
        if (cf->asAColorMode(nullptr, &mode) && mode == SkBlendMode::kSrcIn) {
            this->addColorFilterResources(*cf, &resources);
        }
    }

    return resources;
}

void SkSVGDevice::AutoElement::addGradientShaderResources(const SkShader* shader,
                                                          const SkPaint& paint,
                                                          Resources* resources) {
    SkASSERT(shader);

    SkShaderBase::GradientInfo grInfo;
    const auto gradient_type = as_SB(shader)->asGradient(&grInfo);

    if (gradient_type != SkShaderBase::GradientType::kLinear &&
        gradient_type != SkShaderBase::GradientType::kRadial &&
        gradient_type != SkShaderBase::GradientType::kConical) {
        return;
    }

    AutoSTArray<16, SkColor4f> grColors(grInfo.fColorCount);
    AutoSTArray<16, SkScalar> grOffsets(grInfo.fColorCount);
    grInfo.fColors = grColors.get();
    grInfo.fColorOffsets = grOffsets.get();

    // One more call to get the actual colors/offsets and local matrix.
    SkMatrix localMatrix;
    as_SB(shader)->asGradient(&grInfo, &localMatrix);
    SkASSERT(grInfo.fColorCount <= grColors.count());
    SkASSERT(grInfo.fColorCount <= grOffsets.count());

    SkASSERT(grColors.size() > 0);
    resources->fPaintServer = SkStringPrintf("url(#%s)",
            addGradientDef(gradient_type, grInfo, shader, localMatrix).c_str());
}

void SkSVGDevice::AutoElement::addColorFilterResources(const SkColorFilter& cf,
                                                       Resources* resources) {
    SkString colorfilterID = fResourceBucket->addColorFilter();
    {
        AutoElement filterElement("filter", fWriter);
        filterElement.addAttribute("id", colorfilterID);
        filterElement.addAttribute("x", "0%");
        filterElement.addAttribute("y", "0%");
        filterElement.addAttribute("width", "100%");
        filterElement.addAttribute("height", "100%");

        SkColor filterColor;
        SkBlendMode mode;
        bool asAColorMode = cf.asAColorMode(&filterColor, &mode);
        SkAssertResult(asAColorMode);
        SkASSERT(mode == SkBlendMode::kSrcIn);

        {
            // first flood with filter color
            AutoElement floodElement("feFlood", fWriter);
            floodElement.addAttribute("flood-color", svg_color(filterColor));
            floodElement.addAttribute("flood-opacity", svg_opacity(filterColor));
            floodElement.addAttribute("result", "flood");
        }

        {
            // apply the transform to filter color
            AutoElement compositeElement("feComposite", fWriter);
            compositeElement.addAttribute("in", "flood");
            compositeElement.addAttribute("operator", "in");
        }
    }
    resources->fColorFilter.printf("url(#%s)", colorfilterID.c_str());
}

static bool is_png(const void* bytes, size_t length) {
    static constexpr uint8_t pngSig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    return length >= sizeof(pngSig) && !memcmp(bytes, pngSig, sizeof(pngSig));
}

static bool is_jpeg(const void* bytes, size_t length) {
    static constexpr uint8_t jpegSig[] = {0xFF, 0xD8, 0xFF};
    return length >= sizeof(jpegSig) && !memcmp(bytes, jpegSig, sizeof(jpegSig));
}

// Returns data uri from bytes.
// it will use any cached data if available, otherwise will
// encode as png.
sk_sp<SkData> AsDataUri(SkImage* image) {
    static constexpr char jpgDataPrefix[] = "data:image/jpeg;base64,";
    static constexpr char pngDataPrefix[] = "data:image/png;base64,";

    SkASSERT(!image->isTextureBacked());

    const char* selectedPrefix = pngDataPrefix;
    size_t selectedPrefixLength = sizeof(pngDataPrefix);

    sk_sp<SkData> imageData = image->refEncodedData();
    if (imageData) {  // Already encoded as something
        if (is_jpeg(imageData->data(), imageData->size())) {
            selectedPrefix = jpgDataPrefix;
            selectedPrefixLength = sizeof(jpgDataPrefix);
        } else if (!is_png(imageData->data(), imageData->size())) {
            // re-encode the image as a PNG.
            // GrDirectContext is nullptr because we shouldn't have any texture-based images
            // passed in.
            imageData = SkPngEncoder::Encode(nullptr, image, {});
            if (!imageData) {
                return nullptr;
            }
        }
        // else, it's already encoded as a PNG - we don't need to do anything.
    } else {
        // It was not encoded as something, so we need to encode it as a PNG.
        imageData = SkPngEncoder::Encode(nullptr, image, {});
        if (!imageData) {
            return nullptr;
        }
    }

    size_t b64Size = SkBase64::EncodedSize(imageData->size());
    sk_sp<SkData> dataUri = SkData::MakeUninitialized(selectedPrefixLength + b64Size);
    char* dest = (char*)dataUri->writable_data();
    memcpy(dest, selectedPrefix, selectedPrefixLength);
    SkBase64::Encode(imageData->data(), imageData->size(), dest + selectedPrefixLength - 1);
    dest[dataUri->size() - 1] = 0;
    return dataUri;
}

void SkSVGDevice::AutoElement::addImageShaderResources(const SkShader* shader, const SkPaint& paint,
                                                       Resources* resources) {
    SkMatrix outMatrix;

    SkTileMode xy[2];
    SkImage* image = shader->isAImage(&outMatrix, xy);
    SkASSERT(image);

    SkString patternDims[2];  // width, height

    sk_sp<SkData> dataUri = AsDataUri(image);
    if (!dataUri) {
        return;
    }
    SkIRect imageSize = image->bounds();
    for (int i = 0; i < 2; i++) {
        int imageDimension = i == 0 ? imageSize.width() : imageSize.height();
        switch (xy[i]) {
            case SkTileMode::kRepeat:
                patternDims[i].appendScalar(imageDimension);
            break;
            default:
                // TODO: other tile modes?
                patternDims[i] = "100%";
        }
    }

    SkString patternID = fResourceBucket->addPattern();
    {
        AutoElement pattern("pattern", fWriter);
        pattern.addAttribute("id", patternID);
        pattern.addAttribute("patternUnits", "userSpaceOnUse");
        pattern.addAttribute("patternContentUnits", "userSpaceOnUse");
        pattern.addAttribute("width", patternDims[0]);
        pattern.addAttribute("height", patternDims[1]);
        pattern.addAttribute("x", 0);
        pattern.addAttribute("y", 0);

        {
            SkString imageID = fResourceBucket->addImage();
            AutoElement imageTag("image", fWriter);
            imageTag.addAttribute("id", imageID);
            imageTag.addAttribute("x", 0);
            imageTag.addAttribute("y", 0);
            imageTag.addAttribute("width", image->width());
            imageTag.addAttribute("height", image->height());
            imageTag.addAttribute("xlink:href", static_cast<const char*>(dataUri->data()));
        }
    }
    resources->fPaintServer.printf("url(#%s)", patternID.c_str());
}

void SkSVGDevice::AutoElement::addShaderResources(const SkPaint& paint, Resources* resources) {
    const SkShader* shader = paint.getShader();
    SkASSERT(shader);

    if (as_SB(shader)->type() == SkShaderBase::ShaderType::kColor) {
        auto colorShader = static_cast<const SkColorShader*>(shader);
        resources->fPaintServer = svg_color(colorShader->color().toSkColor());
    } else
    // Checking the shader type is too restrictive, as it rejects local matrix wrappers.
    // Wrappers do forward asGradient() though, so we can reach nested gradients this way.
    if (as_SB(shader)->asGradient() != SkShaderBase::GradientType::kNone) {
        this->addGradientShaderResources(shader, paint, resources);
    } else
    if (shader->isAImage()) {
        this->addImageShaderResources(shader, paint, resources);
    }
    // TODO: other shader types?
}

SkString SkSVGDevice::AutoElement::addGradientDef(SkShaderBase::GradientType type,
                                                  const SkShaderBase::GradientInfo& info,
                                                  const SkShader* shader,
                                                  const SkMatrix& localMatrix) {
    SkASSERT(fResourceBucket);
    SkString id = fResourceBucket->addGradient();

    SkASSERT(type == SkShaderBase::GradientType::kLinear ||
             type == SkShaderBase::GradientType::kRadial ||
             type == SkShaderBase::GradientType::kConical);

    const char* elem_name = type == SkShaderBase::GradientType::kLinear ? "linearGradient"
                                                                        : "radialGradient";
    {
        AutoElement gradient(elem_name, fWriter);

        gradient.addAttribute("id", id);
        gradient.addAttribute("gradientUnits", "userSpaceOnUse");

        switch (type) {
            case SkShaderBase::GradientType::kLinear:
                gradient.addAttribute("x1", info.fPoint[0].x());
                gradient.addAttribute("y1", info.fPoint[0].y());
                gradient.addAttribute("x2", info.fPoint[1].x());
                gradient.addAttribute("y2", info.fPoint[1].y());
                break;
            case SkShaderBase::GradientType::kConical:
                gradient.addAttribute("fx", info.fPoint[1].x());
                gradient.addAttribute("fy", info.fPoint[1].y());
                gradient.addAttribute("fr", info.fRadius[1]);
                [[fallthrough]];
            case SkShaderBase::GradientType::kRadial:
                gradient.addAttribute("cx", info.fPoint[0].x());
                gradient.addAttribute("cy", info.fPoint[0].y());
                gradient.addAttribute("r" , info.fRadius[0]);
                break;
            default:
                SkUNREACHABLE;
        }

        if (!localMatrix.isIdentity()) {
            this->addAttribute("gradientTransform", svg_transform(localMatrix));
        }

        SkASSERT(info.fColorCount >= 2);
        for (int i = 0; i < info.fColorCount; ++i) {
            SkColor color = info.fColors[i].toSkColor();
            SkString colorStr(svg_color(color));

            {
                AutoElement stop("stop", fWriter);
                stop.addAttribute("offset", info.fColorOffsets[i]);
                stop.addAttribute("stop-color", colorStr.c_str());

                if (SK_AlphaOPAQUE != SkColorGetA(color)) {
                    stop.addAttribute("stop-opacity", svg_opacity(color));
                }
            }
        }
    }

    return id;
}

void SkSVGDevice::AutoElement::addRectAttributes(const SkRect& rect) {
    // x, y default to 0
    if (rect.x() != 0) {
        this->addAttribute("x", rect.x());
    }
    if (rect.y() != 0) {
        this->addAttribute("y", rect.y());
    }

    this->addAttribute("width", rect.width());
    this->addAttribute("height", rect.height());
}

void SkSVGDevice::AutoElement::addPathAttributes(const SkPath& path,
                                                 SkParsePath::PathEncoding encoding) {
    this->addAttribute("d", SkParsePath::ToSVGString(path, encoding));
}

void SkSVGDevice::AutoElement::addTextAttributes(const SkFont& font) {
    this->addAttribute("font-size", font.getSize());

    SkString familyName;
    THashSet<SkString> familySet;
    sk_sp<SkTypeface> tface = font.refTypeface();

    SkASSERT(tface);
    SkFontStyle style = tface->fontStyle();
    if (style.slant() == SkFontStyle::kItalic_Slant) {
        this->addAttribute("font-style", "italic");
    } else if (style.slant() == SkFontStyle::kOblique_Slant) {
        this->addAttribute("font-style", "oblique");
    }
    int weightIndex = (SkTPin(style.weight(), 100, 900) - 50) / 100;
    if (weightIndex != 3) {
        static constexpr const char* weights[] = {
            "100", "200", "300", "normal", "400", "500", "600", "bold", "800", "900"
        };
        this->addAttribute("font-weight", weights[weightIndex]);
    }
    int stretchIndex = style.width() - 1;
    if (stretchIndex != 4) {
        static constexpr const char* stretches[] = {
            "ultra-condensed", "extra-condensed", "condensed", "semi-condensed",
            "normal",
            "semi-expanded", "expanded", "extra-expanded", "ultra-expanded"
        };
        this->addAttribute("font-stretch", stretches[stretchIndex]);
    }

    sk_sp<SkTypeface::LocalizedStrings> familyNameIter(tface->createFamilyNameIterator());
    SkTypeface::LocalizedString familyString;
    if (familyNameIter) {
        while (familyNameIter->next(&familyString)) {
            if (familySet.contains(familyString.fString)) {
                continue;
            }
            familySet.add(familyString.fString);
            familyName.appendf((familyName.isEmpty() ? "%s" : ", %s"), familyString.fString.c_str());
        }
    }
    if (!familyName.isEmpty()) {
        this->addAttribute("font-family", familyName);
    }
}

sk_sp<SkDevice> SkSVGDevice::Make(const SkISize& size,
                                  std::unique_ptr<SkXMLWriter> writer,
                                  uint32_t flags) {
    return writer ? sk_sp<SkDevice>(new SkSVGDevice(size, std::move(writer), flags))
                  : nullptr;
}

SkSVGDevice::SkSVGDevice(const SkISize& size, std::unique_ptr<SkXMLWriter> writer, uint32_t flags)
        : SkClipStackDevice(
            SkImageInfo::MakeUnknown(size.fWidth, size.fHeight),
            SkSurfaceProps())
        , fWriter(std::move(writer))
        , fResourceBucket(new ResourceBucket)
        , fFlags(flags)
{
    SkASSERT(fWriter);

    fWriter->writeHeader();

    // The root <svg> tag gets closed by the destructor.
    fRootElement = std::make_unique<AutoElement>("svg", fWriter);

    fRootElement->addAttribute("xmlns", "http://www.w3.org/2000/svg");
    fRootElement->addAttribute("xmlns:xlink", "http://www.w3.org/1999/xlink");
    fRootElement->addAttribute("width", size.width());
    fRootElement->addAttribute("height", size.height());
}

SkSVGDevice::~SkSVGDevice() {
    // Pop order is important.
    while (!fClipStack.empty()) {
        fClipStack.pop_back();
    }
}

SkParsePath::PathEncoding SkSVGDevice::pathEncoding() const {
    return (fFlags & SkSVGCanvas::kRelativePathEncoding_Flag)
        ? SkParsePath::PathEncoding::Relative
        : SkParsePath::PathEncoding::Absolute;
}

void SkSVGDevice::syncClipStack(const SkClipStack& cs) {
    SkClipStack::B2TIter iter(cs);

    const SkClipStack::Element* elem;
    int rec_idx = 0;

    // First, find/preserve the common bottom.
    while ((elem = iter.next()) && (rec_idx < fClipStack.size())) {
        if (fClipStack[SkToInt(rec_idx)].fGenID != elem->getGenID()) {
            break;
        }
        rec_idx++;
    }

    // Discard out-of-date stack top.
    while (fClipStack.size() > rec_idx) {
        fClipStack.pop_back();
    }

    auto define_clip = [this](const SkClipStack::Element* e) {
        const auto cid = SkStringPrintf("cl_%x", e->getGenID());

        AutoElement clip_path("clipPath", fWriter);
        clip_path.addAttribute("id", cid);

        // TODO: handle non-intersect clips.

        switch (e->getDeviceSpaceType()) {
        case SkClipStack::Element::DeviceSpaceType::kEmpty: {
            // TODO: can we skip this?
            AutoElement rect("rect", fWriter);
        } break;
        case SkClipStack::Element::DeviceSpaceType::kRect: {
            AutoElement rect("rect", fWriter);
            rect.addRectAttributes(e->getDeviceSpaceRect());
        } break;
        case SkClipStack::Element::DeviceSpaceType::kRRect: {
            // TODO: complex rrect handling?
            const auto& rr   = e->getDeviceSpaceRRect();
            const auto radii = rr.getSimpleRadii();

            AutoElement rrect("rect", fWriter);
            rrect.addRectAttributes(rr.rect());
            rrect.addAttribute("rx", radii.x());
            rrect.addAttribute("ry", radii.y());
        } break;
        case SkClipStack::Element::DeviceSpaceType::kPath: {
            const auto& p = e->getDeviceSpacePath();
            AutoElement path("path", fWriter);
            path.addPathAttributes(p, this->pathEncoding());
            if (p.getFillType() == SkPathFillType::kEvenOdd) {
                path.addAttribute("clip-rule", "evenodd");
            }
        } break;
        case SkClipStack::Element::DeviceSpaceType::kShader:
            // TODO: handle shader clipping, perhaps rasterize and apply as a mask image?
            break;
        }

        return cid;
    };

    // Rebuild the top.
    while (elem) {
        const auto cid = define_clip(elem);

        auto clip_grp = std::make_unique<AutoElement>("g", fWriter);
        clip_grp->addAttribute("clip-path", SkStringPrintf("url(#%s)", cid.c_str()));

        fClipStack.push_back({ std::move(clip_grp), elem->getGenID() });

        elem = iter.next();
    }
}

void SkSVGDevice::drawPaint(const SkPaint& paint) {
    AutoElement rect("rect", this, fResourceBucket.get(), MxCp(this), paint);
    rect.addRectAttributes(SkRect::MakeWH(SkIntToScalar(this->width()),
                                          SkIntToScalar(this->height())));
}

void SkSVGDevice::drawAnnotation(const SkRect& rect, const char key[], SkData* value) {
    if (!value) {
        return;
    }

    if (!strcmp(SkAnnotationKeys::URL_Key(), key) ||
        !strcmp(SkAnnotationKeys::Link_Named_Dest_Key(), key)) {
        this->cs().save();
        this->cs().clipRect(rect, this->localToDevice(), SkClipOp::kIntersect, true);
        SkRect transformedRect = this->cs().bounds(this->getGlobalBounds());
        this->cs().restore();
        if (transformedRect.isEmpty()) {
            return;
        }

        SkString url(static_cast<const char*>(value->data()), value->size() - 1);
        AutoElement a("a", fWriter);
        a.addAttribute("xlink:href", url.c_str());
        {
            AutoElement r("rect", fWriter);
            r.addAttribute("fill-opacity", "0.0");
            r.addRectAttributes(transformedRect);
        }
    }
}

void SkSVGDevice::drawPoints(SkCanvas::PointMode mode, SkSpan<const SkPoint> pts,
                             const SkPaint& paint) {
    SkPathBuilder path;

    const size_t count = pts.size();
    switch (mode) {
            // todo
        case SkCanvas::kPoints_PointMode:
            for (const auto& pt : pts) {
                path.moveTo(pt);
                path.lineTo(pt);
            }
            break;
        case SkCanvas::kLines_PointMode:
            for (size_t i = 1; i < count; i += 2) {
                path.moveTo(pts[i-1]);
                path.lineTo(pts[i]);
            }
            break;
        case SkCanvas::kPolygon_PointMode:
            if (count > 1) {
                path.addPolygon(pts, false);
            }
            break;
    }

    this->drawPath(path.detach(), paint, true);
}

void SkSVGDevice::drawRect(const SkRect& r, const SkPaint& paint) {
    if (paint.getPathEffect()) {
        this->drawPath(SkPath::Rect(r), paint, true);
        return;
    }

    std::unique_ptr<AutoElement> svg;
    if (RequiresViewportReset(paint)) {
      svg = std::make_unique<AutoElement>("svg", this, fResourceBucket.get(), MxCp(this), paint);
      svg->addRectAttributes(r);
    }

    AutoElement rect("rect", this, fResourceBucket.get(), MxCp(this), paint);

    if (svg) {
      rect.addAttribute("x", 0);
      rect.addAttribute("y", 0);
      rect.addAttribute("width", "100%");
      rect.addAttribute("height", "100%");
    } else {
      rect.addRectAttributes(r);
    }
}

void SkSVGDevice::drawOval(const SkRect& oval, const SkPaint& paint) {
    if (paint.getPathEffect()) {
        this->drawPath(SkPath::Oval(oval), paint, true);
        return;
    }

    AutoElement ellipse("ellipse", this, fResourceBucket.get(), MxCp(this), paint);
    ellipse.addAttribute("cx", oval.centerX());
    ellipse.addAttribute("cy", oval.centerY());
    ellipse.addAttribute("rx", oval.width() / 2);
    ellipse.addAttribute("ry", oval.height() / 2);
}

void SkSVGDevice::drawRRect(const SkRRect& rr, const SkPaint& paint) {
    if (paint.getPathEffect()) {
        this->drawPath(SkPath::RRect(rr), paint, true);
        return;
    }

    AutoElement elem("path", this, fResourceBucket.get(), MxCp(this), paint);
    elem.addPathAttributes(SkPath::RRect(rr), this->pathEncoding());
}

void SkSVGDevice::drawPath(const SkPath& path, const SkPaint& paint, bool pathIsMutable) {
    if (path.isInverseFillType()) {
      SkDebugf("Inverse path fill type not yet implemented.");
      return;
    }

    SkPath pathStorage;
    const SkPath* pathPtr = const_cast<SkPath*>(&path);
    SkTCopyOnFirstWrite<SkPaint> path_paint(paint);

    // Apply path effect from paint to path.
    if (path_paint->getPathEffect()) {
      SkPathBuilder builder;
      bool fill = skpathutils::FillPathWithPaint(path, *path_paint, &builder);
      pathStorage = builder.detach();
      pathPtr = &pathStorage;
      if (fill) {
        // Path should be filled.
        path_paint.writable()->setStyle(SkPaint::kFill_Style);
      } else {
        // Path should be drawn with a hairline (width == 0).
        path_paint.writable()->setStyle(SkPaint::kStroke_Style);
        path_paint.writable()->setStrokeWidth(0);
      }

      path_paint.writable()->setPathEffect(nullptr); // path effect processed
    }

    // Create path element.
    AutoElement elem("path", this, fResourceBucket.get(), MxCp(this), *path_paint);
    elem.addPathAttributes(*pathPtr, this->pathEncoding());

    // TODO: inverse fill types?
    if (pathPtr->getFillType() == SkPathFillType::kEvenOdd) {
        elem.addAttribute("fill-rule", "evenodd");
    }
}

static sk_sp<SkData> encode(const SkBitmap& src) {
    SkDynamicMemoryWStream buf;
    return SkPngEncoder::Encode(&buf, src.pixmap(), {}) ? buf.detachAsData() : nullptr;
}

void SkSVGDevice::drawBitmapCommon(const MxCp& mc, const SkBitmap& bm, const SkPaint& paint) {
    sk_sp<SkData> pngData = encode(bm);
    if (!pngData) {
        return;
    }

    size_t b64Size = SkBase64::EncodedSize(pngData->size());
    AutoTMalloc<char> b64Data(b64Size);
    SkBase64::Encode(pngData->data(), pngData->size(), b64Data.get());

    SkString svgImageData("data:image/png;base64,");
    svgImageData.append(b64Data.get(), b64Size);

    SkString imageID = fResourceBucket->addImage();
    {
        AutoElement defs("defs", fWriter);
        {
            AutoElement image("image", fWriter);
            image.addAttribute("id", imageID);
            image.addAttribute("width", bm.width());
            image.addAttribute("height", bm.height());
            image.addAttribute("xlink:href", svgImageData);
        }
    }

    {
        AutoElement imageUse("use", this, fResourceBucket.get(), mc, paint);
        imageUse.addAttribute("xlink:href", SkStringPrintf("#%s", imageID.c_str()));
    }
}

void SkSVGDevice::drawImageRect(const SkImage* image, const SkRect* src, const SkRect& dst,
                                const SkSamplingOptions& sampling, const SkPaint& paint,
                                SkCanvas::SrcRectConstraint constraint) {
    SkBitmap bm;
    // TODO: support gpu images
    if (!as_IB(image)->getROPixels(nullptr, &bm)) {
        return;
    }

    SkClipStack* cs = &this->cs();
    SkClipStack::AutoRestore ar(cs, false);
    if (src && *src != SkRect::Make(bm.bounds())) {
        cs->save();
        cs->clipRect(dst, this->localToDevice(), SkClipOp::kIntersect, paint.isAntiAlias());
    }

    SkMatrix adjustedMatrix = this->localToDevice()
                            * SkMatrix::RectToRect(src ? *src : SkRect::Make(bm.bounds()), dst);

    drawBitmapCommon(MxCp(&adjustedMatrix, cs), bm, paint);
}

class SVGTextBuilder : SkNoncopyable {
public:
    SVGTextBuilder(SkPoint origin, const sktext::GlyphRun& glyphRun)
            : fOrigin(origin) {
        auto runSize = glyphRun.runSize();
        AutoSTArray<64, SkUnichar> unichars(runSize);
        SkFontPriv::GlyphsToUnichars(glyphRun.font(), glyphRun.glyphsIDs().data(),
                                     runSize, unichars.get());
        auto positions = glyphRun.positions();
        for (size_t i = 0; i < runSize; ++i) {
            this->appendUnichar(unichars[i], positions[i]);
        }
    }

    const SkString& text() const { return fText; }
    const SkString& posX() const { return fPosXStr; }
    const SkString& posY() const { return fHasConstY ? fConstYStr : fPosYStr; }

private:
    void appendUnichar(SkUnichar c, SkPoint position) {
        bool discardPos = false;
        bool isWhitespace = false;

        switch(c) {
            case ' ':
            case '\t':
                // consolidate whitespace to match SVG's xml:space=default munging
                // (http://www.w3.org/TR/SVG/text.html#WhiteSpace)
                if (fLastCharWasWhitespace) {
                    discardPos = true;
                } else {
                    fText.appendUnichar(c);
                }
                isWhitespace = true;
                break;
            case '\0':
                // SkPaint::glyphsToUnichars() returns \0 for inconvertible glyphs, but these
                // are not legal XML characters (http://www.w3.org/TR/REC-xml/#charsets)
                discardPos = true;
                isWhitespace = fLastCharWasWhitespace; // preserve whitespace consolidation
                break;
            case '&':
                fText.append("&amp;");
                break;
            case '"':
                fText.append("&quot;");
                break;
            case '\'':
                fText.append("&apos;");
                break;
            case '<':
                fText.append("&lt;");
                break;
            case '>':
                fText.append("&gt;");
                break;
            default:
                fText.appendUnichar(c);
                break;
        }

        fLastCharWasWhitespace = isWhitespace;

        if (discardPos) {
            return;
        }

        position += fOrigin;
        fPosXStr.appendf("%.8g, ", position.fX);
        fPosYStr.appendf("%.8g, ", position.fY);

        if (fConstYStr.isEmpty()) {
            fConstYStr = fPosYStr;
            fConstY    = position.fY;
        } else {
            fHasConstY &= SkScalarNearlyEqual(fConstY, position.fY);
        }
    }

    const SkPoint   fOrigin;

    SkString fText,
             fPosXStr, fPosYStr,
             fConstYStr;
    SkScalar fConstY;
    bool     fLastCharWasWhitespace = true, // start off in whitespace mode to strip leading space
             fHasConstY             = true;
};

void SkSVGDevice::onDrawGlyphRunList(SkCanvas* canvas,
                                     const sktext::GlyphRunList& glyphRunList,
                                     const SkPaint& paint) {
    SkASSERT(!glyphRunList.hasRSXForm());
    const auto draw_as_path =
            (fFlags & SkSVGCanvas::kConvertTextToPaths_Flag) || paint.getPathEffect();

    if (draw_as_path) {
        // Emit a single <path> element.
        SkPathBuilder builder;
        for (auto& glyphRun : glyphRunList) {
            AddPath(glyphRun, glyphRunList.origin(), &builder);
        }

        this->drawPath(builder.detach(), paint);

        return;
    }

    // Emit one <text> element for each run.
    for (auto& glyphRun : glyphRunList) {
        AutoElement elem("text", this, fResourceBucket.get(), MxCp(this), paint);
        elem.addTextAttributes(glyphRun.font());

        SVGTextBuilder builder(glyphRunList.origin(), glyphRun);
        elem.addAttribute("x", builder.posX());
        elem.addAttribute("y", builder.posY());
        elem.addText(builder.text());
    }
}

void SkSVGDevice::drawVertices(const SkVertices*, sk_sp<SkBlender>, const SkPaint&, bool) {
    // todo
}

void SkSVGDevice::drawMesh(const SkMesh&, sk_sp<SkBlender>, const SkPaint&) {
    // todo
}
