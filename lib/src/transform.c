#include "colorist/transform.h"

#include "colorist/context.h"
#include "colorist/pixelmath.h"
#include "colorist/profile.h"
#include "colorist/task.h"

#include "gb_math.h"

#include <string.h>

#define SRC_8_HAS_ALPHA() (srcPixelBytes > 3)
#define SRC_16_HAS_ALPHA() (srcPixelBytes > 7)
#define SRC_FLOAT_HAS_ALPHA() (srcPixelBytes > 15)

#define DST_8_HAS_ALPHA() (dstPixelBytes > 3)
#define DST_16_HAS_ALPHA() (dstPixelBytes > 7)
#define DST_FLOAT_HAS_ALPHA() (dstPixelBytes > 15)

// ----------------------------------------------------------------------------
// Debug Helpers

#if defined(DEBUG_MATRIX_MATH)
static void DEBUG_PRINT_MATRIX(const char * name, gbMat3 * m)
{
    printf("mat: %s\n", name);
    printf("  %g    %g    %g\n", m->x.x, m->y.x, m->z.x);
    printf("  %g    %g    %g\n", m->x.y, m->y.y, m->z.y);
    printf("  %g    %g    %g\n", m->x.z, m->y.z, m->z.z);
}

static void DEBUG_PRINT_VECTOR(const char * name, gbVec3 * v)
{
    printf("vec: %s\n", name);
    printf("  %g    %g    %g\n", v->x, v->y, v->z);
}
#else
#define DEBUG_PRINT_MATRIX(NAME, M)
#define DEBUG_PRINT_VECTOR(NAME, V)
#endif

// ----------------------------------------------------------------------------
// Color Conversion Math

// From http://docs-hoffmann.de/ciexyz29082000.pdf, Section 11.4
static clBool deriveXYZMatrixAndXTF(struct clContext * C, struct clProfile * profile, gbMat3 * toXYZ, clTransformTransferFunction * xtf, float * gamma)
{
    if (profile) {
        clProfilePrimaries primaries = { 0 };
        clProfileCurve curve = { 0 };
        int luminance = 0;

        gbVec3 U, W;
        gbMat3 P, PInv, D;

        if (clProfileHasPQSignature(C, profile, &primaries)) {
            *xtf = CL_XTF_PQ;
            *gamma = 0.0f;
        } else if (clProfileQuery(C, profile, &primaries, &curve, &luminance)) {
            *xtf = CL_XTF_GAMMA;
            *gamma = curve.gamma;
        } else {
            clContextLogError(C, "deriveXYZMatrix: fatal error querying profile");
            return clFalse;
        }

        P.col[0].x = primaries.red[0];
        P.col[0].y = primaries.red[1];
        P.col[0].z = 1 - primaries.red[0] - primaries.red[1];
        P.col[1].x = primaries.green[0];
        P.col[1].y = primaries.green[1];
        P.col[1].z = 1 - primaries.green[0] - primaries.green[1];
        P.col[2].x = primaries.blue[0];
        P.col[2].y = primaries.blue[1];
        P.col[2].z = 1 - primaries.blue[0] - primaries.blue[1];
        DEBUG_PRINT_MATRIX("P", &P);

        gb_mat3_inverse(&PInv, &P);
        DEBUG_PRINT_MATRIX("PInv", &PInv);

        W.x = primaries.white[0];
        W.y = primaries.white[1];
        W.z = 1 - primaries.white[0] - primaries.white[1];
        DEBUG_PRINT_VECTOR("W", &W);

        gb_mat3_mul_vec3(&U, &PInv, W);
        DEBUG_PRINT_VECTOR("U", &U);

        memset(&D, 0, sizeof(D));
        D.col[0].x = U.x / W.y;
        D.col[1].y = U.y / W.y;
        D.col[2].z = U.z / W.y;
        DEBUG_PRINT_MATRIX("D", &D);

        gb_mat3_mul(toXYZ, &P, &D);
        gb_mat3_transpose(toXYZ);
        DEBUG_PRINT_MATRIX("Cxr", toXYZ);
    } else {
        // No profile; we're already XYZ!
        *xtf = CL_XTF_NONE;
        *gamma = 0.0f;
        gb_mat3_identity(toXYZ);
    }
    return clTrue;
}

void clTransformPrepare(struct clContext * C, struct clTransform * transform)
{
    if (!transform->ccmmReady) {
        gbMat3 srcToXYZ;
        gbMat3 dstToXYZ;
        gbMat3 XYZtoDst;

        deriveXYZMatrixAndXTF(C, transform->srcProfile, &srcToXYZ, &transform->srcEOTF, &transform->srcGamma);
        deriveXYZMatrixAndXTF(C, transform->dstProfile, &dstToXYZ, &transform->dstOETF, &transform->dstInvGamma);
        if ((transform->dstOETF == CL_XTF_GAMMA) && (transform->dstInvGamma != 0.0f)) {
            transform->dstInvGamma = 1.0f / transform->dstInvGamma;
        }
        gb_mat3_inverse(&XYZtoDst, &dstToXYZ);
        gb_mat3_transpose(&XYZtoDst);

        DEBUG_PRINT_MATRIX("XYZtoDst", &XYZtoDst);
        DEBUG_PRINT_MATRIX("MA", &srcToXYZ);
        DEBUG_PRINT_MATRIX("MB", &XYZtoDst);
        gb_mat3_mul(&transform->matSrcToDst, &srcToXYZ, &XYZtoDst);
        // gb_mat3_transpose(&transform->matSrcToDst);
        DEBUG_PRINT_MATRIX("MA*MB", &transform->matSrcToDst);

        transform->ccmmReady = clTrue;
    }
}

// SMPTE ST.2084: https://ieeexplore.ieee.org/servlet/opac?punumber=7291450

static const float PQ_C1 = 0.8359375;       // 3424.0 / 4096.0
static const float PQ_C2 = 18.8515625;      // 2413.0 / 4096.0 * 32.0
static const float PQ_C3 = 18.6875;         // 2392.0 / 4096.0 * 32.0
static const float PQ_M1 = 0.1593017578125; // 2610.0 / 4096.0 / 4.0
static const float PQ_M2 = 78.84375;        // 2523.0 / 4096.0 * 128.0

