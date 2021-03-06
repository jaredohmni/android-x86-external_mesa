/*
 * Copyright (C) 2008 Nicolai Haehnle.
 * Copyright (C) The Weather Channel, Inc.  2002.  All Rights Reserved.
 *
 * The Weather Channel (TM) funded Tungsten Graphics to develop the
 * initial release of the Radeon 8500 driver under the XFree86 license.
 * This notice must be preserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "main/glheader.h"
#include "main/imports.h"
#include "main/context.h"
#include "main/convolve.h"
#include "main/mipmap.h"
#include "main/texcompress.h"
#include "main/texformat.h"
#include "main/texstore.h"
#include "main/teximage.h"
#include "main/texobj.h"
#include "main/texgetimage.h"

#include "xmlpool.h"		/* for symbolic values of enum-type options */

#include "radeon_common.h"

#include "radeon_mipmap_tree.h"


static void copy_rows(void* dst, GLuint dststride, const void* src, GLuint srcstride,
	GLuint numrows, GLuint rowsize)
{
	assert(rowsize <= dststride);
	assert(rowsize <= srcstride);

	if (rowsize == srcstride && rowsize == dststride) {
		memcpy(dst, src, numrows*rowsize);
	} else {
		GLuint i;
		for(i = 0; i < numrows; ++i) {
			memcpy(dst, src, rowsize);
			dst += dststride;
			src += srcstride;
		}
	}
}

/* textures */
/**
 * Allocate an empty texture image object.
 */
struct gl_texture_image *radeonNewTextureImage(GLcontext *ctx)
{
	return CALLOC(sizeof(radeon_texture_image));
}

/**
 * Free memory associated with this texture image.
 */
void radeonFreeTexImageData(GLcontext *ctx, struct gl_texture_image *timage)
{
	radeon_texture_image* image = get_radeon_texture_image(timage);

	if (image->mt) {
		radeon_miptree_unreference(image->mt);
		image->mt = 0;
		assert(!image->base.Data);
	} else {
		_mesa_free_texture_image_data(ctx, timage);
	}
	if (image->bo) {
		radeon_bo_unref(image->bo);
		image->bo = NULL;
	}
	if (timage->Data) {
		_mesa_free_texmemory(timage->Data);
		timage->Data = NULL;
	}
}

/* Set Data pointer and additional data for mapped texture image */
static void teximage_set_map_data(radeon_texture_image *image)
{
	radeon_mipmap_level *lvl = &image->mt->levels[image->mtlevel];

	image->base.Data = image->mt->bo->ptr + lvl->faces[image->mtface].offset;
	image->base.RowStride = lvl->rowstride / image->mt->bpp;
}


/**
 * Map a single texture image for glTexImage and friends.
 */
void radeon_teximage_map(radeon_texture_image *image, GLboolean write_enable)
{
	if (image->mt) {
		assert(!image->base.Data);

		radeon_bo_map(image->mt->bo, write_enable);
		teximage_set_map_data(image);
	}
}


void radeon_teximage_unmap(radeon_texture_image *image)
{
	if (image->mt) {
		assert(image->base.Data);

		image->base.Data = 0;
		radeon_bo_unmap(image->mt->bo);
	}
}

static void map_override(GLcontext *ctx, radeonTexObj *t)
{
	radeon_texture_image *img = get_radeon_texture_image(t->base.Image[0][0]);

	radeon_bo_map(t->bo, GL_FALSE);

	img->base.Data = t->bo->ptr;
	_mesa_set_fetch_functions(&img->base, 2);
}

static void unmap_override(GLcontext *ctx, radeonTexObj *t)
{
	radeon_texture_image *img = get_radeon_texture_image(t->base.Image[0][0]);

	radeon_bo_unmap(t->bo);

	img->base.Data = NULL;
}

/**
 * Map a validated texture for reading during software rendering.
 */
void radeonMapTexture(GLcontext *ctx, struct gl_texture_object *texObj)
{
	radeonTexObj* t = radeon_tex_obj(texObj);
	int face, level;

	if (!radeon_validate_texture_miptree(ctx, texObj))
	  return;

	/* for r100 3D sw fallbacks don't have mt */
	if (t->image_override && t->bo)
		map_override(ctx, t);

	if (!t->mt)
		return;

	radeon_bo_map(t->mt->bo, GL_FALSE);
	for(face = 0; face < t->mt->faces; ++face) {
		for(level = t->mt->firstLevel; level <= t->mt->lastLevel; ++level)
			teximage_set_map_data(get_radeon_texture_image(texObj->Image[face][level]));
	}
}

