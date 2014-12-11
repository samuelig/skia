/*
 * Copyright 2014 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "effects/GrPorterDuffXferProcessor.h"

#include "GrBlend.h"
#include "GrDrawState.h"
#include "GrInvariantOutput.h"
#include "GrProcessor.h"
#include "GrTypes.h"
#include "GrXferProcessor.h"
#include "gl/GrGLProcessor.h"
#include "gl/builders/GrGLFragmentShaderBuilder.h"
#include "gl/builders/GrGLProgramBuilder.h"

static bool can_tweak_alpha_for_coverage(GrBlendCoeff dstCoeff, bool isCoverageDrawing) {
    /*
     The fractional coverage is f.
     The src and dst coeffs are Cs and Cd.
     The dst and src colors are S and D.
     We want the blend to compute: f*Cs*S + (f*Cd + (1-f))D. By tweaking the source color's alpha
     we're replacing S with S'=fS. It's obvious that that first term will always be ok. The second
     term can be rearranged as [1-(1-Cd)f]D. By substituting in the various possibilities for Cd we
     find that only 1, ISA, and ISC produce the correct destination when applied to S' and D.
     Also, if we're directly rendering coverage (isCoverageDrawing) then coverage is treated as
     color by definition.
     */
    // TODO: Once we have a CoverageDrawing XP, we don't need to check is CoverageDrawing here
    return kOne_GrBlendCoeff == dstCoeff ||
           kISA_GrBlendCoeff == dstCoeff ||
           kISC_GrBlendCoeff == dstCoeff ||
           isCoverageDrawing;
}

class GrGLPorterDuffXferProcessor : public GrGLXferProcessor {
public:
    GrGLPorterDuffXferProcessor(const GrProcessor&) {}

    virtual ~GrGLPorterDuffXferProcessor() {}

    virtual void emitCode(GrGLFPBuilder* builder,
                          const GrFragmentProcessor& fp,
                          const char* outputColor,
                          const char* inputColor,
                          const TransformedCoordsArray& coords,
                          const TextureSamplerArray& samplers) SK_OVERRIDE {
        GrGLFPFragmentBuilder* fsBuilder = builder->getFragmentShaderBuilder();
        fsBuilder->codeAppendf("%s = %s;", outputColor, inputColor);
    }

    virtual void setData(const GrGLProgramDataManager&, const GrProcessor&) SK_OVERRIDE {};

    static void GenKey(const GrProcessor&, const GrGLCaps& caps, GrProcessorKeyBuilder* b) {};

private:
    typedef GrGLXferProcessor INHERITED;
};

///////////////////////////////////////////////////////////////////////////////

GrPorterDuffXferProcessor::GrPorterDuffXferProcessor(GrBlendCoeff srcBlend, GrBlendCoeff dstBlend,
                                                     GrColor constant)
    : fSrcBlend(srcBlend), fDstBlend(dstBlend), fBlendConstant(constant) {
    this->initClassID<GrPorterDuffXferProcessor>();
}

GrPorterDuffXferProcessor::~GrPorterDuffXferProcessor() {
}

void GrPorterDuffXferProcessor::getGLProcessorKey(const GrGLCaps& caps,
                                                  GrProcessorKeyBuilder* b) const {
    GrGLPorterDuffXferProcessor::GenKey(*this, caps, b);
}

GrGLFragmentProcessor* GrPorterDuffXferProcessor::createGLInstance() const {
    return SkNEW_ARGS(GrGLPorterDuffXferProcessor, (*this));
}

void GrPorterDuffXferProcessor::onComputeInvariantOutput(GrInvariantOutput* inout) const {
    inout->setToUnknown(GrInvariantOutput::kWill_ReadInput);
}