// SMPTE ST.2084: Equation 4.1
// L = ( (max(N^(1/m2) - c1, 0)) / (c2 - c3*N^(1/m2)) )^(1/m1)
static float PQ_EOTF(float N)
{
    float N1m2, N1m2c1, c2c3N1m2;

    N1m2 = powf(N, 1 / PQ_M2);
    N1m2c1 = N1m2 - PQ_C1;
    if (N1m2c1 < 0.0f)
        N1m2c1 = 0.0f;
    c2c3N1m2 = PQ_C2 - (PQ_C3 * N1m2);
    return powf(N1m2c1 / c2c3N1m2, 1 / PQ_M1);
}

// SMPTE ST.2084: Equation 5.2
// N = ( (c1 + (c2 * L^m1)) / (1 + (c3 * L^m1)) )^m2
static float PQ_OETF(float L)
{
    float Lm1, c2Lm1, c3Lm1;

    Lm1 = powf(L, PQ_M1);
    c2Lm1 = PQ_C2 * Lm1;
    c3Lm1 = PQ_C3 * Lm1;
    return powf((PQ_C1 + c2Lm1) / (1 + c3Lm1), PQ_M2);
}

// The real color conversion function
static void transformFloatToFloat(struct clContext * C, struct clTransform * transform, uint8_t * srcPixels, int srcPixelBytes, uint8_t * dstPixels, int dstPixelBytes, int pixelCount)
{
    int i;
    for (i = 0; i < pixelCount; ++i) {
        float * srcPixel = (float *)&srcPixels[i * srcPixelBytes];
        float * dstPixel = (float *)&dstPixels[i * dstPixelBytes];
        gbVec3 src;
        float tmp[3];
        switch (transform->srcEOTF) {
            case CL_XTF_NONE:
                memcpy(&src, srcPixel, sizeof(src));
                break;
            case CL_XTF_GAMMA:
                src.x = powf((srcPixel[0] >= 0.0f) ? srcPixel[0] : 0.0f, transform->srcGamma);
                src.y = powf((srcPixel[1] >= 0.0f) ? srcPixel[1] : 0.0f, transform->srcGamma);
                src.z = powf((srcPixel[2] >= 0.0f) ? srcPixel[2] : 0.0f, transform->srcGamma);
                break;
            case CL_XTF_PQ:
                src.x = PQ_EOTF((srcPixel[0] >= 0.0f) ? srcPixel[0] : 0.0f);
                src.y = PQ_EOTF((srcPixel[1] >= 0.0f) ? srcPixel[1] : 0.0f);
                src.z = PQ_EOTF((srcPixel[2] >= 0.0f) ? srcPixel[2] : 0.0f);
                break;
        }
        switch (transform->dstOETF) {
            case CL_XTF_NONE:
                gb_mat3_mul_vec3((gbVec3 *)dstPixel, &transform->matSrcToDst, src);
                break;
            case CL_XTF_GAMMA:
                gb_mat3_mul_vec3((gbVec3 *)tmp, &transform->matSrcToDst, src);
                dstPixel[0] = powf((tmp[0] >= 0.0f) ? tmp[0] : 0.0f, transform->dstInvGamma);
                dstPixel[1] = powf((tmp[1] >= 0.0f) ? tmp[1] : 0.0f, transform->dstInvGamma);
                dstPixel[2] = powf((tmp[2] >= 0.0f) ? tmp[2] : 0.0f, transform->dstInvGamma);
                break;
            case CL_XTF_PQ:
                gb_mat3_mul_vec3((gbVec3 *)tmp, &transform->matSrcToDst, src);
                dstPixel[0] = PQ_OETF((tmp[0] >= 0.0f) ? tmp[0] : 0.0f);
                dstPixel[1] = PQ_OETF((tmp[1] >= 0.0f) ? tmp[1] : 0.0f);
                dstPixel[2] = PQ_OETF((tmp[2] >= 0.0f) ? tmp[2] : 0.0f);
                break;
        }
        if (DST_FLOAT_HAS_ALPHA()) {
            if (SRC_FLOAT_HAS_ALPHA()) {
                // Copy alpha
                dstPixel[3] = srcPixel[3];
            } else {
                // Full alpha
                dstPixel[3] = 1.0f;
            }
        }
    }
}

// ----------------------------------------------------------------------------
// Transform wrappers for RGB/RGBA

static void transformFloatToRGB8(struct clContext * C, struct clTransform * transform, uint8_t * srcPixels, int srcPixelBytes, uint8_t * dstPixels, int dstPixelBytes, int pixelCount)
{
    static const float maxChannel = 255.0f;
    int i;
    for (i = 0; i < pixelCount; ++i) {
        float tmpPixel[4];
        float * srcPixel = (float *)&srcPixels[i * srcPixelBytes];
        uint8_t * dstPixel = &dstPixels[i * dstPixelBytes];
        transformFloatToFloat(C, transform, (uint8_t *)srcPixel, srcPixelBytes, (uint8_t *)tmpPixel, dstPixelBytes, 1);
        dstPixel[0] = (uint8_t)clPixelMathRoundf(tmpPixel[0] * maxChannel);
        dstPixel[1] = (uint8_t)clPixelMathRoundf(tmpPixel[1] * maxChannel);
        dstPixel[2] = (uint8_t)clPixelMathRoundf(tmpPixel[2] * maxChannel);
        if (DST_8_HAS_ALPHA()) {
            if (SRC_FLOAT_HAS_ALPHA()) {
                // reformat alpha
                dstPixel[3] = (uint8_t)clPixelMathRoundf(tmpPixel[3] * maxChannel);
            } else {
                // RGB -> RGBA, set full opacity
                dstPixel[3] = 255;
            }
        }
    }
}

static void transformFloatToRGB16(struct clContext * C, struct clTransform * transform, uint8_t * srcPixels, int srcPixelBytes, uint8_t * dstPixels, int dstPixelBytes, int dstDepth, int pixelCount)
{
    const int dstMaxChannel = (1 << dstDepth) - 1;
    const float dstRescale = (float)dstMaxChannel;
    int i;
    for (i = 0; i < pixelCount; ++i) {
        float tmpPixel[4];
        float * srcPixel = (float *)&srcPixels[i * srcPixelBytes];
        uint16_t * dstPixel = (uint16_t *)&dstPixels[i * dstPixelBytes];
        transformFloatToFloat(C, transform, (uint8_t *)srcPixel, srcPixelBytes, (uint8_t *)tmpPixel, dstPixelBytes, 1);
        dstPixel[0] = (uint16_t)clPixelMathRoundf(tmpPixel[0] * dstRescale);
        dstPixel[1] = (uint16_t)clPixelMathRoundf(tmpPixel[1] * dstRescale);
        dstPixel[2] = (uint16_t)clPixelMathRoundf(tmpPixel[2] * dstRescale);
        if (DST_16_HAS_ALPHA()) {
            if (SRC_FLOAT_HAS_ALPHA()) {
                // reformat alpha
                dstPixel[3] = (uint16_t)clPixelMathRoundf(tmpPixel[3] * dstRescale);
            } else {
                // RGB -> RGBA, set full opacity
                dstPixel[3] = dstMaxChannel;
            }
        }
    }
}