void radeonUnmapTexture(GLcontext *ctx, struct gl_texture_object *texObj)
{
	radeonTexObj* t = radeon_tex_obj(texObj);
	int face, level;

	if (t->image_override && t->bo)
		unmap_override(ctx, t);
	/* for r100 3D sw fallbacks don't have mt */
	if (!t->mt)
	  return;

	for(face = 0; face < t->mt->faces; ++face) {
		for(level = t->mt->firstLevel; level <= t->mt->lastLevel; ++level)
			texObj->Image[face][level]->Data = 0;
	}
	radeon_bo_unmap(t->mt->bo);
}

GLuint radeon_face_for_target(GLenum target)
{
	switch (target) {
	case GL_TEXTURE_CUBE_MAP_POSITIVE_X:
	case GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
	case GL_TEXTURE_CUBE_MAP_POSITIVE_Y:
	case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
	case GL_TEXTURE_CUBE_MAP_POSITIVE_Z:
	case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z:
		return (GLuint) target - (GLuint) GL_TEXTURE_CUBE_MAP_POSITIVE_X;
	default:
		return 0;
	}
}

/**
 * Wraps Mesa's implementation to ensure that the base level image is mapped.
 *
 * This relies on internal details of _mesa_generate_mipmap, in particular
 * the fact that the memory for recreated texture images is always freed.
 */
static void radeon_generate_mipmap(GLcontext *ctx, GLenum target,
				   struct gl_texture_object *texObj)
{
	radeonTexObj* t = radeon_tex_obj(texObj);
	GLuint nr_faces = (t->base.Target == GL_TEXTURE_CUBE_MAP) ? 6 : 1;
	int i, face;


	_mesa_generate_mipmap(ctx, target, texObj);

	for (face = 0; face < nr_faces; face++) {
		for (i = texObj->BaseLevel + 1; i < texObj->MaxLevel; i++) {
			radeon_texture_image *image;

			image = get_radeon_texture_image(texObj->Image[face][i]);

			if (image == NULL)
				break;

			image->mtlevel = i;
			image->mtface = face;

			radeon_miptree_unreference(image->mt);
			image->mt = NULL;
		}
	}
	
}

void radeonGenerateMipmap(GLcontext* ctx, GLenum target, struct gl_texture_object *texObj)
{
	GLuint face = radeon_face_for_target(target);
	radeon_texture_image *baseimage = get_radeon_texture_image(texObj->Image[face][texObj->BaseLevel]);

	radeon_teximage_map(baseimage, GL_FALSE);
	radeon_generate_mipmap(ctx, target, texObj);
	radeon_teximage_unmap(baseimage);
}


/* try to find a format which will only need a memcopy */
static const struct gl_texture_format *radeonChoose8888TexFormat(radeonContextPtr rmesa,
								 GLenum srcFormat,
								 GLenum srcType, GLboolean fbo)
{
	const GLuint ui = 1;
	const GLubyte littleEndian = *((const GLubyte *)&ui);

	/* r100 can only do this */
	if (IS_R100_CLASS(rmesa->radeonScreen) || fbo)
	  return _dri_texformat_argb8888;

	if ((srcFormat == GL_RGBA && srcType == GL_UNSIGNED_INT_8_8_8_8) ||
	    (srcFormat == GL_RGBA && srcType == GL_UNSIGNED_BYTE && !littleEndian) ||
	    (srcFormat == GL_ABGR_EXT && srcType == GL_UNSIGNED_INT_8_8_8_8_REV) ||
	    (srcFormat == GL_ABGR_EXT && srcType == GL_UNSIGNED_BYTE && littleEndian)) {
		return &_mesa_texformat_rgba8888;
	} else if ((srcFormat == GL_RGBA && srcType == GL_UNSIGNED_INT_8_8_8_8_REV) ||
		   (srcFormat == GL_RGBA && srcType == GL_UNSIGNED_BYTE && littleEndian) ||
		   (srcFormat == GL_ABGR_EXT && srcType == GL_UNSIGNED_INT_8_8_8_8) ||
		   (srcFormat == GL_ABGR_EXT && srcType == GL_UNSIGNED_BYTE && !littleEndian)) {
		return &_mesa_texformat_rgba8888_rev;
	} else if (IS_R200_CLASS(rmesa->radeonScreen)) {
		return _dri_texformat_argb8888;
	} else if (srcFormat == GL_BGRA && ((srcType == GL_UNSIGNED_BYTE && !littleEndian) ||
					    srcType == GL_UNSIGNED_INT_8_8_8_8)) {
		return &_mesa_texformat_argb8888_rev;
	} else if (srcFormat == GL_BGRA && ((srcType == GL_UNSIGNED_BYTE && littleEndian) ||
					    srcType == GL_UNSIGNED_INT_8_8_8_8_REV)) {
		return &_mesa_texformat_argb8888;
	} else
		return _dri_texformat_argb8888;
}

const struct gl_texture_format *radeonChooseTextureFormat_mesa(GLcontext * ctx,
							  GLint internalFormat,
							  GLenum format,
							  GLenum type)
{
	return radeonChooseTextureFormat(ctx, internalFormat, format,
					 type, 0);
}