GrXferProcessor::OptFlags
GrPorterDuffXferProcessor::getOptimizations(const GrProcOptInfo& colorPOI,
                                            const GrProcOptInfo& coveragePOI,
                                            bool isCoverageDrawing,
                                            bool colorWriteDisabled,
                                            bool doesStencilWrite,
                                            GrColor* color, uint8_t* coverage) {
    if (colorWriteDisabled) {
        fSrcBlend = kZero_GrBlendCoeff;
        fDstBlend = kOne_GrBlendCoeff;
    }

    bool srcAIsOne;
    bool hasCoverage;
    if (isCoverageDrawing) {
        srcAIsOne = colorPOI.isOpaque() && coveragePOI.isOpaque();
        hasCoverage = false;
    } else {
        srcAIsOne = colorPOI.isOpaque();
        hasCoverage = !coveragePOI.isSolidWhite();
    }

    bool dstCoeffIsOne = kOne_GrBlendCoeff == fDstBlend ||
                         (kSA_GrBlendCoeff == fDstBlend && srcAIsOne);
    bool dstCoeffIsZero = kZero_GrBlendCoeff == fDstBlend ||
                         (kISA_GrBlendCoeff == fDstBlend && srcAIsOne);

    // Optimizations when doing RGB Coverage
    if (coveragePOI.isFourChannelOutput()) {
        // We want to force our primary output to be alpha * Coverage, where alpha is the alpha
        // value of the blend the constant. We should already have valid blend coeff's if we are at
        // a point where we have RGB coverage. We don't need any color stages since the known color
        // output is already baked into the blendConstant.
        uint8_t alpha = GrColorUnpackA(fBlendConstant);
        *color = GrColorPackRGBA(alpha, alpha, alpha, alpha);
        return GrXferProcessor::kClearColorStages_OptFlag;
    }

    // When coeffs are (0,1) there is no reason to draw at all, unless
    // stenciling is enabled. Having color writes disabled is effectively
    // (0,1).
    if ((kZero_GrBlendCoeff == fSrcBlend && dstCoeffIsOne)) {
        if (doesStencilWrite) {
            *color = 0xffffffff;
            return GrXferProcessor::kClearColorStages_OptFlag |
                   GrXferProcessor::kSetCoverageDrawing_OptFlag;
        } else {
            fDstBlend = kOne_GrBlendCoeff;
            return GrXferProcessor::kSkipDraw_OptFlag;
        }
    }

    // if we don't have coverage we can check whether the dst
    // has to read at all. If not, we'll disable blending.
    if (!hasCoverage) {
        if (dstCoeffIsZero) {
            if (kOne_GrBlendCoeff == fSrcBlend) {
                // if there is no coverage and coeffs are (1,0) then we
                // won't need to read the dst at all, it gets replaced by src
                fDstBlend = kZero_GrBlendCoeff;
                return GrXferProcessor::kNone_Opt;
            } else if (kZero_GrBlendCoeff == fSrcBlend) {
                // if the op is "clear" then we don't need to emit a color
                // or blend, just write transparent black into the dst.
                fSrcBlend = kOne_GrBlendCoeff;
                fDstBlend = kZero_GrBlendCoeff;
                *color = 0;
                *coverage = 0xff;
                return GrXferProcessor::kClearColorStages_OptFlag |
                       GrXferProcessor::kClearCoverageStages_OptFlag;
            }
        }
    } else if (isCoverageDrawing) {
        // we have coverage but we aren't distinguishing it from alpha by request.
        return GrXferProcessor::kSetCoverageDrawing_OptFlag;
    } else {
        // check whether coverage can be safely rolled into alpha
        // of if we can skip color computation and just emit coverage
        if (can_tweak_alpha_for_coverage(fDstBlend, isCoverageDrawing)) {
            return GrXferProcessor::kSetCoverageDrawing_OptFlag;
        }
        if (dstCoeffIsZero) {
            if (kZero_GrBlendCoeff == fSrcBlend) {
                // the source color is not included in the blend
                // the dst coeff is effectively zero so blend works out to:
                // (c)(0)D + (1-c)D = (1-c)D.
                fDstBlend = kISA_GrBlendCoeff;
                *color = 0xffffffff;
                return GrXferProcessor::kClearColorStages_OptFlag |
                       GrXferProcessor::kSetCoverageDrawing_OptFlag;
            } else if (srcAIsOne) {
                // the dst coeff is effectively zero so blend works out to:
                // cS + (c)(0)D + (1-c)D = cS + (1-c)D.
                // If Sa is 1 then we can replace Sa with c
                // and set dst coeff to 1-Sa.
                fDstBlend = kISA_GrBlendCoeff;
                return GrXferProcessor::kSetCoverageDrawing_OptFlag;
            }
        } else if (dstCoeffIsOne) {
            // the dst coeff is effectively one so blend works out to:
            // cS + (c)(1)D + (1-c)D = cS + D.
            fDstBlend = kOne_GrBlendCoeff;
            return GrXferProcessor::kSetCoverageDrawing_OptFlag;
        }
    }

    return GrXferProcessor::kNone_Opt;
}
///////////////////////////////////////////////////////////////////////////////