static void transformRGB8ToFloat(struct clContext * C, struct clTransform * transform, uint8_t * srcPixels, int srcPixelBytes, uint8_t * dstPixels, int dstPixelBytes, int pixelCount)
{
    static const float srcRescale = 1.0f / 255.0f;
    int i;
    for (i = 0; i < pixelCount; ++i) {
        float tmpPixel[4];
        uint8_t * srcPixel = &srcPixels[i * srcPixelBytes];
        float * dstPixel = (float *)&dstPixels[i * dstPixelBytes];
        tmpPixel[0] = (float)srcPixel[0] * srcRescale;
        tmpPixel[1] = (float)srcPixel[1] * srcRescale;
        tmpPixel[2] = (float)srcPixel[2] * srcRescale;
        if (DST_FLOAT_HAS_ALPHA()) {
            if (SRC_8_HAS_ALPHA()) {
                // reformat alpha
                tmpPixel[3] = (float)srcPixel[3] * srcRescale;
            } else {
                // RGB -> RGBA, set full opacity
                tmpPixel[3] = 1.0f;
            }
        }
        transformFloatToFloat(C, transform, (uint8_t *)tmpPixel, dstPixelBytes, dstPixels, dstPixelBytes, 1);
    }
}

static void transformRGB16ToFloat(struct clContext * C, struct clTransform * transform, uint8_t * srcPixels, int srcPixelBytes, int srcDepth, uint8_t * dstPixels, int dstPixelBytes, int pixelCount)
{
    const float srcRescale = 1.0f / (float)((1 << srcDepth) - 1);
    int i;
    for (i = 0; i < pixelCount; ++i) {
        float tmpPixel[4];
        uint16_t * srcPixel = (uint16_t *)&srcPixels[i * srcPixelBytes];
        float * dstPixel = (float *)&dstPixels[i * dstPixelBytes];
        tmpPixel[0] = (float)srcPixel[0] * srcRescale;
        tmpPixel[1] = (float)srcPixel[1] * srcRescale;
        tmpPixel[2] = (float)srcPixel[2] * srcRescale;
        if (DST_FLOAT_HAS_ALPHA()) {
            if (SRC_16_HAS_ALPHA()) {
                // reformat alpha
                tmpPixel[3] = (float)srcPixel[3] * srcRescale;
            } else {
                // RGB -> RGBA, set full opacity
                tmpPixel[3] = 1.0f;
            }
        }
        transformFloatToFloat(C, transform, (uint8_t *)tmpPixel, dstPixelBytes, dstPixels, dstPixelBytes, 1);
    }
}

static void transformRGB8ToRGB8(struct clContext * C, struct clTransform * transform, uint8_t * srcPixels, int srcPixelBytes, uint8_t * dstPixels, int dstPixelBytes, int pixelCount)
{
    const float srcRescale = 1.0f / 255.0f;
    const float dstRescale = 255.0f;
    int i;
    for (i = 0; i < pixelCount; ++i) {
        float tmpSrc[4];
        float tmpDst[4];
        uint8_t * srcPixel = &srcPixels[i * srcPixelBytes];
        uint8_t * dstPixel = &dstPixels[i * dstPixelBytes];
        int tmpSrcBytes;
        int tmpDstBytes;

        tmpSrc[0] = (float)srcPixel[0] * srcRescale;
        tmpSrc[1] = (float)srcPixel[1] * srcRescale;
        tmpSrc[2] = (float)srcPixel[2] * srcRescale;
        if (SRC_8_HAS_ALPHA()) {
            tmpSrc[3] = (float)srcPixel[3] * srcRescale;
            tmpSrcBytes = 16;
        } else {
            tmpSrcBytes = 12;
        }
        if (DST_8_HAS_ALPHA()) {
            tmpDstBytes = 16;
        } else {
            tmpDstBytes = 12;
        }

        transformFloatToFloat(C, transform, (uint8_t *)tmpSrc, tmpSrcBytes, (uint8_t *)tmpDst, tmpDstBytes, 1);

        dstPixel[0] = (uint8_t)clPixelMathRoundf((float)tmpDst[0] * dstRescale);
        dstPixel[1] = (uint8_t)clPixelMathRoundf((float)tmpDst[1] * dstRescale);
        dstPixel[2] = (uint8_t)clPixelMathRoundf((float)tmpDst[2] * dstRescale);
        if (DST_8_HAS_ALPHA()) {
            if (SRC_8_HAS_ALPHA()) {
                // reformat alpha
                dstPixel[3] = (uint8_t)clPixelMathRoundf((float)tmpDst[3] * dstRescale);
            } else {
                // RGB -> RGBA, set full opacity
                dstPixel[3] = 255;
            }
        }
    }
}