const struct gl_texture_format *radeonChooseTextureFormat(GLcontext * ctx,
							  GLint internalFormat,
							  GLenum format,
							  GLenum type, GLboolean fbo)
{
	radeonContextPtr rmesa = RADEON_CONTEXT(ctx);
	const GLboolean do32bpt =
	    (rmesa->texture_depth == DRI_CONF_TEXTURE_DEPTH_32);
	const GLboolean force16bpt =
	    (rmesa->texture_depth == DRI_CONF_TEXTURE_DEPTH_FORCE_16);
	(void)format;

#if 0
	fprintf(stderr, "InternalFormat=%s(%d) type=%s format=%s\n",
		_mesa_lookup_enum_by_nr(internalFormat), internalFormat,
		_mesa_lookup_enum_by_nr(type), _mesa_lookup_enum_by_nr(format));
	fprintf(stderr, "do32bpt=%d force16bpt=%d\n", do32bpt, force16bpt);
#endif

	switch (internalFormat) {
	case 4:
	case GL_RGBA:
	case GL_COMPRESSED_RGBA:
		switch (type) {
		case GL_UNSIGNED_INT_10_10_10_2:
		case GL_UNSIGNED_INT_2_10_10_10_REV:
			return do32bpt ? _dri_texformat_argb8888 :
			    _dri_texformat_argb1555;
		case GL_UNSIGNED_SHORT_4_4_4_4:
		case GL_UNSIGNED_SHORT_4_4_4_4_REV:
			return _dri_texformat_argb4444;
		case GL_UNSIGNED_SHORT_5_5_5_1:
		case GL_UNSIGNED_SHORT_1_5_5_5_REV:
			return _dri_texformat_argb1555;
		default:
			return do32bpt ? radeonChoose8888TexFormat(rmesa, format, type, fbo) :
			    _dri_texformat_argb4444;
		}

	case 3:
	case GL_RGB:
	case GL_COMPRESSED_RGB:
		switch (type) {
		case GL_UNSIGNED_SHORT_4_4_4_4:
		case GL_UNSIGNED_SHORT_4_4_4_4_REV:
			return _dri_texformat_argb4444;
		case GL_UNSIGNED_SHORT_5_5_5_1:
		case GL_UNSIGNED_SHORT_1_5_5_5_REV:
			return _dri_texformat_argb1555;
		case GL_UNSIGNED_SHORT_5_6_5:
		case GL_UNSIGNED_SHORT_5_6_5_REV:
			return _dri_texformat_rgb565;
		default:
			return do32bpt ? _dri_texformat_argb8888 :
			    _dri_texformat_rgb565;
		}

	case GL_RGBA8:
	case GL_RGB10_A2:
	case GL_RGBA12:
	case GL_RGBA16:
		return !force16bpt ?
			radeonChoose8888TexFormat(rmesa, format, type, fbo) :
			_dri_texformat_argb4444;

	case GL_RGBA4:
	case GL_RGBA2:
		return _dri_texformat_argb4444;

	case GL_RGB5_A1:
		return _dri_texformat_argb1555;

	case GL_RGB8:
	case GL_RGB10:
	case GL_RGB12:
	case GL_RGB16:
		return !force16bpt ? _dri_texformat_argb8888 :
		    _dri_texformat_rgb565;

	case GL_RGB5:
	case GL_RGB4:
	case GL_R3_G3_B2:
		return _dri_texformat_rgb565;

	case GL_ALPHA:
	case GL_ALPHA4:
	case GL_ALPHA8:
	case GL_ALPHA12:
	case GL_ALPHA16:
	case GL_COMPRESSED_ALPHA:
		/* r200: can't use a8 format since interpreting hw I8 as a8 would result
		   in wrong rgb values (same as alpha value instead of 0). */
		if (IS_R200_CLASS(rmesa->radeonScreen))
			return _dri_texformat_al88;
		else
			return _dri_texformat_a8;
	case 1:
	case GL_LUMINANCE:
	case GL_LUMINANCE4:
	case GL_LUMINANCE8:
	case GL_LUMINANCE12:
	case GL_LUMINANCE16:
	case GL_COMPRESSED_LUMINANCE:
		return _dri_texformat_l8;

	case 2:
	case GL_LUMINANCE_ALPHA:
	case GL_LUMINANCE4_ALPHA4:
	case GL_LUMINANCE6_ALPHA2:
	case GL_LUMINANCE8_ALPHA8:
	case GL_LUMINANCE12_ALPHA4:
	case GL_LUMINANCE12_ALPHA12:
	case GL_LUMINANCE16_ALPHA16:
	case GL_COMPRESSED_LUMINANCE_ALPHA:
		return _dri_texformat_al88;

	case GL_INTENSITY:
	case GL_INTENSITY4:
	case GL_INTENSITY8:
	case GL_INTENSITY12:
	case GL_INTENSITY16:
	case GL_COMPRESSED_INTENSITY:
		return _dri_texformat_i8;

	case GL_YCBCR_MESA:
		if (type == GL_UNSIGNED_SHORT_8_8_APPLE ||
		    type == GL_UNSIGNED_BYTE)
			return &_mesa_texformat_ycbcr;
		else
			return &_mesa_texformat_ycbcr_rev;

	case GL_RGB_S3TC:
	case GL_RGB4_S3TC:
	case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
		return &_mesa_texformat_rgb_dxt1;

	case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
		return &_mesa_texformat_rgba_dxt1;

	case GL_RGBA_S3TC:
	case GL_RGBA4_S3TC:
	case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
		return &_mesa_texformat_rgba_dxt3;

	case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
		return &_mesa_texformat_rgba_dxt5;

	case GL_ALPHA16F_ARB:
		return &_mesa_texformat_alpha_float16;
	case GL_ALPHA32F_ARB:
		return &_mesa_texformat_alpha_float32;
	case GL_LUMINANCE16F_ARB:
		return &_mesa_texformat_luminance_float16;
	case GL_LUMINANCE32F_ARB:
		return &_mesa_texformat_luminance_float32;
	case GL_LUMINANCE_ALPHA16F_ARB:
		return &_mesa_texformat_luminance_alpha_float16;
	case GL_LUMINANCE_ALPHA32F_ARB:
		return &_mesa_texformat_luminance_alpha_float32;
	case GL_INTENSITY16F_ARB:
		return &_mesa_texformat_intensity_float16;
	case GL_INTENSITY32F_ARB:
		return &_mesa_texformat_intensity_float32;
	case GL_RGB16F_ARB:
		return &_mesa_texformat_rgba_float16;
	case GL_RGB32F_ARB:
		return &_mesa_texformat_rgba_float32;
	case GL_RGBA16F_ARB:
		return &_mesa_texformat_rgba_float16;
	case GL_RGBA32F_ARB:
		return &_mesa_texformat_rgba_float32;

	case GL_DEPTH_COMPONENT:
	case GL_DEPTH_COMPONENT16:
	case GL_DEPTH_COMPONENT24:
	case GL_DEPTH_COMPONENT32:
	case GL_DEPTH_STENCIL_EXT:
	case GL_DEPTH24_STENCIL8_EXT:
		return &_mesa_texformat_s8_z24;

	/* EXT_texture_sRGB */
	case GL_SRGB:
	case GL_SRGB8:
	case GL_SRGB_ALPHA:
	case GL_SRGB8_ALPHA8:
	case GL_COMPRESSED_SRGB:
	case GL_COMPRESSED_SRGB_ALPHA:
		return &_mesa_texformat_srgba8;

	case GL_SLUMINANCE:
	case GL_SLUMINANCE8:
	case GL_COMPRESSED_SLUMINANCE:
		return &_mesa_texformat_sl8;

	case GL_SLUMINANCE_ALPHA:
	case GL_SLUMINANCE8_ALPHA8:
	case GL_COMPRESSED_SLUMINANCE_ALPHA:
		return &_mesa_texformat_sla8;

	default:
		_mesa_problem(ctx,
			      "unexpected internalFormat 0x%x in %s",
			      (int)internalFormat, __func__);
		return NULL;
	}

	return NULL;		/* never get here */
}