GrPorterDuffXPFactory::GrPorterDuffXPFactory(GrBlendCoeff src, GrBlendCoeff dst)
    : fSrcCoeff(src), fDstCoeff(dst) {
    this->initClassID<GrPorterDuffXPFactory>();
}

GrXPFactory* GrPorterDuffXPFactory::Create(SkXfermode::Mode mode) {
    switch (mode) {
        case SkXfermode::kClear_Mode: {
            static GrPorterDuffXPFactory gClearPDXPF(kZero_GrBlendCoeff, kZero_GrBlendCoeff);
            return SkRef(&gClearPDXPF);
            break;
        }
        case SkXfermode::kSrc_Mode: {
            static GrPorterDuffXPFactory gSrcPDXPF(kOne_GrBlendCoeff, kZero_GrBlendCoeff);
            return SkRef(&gSrcPDXPF);
            break;
        }
        case SkXfermode::kDst_Mode: {
            static GrPorterDuffXPFactory gDstPDXPF(kZero_GrBlendCoeff, kOne_GrBlendCoeff);
            return SkRef(&gDstPDXPF);
            break;
        }
        case SkXfermode::kSrcOver_Mode: {
            static GrPorterDuffXPFactory gSrcOverPDXPF(kOne_GrBlendCoeff, kISA_GrBlendCoeff);
            return SkRef(&gSrcOverPDXPF);
            break;
        }
        case SkXfermode::kDstOver_Mode: {
            static GrPorterDuffXPFactory gDstOverPDXPF(kIDA_GrBlendCoeff, kOne_GrBlendCoeff);
            return SkRef(&gDstOverPDXPF);
            break;
        }
        case SkXfermode::kSrcIn_Mode: {
            static GrPorterDuffXPFactory gSrcInPDXPF(kDA_GrBlendCoeff, kZero_GrBlendCoeff);
            return SkRef(&gSrcInPDXPF);
            break;
        }
        case SkXfermode::kDstIn_Mode: {
            static GrPorterDuffXPFactory gDstInPDXPF(kZero_GrBlendCoeff, kSA_GrBlendCoeff);
            return SkRef(&gDstInPDXPF);
            break;
        }
        case SkXfermode::kSrcOut_Mode: {
            static GrPorterDuffXPFactory gSrcOutPDXPF(kIDA_GrBlendCoeff, kZero_GrBlendCoeff);
            return SkRef(&gSrcOutPDXPF);
            break;
        }
        case SkXfermode::kDstOut_Mode: {
            static GrPorterDuffXPFactory gDstOutPDXPF(kZero_GrBlendCoeff, kISA_GrBlendCoeff);
            return SkRef(&gDstOutPDXPF);
            break;
        }
        case SkXfermode::kSrcATop_Mode: {
            static GrPorterDuffXPFactory gSrcATopPDXPF(kDA_GrBlendCoeff, kISA_GrBlendCoeff);
            return SkRef(&gSrcATopPDXPF);
            break;
        }
        case SkXfermode::kDstATop_Mode: {
            static GrPorterDuffXPFactory gDstATopPDXPF(kIDA_GrBlendCoeff, kSA_GrBlendCoeff);
            return SkRef(&gDstATopPDXPF);
            break;
        }
        case SkXfermode::kXor_Mode: {
            static GrPorterDuffXPFactory gXorPDXPF(kIDA_GrBlendCoeff, kISA_GrBlendCoeff);
            return SkRef(&gXorPDXPF);
            break;
        }
        case SkXfermode::kPlus_Mode: {
            static GrPorterDuffXPFactory gPlusPDXPF(kOne_GrBlendCoeff, kOne_GrBlendCoeff);
            return SkRef(&gPlusPDXPF);
            break;
        }
        case SkXfermode::kModulate_Mode: {
            static GrPorterDuffXPFactory gModulatePDXPF(kZero_GrBlendCoeff, kSC_GrBlendCoeff);
            return SkRef(&gModulatePDXPF);
            break;
        }
        case SkXfermode::kScreen_Mode: {
            static GrPorterDuffXPFactory gScreenPDXPF(kOne_GrBlendCoeff, kISC_GrBlendCoeff);
            return SkRef(&gScreenPDXPF);
            break;
        }
        default:
            return NULL;
    }
}