static void transformRGB8ToRGB16(struct clContext * C, struct clTransform * transform, uint8_t * srcPixels, int srcPixelBytes, uint8_t * dstPixels, int dstPixelBytes, int dstDepth, int pixelCount)
{
    static const float srcRescale = 1.0f / 255.0f;
    const int dstMaxChannel = (1 << dstDepth) - 1;
    const float dstRescale = (float)dstMaxChannel;
    int i;
    for (i = 0; i < pixelCount; ++i) {
        float tmpSrc[4];
        float tmpDst[4];
        uint8_t * srcPixel = &srcPixels[i * srcPixelBytes];
        uint16_t * dstPixel = (uint16_t *)&dstPixels[i * dstPixelBytes];
        int tmpSrcBytes;
        int tmpDstBytes;

        tmpSrc[0] = (float)srcPixel[0] * srcRescale;
        tmpSrc[1] = (float)srcPixel[1] * srcRescale;
        tmpSrc[2] = (float)srcPixel[2] * srcRescale;
        if (SRC_8_HAS_ALPHA()) {
            tmpSrc[3] = (float)srcPixel[3] * srcRescale;
            tmpSrcBytes = 16;
        } else {
            tmpSrcBytes = 12;
        }
        if (DST_16_HAS_ALPHA()) {
            tmpDstBytes = 16;
        } else {
            tmpDstBytes = 12;
        }

        transformFloatToFloat(C, transform, (uint8_t *)tmpSrc, tmpSrcBytes, (uint8_t *)tmpDst, tmpDstBytes, 1);

        dstPixel[0] = (uint16_t)clPixelMathRoundf((float)tmpDst[0] * dstRescale);
        dstPixel[1] = (uint16_t)clPixelMathRoundf((float)tmpDst[1] * dstRescale);
        dstPixel[2] = (uint16_t)clPixelMathRoundf((float)tmpDst[2] * dstRescale);
        if (DST_16_HAS_ALPHA()) {
            if (SRC_8_HAS_ALPHA()) {
                // reformat alpha
                dstPixel[3] = (uint16_t)clPixelMathRoundf((float)tmpDst[3] * dstRescale);
            } else {
                // RGB -> RGBA, set full opacity
                dstPixel[3] = dstMaxChannel;
            }
        }
    }
}

static void transformRGB16ToRGB8(struct clContext * C, struct clTransform * transform, uint8_t * srcPixels, int srcPixelBytes, int srcDepth, uint8_t * dstPixels, int dstPixelBytes, int pixelCount)
{
    const int srcMaxChannel = (1 << srcDepth) - 1;
    const float srcRescale = 1.0f / (float)srcMaxChannel;
    static const float dstRescale = 255.0f;
    int i;
    for (i = 0; i < pixelCount; ++i) {
        float tmpSrc[4];
        float tmpDst[4];
        uint16_t * srcPixel = (uint16_t *)&srcPixels[i * srcPixelBytes];
        uint8_t * dstPixel = &dstPixels[i * dstPixelBytes];
        int tmpSrcBytes;
        int tmpDstBytes;

        tmpSrc[0] = (float)srcPixel[0] * srcRescale;
        tmpSrc[1] = (float)srcPixel[1] * srcRescale;
        tmpSrc[2] = (float)srcPixel[2] * srcRescale;
        if (SRC_16_HAS_ALPHA()) {
            tmpSrc[3] = (float)srcPixel[3] * srcRescale;
            tmpSrcBytes = 16;
        } else {
            tmpSrcBytes = 12;
        }
        if (DST_8_HAS_ALPHA()) {
            tmpDstBytes = 16;
        } else {
            tmpDstBytes = 12;
        }

        transformFloatToFloat(C, transform, (uint8_t *)tmpSrc, tmpSrcBytes, (uint8_t *)tmpDst, tmpDstBytes, 1);

        dstPixel[0] = (uint8_t)clPixelMathRoundf((float)tmpDst[0] * dstRescale);
        dstPixel[1] = (uint8_t)clPixelMathRoundf((float)tmpDst[1] * dstRescale);
        dstPixel[2] = (uint8_t)clPixelMathRoundf((float)tmpDst[2] * dstRescale);
        if (DST_8_HAS_ALPHA()) {
            if (SRC_16_HAS_ALPHA()) {
                // reformat alpha
                dstPixel[3] = (uint8_t)clPixelMathRoundf((float)tmpDst[3] * dstRescale);
            } else {
                // RGB -> RGBA, set full opacity
                dstPixel[3] = 255;
            }
        }
    }
}

static void transformRGB16ToRGB16(struct clContext * C, struct clTransform * transform, uint8_t * srcPixels, int srcPixelBytes, int srcDepth, uint8_t * dstPixels, int dstPixelBytes, int dstDepth, int pixelCount)
{
    const int srcMaxChannel = (1 << srcDepth) - 1;
    const float srcRescale = 1.0f / (float)srcMaxChannel;
    const int dstMaxChannel = (1 << dstDepth) - 1;
    const float dstRescale = (float)dstMaxChannel;
    int i;
    for (i = 0; i < pixelCount; ++i) {
        float tmpSrc[4];
        float tmpDst[4];
        uint16_t * srcPixel = (uint16_t *)&srcPixels[i * srcPixelBytes];
        uint16_t * dstPixel = (uint16_t *)&dstPixels[i * dstPixelBytes];
        int tmpSrcBytes;
        int tmpDstBytes;

        tmpSrc[0] = (float)srcPixel[0] * srcRescale;
        tmpSrc[1] = (float)srcPixel[1] * srcRescale;
        tmpSrc[2] = (float)srcPixel[2] * srcRescale;
        if (SRC_16_HAS_ALPHA()) {
            tmpSrc[3] = (float)srcPixel[3] * srcRescale;
            tmpSrcBytes = 16;
        } else {
            tmpSrcBytes = 12;
        }
        if (DST_16_HAS_ALPHA()) {
            tmpDstBytes = 16;
        } else {
            tmpDstBytes = 12;
        }

        transformFloatToFloat(C, transform, (uint8_t *)tmpSrc, tmpSrcBytes, (uint8_t *)tmpDst, tmpDstBytes, 1);

        dstPixel[0] = (uint16_t)clPixelMathRoundf((float)tmpDst[0] * dstRescale);
        dstPixel[1] = (uint16_t)clPixelMathRoundf((float)tmpDst[1] * dstRescale);
        dstPixel[2] = (uint16_t)clPixelMathRoundf((float)tmpDst[2] * dstRescale);
        if (DST_16_HAS_ALPHA()) {
            if (SRC_16_HAS_ALPHA()) {
                // reformat alpha
                dstPixel[3] = (uint16_t)clPixelMathRoundf((float)tmpDst[3] * dstRescale);
            } else {
                // RGB -> RGBA, set full opacity
                dstPixel[3] = dstMaxChannel;
            }
        }
    }
}

// ----------------------------------------------------------------------------
// Reformatting