/**
 * All glTexImage calls go through this function.
 */
static void radeon_teximage(
	GLcontext *ctx, int dims,
	GLenum target, GLint level,
	GLint internalFormat,
	GLint width, GLint height, GLint depth,
	GLsizei imageSize,
	GLenum format, GLenum type, const GLvoid * pixels,
	const struct gl_pixelstore_attrib *packing,
	struct gl_texture_object *texObj,
	struct gl_texture_image *texImage,
	int compressed)
{
	radeonContextPtr rmesa = RADEON_CONTEXT(ctx);
	radeonTexObj* t = radeon_tex_obj(texObj);
	radeon_texture_image* image = get_radeon_texture_image(texImage);
	GLuint dstRowStride;
	GLint postConvWidth = width;
	GLint postConvHeight = height;
	GLuint texelBytes;
	GLuint face = radeon_face_for_target(target);

	radeon_firevertices(rmesa);

	t->validated = GL_FALSE;

	if (ctx->_ImageTransferState & IMAGE_CONVOLUTION_BIT) {
	       _mesa_adjust_image_for_convolution(ctx, dims, &postConvWidth,
						  &postConvHeight);
	}

	/* Choose and fill in the texture format for this image */
	texImage->TexFormat = radeonChooseTextureFormat(ctx, internalFormat, format, type, 0);
	_mesa_set_fetch_functions(texImage, dims);

	if (texImage->TexFormat->TexelBytes == 0) {
		texelBytes = 0;
		texImage->IsCompressed = GL_TRUE;
		texImage->CompressedSize =
			ctx->Driver.CompressedTextureSize(ctx, texImage->Width,
					   texImage->Height, texImage->Depth,
					   texImage->TexFormat->MesaFormat);
	} else {
		texImage->IsCompressed = GL_FALSE;
		texImage->CompressedSize = 0;

		texelBytes = texImage->TexFormat->TexelBytes;
		/* Minimum pitch of 32 bytes */
		if (postConvWidth * texelBytes < 32) {
		  postConvWidth = 32 / texelBytes;
		  texImage->RowStride = postConvWidth;
		}
		if (!image->mt) {      
			assert(texImage->RowStride == postConvWidth);
		}
	}

	/* Allocate memory for image */
	radeonFreeTexImageData(ctx, texImage); /* Mesa core only clears texImage->Data but not image->mt */

	if (t->mt &&
	    t->mt->firstLevel == level &&
	    t->mt->lastLevel == level &&
	    t->mt->target != GL_TEXTURE_CUBE_MAP_ARB &&
	    !radeon_miptree_matches_image(t->mt, texImage, face, level)) {
	  radeon_miptree_unreference(t->mt);
	  t->mt = NULL;
	}

	if (!t->mt)
		radeon_try_alloc_miptree(rmesa, t, image, face, level);
	if (t->mt && radeon_miptree_matches_image(t->mt, texImage, face, level)) {
		radeon_mipmap_level *lvl;
		image->mt = t->mt;
		image->mtlevel = level - t->mt->firstLevel;
		image->mtface = face;
		radeon_miptree_reference(t->mt);
		lvl = &image->mt->levels[image->mtlevel];
		dstRowStride = lvl->rowstride;
	} else {
		int size;
		if (texImage->IsCompressed) {
			size = texImage->CompressedSize;
		} else {
			size = texImage->Width * texImage->Height * texImage->Depth * texImage->TexFormat->TexelBytes;
		}
		texImage->Data = _mesa_alloc_texmemory(size);
	}

	/* Upload texture image; note that the spec allows pixels to be NULL */
	if (compressed) {
		pixels = _mesa_validate_pbo_compressed_teximage(
			ctx, imageSize, pixels, packing, "glCompressedTexImage");
	} else {
		pixels = _mesa_validate_pbo_teximage(
			ctx, dims, width, height, depth,
			format, type, pixels, packing, "glTexImage");
	}

	if (pixels) {
		radeon_teximage_map(image, GL_TRUE);
		if (compressed) {
			if (image->mt) {
				uint32_t srcRowStride, bytesPerRow, rows;
				srcRowStride = _mesa_compressed_row_stride(texImage->TexFormat->MesaFormat, width);
				bytesPerRow = srcRowStride;
				rows = (height + 3) / 4;
				copy_rows(texImage->Data, image->mt->levels[level].rowstride,
					  pixels, srcRowStride, rows, bytesPerRow);
			} else {
				memcpy(texImage->Data, pixels, imageSize);
			}
		} else {
			GLuint dstRowStride;
			GLuint *dstImageOffsets;

			if (image->mt) {
				radeon_mipmap_level *lvl = &image->mt->levels[image->mtlevel];
				dstRowStride = lvl->rowstride;
			} else {
				dstRowStride = texImage->Width * texImage->TexFormat->TexelBytes;
			}

			if (dims == 3) {
				int i;

				dstImageOffsets = _mesa_malloc(depth * sizeof(GLuint)) ;
				if (!dstImageOffsets)
					_mesa_error(ctx, GL_OUT_OF_MEMORY, "glTexImage");

				for (i = 0; i < depth; ++i) {
					dstImageOffsets[i] = dstRowStride/texImage->TexFormat->TexelBytes * height * i;
				}
			} else {
				dstImageOffsets = texImage->ImageOffsets;
			}

			if (!texImage->TexFormat->StoreImage(ctx, dims,
						texImage->_BaseFormat,
						texImage->TexFormat,
						texImage->Data, 0, 0, 0, /* dstX/Y/Zoffset */
						dstRowStride,
						dstImageOffsets,
						width, height, depth,
						format, type, pixels, packing))
				_mesa_error(ctx, GL_OUT_OF_MEMORY, "glTexImage");

			if (dims == 3)
				_mesa_free(dstImageOffsets);
		}

		/* SGIS_generate_mipmap */
		if (level == texObj->BaseLevel && texObj->GenerateMipmap) {
			radeon_generate_mipmap(ctx, target, texObj);
		}
	}

	_mesa_unmap_teximage_pbo(ctx, packing);

	if (pixels)
	  radeon_teximage_unmap(image);


}