GrXferProcessor* GrPorterDuffXPFactory::createXferProcessor(const GrProcOptInfo& colorPOI,
                                                            const GrProcOptInfo& covPOI) const {
    if (!covPOI.isFourChannelOutput()) {
        return GrPorterDuffXferProcessor::Create(fSrcCoeff, fDstCoeff);
    } else {
        if (this->supportsRGBCoverage(colorPOI.color(), colorPOI.validFlags())) {
            SkASSERT(kRGBA_GrColorComponentFlags == colorPOI.validFlags());
            GrColor blendConstant = GrUnPreMulColor(colorPOI.color());
            return GrPorterDuffXferProcessor::Create(kConstC_GrBlendCoeff, kISC_GrBlendCoeff,
                                                     blendConstant);
        } else {
            return NULL;
        }
    }
}

bool GrPorterDuffXPFactory::supportsRGBCoverage(GrColor /*knownColor*/,
                                                uint32_t knownColorFlags) const {
    if (kOne_GrBlendCoeff == fSrcCoeff && kISA_GrBlendCoeff == fDstCoeff &&
        kRGBA_GrColorComponentFlags == knownColorFlags) {
        return true;
    }
    return false;
}

bool GrPorterDuffXPFactory::canApplyCoverage(const GrProcOptInfo& colorPOI,
                                             const GrProcOptInfo& coveragePOI,
                                             bool isCoverageDrawing,
                                             bool colorWriteDisabled) const {
    bool srcAIsOne = colorPOI.isOpaque() && (!isCoverageDrawing || coveragePOI.isOpaque());

    if (colorWriteDisabled) {
        return true;
    }

    bool dstCoeffIsOne = kOne_GrBlendCoeff == fDstCoeff ||
                         (kSA_GrBlendCoeff == fDstCoeff && srcAIsOne);
    bool dstCoeffIsZero = kZero_GrBlendCoeff == fDstCoeff ||
                         (kISA_GrBlendCoeff == fDstCoeff && srcAIsOne);

    if ((kZero_GrBlendCoeff == fSrcCoeff && dstCoeffIsOne)) {
        return true;
    }

    // if we don't have coverage we can check whether the dst
    // has to read at all.
    if (isCoverageDrawing) {
        // we have coverage but we aren't distinguishing it from alpha by request.
        return true;
    } else {
        // check whether coverage can be safely rolled into alpha
        // of if we can skip color computation and just emit coverage
        if (this->canTweakAlphaForCoverage(isCoverageDrawing)) {
            return true;
        }
        if (dstCoeffIsZero) {
            if (kZero_GrBlendCoeff == fSrcCoeff) {
                return true;
            } else if (srcAIsOne) {
                return  true;
            }
        } else if (dstCoeffIsOne) {
            return true;
        }
    }

    // TODO: once all SkXferEffects are XP's then we will never reads dst here since only XP's
    // will readDst and PD XP's don't read dst.
    if ((colorPOI.readsDst() || coveragePOI.readsDst()) &&
        kOne_GrBlendCoeff == fSrcCoeff && kZero_GrBlendCoeff == fDstCoeff) {
        return true;
    }

    return false;
}

