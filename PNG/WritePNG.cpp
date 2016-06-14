/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of openfx-io <https://github.com/MrKepzie/openfx-io>,
 * Copyright (C) 2015 INRIA
 *
 * openfx-io is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * openfx-io is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with openfx-io.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

/*
 * OFX PNG writer plugin.
 * Writes an image in the PNG format
 */


#include <cstdio> // fopen, fwrite...
#include <vector>
#include <algorithm>

#include <png.h>
#include <zlib.h>

#include "GenericOCIO.h"

#include "GenericWriter.h"
#include "ofxsMacros.h"
#include "ofxsFileOpen.h"
using namespace OFX;

OFXS_NAMESPACE_ANONYMOUS_ENTER

#define kPluginName "WritePNG"
#define kPluginGrouping "Image/Writers"
#define kPluginDescription "Write PNG files."
#define kPluginIdentifier "fr.inria.openfx.WritePNG"
#define kPluginVersionMajor 1 // Incrementing this number means that you have broken backwards compatibility of the plug-in.
#define kPluginVersionMinor 0 // Increment this when you have fixed a bug or made it faster.
#define kPluginEvaluation 92 // plugin quality from 0 (bad) to 100 (perfect) or -1 if not evaluated. Better than WriteOIIO

#define kSupportsRGBA true
#define kSupportsRGB true
#define kSupportsAlpha true
#define kSupportsXY false

#define kWritePNGParamCompression "compression"
#define kWritePNGParamCompressionLabel "Compression"
#define kWritePNGParamCompressionHint "Compression used by the internal zlib library when encoding the file. This parameter is used to tune the compression algorithm.\n" \
"Filtered data consists mostly of small values with a somewhat " \
"random distribution.  In this case, the compression algorithm is tuned to " \
"compress them better.  The effect of Filtered is to force more Huffman " \
"coding and less string matching; it is somewhat intermediate between " \
"Default and Huffman Only.  RLE is designed to be almost as " \
"fast as Huffman Only, but give better compression for PNG image data.  The " \
"strategy parameter only affects the compression ratio but not the " \
"correctness of the compressed output even if it is not set appropriately. " \
"Fixed prevents the use of dynamic Huffman codes, allowing for a simpler " \
"decoder for special applications."

#define kWritePNGParamCompressionDefault "Default"
#define kWritePNGParamCompressionDefaultHint "Use this for normal data"

#define kWritePNGParamCompressionFiltered "Filtered"
#define kWritePNGParamCompressionFilteredHint "Use this for data produced by a filter (or predictor)"

#define kWritePNGParamCompressionHuffmanOnly "Huffman Only"
#define kWritePNGParamCompressionHuffmanOnlyHint "Forces Huffman encoding only (nostring match)"

#define kWritePNGParamCompressionRLE "RLE"
#define kWritePNGParamCompressionRLEHint "Limit match distances to one (run-length encoding)"

#define kWritePNGParamCompressionFixed "Fixed"
#define kWritePNGParamCompressionFixedHint "Prevents the use of dynamic Huffman codes, allowing for a simpler decoder for special applications"

#define kWritePNGParamCompressionLevel "compressionLevel"
#define kWritePNGParamCompressionLevelLabel "Compression Level"
#define kWritePNGParamCompressionLevelHint "Between 0 and 9:\n " \
"1 gives best speed, 9 gives best compression, 0 gives no compression at all " \
"(the input data is simply copied a block at a time). Default compromise between speed and compression is 6."


#define kWritePNGParamBitDepth "bitDepth"
#define kWritePNGParamBitDepthLabel "Depth"
#define kWritePNGParamBitDepthHint "The depth of the internal PNG. Only 8bit and 16bit are supported by this writer"

#define kWritePNGParamBitDepthUByte "8-bit"
#define kWritePNGParamBitDepthUShort "16-bit"

#define kWritePNGParamDither "enableDithering"
#define kWritePNGParamDitherLabel "Dithering"
#define kWritePNGParamDitherHint "When checked, conversion from float input buffers to 8-bit PNG will use a dithering algorithm to reduce quantization artifacts. This has no effect when writing to 16bit PNG"

// Try to deduce endianness
#if (defined(_WIN32) || defined(__i386__) || defined(__x86_64__))
#  ifndef __LITTLE_ENDIAN__
#    define __LITTLE_ENDIAN__ 1
#    undef __BIG_ENDIAN__
#  endif
#endif