void radeonTexImage1D(GLcontext * ctx, GLenum target, GLint level,
		      GLint internalFormat,
		      GLint width, GLint border,
		      GLenum format, GLenum type, const GLvoid * pixels,
		      const struct gl_pixelstore_attrib *packing,
		      struct gl_texture_object *texObj,
		      struct gl_texture_image *texImage)
{
	radeon_teximage(ctx, 1, target, level, internalFormat, width, 1, 1,
		0, format, type, pixels, packing, texObj, texImage, 0);
}

void radeonTexImage2D(GLcontext * ctx, GLenum target, GLint level,
			   GLint internalFormat,
			   GLint width, GLint height, GLint border,
			   GLenum format, GLenum type, const GLvoid * pixels,
			   const struct gl_pixelstore_attrib *packing,
			   struct gl_texture_object *texObj,
			   struct gl_texture_image *texImage)

{
	radeon_teximage(ctx, 2, target, level, internalFormat, width, height, 1,
		0, format, type, pixels, packing, texObj, texImage, 0);
}

void radeonCompressedTexImage2D(GLcontext * ctx, GLenum target,
				     GLint level, GLint internalFormat,
				     GLint width, GLint height, GLint border,
				     GLsizei imageSize, const GLvoid * data,
				     struct gl_texture_object *texObj,
				     struct gl_texture_image *texImage)
{
	radeon_teximage(ctx, 2, target, level, internalFormat, width, height, 1,
		imageSize, 0, 0, data, &ctx->Unpack, texObj, texImage, 1);
}