bool GrPorterDuffXPFactory::willBlendWithDst(const GrProcOptInfo& colorPOI,
                                             const GrProcOptInfo& coveragePOI,
                                             bool isCoverageDrawing,
                                             bool colorWriteDisabled) const {
    if (!(isCoverageDrawing || coveragePOI.isSolidWhite())) {
        return true;
    }

    // TODO: once all SkXferEffects are XP's then we will never reads dst here since only XP's
    // will readDst and PD XP's don't read dst.
    if ((!colorWriteDisabled && colorPOI.readsDst()) || coveragePOI.readsDst()) {
        return true;
    }

    if (GrBlendCoeffRefsDst(fSrcCoeff)) {
        return true;
    }

    bool srcAIsOne = colorPOI.isOpaque() && (!isCoverageDrawing || coveragePOI.isOpaque());

    if (!(kZero_GrBlendCoeff == fDstCoeff ||
          (kISA_GrBlendCoeff == fDstCoeff && srcAIsOne))) {
        return true;
    }

    return false;
}

bool GrPorterDuffXPFactory::canTweakAlphaForCoverage(bool isCoverageDrawing) const {
    return can_tweak_alpha_for_coverage(fDstCoeff, isCoverageDrawing);
}

bool GrPorterDuffXPFactory::getOpaqueAndKnownColor(const GrProcOptInfo& colorPOI,
                                                   const GrProcOptInfo& coveragePOI,
                                                   GrColor* solidColor,
                                                   uint32_t* solidColorKnownComponents) const {
    if (!coveragePOI.isSolidWhite()) {
        return false;
    }

    SkASSERT((NULL == solidColor) == (NULL == solidColorKnownComponents));

    GrBlendCoeff srcCoeff = fSrcCoeff;
    GrBlendCoeff dstCoeff = fDstCoeff;

    // TODO: figure out to merge this simplify with other current optimization code paths and
    // eventually remove from GrBlend
    GrSimplifyBlend(&srcCoeff, &dstCoeff, colorPOI.color(), colorPOI.validFlags(),
                    0, 0, 0);

    bool opaque = kZero_GrBlendCoeff == dstCoeff && !GrBlendCoeffRefsDst(srcCoeff);
    if (solidColor) {
        if (opaque) {
            switch (srcCoeff) {
                case kZero_GrBlendCoeff:
                    *solidColor = 0;
                    *solidColorKnownComponents = kRGBA_GrColorComponentFlags;
                    break;

                case kOne_GrBlendCoeff:
                    *solidColor = colorPOI.color();
                    *solidColorKnownComponents = colorPOI.validFlags();
                    break;

                // The src coeff should never refer to the src and if it refers to dst then opaque
                // should have been false.
                case kSC_GrBlendCoeff:
                case kISC_GrBlendCoeff:
                case kDC_GrBlendCoeff:
                case kIDC_GrBlendCoeff:
                case kSA_GrBlendCoeff:
                case kISA_GrBlendCoeff:
                case kDA_GrBlendCoeff:
                case kIDA_GrBlendCoeff:
                default:
                    SkFAIL("srcCoeff should not refer to src or dst.");
                    break;

                // TODO: update this once GrPaint actually has a const color.
                case kConstC_GrBlendCoeff:
                case kIConstC_GrBlendCoeff:
                case kConstA_GrBlendCoeff:
                case kIConstA_GrBlendCoeff:
                    *solidColorKnownComponents = 0;
                    break;
            }
        } else {
            solidColorKnownComponents = 0;
        }
    }
    return opaque;
}

