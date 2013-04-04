/**
 * Copyright (c) 2006-2013 LOVE Development Team
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 **/

#include "Image.h"

// STD
#include <cstring> // For memcpy
#include <algorithm> // for min/max

#include <iostream>

namespace love
{
namespace graphics
{
namespace opengl
{

float Image::maxMipmapSharpness = 0.0f;

Image::FilterMode Image::defaultMipmapFilter = Image::FILTER_NONE;
float Image::defaultMipmapSharpness = 0.0f;

Image::Image(love::image::ImageData *data)
	: cdata(0)
	, width((float)(data->getWidth()))
	, height((float)(data->getHeight()))
	, texture(0)
	, mipmapSharpness(defaultMipmapSharpness)
	, mipmapsCreated(false)
	, isCompressed(false)
{
	data->retain();
	this->data = data;
	preload();
}

Image::Image(love::image::CompressedData *cdata)
	: data(0)
	, width((float)(cdata->getWidth(0)))
	, height((float)(cdata->getHeight(0)))
	, texture(0)
	, mipmapSharpness(defaultMipmapSharpness)
	, mipmapsCreated(false)
	, isCompressed(true)
{
	cdata->retain();
	this->cdata = cdata;
	preload();
}

Image::~Image()
{
	if (data != 0)
		data->release();
	if (cdata != 0)
		cdata->release();
	unload();
}

float Image::getWidth() const
{
	return width;
}

float Image::getHeight() const
{
	return height;
}

const vertex *Image::getVertices() const
{
	return vertices;
}

love::image::ImageData *Image::getData() const
{
	return data;
}

void Image::getRectangleVertices(int x, int y, int w, int h, vertex *vertices) const
{
	// Check upper.
	x = (x+w > (int)width) ? (int)width-w : x;
	y = (y+h > (int)height) ? (int)height-h : y;

	// Check lower.
	x = (x < 0) ? 0 : x;
	y = (y < 0) ? 0 : y;

	vertices[0].x = 0;
	vertices[0].y = 0;
	vertices[1].x = 0;
	vertices[1].y = (float)h;
	vertices[2].x = (float)w;
	vertices[2].y = (float)h;
	vertices[3].x = (float)w;
	vertices[3].y = 0;

	float tx = (float)x/width;
	float ty = (float)y/height;
	float tw = (float)w/width;
	float th = (float)h/height;

	vertices[0].s = tx;
	vertices[0].t = ty;
	vertices[1].s = tx;
	vertices[1].t = ty+th;
	vertices[2].s = tx+tw;
	vertices[2].t = ty+th;
	vertices[3].s = tx+tw;
	vertices[3].t = ty;
}

void Image::draw(float x, float y, float angle, float sx, float sy, float ox, float oy, float kx, float ky) const
{
	static Matrix t;

	t.setTransformation(x, y, angle, sx, sy, ox, oy, kx, ky);
	drawv(t, vertices);
}

void Image::drawq(love::graphics::Quad *quad, float x, float y, float angle, float sx, float sy, float ox, float oy, float kx, float ky) const
{
	static Matrix t;
	const vertex *v = quad->getVertices();

	t.setTransformation(x, y, angle, sx, sy, ox, oy, kx, ky);
	drawv(t, v);
}

void Image::checkMipmapsCreated()
{
	if (mipmapsCreated || (filter.mipmap != FILTER_NEAREST && filter.mipmap != FILTER_LINEAR))
		return;

	if (!(isCompressed && cdata) && !hasMipmapSupport())
		throw love::Exception("Mipmap filtering is not supported on this system.");

	// Some old drivers claim support for NPOT textures, but fail when creating mipmaps.
	// we can't detect which systems will do this, so we fail gracefully for all NPOT images.
	int w = int(width), h = int(height);
	if (!isCompressed && (w != next_p2(w) || h != next_p2(h)))
		throw love::Exception("Cannot create mipmaps: image does not have power of two dimensions.");

	bind();

	if (isCompressed && cdata && hasCompressedTextureSupport(cdata->getType()))
	{
		int numMipmaps = cdata->getNumMipmaps();

		// We have to inform OpenGL if the image doesn't have all mipmap levels.
		if (GLEE_VERSION_1_2 || GLEE_SGIS_texture_lod)
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, numMipmaps - 1);
		else if (cdata->getWidth(numMipmaps-1) > 1 || cdata->getHeight(numMipmaps-1) > 1)
		{
			// Telling OpenGL to ignore certain levels isn't always supported.
			throw love::Exception("Cannot load mipmaps: "
								  "compressed image does not have all required levels.");
		}

		GLenum format = getCompressedFormat(cdata->getType());

		for (int i = 1; i < numMipmaps; i++)
		{
			glCompressedTexImage2DARB(GL_TEXTURE_2D,
			                          i,
			                          format,
			                          cdata->getWidth(i),
			                          cdata->getHeight(i),
			                          0,
			                          GLsizei(cdata->getSize(i)),
			                          cdata->getData(i));
		}
	}
	else if (data && hasNpot() && (GLEE_VERSION_3_0 || GLEE_ARB_framebuffer_object))
	{
		// AMD/ATI drivers have several bugs when generating mipmaps,
		// re-uploading the entire base image seems to be required.
		glTexImage2D(GL_TEXTURE_2D,
		             0,
		             GL_RGBA8,
		             (GLsizei)width,
		             (GLsizei)height,
		             0,
		             GL_RGBA,
		             GL_UNSIGNED_BYTE,
		             data->getData());

		// More bugs: http://www.opengl.org/wiki/Common_Mistakes#Automatic_mipmap_generation
		glEnable(GL_TEXTURE_2D);
		glGenerateMipmap(GL_TEXTURE_2D);
	}
	else if (data)
	{
		glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
		glTexSubImage2D(GL_TEXTURE_2D,
		                0,
		                0,
		                0,
		                (GLsizei)width,
		                (GLsizei)height,
		                GL_RGBA,
		                GL_UNSIGNED_BYTE,
		                data->getData());
	}
	else
		return;