void radeonTexImage3D(GLcontext * ctx, GLenum target, GLint level,
		      GLint internalFormat,
		      GLint width, GLint height, GLint depth,
		      GLint border,
		      GLenum format, GLenum type, const GLvoid * pixels,
		      const struct gl_pixelstore_attrib *packing,
		      struct gl_texture_object *texObj,
		      struct gl_texture_image *texImage)
{
	radeon_teximage(ctx, 3, target, level, internalFormat, width, height, depth,
		0, format, type, pixels, packing, texObj, texImage, 0);
}

/**
 * Update a subregion of the given texture image.
 */
static void radeon_texsubimage(GLcontext* ctx, int dims, GLenum target, int level,
		GLint xoffset, GLint yoffset, GLint zoffset,
		GLsizei width, GLsizei height, GLsizei depth,
		GLsizei imageSize,
		GLenum format, GLenum type,
		const GLvoid * pixels,
		const struct gl_pixelstore_attrib *packing,
		struct gl_texture_object *texObj,
		struct gl_texture_image *texImage,
		int compressed)
{
	radeonContextPtr rmesa = RADEON_CONTEXT(ctx);
	radeonTexObj* t = radeon_tex_obj(texObj);
	radeon_texture_image* image = get_radeon_texture_image(texImage);

	radeon_firevertices(rmesa);

	t->validated = GL_FALSE;
	if (compressed) {
		pixels = _mesa_validate_pbo_compressed_teximage(
			ctx, imageSize, pixels, packing, "glCompressedTexImage");
	} else {
		pixels = _mesa_validate_pbo_teximage(ctx, dims,
			width, height, depth, format, type, pixels, packing, "glTexSubImage1D");
	}

	if (pixels) {
		GLint dstRowStride;
		radeon_teximage_map(image, GL_TRUE);

		if (image->mt) {
			radeon_mipmap_level *lvl = &image->mt->levels[image->mtlevel];
			dstRowStride = lvl->rowstride;
		} else {
			dstRowStride = texImage->RowStride * texImage->TexFormat->TexelBytes;
		}

		if (compressed) {
			uint32_t srcRowStride, bytesPerRow, rows;
			GLubyte *img_start;
			if (!image->mt) {
				dstRowStride = _mesa_compressed_row_stride(texImage->TexFormat->MesaFormat, texImage->Width);
				img_start = _mesa_compressed_image_address(xoffset, yoffset, 0,
									   texImage->TexFormat->MesaFormat,
									   texImage->Width, texImage->Data);
			}
			else {
				uint32_t blocks_x = dstRowStride / (image->mt->bpp * 4);
				img_start = texImage->Data + image->mt->bpp * 4 * (blocks_x * (yoffset / 4) + xoffset / 4);
			}
			srcRowStride = _mesa_compressed_row_stride(texImage->TexFormat->MesaFormat, width);
			bytesPerRow = srcRowStride;
			rows = (height + 3) / 4;

			copy_rows(img_start, dstRowStride,  pixels, srcRowStride, rows,  bytesPerRow);
			
		} else {
			if (!texImage->TexFormat->StoreImage(ctx, dims, texImage->_BaseFormat,
							     texImage->TexFormat, texImage->Data,
							     xoffset, yoffset, zoffset,
							     dstRowStride,
							     texImage->ImageOffsets,
							     width, height, depth,
							     format, type, pixels, packing))
				_mesa_error(ctx, GL_OUT_OF_MEMORY, "glTexSubImage");
		}

		/* GL_SGIS_generate_mipmap */
		if (level == texObj->BaseLevel && texObj->GenerateMipmap) {
			radeon_generate_mipmap(ctx, target, texObj);
		}
	}

	radeon_teximage_unmap(image);

	_mesa_unmap_teximage_pbo(ctx, packing);


}