inline bool littleendian (void)
{
#if defined(__BIG_ENDIAN__)
    return false;
#elif defined(__LITTLE_ENDIAN__)
    return true;
#else
    // Otherwise, do something quick to compute it
    int i = 1;
    return *((char *) &i);
#endif
}

/// Change endian-ness of one or more data items that are each 2, 4,
/// or 8 bytes.  This should work for any of short, unsigned short, int,
/// unsigned int, float, long long, pointers.
template<class T>
inline void
swap_endian (T *f, int len=1)
{
    for (char *c = (char *) f;  len--;  c += sizeof(T)) {
        if (sizeof(T) == 2) {
            std::swap (c[0], c[1]);
        } else if (sizeof(T) == 4) {
            std::swap (c[0], c[3]);
            std::swap (c[1], c[2]);
        } else if (sizeof(T) == 8) {
            std::swap (c[0], c[7]);
            std::swap (c[1], c[6]);
            std::swap (c[2], c[5]);
            std::swap (c[3], c[4]);
        }
    }
}


/// Initializes a PNG write struct.
/// \return empty string on success, C-string error message on failure.
///
inline void
create_write_struct (png_structp& sp,
                     png_infop& ip,
                     int nChannels,
                     int* color_type)
{

    switch (nChannels) {
        case 1 : *color_type = PNG_COLOR_TYPE_GRAY; break;
        case 2 : *color_type = PNG_COLOR_TYPE_GRAY_ALPHA; break;
        case 3 : *color_type = PNG_COLOR_TYPE_RGB; break;
        case 4 : *color_type = PNG_COLOR_TYPE_RGB_ALPHA; break;
        default:
            throw std::runtime_error("PNG only supports 1-4 channels");
    }

    sp = png_create_write_struct (PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (! sp)
        throw std::runtime_error("Could not create PNG write structure");

    ip = png_create_info_struct (sp);
    if (! ip)
        throw std::runtime_error("Could not create PNG info structure");

    // Must call this setjmp in every function that does PNG writes
    if (setjmp (png_jmpbuf(sp)))
        throw std::runtime_error("PNG library error");
}

/// Helper function - finalizes writing the image.
///
inline void
finish_image (png_structp& sp)
{
    // Must call this setjmp in every function that does PNG writes
    if (setjmp (png_jmpbuf(sp))) {
        //error ("PNG library error");
        return;
    }
    png_write_end (sp, NULL);
}


/// Destroys a PNG write struct.
///
inline void
destroy_write_struct (png_structp& sp, png_infop& ip)
{
    if (sp && ip) {
        finish_image (sp);
        png_destroy_write_struct (&sp, &ip);
        sp = NULL;
        ip = NULL;
    }
}

/// Helper function - writes a single parameter.
///
/*inline bool
put_parameter (png_structp& sp, png_infop& ip, const std::string &_name,
               TypeDesc type, const void *data, std::vector<png_text>& text)
{
    std::string name = _name;

    // Things to skip
   if (Strutil::iequals(name, "planarconfig"))  // No choice for PNG files
        return false;
    if (Strutil::iequals(name, "compression"))
        return false;
    if (Strutil::iequals(name, "ResolutionUnit") ||
        Strutil::iequals(name, "XResolution") || Strutil::iequals(name, "YResolution"))
        return false;

    // Remap some names to PNG conventions
    if (Strutil::iequals(name, "Artist") && type == TypeDesc::STRING)
        name = "Author";
    if ((Strutil::iequals(name, "name") || Strutil::iequals(name, "DocumentName")) &&
        type == TypeDesc::STRING)
        name = "Title";
    if ((Strutil::iequals(name, "description") || Strutil::iequals(name, "ImageDescription")) &&
        type == TypeDesc::STRING)
        name = "Description";

    if (Strutil::iequals(name, "DateTime") && type == TypeDesc::STRING) {
        png_time mod_time;
        int year, month, day, hour, minute, second;
        if (sscanf (*(const char **)data, "%4d:%02d:%02d %2d:%02d:%02d",
                    &year, &month, &day, &hour, &minute, &second) == 6) {
            mod_time.year = year;
            mod_time.month = month;
            mod_time.day = day;
            mod_time.hour = hour;
            mod_time.minute = minute;
            mod_time.second = second;
            png_set_tIME (sp, ip, &mod_time);
            return true;
        } else {
            return false;
        }
    }

    if (type == TypeDesc::STRING) {
        png_text t;
        t.compression = PNG_TEXT_COMPRESSION_NONE;
        t.key = (char *)ustring(name).c_str();
        t.text = *(char **)data;   // Already uniquified
        text.push_back (t);
    }

    return false;
}*/



class WritePNGPlugin : public GenericWriterPlugin
{
public:

    WritePNGPlugin(OfxImageEffectHandle handle, const std::vector<std::string>& extensions);

    virtual ~WritePNGPlugin();

private:

    virtual void encode(const std::string& filename,
                        const OfxTime time,
                        const std::string& viewName,
                        const float *pixelData,
                        const OfxRectI& bounds,
                        const float pixelAspectRatio,
                        const int pixelDataNComps,
                        const int dstNCompsStartIndex,
                        const int dstNComps,
                        const int rowBytes) OVERRIDE FINAL;

    virtual bool isImageFile(const std::string& fileExtension) const OVERRIDE FINAL;

    virtual OFX::PreMultiplicationEnum getExpectedInputPremultiplication() const OVERRIDE FINAL { return OFX::eImageUnPreMultiplied; }

    virtual void onOutputFileChanged(const std::string& newFile, bool setColorSpace) OVERRIDE FINAL;

    void openFile(const std::string& filename,
                  int nChannels,
                  png_structp* png,
                  png_infop* info,
                  FILE** file,
                  int *color_type);

    void write_info (png_structp& sp,
                png_infop& ip,
                int color_type,
                int x1, int y1,
                int width,
                int height,
                double par,
                const std::string& outputColorspace,
                BitDepthEnum bitdepth);


    OFX::ChoiceParam* _compression;
    OFX::IntParam* _compressionLevel;
    OFX::ChoiceParam* _bitdepth;
    OFX::BooleanParam* _ditherEnabled;
};

WritePNGPlugin::WritePNGPlugin(OfxImageEffectHandle handle, const std::vector<std::string>& extensions)
: GenericWriterPlugin(handle, extensions, kSupportsRGBA, kSupportsRGB, kSupportsAlpha, kSupportsXY)
, _compression(0)
, _compressionLevel(0)
, _bitdepth(0)
, _ditherEnabled(0)
{
    _compression = fetchChoiceParam(kWritePNGParamCompression);
    _compressionLevel = fetchIntParam(kWritePNGParamCompressionLevel);
    _bitdepth = fetchChoiceParam(kWritePNGParamBitDepth);
    _ditherEnabled = fetchBooleanParam(kWritePNGParamDither);
    assert(_compression && _compressionLevel && _bitdepth && _ditherEnabled);
}


WritePNGPlugin::~WritePNGPlugin()
{
}

void
WritePNGPlugin::openFile(const std::string& filename,
                         int nChannels,
                         png_structp* png,
                         png_infop* info,
                         FILE** file,
                         int *color_type)
{
    *file = OFX::open_file(filename, "wb");
    if (!*file) {
        throw std::runtime_error("Couldn't not open file");
    }

    try {
        create_write_struct (*png, *info, nChannels, color_type);
    } catch (const std::exception& e) {
        destroy_write_struct(*png, *info);
        fclose(*file);
        throw e;
    }

}


/// Writes PNG header according to the ImageSpec.
///
void
WritePNGPlugin::write_info (png_structp& sp,
                            png_infop& ip,
                            int color_type,
                            int x1, int y1,
                            int width,
                            int height,
                            double par,
                            const std::string& ocioColorspace,
                            BitDepthEnum bitdepth)
{
    int pixelBytes = bitdepth == eBitDepthUByte ? sizeof(unsigned char) : sizeof(unsigned short);
    png_set_IHDR (sp, ip, width, height, pixelBytes*8, color_type, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_set_oFFs (sp, ip, x1, y1, PNG_OFFSET_PIXEL);


    if (ocioColorspace == "sRGB" || ocioColorspace == "sRGB D65" || ocioColorspace == "sRGB (D60 sim.)" || ocioColorspace == "out_srgbd60sim" || ocioColorspace == "rrt_srgb" || ocioColorspace == "srgb8") {
        png_set_sRGB_gAMA_and_cHRM (sp, ip, PNG_sRGB_INTENT_ABSOLUTE);
    } else if (ocioColorspace == "Gamma1.8") {
        png_set_gAMA (sp, ip, 1.0f/1.8);
    } else if (ocioColorspace == "Gamma2.2" || ocioColorspace == "vd8" || ocioColorspace == "vd10" || ocioColorspace == "vd16" || ocioColorspace == "VD16") {
        png_set_gAMA (sp, ip, 1.0f/2.2);
    } else if (ocioColorspace == "Linear" || ocioColorspace == "linear" || ocioColorspace == "ACES2065-1" || ocioColorspace == "aces" || ocioColorspace == "lnf" || ocioColorspace == "ln16") {
        png_set_gAMA (sp, ip, 1.0);
    }


    // Write ICC profile, if we have anything
    /*const ImageIOParameter* icc_profile_parameter = spec.find_attribute(ICC_PROFILE_ATTR);
     if (icc_profile_parameter != NULL) {
     unsigned int length = icc_profile_parameter->type().size();
     #if OIIO_LIBPNG_VERSION > 10500 // PNG function signatures changed
     unsigned char *icc_profile = (unsigned char*)icc_profile_parameter->data();
     if (icc_profile && length)
     png_set_iCCP (sp, ip, "Embedded Profile", 0, icc_profile, length);
     #else
     char *icc_profile = (char*)icc_profile_parameter->data();
     if (icc_profile && length)
     png_set_iCCP (sp, ip, (png_charp)"Embedded Profile", 0, icc_profile, length);
     #endif
     }*/

    /*if (false && ! spec.find_attribute("DateTime")) {
     time_t now;
     time (&now);
     struct tm mytm;
     Sysutil::get_local_time (&now, &mytm);
     std::string date = Strutil::format ("%4d:%02d:%02d %2d:%02d:%02d",
     mytm.tm_year+1900, mytm.tm_mon+1, mytm.tm_mday,
     mytm.tm_hour, mytm.tm_min, mytm.tm_sec);
     spec.attribute ("DateTime", date);
     }*/

    /*string_view unitname = spec.get_string_attribute ("ResolutionUnit");
     float xres = spec.get_float_attribute ("XResolution");
     float yres = spec.get_float_attribute ("YResolution");*/
    int unittype = PNG_RESOLUTION_METER;
    float scale = 100.0/2.54;
    float xres = 100.0f;
    float yres = xres * (par ? par : 1.0f);
    png_set_pHYs (sp, ip, (png_uint_32)(xres*scale),
                  (png_uint_32)(yres*scale), unittype);


    // Deal with all other params
    /*for (size_t p = 0;  p < spec.extra_attribs.size();  ++p)
     put_parameter (sp, ip,
     spec.extra_attribs[p].name().string(),
     spec.extra_attribs[p].type(),
     spec.extra_attribs[p].data(),
     text);*/

    /*if (text.size())
     png_set_text (sp, ip, &text[0], text.size());*/

    png_write_info (sp, ip);
    png_set_packing (sp);   // Pack 1, 2, 4 bit into bytes
}


/// Bitwise circular rotation left by k bits (for 32 bit unsigned integers)
inline unsigned int rotl32 (unsigned int x, int k) {
    return (x<<k) | (x>>(32-k));
}

// Bob Jenkins "lookup3" hashes:  http://burtleburtle.net/bob/c/lookup3.c
// It's in the public domain.

// Mix up the bits of a, b, and c (changing their values in place).
inline void bjmix (unsigned int &a, unsigned int &b, unsigned int &c)
{
    a -= c;  a ^= rotl32(c, 4);  c += b;
    b -= a;  b ^= rotl32(a, 6);  a += c;
    c -= b;  c ^= rotl32(b, 8);  b += a;
    a -= c;  a ^= rotl32(c,16);  c += b;
    b -= a;  b ^= rotl32(a,19);  a += c;
    c -= b;  c ^= rotl32(b, 4);  b += a;
}

static void add_dither (int nchannels, int width, int height,
                        float *data, std::size_t xstride, std::size_t ystride,
                        float ditheramplitude,
                        int alpha_channel, unsigned int ditherseed)
{
    
    assert(sizeof(unsigned int) == 4);
    
    char *scanline = (char*)data;
    for (int y = 0;  y < height;  ++y, scanline += ystride) {
        char *pixel = (char*)data;
        unsigned int ba = y;
        unsigned int bb = ditherseed + (0 << 24);
        unsigned int bc = 0;
        for (int x = 0;  x < width;  ++x, pixel += xstride) {
            float *val = (float *)pixel;
            for (int c = 0;  c < nchannels;  ++c, ++val, ++bc) {
                bjmix (ba, bb, bc);
                int channel = c;
                if (channel == alpha_channel)
                    continue;
                float dither = bc / float(std::numeric_limits<uint32_t>::max());
                *val += ditheramplitude * (dither - 0.5f);
            }
        }
    }
}


void WritePNGPlugin::encode(const std::string& filename,
                            const OfxTime /*time*/,
                            const std::string& /*viewName*/,
                            const float *pixelData,
                            const OfxRectI& bounds,
                            const float pixelAspectRatio,
                            const int pixelDataNComps,
                            const int dstNCompsStartIndex,
                            const int dstNComps,
                            const int rowBytes)
{
    if (dstNComps != 4 && dstNComps != 3 && dstNComps != 1) {
        setPersistentMessage(OFX::Message::eMessageError, "", "PFM: can only write RGBA, RGB or Alpha components images");
        OFX::throwSuiteStatusException(kOfxStatErrFormat);
        return;
    }

    png_structp png;
    png_infop info;
    FILE* file;
    int color_type;
    try {
        openFile(filename, dstNComps, &png, &info, &file, &color_type);
    } catch (const std::exception& e) {
        setPersistentMessage(OFX::Message::eMessageError, "", e.what());
        throwSuiteStatusException(kOfxStatFailed);
    }


    png_init_io (png, file);

    int compressionLevelParam;
    _compressionLevel->getValue(compressionLevelParam);
    assert(compressionLevelParam >= 0 && compressionLevelParam <= 9);
    int compressionLevel = std::max(std::min(compressionLevelParam, Z_BEST_COMPRESSION), Z_NO_COMPRESSION);
    png_set_compression_level(png, compressionLevel);

    int compression_i;
    _compression->getValue(compression_i);
    switch (compression_i) {
        case 1:
            png_set_compression_strategy(png, Z_FILTERED);
            break;
        case 2:
            png_set_compression_strategy(png, Z_HUFFMAN_ONLY);
            break;
        case 3:
            png_set_compression_strategy(png, Z_RLE);
            break;
        case 4:
            png_set_compression_strategy(png, Z_FIXED);
            break;
        case 0:
        default:
            png_set_compression_strategy(png, Z_DEFAULT_STRATEGY);
            break;
    }

    int bitdepth_i;
    _bitdepth->getValue(bitdepth_i);

    BitDepthEnum pngDetph = bitdepth_i == 0 ? eBitDepthUByte : eBitDepthUShort;
    write_info(png, info, color_type, bounds.x1, bounds.y1, bounds.x2 - bounds.x1, bounds.y2 - bounds.y1, pixelAspectRatio, std::string() /*colorSpace*/, pngDetph);

    int bitDepthSize = pngDetph == eBitDepthUShort ? sizeof(unsigned short) : sizeof(unsigned char);

    if (pngDetph == eBitDepthUByte) {
        bool enableDither;
        _ditherEnabled->getValue(enableDither);
        if (enableDither) {
            add_dither(pixelDataNComps, bounds.x2 - bounds.x1, bounds.y2 - bounds.y1, const_cast<float*>(pixelData), pixelDataNComps * bitDepthSize, pixelDataNComps * bitDepthSize * (bounds.x2 - bounds.x1), 1.f / 255.f, pixelDataNComps == 4 ? 3 : -1 /*alphaChannel*/, 1 /*ditherSeed*/);
        }
    }

    // Convert the float buffer to the buffer used by PNG
    std::size_t pngRowSize =  (bounds.x2 - bounds.x1) * dstNComps;
    int dstRowElements = (int)pngRowSize;
    pngRowSize *= bitDepthSize;
    std::size_t scratchBufSize = (bounds.y2 - bounds.y1) * pngRowSize;

    RamBuffer scratchBuffer(scratchBufSize);

    int nComps = std::min(dstNComps,pixelDataNComps);
    const int srcRowElements = rowBytes / sizeof(float);
    if (pngDetph == eBitDepthUByte) {


        unsigned char* dst_pixels = scratchBuffer.getData();
        const float* src_pixels = pixelData;
        for (int y = bounds.y1; y < bounds.y2; ++y,
             src_pixels += (srcRowElements - ((bounds.x2 - bounds.x1) * pixelDataNComps)),
             dst_pixels += (dstRowElements - ((bounds.x2 - bounds.x1) * dstNComps))) {
            for (int x = bounds.x1; x < bounds.x2; ++x,
                 dst_pixels += dstNComps,
                 src_pixels += pixelDataNComps) {
                for (int c = 0; c < nComps; ++c) {
                    dst_pixels[c] = floatToInt<256>(src_pixels[dstNCompsStartIndex + c]);
                }
            }
        }

    } else {
        assert(pngDetph == eBitDepthUShort);

        unsigned short* dst_pixels = reinterpret_cast<unsigned short*>(scratchBuffer.getData());
        const float* src_pixels = pixelData;
        for (int y = bounds.y1; y < bounds.y2; ++y,
             src_pixels += srcRowElements, // move to next row
             dst_pixels += dstRowElements) {
            for (int x = bounds.x1; x < bounds.x2; ++x,
                 dst_pixels += dstNComps,
                 src_pixels += pixelDataNComps) {
                for (int c = 0; c < nComps; ++c) {
                    dst_pixels[c] = floatToInt<65536>(src_pixels[dstNCompsStartIndex + c]);
                }
            }

            // Remove what was done at the previous iteration
            src_pixels -= ((bounds.x2 - bounds.x1) * pixelDataNComps);
            dst_pixels -= ((bounds.x2 - bounds.x1) * dstNComps);

            // PNG is always big endian
            if (littleendian()) {
                swap_endian ((unsigned short *)dst_pixels, dstRowElements);
            }
            

        }

    }


    // Y is top down in PNG, so invert it now
    for (int y = (bounds.y2 - bounds.y1 - 1); y >= 0; --y) {
        if (setjmp (png_jmpbuf(png))) {
            destroy_write_struct(png, info);
            fclose(file);
            setPersistentMessage(OFX::Message::eMessageError,"", "PNG library error");
            throwSuiteStatusException(kOfxStatFailed);
        }
        png_write_row (png, (png_byte*)scratchBuffer.getData() + y * pngRowSize);

    }

    finish_image(png);
    destroy_write_struct(png, info);
    fclose(file);
}

bool WritePNGPlugin::isImageFile(const std::string& /*fileExtension*/) const {
    return true;
}

void
WritePNGPlugin::onOutputFileChanged(const std::string &/*filename*/,
                                    bool setColorSpace)
{
    if (setColorSpace) {
        int bitdepth_i;
        _bitdepth->getValue(bitdepth_i);
#     ifdef OFX_IO_USING_OCIO
        // Unless otherwise specified, pfm files are assumed to be linear.
        if (bitdepth_i == 0) {
            // byte, use sRGB
            if (_ocio->hasColorspace("sRGB")) {
                // nuke-default
                _ocio->setOutputColorspace("sRGB");
            } else if (_ocio->hasColorspace("sRGB D65")) {
                // blender-cycles
                _ocio->setOutputColorspace("sRGB D65");
            } else if (_ocio->hasColorspace("rrt_srgb")) {
                // rrt_srgb in aces
                _ocio->setOutputColorspace("rrt_srgb");
            } else if (_ocio->hasColorspace("srgb8")) {
                // srgb8 in spi-vfx
                _ocio->setOutputColorspace("srgb8");
            }
        } else {
            // short, use Rec709
            if (_ocio->hasColorspace("Rec709")) {
                // nuke-default
                _ocio->setOutputColorspace("Rec709");
            } else if (_ocio->hasColorspace("nuke_rec709")) {
                // blender
                _ocio->setOutputColorspace("nuke_rec709");
            } else if (_ocio->hasColorspace("Rec.709 - Full")) {
                // out_rec709full or "Rec.709 - Full" in aces 1.0.0
                _ocio->setOutputColorspace("Rec.709 - Full");
            } else if (_ocio->hasColorspace("out_rec709full")) {
                // out_rec709full or "Rec.709 - Full" in aces 1.0.0
                _ocio->setOutputColorspace("out_rec709full");
            } else if (_ocio->hasColorspace("rrt_rec709_full_100nits")) {
                // rrt_rec709_full_100nits in aces 0.7.1
                _ocio->setOutputColorspace("rrt_rec709_full_100nits");
            } else if (_ocio->hasColorspace("rrt_rec709")) {
                // rrt_rec709 in aces 0.1.1
                _ocio->setOutputColorspace("rrt_rec709");
            } else if (_ocio->hasColorspace("hd10")) {
                // hd10 in spi-anim and spi-vfx
                _ocio->setOutputColorspace("hd10");
            }

        }

#     endif
    }
}


mDeclareWriterPluginFactory(WritePNGPluginFactory, {}, false);

void WritePNGPluginFactory::load()
{
    _extensions.clear();
    _extensions.push_back("png");
}

/** @brief The basic describe function, passed a plugin descriptor */
void WritePNGPluginFactory::describe(OFX::ImageEffectDescriptor &desc)
{
    GenericWriterDescribe(desc,OFX::eRenderFullySafe, _extensions, kPluginEvaluation, false, false);
    // basic labels
    desc.setLabel(kPluginName);
    desc.setPluginDescription(kPluginDescription);
}

/** @brief The describe in context function, passed a plugin descriptor and a context */
void WritePNGPluginFactory::describeInContext(OFX::ImageEffectDescriptor &desc, ContextEnum context)
{    
    // make some pages and to things in
    PageParamDescriptor *page = GenericWriterDescribeInContextBegin(desc, context,
                                                                    kSupportsRGBA, kSupportsRGB, kSupportsAlpha,kSupportsXY,
                                                                    "reference", "reference", false);

    {
        OFX::ChoiceParamDescriptor* param = desc.defineChoiceParam(kWritePNGParamCompression);
        param->setLabel(kWritePNGParamCompressionLabel);
        param->setHint(kWritePNGParamCompressionHint);
        param->appendOption(kWritePNGParamCompressionDefault, kWritePNGParamCompressionDefaultHint);
        param->appendOption(kWritePNGParamCompressionFiltered, kWritePNGParamCompressionFilteredHint);
        param->appendOption(kWritePNGParamCompressionHuffmanOnly, kWritePNGParamCompressionHuffmanOnlyHint);
        param->appendOption(kWritePNGParamCompressionRLE, kWritePNGParamCompressionRLEHint);
        param->appendOption(kWritePNGParamCompressionFixed, kWritePNGParamCompressionFixedHint);
        param->setDefault(0);
        param->setLayoutHint(OFX::eLayoutHintNoNewLine);
        if (page) {
            page->addChild(*param);
        }
    }

    {
        OFX::IntParamDescriptor* param = desc.defineIntParam(kWritePNGParamCompressionLevel);
        param->setLabel(kWritePNGParamCompressionLevelLabel);
        param->setHint(kWritePNGParamCompressionLevelHint);
        param->setRange(0, 9);
        param->setDefault(6);
        if (page) {
            page->addChild(*param);
        }
    }

    {
        OFX::ChoiceParamDescriptor* param = desc.defineChoiceParam(kWritePNGParamBitDepth);
        param->setLabel(kWritePNGParamBitDepthLabel);
        param->setHint(kWritePNGParamBitDepthHint);
        param->appendOption(kWritePNGParamBitDepthUByte);
        param->appendOption(kWritePNGParamBitDepthUShort);
        param->setDefault(0);
        param->setLayoutHint(OFX::eLayoutHintNoNewLine);
        if (page) {
            page->addChild(*param);
        }
    }

    {
        OFX::BooleanParamDescriptor* param = desc.defineBooleanParam(kWritePNGParamDither);
        param->setLabel(kWritePNGParamDitherLabel);
        param->setHint(kWritePNGParamDitherHint);
        param->setDefault(true);
        if (page) {
            page->addChild(*param);
        }
    }
    GenericWriterDescribeInContextEnd(desc, context, page);
}

/** @brief The create instance function, the plugin must return an object derived from the \ref OFX::ImageEffect class */
ImageEffect* WritePNGPluginFactory::createInstance(OfxImageEffectHandle handle, ContextEnum /*context*/)
{
    WritePNGPlugin* ret = new WritePNGPlugin(handle, _extensions);
    ret->restoreState();
    return ret;
}


static WritePNGPluginFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
mRegisterPluginFactoryInstance(p)

OFXS_NAMESPACE_ANONYMOUS_EXIT