	mipmapsCreated = true;
}

void Image::setFilter(const Image::Filter &f)
{
	filter = f;

	bind();
	filter.anisotropy = setTextureFilter(f);
	checkMipmapsCreated();
}

const Image::Filter &Image::getFilter() const
{
	return filter;
}

void Image::setWrap(const Image::Wrap &w)
{
	wrap = w;

	bind();
	setTextureWrap(w);
}

const Image::Wrap &Image::getWrap() const
{
	return wrap;
}

void Image::setMipmapSharpness(float sharpness)
{
	if (hasMipmapSharpnessSupport())
	{
		// LOD bias has the range (-maxbias, maxbias)
		mipmapSharpness = std::min(std::max(sharpness, -maxMipmapSharpness + 0.01f), maxMipmapSharpness - 0.01f);

		bind();
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, -mipmapSharpness); // negative bias is sharper
	}
	else
		mipmapSharpness = 0.0f;
}

float Image::getMipmapSharpness() const
{
	return mipmapSharpness;
}

void Image::bind() const
{
	if (texture == 0)
		return;

	bindTexture(texture);
}

void Image::preload()
{
	memset(vertices, 255, sizeof(vertex)*4);

	vertices[0].x = 0;
	vertices[0].y = 0;
	vertices[1].x = 0;
	vertices[1].y = height;
	vertices[2].x = width;
	vertices[2].y = height;
	vertices[3].x = width;
	vertices[3].y = 0;

	vertices[0].s = 0;
	vertices[0].t = 0;
	vertices[1].s = 0;
	vertices[1].t = 1;
	vertices[2].s = 1;
	vertices[2].t = 1;
	vertices[3].s = 1;
	vertices[3].t = 0;

	filter = getDefaultFilter();
	filter.mipmap = defaultMipmapFilter;
}

bool Image::load()
{
	return loadVolatile();
}

void Image::unload()
{
	return unloadVolatile();
}

bool Image::loadVolatile()
{
	if (isCompressed && cdata && !hasCompressedTextureSupport(cdata->getType()))
	{
		const char *str;
		if (image::CompressedData::getConstant(cdata->getType(), str))
		{
			throw love::Exception("Cannot create image: "
								  "%s compressed images are not supported on this system.", str);
		}
		else
			throw love::Exception("cannot create image: format is not supported on this system.");
	}

	if (hasMipmapSharpnessSupport() && maxMipmapSharpness == 0.0f)
		glGetFloatv(GL_MAX_TEXTURE_LOD_BIAS, &maxMipmapSharpness);

	if (hasNpot())
		return loadVolatileNPOT();
	else
		return loadVolatilePOT();
}

bool Image::loadVolatilePOT()
{
	glGenTextures(1,(GLuint *)&texture);
	bindTexture(texture);

	filter.anisotropy = setTextureFilter(filter);
	setTextureWrap(wrap);

	float p2width = next_p2(width);
	float p2height = next_p2(height);
	float s = width/p2width;
	float t = height/p2height;

	vertices[1].t = t;
	vertices[2].t = t;
	vertices[2].s = s;
	vertices[3].s = s;

	while (glGetError() != GL_NO_ERROR); // clear errors

	if (isCompressed && cdata)
	{
		if (s != 1.0f || t != 1.0f)
		{
			throw love::Exception("Cannot create image: "
								  "NPOT compressed images are not supported on this system.");
		}

		GLenum format = getCompressedFormat(cdata->getType());

		glCompressedTexImage2DARB(GL_TEXTURE_2D,
		                          0,
		                          format,
		                          cdata->getWidth(0),
		                          cdata->getHeight(0),
		                          0,
		                          GLsizei(cdata->getSize(0)),
		                          cdata->getData(0));
	}
	else if (data)
	{
		glTexImage2D(GL_TEXTURE_2D,
		             0,
		             GL_RGBA8,
		             (GLsizei)p2width,
		             (GLsizei)p2height,
		             0,
		             GL_RGBA,
		             GL_UNSIGNED_BYTE,
		             0);

		glTexSubImage2D(GL_TEXTURE_2D,
		                0,
		                0,
		                0,
		                (GLsizei)width,
		                (GLsizei)height,
		                GL_RGBA,
		                GL_UNSIGNED_BYTE,
		                data->getData());
	}

	if (glGetError() != GL_NO_ERROR)
		throw love::Exception("Cannot create image: size may be too large for this system.");

	mipmapsCreated = false;
	checkMipmapsCreated();

	setMipmapSharpness(mipmapSharpness);

	return true;
}

bool Image::loadVolatileNPOT()
{
	glGenTextures(1,(GLuint *)&texture);
	bindTexture(texture);

	filter.anisotropy = setTextureFilter(filter);
	setTextureWrap(wrap);

	while (glGetError() != GL_NO_ERROR); // clear errors

	if (isCompressed && cdata)
	{
		GLenum format = getCompressedFormat(cdata->getType());
		glCompressedTexImage2DARB(GL_TEXTURE_2D,
		                          0,
		                          format,
		                          cdata->getWidth(0),
		                          cdata->getHeight(0),
		                          0,
		                          GLsizei(cdata->getSize(0)),
		                          cdata->getData(0));
	}
	else if (data)
	{
		glTexImage2D(GL_TEXTURE_2D,
		             0,
		             GL_RGBA8,
		             (GLsizei)width,
		             (GLsizei)height,
		             0,
		             GL_RGBA,
		             GL_UNSIGNED_BYTE,
		             data->getData());
	}

	if (glGetError() != GL_NO_ERROR)
		throw love::Exception("Cannot create image: size may be too large for this system.");

	mipmapsCreated = false;
	checkMipmapsCreated();

	setMipmapSharpness(mipmapSharpness);

	return true;
}

void Image::unloadVolatile()
{
	// Delete the hardware texture.
	if (texture != 0)
	{
		deleteTexture(texture);
		texture = 0;
	}
}

void Image::drawv(const Matrix &t, const vertex *v) const
{
	bind();

	glPushMatrix();

	glMultMatrixf((const GLfloat *)t.getElements());

	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glVertexPointer(2, GL_FLOAT, sizeof(vertex), (GLvoid *)&v[0].x);
	glTexCoordPointer(2, GL_FLOAT, sizeof(vertex), (GLvoid *)&v[0].s);
	glDrawArrays(GL_QUADS, 0, 4);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_VERTEX_ARRAY);