void radeonTexSubImage1D(GLcontext * ctx, GLenum target, GLint level,
			 GLint xoffset,
			 GLsizei width,
			 GLenum format, GLenum type,
			 const GLvoid * pixels,
			 const struct gl_pixelstore_attrib *packing,
			 struct gl_texture_object *texObj,
			 struct gl_texture_image *texImage)
{
	radeon_texsubimage(ctx, 1, target, level, xoffset, 0, 0, width, 1, 1, 0,
		format, type, pixels, packing, texObj, texImage, 0);
}

void radeonTexSubImage2D(GLcontext * ctx, GLenum target, GLint level,
			 GLint xoffset, GLint yoffset,
			 GLsizei width, GLsizei height,
			 GLenum format, GLenum type,
			 const GLvoid * pixels,
			 const struct gl_pixelstore_attrib *packing,
			 struct gl_texture_object *texObj,
			 struct gl_texture_image *texImage)
{
	radeon_texsubimage(ctx, 2, target, level, xoffset, yoffset, 0, width, height, 1,
			   0, format, type, pixels, packing, texObj, texImage,
			   0);
}

void radeonCompressedTexSubImage2D(GLcontext * ctx, GLenum target,
				   GLint level, GLint xoffset,
				   GLint yoffset, GLsizei width,
				   GLsizei height, GLenum format,
				   GLsizei imageSize, const GLvoid * data,
				   struct gl_texture_object *texObj,
				   struct gl_texture_image *texImage)
{
	radeon_texsubimage(ctx, 2, target, level, xoffset, yoffset, 0, width, height, 1,
		imageSize, format, 0, data, &ctx->Unpack, texObj, texImage, 1);
}


void radeonTexSubImage3D(GLcontext * ctx, GLenum target, GLint level,
			 GLint xoffset, GLint yoffset, GLint zoffset,
			 GLsizei width, GLsizei height, GLsizei depth,
			 GLenum format, GLenum type,
			 const GLvoid * pixels,
			 const struct gl_pixelstore_attrib *packing,
			 struct gl_texture_object *texObj,
			 struct gl_texture_image *texImage)
{
	radeon_texsubimage(ctx, 3, target, level, xoffset, yoffset, zoffset, width, height, depth, 0,
		format, type, pixels, packing, texObj, texImage, 0);
}



/**
 * Ensure that the given image is stored in the given miptree from now on.
 */
static void migrate_image_to_miptree(radeon_mipmap_tree *mt, radeon_texture_image *image, int face, int level)
{
	radeon_mipmap_level *dstlvl = &mt->levels[level - mt->firstLevel];
	unsigned char *dest;

	assert(image->mt != mt);
	assert(dstlvl->width == image->base.Width);
	assert(dstlvl->height == image->base.Height);
	assert(dstlvl->depth == image->base.Depth);


	radeon_bo_map(mt->bo, GL_TRUE);
	dest = mt->bo->ptr + dstlvl->faces[face].offset;

	if (image->mt) {
		/* Format etc. should match, so we really just need a memcpy().
		 * In fact, that memcpy() could be done by the hardware in many
		 * cases, provided that we have a proper memory manager.
		 */
		radeon_mipmap_level *srclvl = &image->mt->levels[image->mtlevel-image->mt->firstLevel];

		assert(srclvl->size == dstlvl->size);
		assert(srclvl->rowstride == dstlvl->rowstride);

		radeon_bo_map(image->mt->bo, GL_FALSE);

		memcpy(dest,
			image->mt->bo->ptr + srclvl->faces[face].offset,
			dstlvl->size);
		radeon_bo_unmap(image->mt->bo);

		radeon_miptree_unreference(image->mt);
	} else {
		uint32_t srcrowstride;
		uint32_t height;
		/* need to confirm this value is correct */
		if (mt->compressed) {
			height = (image->base.Height + 3) / 4;
			srcrowstride = _mesa_compressed_row_stride(image->base.TexFormat->MesaFormat, image->base.Width);
		} else {
			height = image->base.Height * image->base.Depth;
			srcrowstride = image->base.Width * image->base.TexFormat->TexelBytes;
		}

//		if (mt->tilebits)
//			WARN_ONCE("%s: tiling not supported yet", __FUNCTION__);

		copy_rows(dest, dstlvl->rowstride, image->base.Data, srcrowstride,
			  height, srcrowstride);

		_mesa_free_texmemory(image->base.Data);
		image->base.Data = 0;
	}

	radeon_bo_unmap(mt->bo);

	image->mt = mt;
	image->mtface = face;
	image->mtlevel = level;
	radeon_miptree_reference(image->mt);
}