static void reformatFloatToFloat(struct clContext * C, uint8_t * srcPixels, int srcPixelBytes, uint8_t * dstPixels, int dstPixelBytes, int pixelCount)
{
    int i;
    for (i = 0; i < pixelCount; ++i) {
        float * srcPixel = (float *)&srcPixels[i * srcPixelBytes];
        float * dstPixel = (float *)&dstPixels[i * dstPixelBytes];
        memcpy(dstPixel, srcPixel, sizeof(float) * 3); // all float formats are at least 3 floats
        if (DST_FLOAT_HAS_ALPHA()) {
            if (SRC_FLOAT_HAS_ALPHA()) {
                dstPixel[3] = srcPixel[3];
            } else {
                // RGB -> RGBA, set full opacity
                dstPixel[3] = 1.0f;
            }
        }
    }
}

static void reformatFloatToRGB8(struct clContext * C, uint8_t * srcPixels, int srcPixelBytes, uint8_t * dstPixels, int dstPixelBytes, int pixelCount)
{
    static const float dstRescale = 255.0f;
    int i;
    for (i = 0; i < pixelCount; ++i) {
        float * srcPixel = (float *)&srcPixels[i * srcPixelBytes];
        uint8_t * dstPixel = &dstPixels[i * dstPixelBytes];
        dstPixel[0] = (uint8_t)clPixelMathRoundf(srcPixel[0] * dstRescale);
        dstPixel[1] = (uint8_t)clPixelMathRoundf(srcPixel[1] * dstRescale);
        dstPixel[2] = (uint8_t)clPixelMathRoundf(srcPixel[2] * dstRescale);
        if (DST_8_HAS_ALPHA()) {
            if (SRC_FLOAT_HAS_ALPHA()) {
                // reformat alpha
                dstPixel[3] = (uint8_t)clPixelMathRoundf(srcPixel[3] * dstRescale);
            } else {
                // RGB -> RGBA, set full opacity
                dstPixel[3] = 255;
            }
        }
    }
}

static void reformatFloatToRGB16(struct clContext * C, uint8_t * srcPixels, int srcPixelBytes, uint8_t * dstPixels, int dstPixelBytes, int dstDepth, int pixelCount)
{
    const int dstMaxChannel = (1 << dstDepth) - 1;
    const float dstRescale = (float)dstMaxChannel;
    int i;
    for (i = 0; i < pixelCount; ++i) {
        float * srcPixel = (float *)&srcPixels[i * srcPixelBytes];
        uint16_t * dstPixel = (uint16_t *)&dstPixels[i * dstPixelBytes];
        dstPixel[0] = (uint16_t)clPixelMathRoundf(srcPixel[0] * dstRescale);
        dstPixel[1] = (uint16_t)clPixelMathRoundf(srcPixel[1] * dstRescale);
        dstPixel[2] = (uint16_t)clPixelMathRoundf(srcPixel[2] * dstRescale);
        if (DST_16_HAS_ALPHA()) {
            if (SRC_FLOAT_HAS_ALPHA()) {
                // reformat alpha
                dstPixel[3] = (uint16_t)clPixelMathRoundf(srcPixel[3] * dstRescale);
            } else {
                // RGB -> RGBA, set full opacity
                dstPixel[3] = dstMaxChannel;
            }
        }
    }
}

static void reformatRGB8ToFloat(struct clContext * C, uint8_t * srcPixels, int srcPixelBytes, uint8_t * dstPixels, int dstPixelBytes, int pixelCount)
{
    static const float srcRescale = 1.0f / 255.0f;
    int i;
    for (i = 0; i < pixelCount; ++i) {
        uint8_t * srcPixel = &srcPixels[i * srcPixelBytes];
        float * dstPixel = (float *)&dstPixels[i * dstPixelBytes];
        dstPixel[0] = (float)srcPixel[0] * srcRescale;
        dstPixel[1] = (float)srcPixel[1] * srcRescale;
        dstPixel[2] = (float)srcPixel[2] * srcRescale;
        if (DST_FLOAT_HAS_ALPHA()) {
            if (SRC_8_HAS_ALPHA()) {
                // reformat alpha
                dstPixel[3] = (float)srcPixel[3] * srcRescale;
            } else {
                // RGB -> RGBA, set full opacity
                dstPixel[3] = 1.0f;
            }
        }
    }
}

static void reformatRGB16ToFloat(struct clContext * C, uint8_t * srcPixels, int srcPixelBytes, int srcDepth, uint8_t * dstPixels, int dstPixelBytes, int pixelCount)
{
    const int srcMaxChannel = (1 << srcDepth) - 1;
    const float srcRescale = 1.0f / (float)srcMaxChannel;
    int i;
    for (i = 0; i < pixelCount; ++i) {
        uint16_t * srcPixel = (uint16_t *)&srcPixels[i * srcPixelBytes];
        float * dstPixel = (float *)&dstPixels[i * dstPixelBytes];
        dstPixel[0] = (float)srcPixel[0] * srcRescale;
        dstPixel[1] = (float)srcPixel[1] * srcRescale;
        dstPixel[2] = (float)srcPixel[2] * srcRescale;
        if (DST_FLOAT_HAS_ALPHA()) {
            if (SRC_16_HAS_ALPHA()) {
                // reformat alpha
                dstPixel[3] = (float)srcPixel[3] * srcRescale;
            } else {
                // RGB -> RGBA, set full opacity
                dstPixel[3] = 1.0f;
            }
        }
    }
}

static void reformatRGB8ToRGB8(struct clContext * C, uint8_t * srcPixels, int srcPixelBytes, uint8_t * dstPixels, int dstPixelBytes, int pixelCount)
{
    int i;
    for (i = 0; i < pixelCount; ++i) {
        uint8_t * srcPixel = &srcPixels[i * srcPixelBytes];
        uint8_t * dstPixel = &dstPixels[i * dstPixelBytes];
        dstPixel[0] = srcPixel[0];
        dstPixel[1] = srcPixel[1];
        dstPixel[2] = srcPixel[2];
        if (DST_8_HAS_ALPHA()) {
            if (SRC_8_HAS_ALPHA()) {
                dstPixel[3] = srcPixel[3];
            } else {
                // RGB -> RGBA, set full opacity
                dstPixel[3] = 255;
            }
        }
    }
}

