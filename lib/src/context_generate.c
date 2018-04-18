// ---------------------------------------------------------------------------
//                         Copyright Joe Drago 2018.
//         Distributed under the Boost Software License, Version 1.0.
//            (See accompanying file LICENSE_1_0.txt or copy at
//                  http://www.boost.org/LICENSE_1_0.txt)
// ---------------------------------------------------------------------------

#include "colorist/context.h"

#include "colorist/image.h"
#include "colorist/profile.h"

#include <string.h>

int clContextGenerate(clContext * C)
{
    Timer overall;
    clProfilePrimaries primaries;
    clProfileCurve curve;
    int luminance;
    clProfile * dstProfile = NULL;
    clImage * image = NULL;

    clFormat outputFileFormat = C->format;
    if (outputFileFormat == CL_FORMAT_AUTO)
        outputFileFormat = clFormatDetect(C, C->outputFilename);
    if (outputFileFormat == CL_FORMAT_ERROR) {
        return 1;
    }

    clContextLog(C, "action", 0, "Generating: %s", C->outputFilename);
    timerStart(&overall);

    if (outputFileFormat == CL_FORMAT_ICC) {
        if (C->inputFilename != NULL) {
            clContextLogError(C, "generate cannot accept an image string to generate a .icc file.");
            return 1;
        }
    } else {
        if (C->inputFilename == NULL) {
            clContextLogError(C, "generate requires an image string to generate an image.");
            return 1;
        }
    }

    if (C->primaries[0] <= 0.0f) {
        clBool ret = clContextGetStockPrimaries(C, "bt709", &primaries);
        COLORIST_ASSERT(ret == clTrue);
        (void)ret; // unused in Release
        clContextLog(C, "generate", 1, "No primaries specified (-p). Using default sRGB (BT.709) primaries.");
    } else {
        primaries.red[0] = C->primaries[0];
        primaries.red[1] = C->primaries[1];
        primaries.green[0] = C->primaries[2];
        primaries.green[1] = C->primaries[3];
        primaries.blue[0] = C->primaries[4];
        primaries.blue[1] = C->primaries[5];
        primaries.white[0] = C->primaries[6];
        primaries.white[1] = C->primaries[7];
    }

    curve.type = CL_PCT_GAMMA;
    if (C->gamma <= 0.0f) {
        clContextLog(C, "generate", 1, "No gamma specified (-g). Using default sRGB gamma.");
        curve.gamma = 2.4f;
    } else {
        curve.gamma = C->gamma;
    }

    if (C->luminance <= 0) {
        clContextLog(C, "generate", 1, "No luminance specified (-l). Using default Colorist luminance.");
        luminance = COLORIST_DEFAULT_LUMINANCE;
    } else {
        luminance = C->luminance;
    }

    {
        char * description = NULL;

        if (C->description) {
            description = clContextStrdup(C, C->description);
        } else {
            description = clGenerateDescription(C, &primaries, &curve, luminance);
        }

        clContextLog(C, "generate", 0, "Generating ICC profile: \"%s\"", description);
        dstProfile = clProfileCreate(C, &primaries, &curve, luminance, description);
        clFree(description);

        if (C->copyright) {
            clContextLog(C, "generate", 1, "Setting copyright: \"%s\"", C->copyright);
            clProfileSetMLU(C, dstProfile, "cprt", "en", "US", C->copyright);
        }
    }

    if (C->inputFilename) {
        clImage * image;
        int depth;

        depth = C->bpp;
        if (depth == 0) {
            depth = 16;
        }
        if ((depth != 8) && (outputFileFormat != CL_FORMAT_ICC) && (clFormatMaxDepth(C, outputFileFormat) < depth)) {
            clContextLog(C, "validate", 0, "Forcing output to 8-bit (format limitations)");
            depth = 8;
        }

        image = clImageParseString(C, C->inputFilename, depth, dstProfile);
        if (image == NULL) {
            clProfileDestroy(C, dstProfile);
            return 1;
        } else {
            clContextLog(C, "generate", 0, "Writing Image: %s", C->outputFilename);
            clImageDebugDump(C, image, C->rect[0], C->rect[1], C->rect[2], C->rect[3], 0);
            if (!clContextWrite(C, image, C->outputFilename, outputFileFormat, C->quality, C->rate)) {
                clImageDestroy(C, image);
                clProfileDestroy(C, dstProfile);
                return 1;
            }
            clImageDestroy(C, image);
        }
    } else {
        clContextLog(C, "generate", 0, "Writing ICC: %s", C->outputFilename);
        clProfileDebugDump(C, dstProfile, 0);
        if (!clProfileWrite(C, dstProfile, C->outputFilename)) {
            clProfileDestroy(C, dstProfile);
            return 1;
        }
    }

    clProfileDestroy(C, dstProfile);
    clContextLog(C, "encode", 1, "Wrote %d bytes.", clFileSize(C->outputFilename));
    clContextLog(C, "action", 0, "Generation complete (%g sec).", timerElapsedSeconds(&overall));
    return 0;
}