int radeon_validate_texture_miptree(GLcontext * ctx, struct gl_texture_object *texObj)
{
	radeonContextPtr rmesa = RADEON_CONTEXT(ctx);
	radeonTexObj *t = radeon_tex_obj(texObj);
	radeon_texture_image *baseimage = get_radeon_texture_image(texObj->Image[0][texObj->BaseLevel]);
	int face, level;

	if (t->validated || t->image_override)
		return GL_TRUE;

	if (RADEON_DEBUG & RADEON_TEXTURE)
		fprintf(stderr, "%s: Validating texture %p now\n", __FUNCTION__, texObj);

	if (baseimage->base.Border > 0)
		return GL_FALSE;

	/* Ensure a matching miptree exists.
	 *
	 * Differing mipmap trees can result when the app uses TexImage to
	 * change texture dimensions.
	 *
	 * Prefer to use base image's miptree if it
	 * exists, since that most likely contains more valid data (remember
	 * that the base level is usually significantly larger than the rest
	 * of the miptree, so cubemaps are the only possible exception).
	 */
	if (baseimage->mt &&
	    baseimage->mt != t->mt &&
	    radeon_miptree_matches_texture(baseimage->mt, &t->base)) {
		radeon_miptree_unreference(t->mt);
		t->mt = baseimage->mt;
		radeon_miptree_reference(t->mt);
	} else if (t->mt && !radeon_miptree_matches_texture(t->mt, &t->base)) {
		radeon_miptree_unreference(t->mt);
		t->mt = 0;
	}

	if (!t->mt) {
		if (RADEON_DEBUG & RADEON_TEXTURE)
			fprintf(stderr, " Allocate new miptree\n");
		radeon_try_alloc_miptree(rmesa, t, baseimage, 0, texObj->BaseLevel);
		if (!t->mt) {
			_mesa_problem(ctx, "radeon_validate_texture failed to alloc miptree");
			return GL_FALSE;
		}
	}

	/* Ensure all images are stored in the single main miptree */
	for(face = 0; face < t->mt->faces; ++face) {
		for(level = t->mt->firstLevel; level <= t->mt->lastLevel; ++level) {
			radeon_texture_image *image = get_radeon_texture_image(texObj->Image[face][level]);
			if (RADEON_DEBUG & RADEON_TEXTURE)
				fprintf(stderr, " face %i, level %i... %p vs %p ", face, level, t->mt, image->mt);
			if (t->mt == image->mt) {
				if (RADEON_DEBUG & RADEON_TEXTURE)
					fprintf(stderr, "OK\n");

				continue;
			}

			if (RADEON_DEBUG & RADEON_TEXTURE)
				fprintf(stderr, "migrating\n");
			migrate_image_to_miptree(t->mt, image, face, level);
		}
	}

	return GL_TRUE;
}


/**
 * Need to map texture image into memory before copying image data,
 * then unmap it.
 */
static void
radeon_get_tex_image(GLcontext * ctx, GLenum target, GLint level,
		     GLenum format, GLenum type, GLvoid * pixels,
		     struct gl_texture_object *texObj,
		     struct gl_texture_image *texImage, int compressed)
{
	radeon_texture_image *image = get_radeon_texture_image(texImage);

	if (image->mt) {
		/* Map the texture image read-only */
		radeon_teximage_map(image, GL_FALSE);
	} else {
		/* Image hasn't been uploaded to a miptree yet */
		assert(image->base.Data);
	}

	if (compressed) {
		/* FIXME: this can't work for small textures (mips) which
		         use different hw stride */
		_mesa_get_compressed_teximage(ctx, target, level, pixels,
					      texObj, texImage);
	} else {
		_mesa_get_teximage(ctx, target, level, format, type, pixels,
				   texObj, texImage);
	}
     
	if (image->mt) {
		radeon_teximage_unmap(image);
	}
}

void
radeonGetTexImage(GLcontext * ctx, GLenum target, GLint level,
		  GLenum format, GLenum type, GLvoid * pixels,
		  struct gl_texture_object *texObj,
		  struct gl_texture_image *texImage)
{
	radeon_get_tex_image(ctx, target, level, format, type, pixels,
			     texObj, texImage, 0);
}

void
radeonGetCompressedTexImage(GLcontext *ctx, GLenum target, GLint level,
			    GLvoid *pixels,
			    struct gl_texture_object *texObj,
			    struct gl_texture_image *texImage)
{
	radeon_get_tex_image(ctx, target, level, 0, 0, pixels,
			     texObj, texImage, 1);
}
