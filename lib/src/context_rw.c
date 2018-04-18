// ---------------------------------------------------------------------------
//                         Copyright Joe Drago 2018.
//         Distributed under the Boost Software License, Version 1.0.
//            (See accompanying file LICENSE_1_0.txt or copy at
//                  http://www.boost.org/LICENSE_1_0.txt)
// ---------------------------------------------------------------------------

#include "colorist/context.h"

#include "colorist/image.h"

struct clImage * clContextRead(clContext * C, const char * filename, clFormat * outFormat)
{
    clImage * image = NULL;
    clFormat format = clFormatDetect(C, filename);
    if (outFormat)
        *outFormat = format;
    if (format == CL_FORMAT_ERROR) {
        return NULL;
    }
    switch (format) {
        case CL_FORMAT_JP2:
            image = clImageReadJP2(C, filename);
            break;
        case CL_FORMAT_JPG:
            image = clImageReadJPG(C, filename);
            break;
        case CL_FORMAT_PNG:
            image = clImageReadPNG(C, filename);
            break;
        case CL_FORMAT_WEBP:
            image = clImageReadWebP(C, filename);
            break;
        default:
            clContextLogError(C, "Unimplemented file reader '%s'", clFormatToString(C, format));
            break;
    }
    return image;
}

clBool clContextWrite(clContext * C, struct clImage * image, const char * filename, clFormat format, int quality, int rate)
{
    if (format == CL_FORMAT_AUTO) {
        format = clFormatDetect(C, filename);
        if (format == CL_FORMAT_ERROR) {
            clContextLogError(C, "Unknown output file format '%s', please specify with -f", filename);
            return clFalse;
        }
    }
    switch (format) {
        case CL_FORMAT_JP2:
            return clImageWriteJP2(C, image, filename, quality, rate);
            break;
        case CL_FORMAT_JPG:
            return clImageWriteJPG(C, image, filename, quality);
            break;
        case CL_FORMAT_PNG:
            return clImageWritePNG(C, image, filename);
            break;
        case CL_FORMAT_WEBP:
            return clImageWriteWebP(C, image, filename, quality);
            break;
        default:
            clContextLogError(C, "Unimplemented file writer '%s'", clFormatToString(C, format));
            break;
    }
    return clFalse;
}