	glPopMatrix();
}

void Image::setDefaultMipmapSharpness(float sharpness)
{
	defaultMipmapSharpness = sharpness;
}

float Image::getDefaultMipmapSharpness()
{
	return defaultMipmapSharpness;
}

void Image::setDefaultMipmapFilter(Image::FilterMode f)
{
	defaultMipmapFilter = f;
}

Image::FilterMode Image::getDefaultMipmapFilter()
{
	return defaultMipmapFilter;
}

GLenum Image::getCompressedFormat(image::CompressedData::TextureType type) const
{
	switch (type)
	{
	case image::CompressedData::TYPE_DXT1:
		return GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
	case image::CompressedData::TYPE_DXT3:
		return GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
	case image::CompressedData::TYPE_DXT5:
		return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
	case image::CompressedData::TYPE_BC5s:
		return GL_COMPRESSED_SIGNED_RG_RGTC2;
	case image::CompressedData::TYPE_BC5u:
		return GL_COMPRESSED_RG_RGTC2;
	case image::CompressedData::TYPE_BC7:
		return GL_COMPRESSED_RGBA_BPTC_UNORM_ARB;
	case image::CompressedData::TYPE_BC7srgb:
		return GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM_ARB;
	default:
		return GL_RGBA8;
	}
}

bool Image::hasNpot()
{
	return GLEE_VERSION_2_0 || GLEE_ARB_texture_non_power_of_two;
}

bool Image::hasAnisotropicFilteringSupport()
{
	return GLEE_EXT_texture_filter_anisotropic;
}

bool Image::hasMipmapSupport()
{
	return GLEE_VERSION_1_4 || GLEE_SGIS_generate_mipmap;
}

bool Image::hasMipmapSharpnessSupport()
{
	return GLEE_VERSION_1_4 || GLEE_EXT_texture_lod_bias;
}

bool Image::hasCompressedTextureSupport()
{
	return GLEE_VERSION_1_3 || GLEE_ARB_texture_compression;
}

bool Image::hasCompressedTextureSupport(image::CompressedData::TextureType type)
{
	if (!hasCompressedTextureSupport())
		return false;

	switch (type)
	{
	case image::CompressedData::TYPE_DXT1:
	case image::CompressedData::TYPE_DXT3:
	case image::CompressedData::TYPE_DXT5:
		return GLEE_EXT_texture_compression_s3tc;

	case image::CompressedData::TYPE_BC5s:
	case image::CompressedData::TYPE_BC5u:
		return (GLEE_VERSION_3_0 || GLEE_ARB_texture_compression_rgtc);

	case image::CompressedData::TYPE_BC7:
	case image::CompressedData::TYPE_BC7srgb:
		return (GLEE_VERSION_4_2 || GLEE_ARB_texture_compression_bptc);

	default:
		return false;

	}
}

} // opengl
} // graphics
} // love