static void reformatRGB16ToRGB16(struct clContext * C, uint8_t * srcPixels, int srcPixelBytes, int srcDepth, uint8_t * dstPixels, int dstPixelBytes, int dstDepth, int pixelCount)
{
    const int srcMaxChannel = (1 << srcDepth) - 1;
    const float srcRescale = 1.0f / (float)srcMaxChannel;
    const int dstMaxChannel = (1 << dstDepth) - 1;
    const float dstRescale = (float)dstMaxChannel;
    const float rescale = srcRescale * dstRescale;
    int i;
    for (i = 0; i < pixelCount; ++i) {
        uint16_t * srcPixel = (uint16_t *)&srcPixels[i * srcPixelBytes];
        uint16_t * dstPixel = (uint16_t *)&dstPixels[i * dstPixelBytes];
        dstPixel[0] = (uint16_t)((float)srcPixel[0] * rescale);
        dstPixel[1] = (uint16_t)((float)srcPixel[1] * rescale);
        dstPixel[2] = (uint16_t)((float)srcPixel[2] * rescale);
        if (DST_16_HAS_ALPHA()) {
            if (SRC_16_HAS_ALPHA()) {
                dstPixel[3] = (uint16_t)((float)srcPixel[3] * rescale);
            } else {
                // RGB -> RGBA, set full opacity
                dstPixel[3] = dstMaxChannel;
            }
        }
    }
}

static void reformatRGB8ToRGB16(struct clContext * C, uint8_t * srcPixels, int srcPixelBytes, uint8_t * dstPixels, int dstPixelBytes, int dstDepth, int pixelCount)
{
    const int dstMaxChannel = (1 << dstDepth) - 1;
    const float dstRescale = (float)dstMaxChannel;
    const float rescale = dstRescale / 255.0f;
    int i;
    for (i = 0; i < pixelCount; ++i) {
        uint8_t * srcPixel = &srcPixels[i * srcPixelBytes];
        uint16_t * dstPixel = (uint16_t *)&dstPixels[i * dstPixelBytes];
        dstPixel[0] = (uint16_t)clPixelMathRoundf((float)srcPixel[0] * rescale);
        dstPixel[1] = (uint16_t)clPixelMathRoundf((float)srcPixel[1] * rescale);
        dstPixel[2] = (uint16_t)clPixelMathRoundf((float)srcPixel[2] * rescale);
        if (DST_16_HAS_ALPHA()) {
            if (SRC_8_HAS_ALPHA()) {
                // reformat alpha
                dstPixel[3] = (uint16_t)clPixelMathRoundf((float)srcPixel[3] * rescale);
            } else {
                // RGB -> RGBA, set full opacity
                dstPixel[3] = dstMaxChannel;
            }
        }
    }
}

static void reformatRGB16ToRGB8(struct clContext * C, uint8_t * srcPixels, int srcPixelBytes, int srcDepth, uint8_t * dstPixels, int dstPixelBytes, int pixelCount)
{
    const int srcMaxChannel = (1 << srcDepth) - 1;
    const float rescale = 255.0f / (float)srcMaxChannel;
    int i;
    for (i = 0; i < pixelCount; ++i) {
        uint16_t * srcPixel = (uint16_t *)&srcPixels[i * srcPixelBytes];
        uint8_t * dstPixel = &dstPixels[i * dstPixelBytes];
        dstPixel[0] = (uint8_t)clPixelMathRoundf((float)srcPixel[0] * rescale);
        dstPixel[1] = (uint8_t)clPixelMathRoundf((float)srcPixel[1] * rescale);
        dstPixel[2] = (uint8_t)clPixelMathRoundf((float)srcPixel[2] * rescale);
        if (DST_8_HAS_ALPHA()) {
            if (SRC_16_HAS_ALPHA()) {
                // reformat alpha
                dstPixel[3] = (uint8_t)clPixelMathRoundf((float)srcPixel[3] * rescale);
            } else {
                // RGB -> RGBA, set full opacity
                dstPixel[3] = 255;
            }
        }
    }
}

// ----------------------------------------------------------------------------
// Transform entry point

#define USES_UINT8_T(V) (V == 8)
#define USES_UINT16_T(V) ((V >= 9) && (V <= 16))

void clCCMMTransform(struct clContext * C, struct clTransform * transform, void * srcPixels, void * dstPixels, int pixelCount)
{
    int srcDepth = transform->srcDepth;
    int dstDepth = transform->dstDepth;
    int srcPixelBytes = clTransformFormatToPixelBytes(C, transform->srcFormat, srcDepth);
    int dstPixelBytes = clTransformFormatToPixelBytes(C, transform->dstFormat, dstDepth);

    COLORIST_ASSERT(!transform->srcProfile || transform->srcProfile->ccmm);
    COLORIST_ASSERT(!transform->dstProfile || transform->dstProfile->ccmm);
    COLORIST_ASSERT(transform->ccmmReady);

    // After this point, find a single valid return point from this function, or die

    if (clProfileMatches(C, transform->srcProfile, transform->dstProfile)) {
        // No color conversion necessary, just format conversion

        if (clTransformFormatIsFloat(C, transform->srcFormat, srcDepth) && clTransformFormatIsFloat(C, transform->dstFormat, dstDepth)) {
            // Float to Float, losing or gaining alpha
            reformatFloatToFloat(C, srcPixels, srcPixelBytes, dstPixels, dstPixelBytes, pixelCount);
            return;
        } else if (clTransformFormatIsFloat(C, transform->srcFormat, srcDepth)) {
            // Float -> 8 or 16
            if (USES_UINT8_T(dstDepth)) {
                reformatFloatToRGB8(C, srcPixels, srcPixelBytes, dstPixels, dstPixelBytes, pixelCount);
                return;
            } else if (USES_UINT16_T(dstDepth)) {
                reformatFloatToRGB16(C, srcPixels, srcPixelBytes, dstPixels, dstPixelBytes, dstDepth, pixelCount);
                return;
            }
        } else if (clTransformFormatIsFloat(C, transform->dstFormat, dstDepth)) {
            // 8 or 16 -> Float
            if (USES_UINT8_T(srcDepth)) {
                reformatRGB8ToFloat(C, srcPixels, srcPixelBytes, dstPixels, dstPixelBytes, pixelCount);
                return;
            } else if (USES_UINT16_T(srcDepth)) {
                reformatRGB16ToFloat(C, srcPixels, srcPixelBytes, srcDepth, dstPixels, dstPixelBytes, pixelCount);
                return;
            }
        } else {
            // 8 or 16 -> 8 or 16
            if (USES_UINT8_T(srcDepth) && USES_UINT8_T(dstDepth)) {
                reformatRGB8ToRGB8(C, srcPixels, srcPixelBytes, dstPixels, dstPixelBytes, pixelCount);
                return;
            } else if (USES_UINT8_T(srcDepth) && USES_UINT16_T(dstDepth)) {
                reformatRGB8ToRGB16(C, srcPixels, srcPixelBytes, dstPixels, dstPixelBytes, dstDepth, pixelCount);
                return;
            } else if (USES_UINT16_T(srcDepth) && USES_UINT8_T(dstDepth)) {
                reformatRGB16ToRGB8(C, srcPixels, srcPixelBytes, srcDepth, dstPixels, dstPixelBytes, pixelCount);
                return;
            } else if (USES_UINT16_T(srcDepth) && USES_UINT16_T(dstDepth)) {
                reformatRGB16ToRGB16(C, srcPixels, srcPixelBytes, srcDepth, dstPixels, dstPixelBytes, dstDepth, pixelCount);
                return;
            }
        }
    } else {
        // Color conversion is required

        if (clTransformFormatIsFloat(C, transform->srcFormat, srcDepth) && clTransformFormatIsFloat(C, transform->dstFormat, dstDepth)) {
            // Float to Float
            transformFloatToFloat(C, transform, srcPixels, srcPixelBytes, dstPixels, dstPixelBytes, pixelCount);
            return;
        } else if (clTransformFormatIsFloat(C, transform->srcFormat, srcDepth)) {
            // Float -> 8 or 16
            if (USES_UINT8_T(dstDepth)) {
                transformFloatToRGB8(C, transform, srcPixels, srcPixelBytes, dstPixels, dstPixelBytes, pixelCount);
                return;
            } else if (USES_UINT16_T(dstDepth)) {
                transformFloatToRGB16(C, transform, srcPixels, srcPixelBytes, dstPixels, dstPixelBytes, dstDepth, pixelCount);
                return;
            }
        } else if (clTransformFormatIsFloat(C, transform->dstFormat, dstDepth)) {
            // 8 or 16 -> Float
            if (USES_UINT8_T(srcDepth)) {
                transformRGB8ToFloat(C, transform, srcPixels, srcPixelBytes, dstPixels, dstPixelBytes, pixelCount);
                return;
            } else if (USES_UINT16_T(srcDepth)) {
                transformRGB16ToFloat(C, transform, srcPixels, srcPixelBytes, srcDepth, dstPixels, dstPixelBytes, pixelCount);
                return;
            }
        } else {
            // 8 or 16 -> 8 or 16
            if (USES_UINT8_T(srcDepth) && USES_UINT8_T(dstDepth)) {
                transformRGB8ToRGB8(C, transform, srcPixels, srcPixelBytes, dstPixels, dstPixelBytes, pixelCount);
                return;
            } else if (USES_UINT8_T(srcDepth) && USES_UINT16_T(dstDepth)) {
                transformRGB8ToRGB16(C, transform, srcPixels, srcPixelBytes, dstPixels, dstPixelBytes, dstDepth, pixelCount);
                return;
            } else if (USES_UINT16_T(srcDepth) && USES_UINT8_T(dstDepth)) {
                transformRGB16ToRGB8(C, transform, srcPixels, srcPixelBytes, srcDepth, dstPixels, dstPixelBytes, pixelCount);
                return;
            } else if (USES_UINT16_T(srcDepth) && USES_UINT16_T(dstDepth)) {
                transformRGB16ToRGB16(C, transform, srcPixels, srcPixelBytes, srcDepth, dstPixels, dstPixelBytes, dstDepth, pixelCount);
                return;
            }
        }
    }

    COLORIST_FAILURE("clCCMMTransform: Failed to find correct conversion method");
}

// ----------------------------------------------------------------------------
// clTransform API

void clTransformXYZToXYY(struct clContext * C, float * dstXYY, float * srcXYZ, float whitePointX, float whitePointY)
{
    float sum = srcXYZ[0] + srcXYZ[1] + srcXYZ[2];
    if (sum <= 0.0f) {
        dstXYY[0] = whitePointX;
        dstXYY[1] = whitePointY;
        dstXYY[2] = 0.0f;
        return;
    }
    dstXYY[0] = srcXYZ[0] / sum;
    dstXYY[1] = srcXYZ[1] / sum;
    dstXYY[2] = srcXYZ[1];
}

void clTransformXYYToXYZ(struct clContext * C, float * dstXYZ, float * srcXYY)
{
    if (srcXYY[2] <= 0.0f) {
        dstXYZ[0] = 0.0f;
        dstXYZ[1] = 0.0f;
        dstXYZ[2] = 0.0f;
        return;
    }
    dstXYZ[0] = (srcXYY[0] * srcXYY[2]) / srcXYY[1];
    dstXYZ[1] = srcXYY[2];
    dstXYZ[2] = ((1 - srcXYY[0] - srcXYY[1]) * srcXYY[2]) / srcXYY[1];
}

clTransform * clTransformCreate(struct clContext * C, struct clProfile * srcProfile, clTransformFormat srcFormat, int srcDepth, struct clProfile * dstProfile, clTransformFormat dstFormat, int dstDepth)
{
    clTransform * transform = clAllocateStruct(clTransform);
    transform->srcProfile = srcProfile;
    transform->dstProfile = dstProfile;
    transform->srcFormat = srcFormat;
    transform->dstFormat = dstFormat;
    transform->srcDepth = srcDepth;
    transform->dstDepth = dstDepth;
    transform->ccmmReady = clFalse;
    transform->xyzProfile = NULL;
    transform->hTransform = NULL;
    return transform;
}

void clTransformDestroy(struct clContext * C, clTransform * transform)
{
    if (transform->hTransform) {
        cmsDeleteTransform(transform->hTransform);
    }
    if (transform->xyzProfile) {
        cmsCloseProfile(transform->xyzProfile);
    }
    clFree(transform);
}

static cmsUInt32Number clTransformFormatToLCMSFormat(struct clContext * C, clTransformFormat format)
{
    switch (format) {
        case CL_XF_XYZ:  return TYPE_XYZ_FLT;
        case CL_XF_RGB:
            return TYPE_RGB_FLT;
        case CL_XF_RGBA:
            return TYPE_RGBA_FLT;
    }

    COLORIST_FAILURE("clTransformFormatToLCMSFormat: Unknown transform format");
    return TYPE_RGBA_FLT;
}

clBool clTransformFormatIsFloat(struct clContext * C, clTransformFormat format, int depth)
{
    switch (format) {
        case CL_XF_XYZ:
            return clTrue;
        case CL_XF_RGB:
        case CL_XF_RGBA:
            return depth == 32;
    }
    return clFalse;
}

int clTransformFormatToPixelBytes(struct clContext * C, clTransformFormat format, int depth)
{
    switch (format) {
        case CL_XF_XYZ:
            return sizeof(float) * 3;

        case CL_XF_RGB:
            if (depth == 32)
                return sizeof(float) * 3;
            else if (depth > 8)
                return sizeof(uint16_t) * 3;
            else
                return sizeof(uint8_t) * 3;

        case CL_XF_RGBA:
            if (depth == 32)
                return sizeof(float) * 4;
            else if (depth > 8)
                return sizeof(uint16_t) * 4;
            else
                return sizeof(uint8_t) * 4;
    }

    COLORIST_FAILURE("clTransformFormatToPixelBytes: Unknown transform format");
    return sizeof(float) * 4;
}

clBool clTransformUsesCCMM(struct clContext * C, clTransform * transform)
{
    clBool useCCMM = C->ccmmAllowed;
    if (!clProfileUsesCCMM(C, transform->srcProfile)) {
        useCCMM = clFalse;
    }
    if (!clProfileUsesCCMM(C, transform->dstProfile)) {
        useCCMM = clFalse;
    }
    return useCCMM;
}

const char * clTransformCMMName(struct clContext * C, clTransform * transform)
{
    return clTransformUsesCCMM(C, transform) ? "CCMM" : "LCMS";
}

// Perhaps kill this function and just merge it with clTransformRun?
static void doMultithreadedTransform(clContext * C, int taskCount, clTransform * transform, clBool useCCMM, uint8_t * srcPixels, int srcPixelBytes, uint8_t * dstPixels, int dstPixelBytes, int pixelCount);

void clTransformRun(struct clContext * C, clTransform * transform, int taskCount, void * srcPixels, void * dstPixels, int pixelCount)
{
    int srcPixelBytes = clTransformFormatToPixelBytes(C, transform->srcFormat, transform->srcDepth);
    int dstPixelBytes = clTransformFormatToPixelBytes(C, transform->dstFormat, transform->dstDepth);

    clBool useCCMM = clTransformUsesCCMM(C, transform);

    if (taskCount > 1) {
        clContextLog(C, "convert", 1, "Using %d threads to pixel transform.", taskCount);
    }

    if (useCCMM) {
        // Use colorist CMM
        clTransformPrepare(C, transform);
    } else {
        // Use LittleCMS
        if (!transform->hTransform) {
            cmsUInt32Number srcFormat = clTransformFormatToLCMSFormat(C, transform->srcFormat);
            cmsUInt32Number dstFormat = clTransformFormatToLCMSFormat(C, transform->dstFormat);
            cmsHPROFILE srcProfileHandle;
            cmsHPROFILE dstProfileHandle;

            // Choose src profile handle
            if (transform->srcProfile) {
                srcProfileHandle = transform->srcProfile->handle;
            } else {
                if (!transform->xyzProfile) {
                    transform->xyzProfile = cmsCreateXYZProfileTHR(C->lcms);
                }
                srcProfileHandle = transform->xyzProfile;
            }

            // Choose dst profile handle
            if (transform->dstProfile) {
                dstProfileHandle = transform->dstProfile->handle;
            } else {
                if (!transform->xyzProfile) {
                    transform->xyzProfile = cmsCreateXYZProfileTHR(C->lcms);
                }
                dstProfileHandle = transform->xyzProfile;
            }

            // Lazily create hTransform
            transform->hTransform = cmsCreateTransformTHR(C->lcms, srcProfileHandle, srcFormat, dstProfileHandle, dstFormat, INTENT_ABSOLUTE_COLORIMETRIC, cmsFLAGS_COPY_ALPHA | cmsFLAGS_NOOPTIMIZE);
        }
    }
    doMultithreadedTransform(C, taskCount, transform, useCCMM, srcPixels, srcPixelBytes, dstPixels, dstPixelBytes, pixelCount);
}

typedef struct clTransformTask
{
    clContext * C;
    clTransform * transform;
    void * inPixels;
    void * outPixels;
    int pixelCount;
    clBool useCCMM;
} clTransformTask;

static void transformTaskFunc(clTransformTask * info)
{
    if (info->useCCMM)
        clCCMMTransform(info->C, info->transform, info->inPixels, info->outPixels, info->pixelCount);
    else
        cmsDoTransform(info->transform->hTransform, info->inPixels, info->outPixels, info->pixelCount);
}

static void doMultithreadedTransform(clContext * C, int taskCount, clTransform * transform, clBool useCCMM, uint8_t * srcPixels, int srcPixelBytes, uint8_t * dstPixels, int dstPixelBytes, int pixelCount)
{
    if (taskCount > pixelCount) {
        // This is a dumb corner case I'm not too worried about.
        taskCount = pixelCount;
    }

    if (taskCount == 1) {
        // Don't bother making any new threads
        clTransformTask info;
        info.C = C;
        info.transform = transform;
        info.inPixels = srcPixels;
        info.outPixels = dstPixels;
        info.pixelCount = pixelCount;
        info.useCCMM = useCCMM;
        transformTaskFunc(&info);
    } else {
        int pixelsPerTask = pixelCount / taskCount;
        int lastTaskPixelCount = pixelCount - (pixelsPerTask * (taskCount - 1));
        clTask ** tasks;
        clTransformTask * infos;
        int i;

        tasks = clAllocate(taskCount * sizeof(clTask *));
        infos = clAllocate(taskCount * sizeof(clTransformTask));
        for (i = 0; i < taskCount; ++i) {
            infos[i].C = C;
            infos[i].transform = transform;
            infos[i].inPixels = &srcPixels[i * pixelsPerTask * srcPixelBytes];
            infos[i].outPixels = &dstPixels[i * pixelsPerTask * dstPixelBytes];
            infos[i].pixelCount = (i == (taskCount - 1)) ? lastTaskPixelCount : pixelsPerTask;
            infos[i].useCCMM = useCCMM;
            tasks[i] = clTaskCreate(C, (clTaskFunc)transformTaskFunc, &infos[i]);
        }

        for (i = 0; i < taskCount; ++i) {
            clTaskDestroy(C, tasks[i]);
        }

        clFree(tasks);
        clFree(infos);
    }
}
