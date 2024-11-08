/*	png.c
	Copyright (C) 2004-2024 Mark Tyler and Dmitry Groshev

	This file is part of mtPaint.

	mtPaint is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 3 of the License, or
	(at your option) any later version.

	mtPaint is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with mtPaint in the file COPYING.
*/

/* Rewritten for version 3.10 by Dmitry Groshev */

#include <stdio.h>
#include <errno.h>

#define PNG_READ_PACK_SUPPORTED

#include <png.h>
#include <zlib.h>
#ifdef U_JPEG
#define NEED_CMYK
/* !!! libjpeg 9 headers conflict with these */
#undef TRUE
#undef FALSE
#include <jpeglib.h>
/* !!! Since libjpeg 7, this conflicts with <windows.h>; with libjpeg 8a,
 * conflict can be avoided if windows.h is included BEFORE this - WJ */
#endif
#ifdef U_JP2
#if U_JP2 < 2
#define NEED_FILE2MEM
#endif
#define HANDLE_JP2
#include <openjpeg.h>
#endif
#ifdef U_JASPER
#define HANDLE_JP2
#include <jasper/jasper.h>
#endif
#ifdef U_TIFF
#define NEED_CMYK
#include <tiffio.h>
#endif
#ifdef U_WEBP
#include <webp/encode.h>
#include <webp/decode.h>
#endif
#if U_LCMS == 2
#include <lcms2.h>
/* For version 1.x compatibility */
#define icSigCmykData cmsSigCmykData
#define icSigRgbData cmsSigRgbData
#define icHeader cmsICCHeader
#elif defined U_LCMS
#include <lcms.h>
#endif

#include "global.h"
#undef _
#define _(X) X

#include "mygtk.h"
#include "memory.h"
#include "ani.h"
#include "png.h"
#include "canvas.h"
#include "toolbar.h"
#include "layer.h"
#include "spawn.h"
#include "thread.h"

/* Compatibility defines will be added when they ARE needed, not beforehand */
G_GNUC_BEGIN_IGNORE_DEPRECATIONS

/* Make fseek() and ftell() on Win64 have the same limits as on 64-bit Unix */
#ifdef _WIN64 /* LLP64 */
#define F_LONG_MAX LLONG_MAX /* What can be handled - including allocated */
typedef long long f_long;
#define ftell(A) _ftelli64(A)
#define fseek(A,B,C) _fseeki64(A, B, C)

#else /* LP64 or ILP32 */
#define F_LONG_MAX LONG_MAX /* What can be handled - including allocated */
typedef long f_long;

#endif

#include <stdint.h>
#if F_LONG_MAX > SIZE_MAX
#error "File offset limit exceeds allocation limit"
#endif


/* Macro for big-endian tags (IFF and BMP) */
#define TAG4B(A,B,C,D) (((A) << 24) + ((B) << 16) + ((C) << 8) + (D))

/* All-in-one transport container for animation save/load */
typedef struct {
	frameset fset;
	ls_settings settings;
	int mode;
	/* Explode frames mode */
	int desttype;
	int error, miss, cnt;
	int lastzero;
	char *destdir;
} ani_settings;

int silence_limit, jpeg_quality, png_compression;
int tga_RLE, tga_565, tga_defdir, jp2_rate;
int lzma_preset, zstd_level, tiff_predictor, tiff_rtype, tiff_itype, tiff_btype;
int webp_preset, webp_quality, webp_compression;
int lbm_mask, lbm_untrans, lbm_pack, lbm_pbm;
int apply_icc;

fformat file_formats[NUM_FTYPES] = {
	{ "", "", "", 0},
	{ "PNG", "png", "apng", FF_256 | FF_RGB | FF_ANIM | FF_ALPHA | FF_MULTI
		| FF_MEM, XF_TRANS | XF_COMPZ },
#ifdef U_JPEG
	{ "JPEG", "jpg", "jpeg", FF_RGB, XF_COMPJ },
#else
	{ "", "", "", 0},
#endif
#ifdef HANDLE_JP2
	{ "JPEG2000", "jp2", "", FF_RGB | FF_ALPHA, XF_COMPJ2 },
	{ "J2K", "j2k", "jpc", FF_RGB | FF_ALPHA, XF_COMPJ2 },
#else
	{ "", "", "", 0},
	{ "", "", "", 0},
#endif
#ifdef U_TIFF
#define TIFF0FLAGS FF_LAYER | FF_MEM
#define TIFFLAGS (FF_BW | FF_256 | FF_RGB | FF_ALPHA | TIFF0FLAGS)
	{ "TIFF", "tif", "tiff", TIFFLAGS, XF_COMPT },
#else
	{ "", "", "", 0},
#endif
	{ "GIF", "gif", "", FF_256 | FF_ANIM, XF_TRANS },
	{ "BMP", "bmp", "", FF_256 | FF_RGB | FF_ALPHAR | FF_MEM },
	{ "XPM", "xpm", "", FF_256 | FF_RGB, XF_TRANS | XF_SPOT },
	{ "XBM", "xbm", "", FF_BW, XF_SPOT },
	{ "LSS16", "lss", "", FF_16 },
	{ "TGA", "tga", "", FF_256 | FF_RGB | FF_ALPHAR, XF_TRANS | XF_COMPR },
	{ "PCX", "pcx", "", FF_256 | FF_RGB },
	{ "PBM", "pbm", "", FF_BW | FF_LAYER },
	{ "PGM", "pgm", "", FF_256 | FF_LAYER | FF_NOSAVE },
	{ "PPM", "ppm", "pnm", FF_RGB | FF_LAYER },
	{ "PAM", "pam", "", FF_BW | FF_RGB | FF_ALPHA | FF_LAYER },
	{ "GPL", "gpl", "", FF_PALETTE },
	{ "TXT", "txt", "", FF_PALETTE },
	{ "PAL", "pal", "", FF_PALETTE },
	{ "ACT", "act", "", FF_PALETTE },
	{ "LAYERS", "txt", "", FF_LAYER },
/* !!! No 2nd layers format yet */
	{ "", "", "", 0},
/* An X pixmap - not a file at all */
	{ "PIXMAP", "", "", FF_RGB | FF_NOSAVE },
/* SVG image - import only */
	{ "SVG", "svg", "svgz", FF_RGB | FF_ALPHA | FF_SCALE | FF_NOSAVE },
/* mtPaint's own format - extended PAM */
	{ "* PMM *", "pmm", "", FF_256 | FF_RGB | FF_ANIM | FF_ALPHA | FF_MULTI
		| FF_LAYER | FF_PALETTE | FF_MEM, XF_TRANS },
#ifdef U_WEBP
	{ "WEBP", "webp", "", FF_RGB | FF_ANIM | FF_ALPHA, XF_COMPW },
#else
	{ "", "", "", 0},
#endif
	{ "LBM", "lbm", "ilbm", FF_256 | FF_RGB | FF_ALPHA, XF_TRANS | XF_COMPRL },
};

#ifndef U_TIFF
tiff_format tiff_formats[] = { { NULL } };
#endif
#ifndef U_WEBP
char *webp_presets[] = { NULL };
#endif

int file_type_by_ext(char *name, guint32 mask)
{
	char *ext;
	int i, l = LONGEST_EXT;


	ext = strrchr(name, '.');
	if (!ext || !ext[0]) return (FT_NONE);

	/* Special case for exploded frames (*.gif.000 etc.) */
	if (!ext[strspn(ext, ".0123456789")] && memchr(name, '.', ext - name))
	{
		char *tmp = ext;

		while (*(--ext) != '.');
		if (tmp - ext - 1 < LONGEST_EXT) l = tmp - ext - 1;
	}

	ext++;
	for (i = 0; i < NUM_FTYPES; i++)
	{
		unsigned int flags = file_formats[i].flags;

		if ((flags & FF_NOSAVE) || !(flags & mask)) continue;
		if (!strncasecmp(ext, file_formats[i].ext, l))
			return (i);
		if (!file_formats[i].ext2[0]) continue;
		if (!strncasecmp(ext, file_formats[i].ext2, l))
			return (i);
	}

	return (FT_NONE);
}

/* Which of 2 palette colors is more like black */
static int get_bw(ls_settings *settings)
{
	return (pal2B(settings->pal + 0) > pal2B(settings->pal + 1));
}

/* Set palette to white and black */
static void set_bw(ls_settings *settings)
{
	static const png_color wb[2] = { { 255, 255, 255 }, { 0, 0, 0 } };
	settings->colors = 2;
	memcpy(settings->pal, wb, sizeof(wb));
}

/* Set palette to grayscale */
static void set_gray(ls_settings *settings)
{
	settings->colors = 256;
	mem_bw_pal(settings->pal, 0, 255);
}

/* Map RGB transparency to indexed */
static void map_rgb_trans(ls_settings *settings)
{
	int i;

	if ((settings->rgb_trans < 0) || (settings->bpp < 3)) return;
	// Look for transparent colour in palette
	for (i = 0; i < settings->colors; i++)
	{
		if (PNG_2_INT(settings->pal[i]) != settings->rgb_trans) continue;
		settings->xpm_trans = i;
		return;
	}
	// Colour not in palette so force it into last entry
	settings->pal[255].red = INT_2_R(settings->rgb_trans);
	settings->pal[255].green = INT_2_G(settings->rgb_trans);
	settings->pal[255].blue = INT_2_B(settings->rgb_trans);
	settings->xpm_trans = 255;
	settings->colors = 256;
}

static int check_next_frame(frameset *fset, int mode, int anim)
{
	int lim = mode != FS_LAYER_LOAD ? FRAMES_MAX : anim ? MAX_LAYERS - 1 :
		MAX_LAYERS;
	return (fset->cnt < lim);
}

static int write_out_frame(char *file_name, ani_settings *ani, ls_settings *f_set);

static int process_page_frame(char *file_name, ani_settings *ani, ls_settings *w_set)
{
	image_frame *frame;

	if (ani->settings.mode == FS_EXPLODE_FRAMES)
		return (write_out_frame(file_name, ani, w_set));

	/* Store a new frame */
// !!! Currently, frames are allocated without checking any limits
	if (!mem_add_frame(&ani->fset, w_set->width, w_set->height,
		w_set->bpp, CMASK_NONE, w_set->pal)) return (FILE_MEM_ERROR);
	frame = ani->fset.frames + (ani->fset.cnt - 1);
	frame->cols = w_set->colors;
	frame->trans = w_set->xpm_trans;
	frame->delay = w_set->gif_delay > 0 ? w_set->gif_delay : 0;
	frame->x = w_set->x;
	frame->y = w_set->y;
	memcpy(frame->img, w_set->img, sizeof(chanlist));
	return (0);
}

/* Receives struct with image parameters, and channel flags;
 * returns 0 for success, or an error code;
 * success doesn't mean that anything was allocated, loader must check that;
 * loader may call this multiple times - say, for each channel */
static int allocate_image(ls_settings *settings, int cmask)
{
	size_t sz, l;
	int i, j, oldmask, wbpp, mode = settings->mode;

	if ((settings->width < 1) || (settings->height < 1)) return (-1);

	if ((settings->width > MAX_WIDTH) || (settings->height > MAX_HEIGHT))
		return (TOO_BIG);

	/* Don't show progress bar where there's no need */
	if (settings->width * settings->height <= (1 << silence_limit))
		settings->silent = TRUE;
	if (mode == FS_PATTERN_LOAD) settings->silent = TRUE;

	/* Reduce cmask according to mode */
	if (mode == FS_CLIP_FILE) cmask &= CMASK_CLIP;
	else if (mode == FS_CLIPBOARD) cmask &= CMASK_RGBA;
	else if ((mode == FS_CHANNEL_LOAD) || (mode == FS_PATTERN_LOAD))
		cmask &= CMASK_IMAGE;

	/* Overwriting is allowed */
	oldmask = cmask_from(settings->img);
	cmask &= ~oldmask;
	if (!cmask) return (0); // Already allocated

	/* No utility channels without image */
	oldmask |= cmask;
	if (!(oldmask & CMASK_IMAGE)) return (-1);

	/* Can ask for RGBA-capable image channel */
	wbpp = settings->bpp;
	if (wbpp > 3) settings->bpp = 3;

	j = TRUE; // For FS_LAYER_LOAD
	sz = (size_t)settings->width * settings->height;
	switch (mode)
	{
	case FS_PNG_LOAD: /* Regular image */
		/* Reserve memory */
		j = undo_next_core(UC_CREATE | UC_GETMEM, settings->width,
			settings->height, settings->bpp, oldmask);
		/* Drop current image if not enough memory for undo */
		if (j) mem_free_image(&mem_image, FREE_IMAGE);
	case FS_EXPLODE_FRAMES: /* Frames' temporaries */
	case FS_LAYER_LOAD: /* Layers */
		/* Allocate, or at least try to */
		for (i = 0; i < NUM_CHANNELS; i++)
		{
			if (!(cmask & CMASK_FOR(i))) continue;
			l = i == CHN_IMAGE ? sz * wbpp : sz;
			settings->img[i] = j ? malloc(l) : mem_try_malloc(l);
			if (!settings->img[i]) return (FILE_MEM_ERROR);
		}
		break;
	case FS_CLIP_FILE: /* Clipboard */
	case FS_CLIPBOARD:
		/* Allocate the entire batch at once */
		if (cmask & CMASK_IMAGE)
		{
			j = mem_clip_new(settings->width, settings->height,
				settings->bpp, cmask, NULL);
			if (j) return (FILE_MEM_ERROR);
			memcpy(settings->img, mem_clip.img, sizeof(chanlist));
			break;
		}
		/* Try to extend image channel to RGBA if asked */
		if (wbpp > 3)
		{
			unsigned char *w = realloc(mem_clipboard, sz * wbpp);
			if (!w) return (FILE_MEM_ERROR);
			settings->img[CHN_IMAGE] = mem_clipboard = w;
		}
		/* Try to add clipboard alpha and/or mask */
		for (i = 0; i < NUM_CHANNELS; i++)
		{
			if (!(cmask & CMASK_FOR(i))) continue;
			if (!(settings->img[i] = mem_clip.img[i] = malloc(sz)))
				return (FILE_MEM_ERROR);
		}
		break;
	case FS_CHANNEL_LOAD: /* Current channel */
		/* Dimensions & depth have to be the same */
		if ((settings->width != mem_width) ||
			(settings->height != mem_height) ||
			(settings->bpp != MEM_BPP)) return (-1);
		/* Reserve memory */
		j = undo_next_core(UC_CREATE | UC_GETMEM, settings->width,
			settings->height, settings->bpp, CMASK_CURR);
		if (j) return (FILE_MEM_ERROR);
		/* Allocate */
		settings->img[CHN_IMAGE] = mem_try_malloc(sz * wbpp);
		if (!settings->img[CHN_IMAGE]) return (FILE_MEM_ERROR);
		break;
	case FS_PATTERN_LOAD: /* Patterns */
		/* Check dimensions & depth */
		if (!set_patterns(settings)) return (-1);
		/* Allocate temp memory */
		settings->img[CHN_IMAGE] = calloc(1, sz * wbpp);
		if (!settings->img[CHN_IMAGE]) return (FILE_MEM_ERROR);
		break;
	case FS_PALETTE_LOAD: /* Palette */
	case FS_PALETTE_DEF:
		return (-1); // Should not arrive here if palette is present
	}
	return (0);
}

/* Receives struct with image parameters, and which channels to deallocate */
static void deallocate_image(ls_settings *settings, int cmask)
{
	int i;

	/* No deallocating image channel */
	if (!(cmask &= ~CMASK_IMAGE)) return;

	for (i = 0; i < NUM_CHANNELS; i++)
	{
		if (!(cmask & CMASK_FOR(i))) continue;
		if (!settings->img[i]) continue;

		free(settings->img[i]);
		settings->img[i] = NULL;

		/* Clipboard */
		if ((settings->mode == FS_CLIP_FILE) ||
			(settings->mode == FS_CLIPBOARD))
			mem_clip.img[i] = NULL;
	}
}

/* Deallocate alpha channel if useless; namely, all filled with given value */
static void delete_alpha(ls_settings *settings, int v)
{
	if (settings->img[CHN_ALPHA] && is_filled(settings->img[CHN_ALPHA], v,
		settings->width * settings->height))
		deallocate_image(settings, CMASK_ALPHA);
}

typedef struct {
	FILE *file; // for traditional use
	memx2 m; // data
	int top;  // end of data
} memFILE;
#define MEMFILE_MAX INT_MAX /* How much it can hold */

#if MEMFILE_MAX != MEMX2_MAX
#error "Mismatched max sizes"
#endif

static size_t mfread(void *ptr, size_t size, size_t nmemb, memFILE *mf)
{
	size_t l, m;

	if (mf->file) return (fread(ptr, size, nmemb, mf->file));

	if ((mf->m.here < 0) || (mf->m.here > mf->top)) return (0);
	l = size * nmemb; m = mf->top - mf->m.here;
	if (l > m) l = m , nmemb = m / size;
	memcpy(ptr, mf->m.buf + mf->m.here, l);
	mf->m.here += l;
	return (nmemb);
}

static size_t mfwrite(void *ptr, size_t size, size_t nmemb, memFILE *mf)
{
	size_t l;

	if (mf->file) return (fwrite(ptr, size, nmemb, mf->file));

	if (mf->m.here < 0) return (0);
	l = getmemx2(&mf->m, size * nmemb);
	nmemb = l / size;
	memcpy(mf->m.buf + mf->m.here, ptr, l);
	mf->m.here += l;
	if (mf->top < mf->m.here) mf->top = mf->m.here;
	return (nmemb);
}

static int mfseek(memFILE *mf, f_long offset, int mode)
{
// !!! For operating on tarballs, adjust fseek() params here
	if (mf->file) return (fseek(mf->file, offset, mode));

	if (mode == SEEK_SET);
	else if (mode == SEEK_CUR) offset += mf->m.here;
	else if (mode == SEEK_END) offset += mf->top;
	else return (-1);
	if ((offset < 0) || (offset > MEMFILE_MAX)) return (-1);
	mf->m.here = offset;
	return (0);
}

static char *mfgets(char *s, int size, memFILE *mf)
{
	size_t m;
	char *t, *v;

	if (mf->file) return (fgets(s, size, mf->file));

	if (size < 1) return (NULL);
	if ((mf->m.here < 0) || (mf->m.here > mf->top)) return (NULL);
	m = mf->top - mf->m.here;
	if (m >= (unsigned)size) m = size - 1;
	t = memchr(v = mf->m.buf + mf->m.here, '\n', m);
	if (t) m = t - v + 1;
	memcpy(s, v, m);
	s[m] = '\0';
	mf->m.here += m;
	return (s);
}

static int mfputs(const char *s, memFILE *mf)
{
	size_t l;

	if (mf->file) return (fputs(s, mf->file));

	l = strlen(s);
	return (!l || mfwrite((void *)s, l, 1, mf) ? 0 : EOF);
}

static int mfputss(memFILE *mf, const char *s, ...)
{
	va_list args;
	size_t l;
	int m = 0;

	va_start(args, s);
	while (s)
	{
		if (mf->file) m = fputs(s, mf->file);
		else
		{
			l = strlen(s);
			m = !l || mfwrite((void *)s, l, 1, mf) ? 0 : EOF;
		}
		if (m < 0) break;
		s = va_arg(args, const char *);
	}
	va_end(args);
	return (m);
}

static void copy_run(unsigned char *dest, unsigned char *src, int len,
	int dstep, int sstep, int bgr)
{
	if (bgr) bgr = 2;
	while (len-- > 0)
	{
		dest[0] = src[bgr];
		dest[1] = src[1];
		dest[2] = src[bgr ^ 2];
		dest += dstep;
		src += sstep;
	}
}

/* Fills temp buffer row, or returns image row if no buffer */
static unsigned char *prepare_row(unsigned char *buf, ls_settings *settings,
	int bpp, int y)
{
	unsigned char *tmp, *tmi, *tma, *tms;
	int i, j, w = settings->width, h = y * w;
	int bgr = (settings->ftype == FT_BMP) || (settings->ftype == FT_TGA) ? 2 : 0;

	tmi = settings->img[CHN_IMAGE] + h * settings->bpp;
	if (bpp < (bgr ? 3 : 4)) /* Return/copy image row */
	{
		if (!buf) return (tmi);
		memcpy(buf, tmi, w * bpp);
		return (buf);
	}

	/* Produce BGR / BGRx / RGBx */
	tmp = buf;
	if (settings->bpp == 1) // Indexed
	{
		png_color *pal = settings->pal;

		for (i = 0; i < w; tmp += bpp , i++)
		{
			png_color *col = pal + *tmi++;
			tmp[bgr] = col->red;
			tmp[1] = col->green;
			tmp[bgr ^ 2] = col->blue;
		}
	}
	else copy_run(tmp, tmi, w, bpp, 3, bgr); // RGB

	/* Add alpha to the mix */
	tmp = buf + 3;
	tma = settings->img[CHN_ALPHA] + h;
	if (bpp == 3); // No alpha - all done
	else if ((settings->mode != FS_CLIPBOARD) || !settings->img[CHN_SEL])
	{
		// Only alpha here
		for (i = 0; i < w; tmp += bpp , i++)
			*tmp = *tma++;
	}
	else
	{
		// Merge alpha and selection
		tms = settings->img[CHN_SEL] + h;
		for (i = 0; i < w; tmp += bpp , i++)
		{
			j = *tma++ * *tms++;
			*tmp = (j + (j >> 8) + 1) >> 8;
		}
	}

	return (buf);
}

/* Converts palette-based transparency to color transparency or alpha channel */
static int palette_trans(ls_settings *settings, unsigned char ttb[256])
{
	int i, n, res;

	/* Count transparent colors */
	for (i = n = 0; i < 256; i++) n += ttb[i] < 255;
	/* None means no transparency */
	settings->xpm_trans = -1;
	if (!n) return (0);
	/* One fully transparent color means color transparency */
	if (n == 1)
	{
		for (i = 0; i < 256; i++)
		{
			if (ttb[i]) continue;
			settings->xpm_trans = i;
			return (0);
		}
	}
	/* Anything else means alpha transparency */
	res = allocate_image(settings, CMASK_ALPHA);
	if (!res && settings->img[CHN_ALPHA])
	{
		unsigned char *src, *dest;
		size_t i = (size_t)settings->width * settings->height;

		src = settings->img[CHN_IMAGE];
		dest = settings->img[CHN_ALPHA];
		while (i-- > 0) *dest++ = ttb[*src++];
	}
	return (res);
}

static void ls_init(char *what, int save)
{
	what = g_strdup_printf(save ? __("Saving %s image") : __("Loading %s image"), what);
	progress_init(what, 0);
	g_free(what);
}

static void ls_progress(ls_settings *settings, int n, int steps)
{
	int h = settings->height;

	if (!settings->silent && ((n * steps) % h >= h - steps))
		progress_update((float)n / h);
}

#if PNG_LIBPNG_VER >= 10400 /* 1.4+ */
#define png_set_gray_1_2_4_to_8(X) png_set_expand_gray_1_2_4_to_8(X)
#endif

/* !!! libpng 1.2.17-1.2.24 was losing extra chunks if there was no callback */
static int buggy_libpng_handler()
{
	return (0);
}

static void png_memread(png_structp png_ptr, png_bytep data, png_size_t length)
{
	memFILE *mf = (memFILE *)png_get_io_ptr(png_ptr);
//	memFILE *mf = (memFILE *)png_ptr->io_ptr;
	size_t l = mf->top - mf->m.here;

	if (l > length) l = length;
	memcpy(data, mf->m.buf + mf->m.here, l);
	mf->m.here += l;
	if (l < length) png_error(png_ptr, "Read Error");
}

static void png_memwrite(png_structp png_ptr, png_bytep data, png_size_t length)
{
	memFILE *mf = (memFILE *)png_get_io_ptr(png_ptr);
//	memFILE *mf = (memFILE *)png_ptr->io_ptr;

	if (getmemx2(&mf->m, length) < length)
		png_error(png_ptr, "Write Error");
	else
	{
		memcpy(mf->m.buf + mf->m.here, data, length);
		mf->top = mf->m.here += length;
	}
}

static void png_memflush(png_structp png_ptr)
{
	/* Does nothing */
}

#define PNG_BYTES_TO_CHECK 8
#define PNG_HANDLE_CHUNK_NEVER  1
#define PNG_HANDLE_CHUNK_ALWAYS 3

static const char *chunk_names[NUM_CHANNELS] = { "", "alPh", "seLc", "maSk" };

static int load_png(char *file_name, ls_settings *settings, memFILE *mf, int frame)
{
	/* Description of PNG interlacing passes as X0, DX, Y0, DY */
	static const unsigned char png_interlace[8][4] = {
		{0, 1, 0, 1}, /* One pass for non-interlaced */
		{0, 8, 0, 8}, /* Seven passes for Adam7 interlaced */
		{4, 8, 0, 8},
		{0, 4, 4, 8},
		{2, 4, 0, 4},
		{0, 2, 2, 4},
		{1, 2, 0, 2},
		{0, 1, 1, 2}
	};
	static png_bytep *row_pointers;
	static char *msg;
	png_structp png_ptr;
	png_infop info_ptr;
	png_unknown_chunkp uk_p;
	png_uint_32 pwidth, pheight;
	char buf[PNG_BYTES_TO_CHECK + 1];
	unsigned char trans[256], *src, *dest, *dsta;
	long dest_len;
	FILE *fp = NULL;
	int i, j, k, bit_depth, color_type, interlace_type, num_uk, res = -1;
	int maxpass, x0, dx, y0, dy, n, nx, height, width, itrans = FALSE, anim = FALSE;

	if (!mf)
	{
		if ((fp = fopen(file_name, "rb")) == NULL) return -1;
		i = fread(buf, 1, PNG_BYTES_TO_CHECK, fp);
	}
	else i = mfread(buf, 1, PNG_BYTES_TO_CHECK, mf);
	if (i != PNG_BYTES_TO_CHECK) goto fail;
	if (png_sig_cmp(buf, 0, PNG_BYTES_TO_CHECK)) goto fail;

	png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!png_ptr) goto fail;

	row_pointers = NULL; msg = NULL;
	info_ptr = png_create_info_struct(png_ptr);
	if (!info_ptr) goto fail2;

	if (setjmp(png_jmpbuf(png_ptr)))
	{
		res = settings->width ? FILE_LIB_ERROR : -1;
		goto fail2;
	}

	/* Frame pseudo-files have wrong CRCs */
	if (frame) png_set_crc_action(png_ptr, PNG_CRC_QUIET_USE, PNG_CRC_QUIET_USE);
	/* !!! libpng 1.2.17-1.2.24 needs this to read extra channels */
	else png_set_read_user_chunk_fn(png_ptr, NULL, buggy_libpng_handler);

	if (!mf) png_init_io(png_ptr, fp);
	else png_set_read_fn(png_ptr, mf, png_memread);
	png_set_sig_bytes(png_ptr, PNG_BYTES_TO_CHECK);

	/* Stupid libpng handles private chunks on all-or-nothing basis */
	png_set_keep_unknown_chunks(png_ptr, frame ? PNG_HANDLE_CHUNK_NEVER :
		PNG_HANDLE_CHUNK_ALWAYS, NULL, 0);

	png_read_info(png_ptr, info_ptr);

	/* Check whether the file is APNG */
	num_uk = png_get_unknown_chunks(png_ptr, info_ptr, &uk_p);
	for (i = 0; i < num_uk; i++)
	{
		if (strcmp(uk_p[i].name, "acTL")) continue;
		anim = TRUE;
		/* mtPaint does not write APNGs with extra channels, and
		 * no reason to waste memory on APNG frames now */
		png_set_keep_unknown_chunks(png_ptr, PNG_HANDLE_CHUNK_NEVER, NULL, 0);
		png_set_read_user_chunk_fn(png_ptr, NULL, NULL);
	}

	png_get_IHDR(png_ptr, info_ptr, &pwidth, &pheight, &bit_depth, &color_type,
		&interlace_type, NULL, NULL);
	if (png_get_valid(png_ptr, info_ptr, PNG_INFO_PLTE))
	{
		png_colorp png_palette;

		png_get_PLTE(png_ptr, info_ptr, &png_palette, &settings->colors);
		memcpy(settings->pal, png_palette, settings->colors * sizeof(png_color));
		/* If palette is all we need */
		res = 1;
		if ((settings->mode == FS_PALETTE_LOAD) ||
			(settings->mode == FS_PALETTE_DEF)) goto fail3;
	}

	res = TOO_BIG;
	if ((pwidth > MAX_WIDTH) || (pheight > MAX_HEIGHT)) goto fail2;

	/* Call allocator for image data */
	settings->width = width = (int)pwidth;
	settings->height = height = (int)pheight;
	settings->bpp = 1;
	if ((color_type != PNG_COLOR_TYPE_PALETTE) || (bit_depth > 8))
		settings->bpp = 3;
	i = CMASK_IMAGE;
	if ((color_type == PNG_COLOR_TYPE_RGB_ALPHA) ||
		(color_type == PNG_COLOR_TYPE_GRAY_ALPHA)) i = CMASK_RGBA;
	if ((res = allocate_image(settings, i))) goto fail2;
	res = FILE_MEM_ERROR;

	i = sizeof(png_bytep) * height;
	row_pointers = malloc(i + width * 4);
	if (!row_pointers) goto fail2;
	row_pointers[0] = (char *)row_pointers + i;

	if (!settings->silent)
	{
		switch(settings->mode)
		{
		case FS_PNG_LOAD:
			msg = "PNG";
			break;
		case FS_CLIP_FILE:
		case FS_CLIPBOARD:
			msg = __("Clipboard");
			break;
		}
	}
	if (msg) ls_init(msg, 0);
	res = -1;

	/* RGB PNG file */
	if (settings->bpp == 3)
	{
		png_set_strip_16(png_ptr);
		png_set_gray_1_2_4_to_8(png_ptr);
		png_set_palette_to_rgb(png_ptr);
		png_set_gray_to_rgb(png_ptr);

		/* Is there a transparent color? */
		settings->rgb_trans = -1;
		if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
		{
			png_color_16p trans_rgb;

			png_get_tRNS(png_ptr, info_ptr, NULL, NULL, &trans_rgb);
			if (color_type == PNG_COLOR_TYPE_GRAY)
			{
				i = trans_rgb->gray;
				switch (bit_depth)
				{
				case 1: i *= 0xFF; break;
				case 2: i *= 0x55; break;
				case 4: i *= 0x11; break;
				case 8: default: break;
				/* Hope libpng compiled w/o accurate transform */
				case 16: i >>= 8; break;
				}
				settings->rgb_trans = RGB_2_INT(i, i, i);
			}
			else settings->rgb_trans = RGB_2_INT(trans_rgb->red,
				trans_rgb->green, trans_rgb->blue);
		}

		if (settings->img[CHN_ALPHA]) /* RGBA */
		{
			nx = height;
			/* Have to do deinterlacing myself */
			if (interlace_type == PNG_INTERLACE_NONE)
			{
				k = 0; maxpass = 1;
			}
			else if (interlace_type == PNG_INTERLACE_ADAM7)
			{
				k = 1; maxpass = 8;
				nx = (nx + 7) & ~7; nx += 7 * (nx >> 3);
			}
			else goto fail2; /* Unknown type */

			for (n = 0; k < maxpass; k++)
			{
				x0 = png_interlace[k][0];
				dx = png_interlace[k][1];
				y0 = png_interlace[k][2];
				dy = png_interlace[k][3];
				for (i = y0; i < height; i += dy , n++)
				{
					png_read_rows(png_ptr, &row_pointers[0], NULL, 1);
					src = row_pointers[0];
					dest = settings->img[CHN_IMAGE] + (i * width + x0) * 3;
					dsta = settings->img[CHN_ALPHA] + i * width;
					for (j = x0; j < width; j += dx)
					{
						dest[0] = src[0];
						dest[1] = src[1];
						dest[2] = src[2];
						dsta[j] = src[3];
						src += 4; dest += 3 * dx;
					}
					if (msg && ((n * 20) % nx >= nx - 20))
						progress_update((float)n / nx);
				}
			}
		}
		else /* RGB */
		{
			png_set_strip_alpha(png_ptr);
			for (i = 0; i < height; i++)
			{
				row_pointers[i] = settings->img[CHN_IMAGE] + i * width * 3;
			}
			png_read_image(png_ptr, row_pointers);
		}
	}
	/* Paletted PNG file */
	else
	{
		/* Is there a transparent index? */
		if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
		{
			png_bytep ptrans;
			int ltrans;

			png_get_tRNS(png_ptr, info_ptr, &ptrans, &ltrans, NULL);
			memset(trans, 255, 256);
			memcpy(trans, ptrans, ltrans);
			itrans = TRUE;
		}
		png_set_strip_16(png_ptr);
		png_set_strip_alpha(png_ptr);
		png_set_packing(png_ptr);
		if ((color_type == PNG_COLOR_TYPE_GRAY) && (bit_depth < 8))
			png_set_gray_1_2_4_to_8(png_ptr);
		for (i = 0; i < height; i++)
		{
			row_pointers[i] = settings->img[CHN_IMAGE] + i * width;
		}
		png_read_image(png_ptr, row_pointers);
	}
	if (msg) progress_update(1.0);

	png_read_end(png_ptr, info_ptr);
	res = 0;

	/* Apply palette transparency */
	if (itrans) res = palette_trans(settings, trans);

	num_uk = png_get_unknown_chunks(png_ptr, info_ptr, &uk_p);
	if (num_uk)	/* File contains mtPaint's private chunks */
	{
		for (i = 0; i < num_uk; i++)	/* Examine each chunk */
		{
			for (j = CHN_ALPHA; j < NUM_CHANNELS; j++)
			{
				if (!strcmp(uk_p[i].name, chunk_names[j])) break;
			}
			if (j >= NUM_CHANNELS) continue;

			/* Try to allocate a channel */
			if ((res = allocate_image(settings, CMASK_FOR(j)))) break;
			/* Skip if not allocated */
			if (!settings->img[j]) continue;

			dest_len = width * height;
			uncompress(settings->img[j], &dest_len, uk_p[i].data,
				uk_p[i].size);
		}
		/* !!! Is this call really needed? */
		png_free_data(png_ptr, info_ptr, PNG_FREE_UNKN, -1);
	}
	if (!res) res = anim ? FILE_HAS_FRAMES : 1;

#ifdef U_LCMS
#ifdef PNG_iCCP_SUPPORTED
	/* Extract ICC profile if it's of use */
	if (!settings->icc_size)
	{
#if PNG_LIBPNG_VER >= 10600 /* 1.6+ */
		png_bytep icc;
#else
		png_charp icc;
#endif
		png_charp name;
		png_uint_32 len;
		int comp;

		if (png_get_iCCP(png_ptr, info_ptr, &name, &comp, &icc, &len) &&
			(len < INT_MAX) && (settings->icc = malloc(len)))
		{
			settings->icc_size = len;
			memcpy(settings->icc, icc, len);
		}
	}
#endif
#endif

fail2:	if (msg) progress_end();
	free(row_pointers);
fail3:	png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
fail:	if (fp) fclose(fp);
	return (res);
}

#ifndef PNG_AFTER_IDAT
#define PNG_AFTER_IDAT 8
#endif

static int save_png(char *file_name, ls_settings *settings, memFILE *mf)
{
	png_unknown_chunk unknown0;
	png_structp png_ptr;
	png_infop info_ptr;
	FILE *fp = NULL;
	int h = settings->height, w = settings->width, bpp = settings->bpp;
	int i, j, res = -1;
	long uninit_(dest_len), res_len;
	char *mess = NULL;
	unsigned char trans[256], *tmp, *rgba_row = NULL;
	png_color_16 trans_rgb;

	/* Baseline PNG format does not support alpha for indexed images, so
	 * we have to convert them to RGBA for clipboard export - WJ */
	if (((settings->mode == FS_CLIPBOARD) || (bpp == 3)) &&
		settings->img[CHN_ALPHA])
	{
		rgba_row = malloc(w * 4);
		if (!rgba_row) return (-1);
		bpp = 4;
	}

	if (!settings->silent)
	switch(settings->mode)
	{
	case FS_PNG_SAVE:
		mess = "PNG";
		break;
	case FS_CLIP_FILE:
	case FS_CLIPBOARD:
		mess = __("Clipboard");
		break;
	case FS_COMPOSITE_SAVE:
		mess = __("Layer");
		break;
	case FS_CHANNEL_SAVE:
		mess = __("Channel");
		break;
	default:
		settings->silent = TRUE;
		break;
	}

	if (!mf && ((fp = fopen(file_name, "wb")) == NULL)) goto exit0;

	png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, (png_voidp)NULL, NULL, NULL);

	if (!png_ptr) goto exit1;

	info_ptr = png_create_info_struct(png_ptr);
	if (!info_ptr) goto exit2;

	res = 0;

	if (!mf) png_init_io(png_ptr, fp);
	else png_set_write_fn(png_ptr, mf, png_memwrite, png_memflush);
	png_set_compression_level(png_ptr, settings->png_compression);

	if (bpp == 1)
	{
		png_set_IHDR(png_ptr, info_ptr, w, h,
			8, PNG_COLOR_TYPE_PALETTE, PNG_INTERLACE_NONE,
			PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
		png_set_PLTE(png_ptr, info_ptr, settings->pal, settings->colors);
		/* Transparent index in use */
		if ((settings->xpm_trans > -1) && (settings->xpm_trans < 256))
		{
			memset(trans, 255, 256);
			trans[settings->xpm_trans] = 0;
			png_set_tRNS(png_ptr, info_ptr, trans, settings->colors, 0);
		}
	}
	else
	{
		png_set_IHDR(png_ptr, info_ptr, w, h,
			8, bpp == 4 ? PNG_COLOR_TYPE_RGB_ALPHA :
			PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
			PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
		if (settings->pal) png_set_PLTE(png_ptr, info_ptr, settings->pal,
			settings->colors);
		/* Transparent index in use */
		if ((settings->rgb_trans > -1) && !settings->img[CHN_ALPHA])
		{
			trans_rgb.red = INT_2_R(settings->rgb_trans);
			trans_rgb.green = INT_2_G(settings->rgb_trans);
			trans_rgb.blue = INT_2_B(settings->rgb_trans);
			png_set_tRNS(png_ptr, info_ptr, 0, 1, &trans_rgb);
		}
	}

	png_write_info(png_ptr, info_ptr);

	if (mess) ls_init(mess, 1);

	for (j = 0; j < h; j++)
	{
		tmp = prepare_row(rgba_row, settings, bpp, j);
		png_write_row(png_ptr, (png_bytep)tmp);
		ls_progress(settings, j, 20);
	}

	/* Save private chunks into PNG file if we need to */
	/* !!! Uncomment if default setting ever gets inadequate (in 1.7+) */
//	png_set_keep_unknown_chunks(png_ptr, PNG_HANDLE_CHUNK_ALWAYS, NULL, 0);
	tmp = NULL;
	i = bpp == 1 ? CHN_ALPHA : CHN_ALPHA + 1;
	if (settings->mode == FS_CLIPBOARD) i = NUM_CHANNELS; // Disable extensions
	for (j = 0; i < NUM_CHANNELS; i++)
	{
		if (!settings->img[i]) continue;
		if (!tmp)
		{
			/* Get size required for each zlib compress */
			w = settings->width * settings->height;
#if ZLIB_VERNUM >= 0x1200
			dest_len = compressBound(w);
#else
			dest_len = w + (w >> 8) + 32;
#endif
			res = -1;
			tmp = malloc(dest_len);	  // Temporary space for compression
			if (!tmp) break;
			res = 0;
		}
		res_len = dest_len;
		if (compress2(tmp, &res_len, settings->img[i], w,
			settings->png_compression) != Z_OK) continue;
		strncpy(unknown0.name, chunk_names[i], 5);
		unknown0.data = tmp;
		unknown0.size = res_len;
		unknown0.location = PNG_AFTER_IDAT;
		png_set_unknown_chunks(png_ptr, info_ptr, &unknown0, 1);
#if PNG_LIBPNG_VER < 10600 /* before 1.6 */
		png_set_unknown_chunk_location(png_ptr, info_ptr,
			j++, PNG_AFTER_IDAT);
#endif
	}
	free(tmp);
	png_write_end(png_ptr, info_ptr);

	if (mess) progress_end();

	/* Tidy up */
exit2:	png_destroy_write_struct(&png_ptr, &info_ptr);
exit1:	if (fp) fclose(fp);
exit0:	free(rgba_row);
	return (res);
}

/* *** PREFACE ***
 * Contrary to what GIF89 docs say, all contemporary browser implementations
 * always render background in an animated GIF as transparent. So does mtPaint,
 * for some GIF animations depend on this rule.
 * An inter-frame delay of 0 normally means that the two (or more) frames
 * are parts of same animation frame and should be rendered as one resulting
 * frame; but some (ancient) GIFs have all delays set to 0, but still contain
 * animation sequences. So the handling of zero-delay frames is user-selectable
 * in mtPaint. */

/* Animation state */
typedef struct {
	unsigned char lmap[MAX_DIM];
	image_frame prev;
	int prev_idx; // Frame index+1, so that 0 means None
	int have_frames; // Set after first
	int defw, defh, bk_rect[4];
	int mode;
	/* Extra fields for paletted images */
	int global_cols, newcols, newtrans;
	png_color global_pal[256], newpal[256];
	unsigned char xlat[513];
	/* Extra fields for RGBA images */
	int blend;
	unsigned char bkg[4];
} ani_status;

/* Initialize palette remapping table */
static void ani_init_xlat(ani_status *stat)
{
	int i;

	for (i = 0; i < 256; i++) stat->xlat[i] = stat->xlat[i + 256] = i;
	stat->xlat[512] = stat->newtrans;
}

/* Calculate new frame dimensions, and point-in-area bitmap */
static void ani_map_frame(ani_status *stat, ls_settings *settings)
{
	unsigned char *lmap;
	int i, j, w, h;


	/* Calculate the new dimensions */
// !!! Offsets considered nonnegative (as in GIF, WebP and APNG)
	w = settings->x + settings->width;
	h = settings->y + settings->height;
	if (!stat->have_frames) // Resize screen to fit the first frame
	{
		if (w > MAX_WIDTH) w = MAX_WIDTH;
		if (h > MAX_HEIGHT) h = MAX_HEIGHT;
		if (stat->defw < w) stat->defw = w;
		if (stat->defh < h) stat->defh = h;
		stat->have_frames = TRUE;
	}
	else // Clip all frames past the first to screen size
	{
		if (stat->defw < w) w = stat->defw;
		if (stat->defh < h) h = stat->defh;
	}

	/* The bitmap works this way: "Xth byte & (Yth byte >> 4)" tells which
	 * area(s) the pixel (X,Y) is in: bit 0 is for image (the new one),
	 * bit 1 is for underlayer (the previous composited frame), and bit 2
	 * is for hole in it (if "restore to background" was last) */
	j = stat->defw > stat->defh ? stat->defw : stat->defh;
	memset(lmap = stat->lmap, 0, j);
	// Mark new frame
	for (i = settings->x; i < w; i++) lmap[i] |= 0x01; // Image bit
	for (i = settings->y; i < h; i++) lmap[i] |= 0x10; // Image mask bit
	// Mark previous frame
	if (stat->prev_idx)
	{
		for (i = stat->prev.x , j = i + stat->prev.width; i < j; i++)
			lmap[i] |= 0x02; // Underlayer bit
		for (i = stat->prev.y , j = i + stat->prev.height; i < j; i++)
			lmap[i] |= 0x20; // Underlayer mask bit
	}
	// Mark disposal area
	if (clip(stat->bk_rect, 0, 0, stat->defw, stat->defh, stat->bk_rect))
	{
		for (i = stat->bk_rect[0] , j = stat->bk_rect[2]; i < j; i++)
			lmap[i] |= 0x04; // Background bit
		for (i = stat->bk_rect[1] , j = stat->bk_rect[3]; i < j; i++)
			lmap[i] |= 0x40; // Background mask bit
	}
}

/* Allowed bpp: 0 - move, 1 - indexed, 3 - RGB, 4 - RGBA
 * Indexed+alpha not supported nor should be: cannot be saved as standard PNG */
static int add_frame(ani_settings *ani, ani_status *stat, ls_settings *settings,
	int bpp, int disposal)
{
	int cmask = !bpp ? CMASK_NONE : bpp > 3 ? CMASK_RGBA : CMASK_IMAGE;
	int fbpp = !bpp ? settings->bpp : bpp > 3 ? 3 : bpp;
	image_frame *frame;

	/* Allocate a new frame */
	if (!mem_add_frame(&ani->fset, stat->defw, stat->defh, fbpp, cmask,
		stat->newpal)) return (FILE_MEM_ERROR);
	frame = ani->fset.frames + (ani->fset.cnt - 1);
	frame->cols = stat->newcols;
	frame->trans = stat->newtrans;
	frame->delay = settings->gif_delay;
	frame->flags = disposal; // Pass to compositing
	/* Tag zero-delay frame for deletion if requested */
	if ((ani->lastzero = (stat->mode == ANM_NOZERO) &&
		!settings->gif_delay)) frame->flags |= FM_NUKE;
	if (!bpp) // Same bpp & dimensions - reassign the chanlist
	{
		memcpy(frame->img, settings->img, sizeof(chanlist));
		memset(settings->img, 0, sizeof(chanlist));
	}
	return (0);
}

static int done_frame(char *file_name, ani_settings *ani, int last)
{
	int res = 1;

	if ((ani->settings.mode == FS_EXPLODE_FRAMES) && (!last ^ ani->lastzero))
		res = write_out_frame(file_name, ani, NULL);
	return (res ? res : 1);
}

static void composite_indexed_frame(image_frame *frame, ani_status *stat,
		ls_settings *settings)
{
	unsigned char *dest, *fg0, *bg0 = NULL, *lmap = stat->lmap;
	image_frame *bkf = &stat->prev;
	int w, fgw, bgw = 0, urgb = 0, tp = settings->xpm_trans;

	w = frame->width;
	dest = frame->img[CHN_IMAGE];
	fgw = settings->width;
	fg0 = settings->img[CHN_IMAGE] ? settings->img[CHN_IMAGE] -
		(settings->y * fgw + settings->x) : dest; // Always indexed (1 bpp)
	/* Pointer to absent underlayer is no problem - it just won't get used */
	bgw = bkf->width;
	bg0 = bkf->img[CHN_IMAGE] - (bkf->y * bgw + bkf->x) * bkf->bpp;
	urgb = bkf->bpp != 1;

	if (frame->bpp == 1) // To indexed
	{
		unsigned char *fg = fg0, *bg = bg0, *xlat = stat->xlat;
		int x, y;

		for (y = 0; y < frame->height; y++)
		{
			int bmask = lmap[y] >> 4;

			for (x = 0; x < w; x++)
			{
				int c0, bflag = lmap[x] & bmask;

				if ((bflag & 1) && ((c0 = fg[x]) != tp)) // New frame
					c0 += 256;
				else if ((bflag & 6) == 2) // Underlayer
					c0 = bg[x];
				else c0 = 512; // Background (transparent)
				*dest++ = xlat[c0];
			}
			fg += fgw; bg += bgw;
		}
	}
	else // To RGB
	{
		unsigned char rgb[513 * 3], *fg = fg0, *bg = bg0;
		int x, y, bpp = urgb + urgb + 1;

		/* Setup global palette map: underlayer, image, background */
		if (bkf->pal) pal2rgb(rgb, bkf->pal, 256, 0);
		pal2rgb(rgb + 256 * 3,
			settings->colors ? settings->pal : stat->global_pal, 256, 257);
		frame->trans = -1; // No color-key transparency

		for (y = 0; y < frame->height; y++)
		{
			int bmask = lmap[y] >> 4;

			for (x = 0; x < w; x++)
			{
				unsigned char *src;
				int c0, bflag = lmap[x] & bmask;

				if ((bflag & 1) && ((c0 = fg[x]) != tp)) // New frame
					src = rgb + (256 * 3) + (c0 * 3);
				else if ((bflag & 6) == 2) // Underlayer
					src = urgb ? bg + x * 3 : rgb + bg[x] * 3;
				else src = rgb + 512 * 3; // Background (black)
				dest[0] = src[0];
				dest[1] = src[1];
				dest[2] = src[2];
				dest += 3;
			}
			fg += fgw; bg += bgw * bpp;
		}
	}

	if (frame->img[CHN_ALPHA]) // To alpha
	{
		unsigned char *fg = fg0, *bg = NULL;
		int x, y, af = 0, utp = -1;

		dest = frame->img[CHN_ALPHA];
		utp = bkf->bpp == 1 ? bkf->trans : -1;
		af = !!bkf->img[CHN_ALPHA]; // Underlayer has alpha
		bg = bkf->img[af ? CHN_ALPHA : CHN_IMAGE] - (bkf->y * bgw + bkf->x);

		for (y = 0; y < frame->height; y++)
		{
			int bmask = lmap[y] >> 4;

			for (x = 0; x < w; x++)
			{
				int c0, bflag = lmap[x] & bmask;

				if ((bflag & 1) && (fg[x] != tp)) // New frame
					c0 = 255;
				else if ((bflag & 6) == 2) // Underlayer
				{
					c0 = bg[x];
					if (!af) c0 = c0 != utp ? 255 : 0;
				}
				else c0 = 0; // Background (transparent)
				*dest++ = c0;
			}
			fg += fgw; bg += bgw;
		}
	}
}

static void composite_rgba_frame(image_frame *frame, ani_status *stat,
		ls_settings *settings)
{
	static unsigned char bkg0[4]; // Default transparent black
	unsigned char mask[MAX_WIDTH], alpha[MAX_WIDTH], pal[768];
	unsigned char *dest, *src, *dsta, *srca, *bg, *bga, *lmap = stat->lmap;
	image_frame *bkf = &stat->prev;
	int rxy[4] = { 0, 0, frame->width, frame->height };
	int x, y, w, bgw, bgoff, fgw, ww, fgoff, dstoff, bpp, tr;


	/* Do the mixing if source is present */
	if (!settings->img[CHN_IMAGE]) return;

	w = frame->width;
	bgw = bkf->width;
	bgoff = bkf->y * bgw + bkf->x;
	bpp = bkf->bpp;
	if (bpp == 1) pal2rgb(pal, bkf->pal, bkf->cols, 256);

	/* First, generate the destination RGB */
	dest = frame->img[CHN_IMAGE];
	bg = bkf->img[CHN_IMAGE] - bgoff * bpp; // Won't get used if not valid
	for (y = 0; y < frame->height; y++)
	{
		int bmask = lmap[y] >> 4;

		for (x = 0; x < w; x++)
		{
			unsigned char *rgb = bkg0; // Default black
			int bflag = lmap[x] & bmask;

			if (bflag & 4) rgb = stat->bkg; // Background in the hole
			else if (bflag & 2) // Underlayer
				rgb = bpp == 1 ? pal + bg[x] * 3 : bg + x * 3;
			dest[0] = rgb[0];
			dest[1] = rgb[1];
			dest[2] = rgb[2];
			dest += 3;
		}
		bg += bgw * bpp;
	}

	/* Then, destination alpha */
	dsta = frame->img[CHN_ALPHA];
	bga = bkf->img[CHN_ALPHA] ? bkf->img[CHN_ALPHA] - bgoff : NULL;
	if (dsta) for (y = 0; y < frame->height; y++)
	{
		int bmask = lmap[y] >> 4;

		for (x = 0; x < w; x++)
		{
			int bflag = lmap[x] & bmask, a = 0; // Default transparent
			if (bflag & 2) a = bga ? bga[x] : 255; // Underlayer
			if (bflag & 4) a = stat->bkg[3]; // Background in the hole
			*dsta++ = a;
		}
		if (bga) bga += bgw;
	}

	/* Then, check if the new frame is in bounds */
	if (!clip(rxy, settings->x, settings->y,
		settings->x + settings->width, settings->y + settings->height, rxy)) return;

	/* Then, paste it over */
	fgw = settings->width;
	ww = rxy[2] - rxy[0];
	fgoff = (rxy[1] - settings->y) * fgw + (rxy[0] - settings->x);
	dstoff = rxy[1] * w + rxy[0];
	memset(alpha, 255, ww);
	tr = settings->rgb_trans;
	for (y = rxy[1]; y < rxy[3]; y++)
	{
		dsta = frame->img[CHN_ALPHA] ? frame->img[CHN_ALPHA] + dstoff : NULL;
		srca = settings->img[CHN_ALPHA] ? settings->img[CHN_ALPHA] + fgoff : NULL;
		dest = frame->img[CHN_IMAGE] + dstoff * 3;
		src = settings->img[CHN_IMAGE] + fgoff * 3;

		if (stat->blend) // Do alpha blend
		{
			memset(mask, 0, ww);
			if (tr >= 0) mem_mask_colors(mask, src, 255, ww, 1, 3, tr, tr);
			process_mask(0, 1, ww, mask, dsta, dsta, alpha, srca, 255, FALSE);
			process_img(0, 1, ww, mask, dest, dest, src, NULL, 3, BLENDF_SET);
		}
		else // Do a copy
		{
			memcpy(dest, src, ww * 3); // Copy image
			if (!dsta); // No alpha
			else if (srca) memcpy(dsta, srca, ww); // Copy alpha
			else
			{
				memset(dsta, 255, ww); // Fill alpha
				if (tr >= 0) mem_mask_colors(dsta, src, 0, ww, 1, 3, tr, tr);
			}
		}
		fgoff += fgw; dstoff += w;
	}
}

static void composite_frame(frameset *fset, ani_status *stat, ls_settings *settings)
{
	image_frame *frame = fset->frames + (fset->cnt - 1);
	int disposal;


	/* In raw mode, just store the offsets */
	if (stat->mode <= ANM_RAW)
	{
		frame->x = settings->x;
		frame->y = settings->y;
	}
	else
	{
		/* Read & clear disposal mode */
		disposal = frame->flags & FM_DISPOSAL;
		frame->flags ^= disposal ^ FM_DISP_REMOVE;

		/* For WebP and RGB[A] APNG */
		if (settings->bpp == 3) composite_rgba_frame(frame, stat, settings);
		/* For GIF & indexed[+T] APNG */
		else
		{
			/* No blend means ignoring transparent color */
			if (!stat->blend) settings->xpm_trans = -1;
			composite_indexed_frame(frame, stat, settings);
		}

		/* Drop alpha if not used */
		if (frame->img[CHN_ALPHA] && is_filled(frame->img[CHN_ALPHA], 255,
			frame->width * frame->height))
		{
			free(frame->img[CHN_ALPHA]);
			frame->img[CHN_ALPHA] = NULL;
		}

		/* If transparent color and alpha are both present, convert the
		 * color into alpha, as PNG does not allow combining them */
		if ((frame->trans >= 0) && frame->img[CHN_ALPHA])
		{
			/* RGBA: indexed+alpha blocked elsewhere for same reason */
			/* Use palette that add_frame() assigns */
			int tr = PNG_2_INT(stat->newpal[frame->trans]);
			mem_mask_colors(frame->img[CHN_ALPHA], frame->img[CHN_IMAGE],
				0, frame->width, frame->height, 3, tr, tr);
			frame->trans = -1;
		}

		/* Prepare the disposal action */
		if (disposal == FM_DISP_REMOVE) // Dispose to background
		{
			// Image-sized hole in underlayer
			stat->bk_rect[2] = (stat->bk_rect[0] = settings->x) +
				settings->width;
			stat->bk_rect[3] = (stat->bk_rect[1] = settings->y) +
				settings->height;
		}
		if (disposal == FM_DISP_LEAVE) // Don't dispose
			memset(&stat->bk_rect, 0, sizeof(stat->bk_rect)); // Clear old
		if ((disposal == FM_DISP_REMOVE) || (disposal == FM_DISP_LEAVE))
		{
			stat->prev = *frame; // Current frame becomes underlayer
			if (!stat->prev.pal) stat->prev.pal = fset->pal;
			if (stat->prev_idx &&
				(fset->frames[stat->prev_idx - 1].flags & FM_NUKE))
				/* Remove the unref'd frame */
				mem_remove_frame(fset, stat->prev_idx - 1);
			stat->prev_idx = fset->cnt;
		}
		/* if (disposal == FM_DISP_RESTORE); // Dispose to previous
			// Underlayer and hole stay unchanged */
	}
	if ((fset->cnt > 1) && (stat->prev_idx != fset->cnt - 1) &&
		(fset->frames[fset->cnt - 2].flags & FM_NUKE))
	{
		/* Remove the next-to-last frame */
		mem_remove_frame(fset, fset->cnt - 2);
		if (stat->prev_idx > fset->cnt)
			stat->prev_idx = fset->cnt;
	}
}

static int analyze_rgba_frame(ani_status *stat, ls_settings *settings)
{
	int same_size, holes, alpha0, alpha1, alpha, bpp;

	if (stat->mode <= ANM_RAW) // Raw frame mode
	{
		stat->defw = settings->width;
		stat->defh = settings->height;
		return (0); // Output matches input
	}
	else if ((stat->defw > MAX_WIDTH) || (stat->defh > MAX_HEIGHT))
		return (-1); // Too large

	ani_map_frame(stat, settings);
	same_size = !(settings->x | settings->y |
		(stat->defw ^ settings->width) | (stat->defh ^ settings->height));
	holes = !same_size || stat->blend;

	if (same_size && !holes) return (0); // New replaces old

	/* Indexed with transparent color (from APNG) stay as they were:
	 * no local palettes there, upgrade to RGB/RGBA never needed */
	if ((settings->bpp == 1) && (settings->xpm_trans >= 0))
		return (same_size ? 0 : 1);

	/* Alpha transparency on underlayer */
	alpha0 = !stat->prev_idx || stat->prev.img[CHN_ALPHA];
	/* Transparency from disposal to background */
	if ((stat->bk_rect[0] < stat->bk_rect[2]) &&
		(stat->bk_rect[1] < stat->bk_rect[3]))
		alpha0 |= stat->bkg[3] < 255;
	/* Alpha transparency from this layer */
	alpha1 = !stat->blend && settings->img[CHN_ALPHA];
	/* Result */
	alpha = alpha1 | (alpha0 & holes);

	/* Output bpp is max of underlayer & image */
	bpp = (stat->prev.bpp == 3) || (settings->bpp == 3) ? 3 : 1;
	/* Do not produce indexed+alpha as regular PNG does not support that */
	if (alpha) bpp = 4;

	/* !!! composite_rgba_frame() as of now cannot handle frame == dest, so
	 * do not return 0 even if same size, bpp & alpha, w/o rewriting that */
	return (bpp);
}

/* In absence of library support, APNG files are read through building in memory
 * a regular PNG file for a frame, and then feeding that to libpng - WJ */

/* Macros for accessing values in Motorola byte order */
#define GET16B(buf) (((buf)[0] << 8) + (buf)[1])
#define GET32B(buf) (((unsigned)(buf)[0] << 24) + ((buf)[1] << 16) + \
	((buf)[2] << 8) + (buf)[3])
#define PUT16B(buf, v) (buf)[0] = (v) >> 8; (buf)[1] = (v) & 0xFF;
#define PUT32B(buf, v) (buf)[0] = (v) >> 24; (buf)[1] = ((v) >> 16) & 0xFF; \
	(buf)[2] = ((v) >> 8) & 0xFF; (buf)[3] = (v) & 0xFF;

/* Macros for relevant PNG tags; big-endian */
#define TAG4B_IHDR TAG4B('I', 'H', 'D', 'R')
#define TAG4B_IDAT TAG4B('I', 'D', 'A', 'T')
#define TAG4B_IEND TAG4B('I', 'E', 'N', 'D')
#define TAG4B_acTL TAG4B('a', 'c', 'T', 'L')
#define TAG4B_fcTL TAG4B('f', 'c', 'T', 'L')
#define TAG4B_fdAT TAG4B('f', 'd', 'A', 'T')

/* PNG block header */
#define PNG_SIZE    0 /* 32b */
#define PNG_TAG     4 /* 32b */
#define PNG_HSIZE   8

/* IHDR block */
#define IHDR_W      0 /* 32b */
#define IHDR_H      4 /* 32b */
#define IHDR_SIZE  13

/* acTL block */
#define acTL_FCNT   0 /* 32b */
#define acTL_SIZE   8

/* fcTL block */
#define fcTL_SEQ    0 /* 32b */
#define fcTL_W      4 /* 32b */
#define fcTL_H      8 /* 32b */
#define fcTL_X     12 /* 32b */
#define fcTL_Y     16 /* 32b */
#define fcTL_DN    20 /* 16b */
#define fcTL_DD    22 /* 16b */
#define fcTL_DISP  24 /*  8b */
#define fcTL_BLEND 25 /*  8b */
#define fcTL_SIZE  26

typedef struct {
	int w, h, disp;
	f_long ihdr, idat0, fdat0, fdat1;
	unsigned frames;
	unsigned char fctl[fcTL_SIZE];
	int phase;
	unsigned char *png;	// Buffer for fake file
	size_t sz;		// Buffer size
	memFILE mf;
} pnghead;

/* Build in-memory PNG from file header and frame data, ignoring CRCs */
static int assemble_png(FILE *fp, pnghead *pg)
{
	size_t l = pg->idat0 + (pg->fdat1 - pg->fdat0) + PNG_HSIZE + 4;
	unsigned char *src, *dest, *wrk = pg->png;
	unsigned tag, tl, u, seq;


	/* Enlarge the buffer if needed */
	if (l > pg->sz)
	{
		if (l > MEMFILE_MAX) return (FILE_MEM_ERROR);
		dest = realloc(pg->png, l);
		if (!dest) return (FILE_MEM_ERROR);
		pg->png = dest;
		pg->sz = l;
	}
	/* Read in the header on first pass */
	if (!wrk)
	{
		fseek(fp, 0, SEEK_SET);
		if (!fread(pg->png, pg->idat0, 1, fp)) return (-1);
	}
	/* Modify the header */
	wrk = pg->png + pg->ihdr;
	memcpy(wrk + IHDR_W, pg->fctl + fcTL_W, 4);
	memcpy(wrk + IHDR_H, pg->fctl + fcTL_H, 4);
	/* Read in body */
	wrk = pg->png + pg->idat0;
	fseek(fp, pg->fdat0, SEEK_SET);
	l = pg->fdat1 - pg->fdat0;
	if (!fread(wrk, l, 1, fp)) return (-1);
	/* Reformat the body blocks if needed */
	seq = GET32B(pg->fctl + fcTL_SEQ);
	src = dest = wrk;
	while (l)
	{
		tag = GET32B(src + PNG_TAG);
		tl = GET32B(src + PNG_SIZE);
		if (tl > l - PNG_HSIZE - 4) return (-1); // Paranoia
		l -= u = PNG_HSIZE + tl + 4;
		if (tag == TAG4B_fdAT)
		{
			if (tl < 4) return (-1); // Paranoia
			if (GET32B(src + PNG_HSIZE) != ++seq) return (-1); // Sequence
			tl -= 4;
			PUT32B(dest + PNG_SIZE, tl);
			memcpy(dest + PNG_TAG, "IDAT", 4);
			memmove(dest + PNG_HSIZE, src + PNG_HSIZE + 4, tl);
		}
		else if (src != dest) memmove(dest, src, u);
		src += u;
		dest += PNG_HSIZE + tl + 4;
	}
	/* Add IEND */
	PUT32B(dest + PNG_SIZE, 0);
	memcpy(dest + PNG_TAG, "IEND", 4);
	/* Prepare file buffer */
	memset(&pg->mf, 0, sizeof(pg->mf));
	pg->mf.m.buf = pg->png;
	pg->mf.top = pg->mf.m.size = dest + PNG_HSIZE + 4 - pg->png;
	return (0);
}

static int png_scan(FILE *fp, pnghead *pg)
{
	/* APNG disposal codes mapping */
	static const unsigned short apng_disposal[3] = {
		FM_DISP_LEAVE, FM_DISP_REMOVE, FM_DISP_RESTORE };
	unsigned char buf[256];
	unsigned tag, tl, w, h;
	f_long p = ftell(fp);

	if (p <= 0) return (-1); // Sanity check

	/* Read block headers & see what we get */
	pg->phase = 0;
	while (TRUE)
	{
		if (fread(buf, 1, PNG_HSIZE, fp) < PNG_HSIZE)
		{
			if (pg->phase != 2) break; // Fail
			pg->phase = 4; // Improper end
			return (0); // Done
		}
		tag = GET32B(buf + PNG_TAG);
		tl = GET32B(buf + PNG_SIZE);
		if (tl > 0x7FFFFFFFU) break; // Limit
		if (p > F_LONG_MAX - tl - PNG_HSIZE - 4) break; // File too large

		if (tag == TAG4B_IHDR)
		{
			if (tl < IHDR_SIZE) break; // Bad
			if (pg->ihdr) break; // There must be only one
			pg->ihdr = p + PNG_HSIZE;
			/* Get canvas dimensions */
			if (!fread(buf, IHDR_SIZE, 1, fp)) break; // Fail
			w = GET32B(buf + IHDR_W);
			h = GET32B(buf + IHDR_H);
			if ((w > 0x7FFFFFFFU) || (h > 0x7FFFFFFFU)) break; // Limit
			pg->w = w;
			pg->h = h;
		}
		else if (tag == TAG4B_IDAT)
		{
			if (!pg->ihdr) break; // Fail
			if (!pg->idat0) pg->idat0 = p;
			if (pg->phase == 1) // Had a fcTL
			{
				pg->fdat0 = p;
				pg->phase = 2;
			}
			if (pg->phase > 1)
			{
				if (pg->fdat0 != pg->idat0) break; // Mixed IDAT & fdAT
				pg->fdat1 = p + PNG_HSIZE + tl + 4;
			}
		}
		else if (tag == TAG4B_acTL)
		{
			if (tl < acTL_SIZE) break; // Bad
			if (!fread(buf, acTL_SIZE, 1, fp)) break; // Fail
			/* Store frames count */
			if (!pg->frames) pg->frames = GET32B(buf + acTL_FCNT);
			if (pg->frames > 0x7FFFFFFFU) break; // Limit
			if (!pg->frames) break; // Fail
		}
		else if (tag == TAG4B_fcTL)
		{
			if (pg->phase > 1)
			{
				/* End of frame data - step back & return */
				fseek(fp, p, SEEK_SET);
				return (0); 
			}
			if (tl < fcTL_SIZE) break; // Bad
			/* Store for later use */
			if (!fread(pg->fctl, fcTL_SIZE, 1, fp)) break; // Fail
			if (pg->fctl[fcTL_DISP] > 2) break; // Unknown value
			pg->disp = apng_disposal[pg->fctl[fcTL_DISP]];
			pg->phase = 1; // Ready for a new frame
		}
		else if (tag == TAG4B_fdAT)
		{
			if (!pg->ihdr) break; // Fail
			if (!pg->phase) break; // Fail - no fcTL
			if (pg->phase == 1) // Had a fcTL
			{
				pg->fdat0 = p;
				pg->phase = 2;
			}
			if (pg->fdat0 == pg->idat0) break; // Mixed IDAT & fdAT
			pg->fdat1 = p + PNG_HSIZE + tl + 4;
		}
		else if (tag == TAG4B_IEND)
		{
			if (pg->phase != 2) break; // Fail
			pg->phase = 3; // End
			return (0); // Done
		}
		/* Skip tag header, data, & CRC field */
		p += PNG_HSIZE + tl + 4;
		if (fseek(fp, p, SEEK_SET)) break;
	}
	return (-1); // Failed
}

static int load_apng_frame(FILE *fp, pnghead *pg, ls_settings *settings)
{
	unsigned char *w;
	int l, res;

	/* Try scanning the frame */
	res = png_scan(fp, pg);
	/* Prepare fake PNG */
	if (!res) res = assemble_png(fp, pg);
	/* Load the frame */
	if (!res) res = load_png(NULL, settings, &pg->mf, TRUE);
	if (res != 1) return (res); // Fail on any error
	/* Convert indexed+alpha to RGBA, to let it be regular PNG */
	w = settings->img[CHN_ALPHA];
	if ((settings->bpp == 1) && w)
	{
		l = settings->width * settings->height;
		w = malloc((size_t)l * 3);
		if (!w) return (FILE_MEM_ERROR); // No memory
		do_convert_rgb(0, 1, l, w, settings->img[CHN_IMAGE], settings->pal);
		free(settings->img[CHN_IMAGE]);
		settings->img[CHN_IMAGE] = w;
		settings->bpp = 3;
	}
	/* Ensure transparent color is in palette */
	else map_rgb_trans(settings);
	return (res);
}

static int load_apng_frames(char *file_name, ani_settings *ani)
{
	char buf[PNG_BYTES_TO_CHECK + 1];
	pnghead pg;
	ani_status stat;
	ls_settings w_set;
	unsigned wx, wy;
	int n, d, bpp, frames = 0, res = -1;
	FILE *fp;


	if (!(fp = fopen(file_name, "rb"))) return (-1);
	memset(w_set.img, 0, sizeof(chanlist));
	memset(&pg, 0, sizeof(pg));

	if (fread(buf, 1, PNG_BYTES_TO_CHECK, fp) != PNG_BYTES_TO_CHECK) goto fail;
	if (png_sig_cmp(buf, 0, PNG_BYTES_TO_CHECK)) goto fail;

	w_set = ani->settings;
	res = load_apng_frame(fp, &pg, &w_set);
	if (res != 1) goto fail;

	/* Init state structure */
	memset(&stat, 0, sizeof(stat));
	stat.mode = ani->mode;
	stat.defw = pg.w;
	stat.defh = pg.h;
	/* Use whatever palette we read */
	mem_pal_copy(stat.newpal, w_set.pal);
	stat.newcols = w_set.colors;
	stat.newtrans = w_set.xpm_trans;
	ani_init_xlat(&stat); // Init palette remapping to 1:1 and leave at that

	/* Init frameset - palette in APNG is global */
	res = FILE_MEM_ERROR;
	if (!(ani->fset.pal = malloc(SIZEOF_PALETTE))) goto fail;
	mem_pal_copy(ani->fset.pal, stat.newpal);

	/* Go through images */
	while (frames++ < pg.frames)
	{
		res = FILE_TOO_LONG;
		if (!check_next_frame(&ani->fset, ani->settings.mode, TRUE))
			goto fail;

		/* Get the next frame, after the first */
		if (frames > 1)
		{
			w_set = ani->settings;
			res = load_apng_frame(fp, &pg, &w_set);
			if (res != 1) goto fail;
		}
		delete_alpha(&w_set, 255);

		stat.blend = pg.fctl[fcTL_BLEND] && (w_set.img[CHN_ALPHA] ||
			(stat.newtrans >= 0));
		/* Within mtPaint delays are 1/100s granular */
		n = GET16B(pg.fctl + fcTL_DN);
		d = GET16B(pg.fctl + fcTL_DD);
		if (!d) d = 100;
		w_set.gif_delay = (n * 100 + d - 1) / d; // Round up

		wx = GET32B(pg.fctl + fcTL_X);
		wy = GET32B(pg.fctl + fcTL_Y);
		if (wx > MAX_WIDTH) wx = MAX_WIDTH; // Out is out
		if (wy > MAX_HEIGHT) wy = MAX_HEIGHT; // Same
		w_set.x = wx;
		w_set.y = wy;

		/* Analyze how we can merge the frames */
		res = TOO_BIG;
		bpp = analyze_rgba_frame(&stat, &w_set);
		if (bpp < 0) goto fail;

		/* Allocate a new frame */
		res = add_frame(ani, &stat, &w_set, bpp, pg.disp);
		if (res) goto fail;

		/* Do actual compositing, remember disposal method */
		composite_frame(&ani->fset, &stat, &w_set);
		mem_free_chanlist(w_set.img);
		memset(w_set.img, 0, sizeof(chanlist));

		/* Write out those frames worthy to be stored */
		res = done_frame(file_name, ani, FALSE);
		if (res != 1) goto fail;

		if (pg.phase > 2) break; // End of file
	}
	/* Write out the final frame if not written before */
	res = done_frame(file_name, ani, TRUE);

fail:	free(pg.png);
	mem_free_chanlist(w_set.img);
	fclose(fp);
	return (res);
}

static int analyze_gif_frame(ani_status *stat, ls_settings *settings)
{
	unsigned char cmap[513], *lmap, *fg, *bg;
	png_color *pal, *prev;
	int tmpal[257], same_size, show_under;
	int i, k, l, x, y, ul, lpal, lprev, fgw, bgw, prevtr = -1;


	/* Locate the new palette */
	pal = prev = stat->global_pal;
	lpal = lprev = stat->global_cols;
	if (settings->colors > 0)
	{
		pal = settings->pal;
		lpal = settings->colors;
	}

	/* Accept new palette as final, for now */
	mem_pal_copy(stat->newpal, pal);
	stat->newcols = lpal;
	stat->newtrans = settings->xpm_trans;

	/* Prepare for new frame */
	if (stat->mode <= ANM_RAW) // Raw frame mode
	{
		stat->defw = settings->width;
		stat->defh = settings->height;
		return (0);
	}
	else if ((stat->defw > MAX_WIDTH) || (stat->defh > MAX_HEIGHT))
		return (-1); // Too large
	ani_map_frame(stat, settings);
	same_size = !(settings->x | settings->y |
		(stat->defw ^ settings->width) | (stat->defh ^ settings->height));

	ani_init_xlat(stat); // Init palette remapping to 1:1

	/* First frame is exceptional */
	if (!stat->prev_idx)
	{
		// Trivial if no background gets drawn
		if (same_size) return (0);
		// Trivial if have transparent color
		if (settings->xpm_trans >= 0) return (1);
	}

	/* Disable transparency by default, enable when needed */
	stat->newtrans = -1;

	/* Now scan the dest area, filling colors bitmap */
	memset(cmap, 0, sizeof(cmap));
	fgw = settings->width;
	fg = settings->img[CHN_IMAGE] - (settings->y * fgw + settings->x);
	// Set underlayer pointer & step (ignore bpp!)
	bgw = stat->prev.width;
	bg = stat->prev.img[CHN_IMAGE] - (stat->prev.y * bgw + stat->prev.x);
	lmap = stat->lmap;
	for (y = 0; y < stat->defh; y++)
	{
		int ww = stat->defw, tp = settings->xpm_trans;
		int bmask = lmap[y] >> 4;

		for (x = 0; x < ww; x++)
		{
			int c0, bflag = lmap[x] & bmask;

			if ((bflag & 1) && ((c0 = fg[x]) != tp)) // New frame
				c0 += 256;
			else if ((bflag & 6) == 2) // Underlayer
				c0 = bg[x];
			else c0 = 512; // Background (transparency)
			cmap[c0] = 1;
		}
		fg += fgw; bg += bgw;
	}

	/* If we have underlayer */
	show_under = 0;
	if (stat->prev_idx)
	{
		// Use per-frame palette if underlayer has it
		prev = stat->prev.pal;
		lprev = stat->prev.cols;
		prevtr = stat->prev.trans;
		// Move underlayer transparency to "transparent"
		if (prevtr >= 0)
		{
			cmap[512] |= cmap[prevtr];
			cmap[prevtr] = 0;
		}
		// Check if underlayer is at all visible
		show_under = !!memchr(cmap, 1, 256);
		// Visible RGB/RGBA underlayer means RGB/RGBA frame
		if (show_under && (stat->prev.bpp == 3)) goto RGB;
	}

	/* Now, check if either frame's palette is enough */
	ul = 2; // Default is new palette
	if (show_under)
	{
		l = lprev > lpal ? lprev : lpal;
		k = lprev > lpal ? lpal : lprev;
		for (ul = 3 , i = 0; ul && (i < l); i++)
		{
			int tf2 = cmap[i] * 2 + cmap[256 + i];
			if (tf2 && ((i >= k) ||
				(PNG_2_INT(prev[i]) != PNG_2_INT(pal[i]))))
				ul &= ~tf2; // Exclude mismatched palette(s)
		}
		if (ul == 1) // Need old palette
		{
			mem_pal_copy(stat->newpal, prev);
			stat->newcols = lprev;
		}
	}
	while (ul) // Place transparency
	{
		if (cmap[512]) // Need transparency
		{
			int i, l = prevtr, nc = stat->newcols;

			/* If cannot use old transparent index */
			if ((l < 0) || (l >= nc) || (cmap[l] | cmap[l + 256]))
				l = settings->xpm_trans;
			/* If cannot use new one either */
			if ((l < 0) || (l >= nc) || (cmap[l] | cmap[l + 256]))
			{
				/* Try to find unused palette slot */
				for (l = -1 , i = 0; (l < 0) && (i < nc); i++)
					if (!(cmap[i] | cmap[i + 256])) l = i;
			}
			if (l < 0) /* Try to add a palette slot */
			{
				png_color *c;

				if (nc >= 256) break; // Failure
				l = stat->newcols++;
				c = stat->newpal + l;
				c->red = c->green = c->blue = 0;
			}
			// Modify mapping
			if (prevtr >= 0) stat->xlat[prevtr] = l;
			stat->xlat[512] = stat->newtrans = l;
		}
		// Successfully mapped everything - use paletted mode
		return (same_size ? 0 : 1);
	}

	/* Try to build combined palette */
	for (ul = i = 0; (ul < 257) && (i < 512); i++)
	{
		png_color *c;
		int j, v;

		if (!cmap[i]) continue;
		c = (i < 256 ? prev : pal - 256) + i;
		v = PNG_2_INT(*c);
		for (j = 0; (j < ul) && (tmpal[j] != v); j++);
		if (j == ul) tmpal[ul++] = v;
		stat->xlat[i] = j;
	}
	// Add transparent color
	if ((ul < 257) && cmap[512])
	{
		// Modify mapping
		if (prevtr >= 0) stat->xlat[prevtr] = ul;
		stat->xlat[512] = stat->newtrans = ul;
		tmpal[ul++] = 0;
	}
	if (ul < 257) // Success!
	{
		png_color *c = stat->newpal;
		for (i = 0; i < ul; i++ , c++) // Build palette
		{
			int v = tmpal[i];
			c->red = INT_2_R(v);
			c->green = INT_2_G(v);
			c->blue = INT_2_B(v);
		}
		stat->newcols = ul;
		// Use paletted mode
		return (same_size ? 0 : 1);
	}

	/* Tried everything in vain - fall back to RGB/RGBA */
RGB:	if (stat->global_cols > 0) // Use default palette if present
	{
		mem_pal_copy(stat->newpal, stat->global_pal);
		stat->newcols = stat->global_cols;
	}
	stat->newtrans = -1; // No color-key transparency
	// RGBA if underlayer with alpha, or transparent backround, is visible
	if ((show_under && stat->prev.img[CHN_ALPHA]) || cmap[512])
		return (4);
	// RGB otherwise
	return (3);
}

/* Macros for accessing values in Intel byte order */
#define GET16(buf) (((buf)[1] << 8) + (buf)[0])
#define GET32(buf) (((unsigned)(buf)[3] << 24) + ((buf)[2] << 16) + \
	((buf)[1] << 8) + (buf)[0])
#define GET32s(buf) (((signed char)(buf)[3] * 0x1000000) + ((buf)[2] << 16) + \
	((buf)[1] << 8) + (buf)[0])
#define PUT16(buf, v) (buf)[0] = (v) & 0xFF; (buf)[1] = (v) >> 8;
#define PUT32(buf, v) (buf)[0] = (v) & 0xFF; (buf)[1] = ((v) >> 8) & 0xFF; \
	(buf)[2] = ((v) >> 16) & 0xFF; (buf)[3] = (v) >> 24;

#define GIF_ID       "GIF87a"
#define GIF_IDLEN    6

/* GIF header */
#define GIF_VER       4 /* Where differing version digit goes */
#define GIF_WIDTH     6 /* 16b */
#define GIF_HEIGHT    8 /* 16b */
#define GIF_GPBITS   10
#define GIF_BKG      11
#define GIF_ASPECT   12
#define GIF_HDRLEN   13 /* Global palette starts here */

#define GIF_GPFLAG   0x80
#define GIF_8BPC     0x70 /* Color resolution to write out */

/* Graphics Control Extension */
#define GIF_GC_FLAGS 0
#define GIF_GC_DELAY 1 /* 16b */
#define GIF_GC_TRANS 3
#define GIF_GC_LEN   4

#define GIF_GC_TFLAG 1
#define GIF_GC_DISP  2 /* Shift amount */

/* Application Extension */
#define GIF_AP_LEN   11

/* Image */
#define GIF_IX       0 /* 16b */
#define GIF_IY       2 /* 16b */
#define GIF_IWIDTH   4 /* 16b */
#define GIF_IHEIGHT  6 /* 16b */
#define GIF_IBITS    8
#define GIF_IHDRLEN  9

#define GIF_LPFLAG   0x80
#define GIF_ILFLAG   0x40 /* Interlace */

/* Read body of GIF block into buffer (or skip if NULL), return length */
static int getblock(unsigned char *buf, FILE *fp)
{
	int l = getc(fp);
	if (l == EOF) l = -1;
	if (l > 0)
	{
		if (!buf) fseek(fp, l, SEEK_CUR); // Just skip
		else if (fread(buf, 1, l, fp) < l) l = -1; // Error
	}
	return (l);
}

#ifdef U_LCMS /* No other uses for it now */
/* Load a sequence of GIF blocks into memory */
static int getgifdata(FILE *fp, char **res, int *len)
{
	unsigned char *src, *dest, *mem;
	f_long r, p = ftell(fp);
	int l, size;

	*res = NULL;
	*len = 0;
	if (p < 0) return (1); /* Leave 2Gb+ GIFs to systems with longer f_long */

	/* Measure */
	while ((l = getblock(NULL, fp)) > 0);
	if (l < 0) return (-1); // Error
	r = ftell(fp);
	fseek(fp, p, SEEK_SET);
	if (r <= p) return (1); // Paranoia
	if (r - p > INT_MAX) return (1); // A 2Gb+ profile is unusable anyway :-)

	/* Allocate */
	size = r - p;
	mem = malloc(size);
	if (!mem) return (1); // No memory for this

	/* Read */
	if (fread(mem, 1, size, fp) < size) goto fail; // Error

	/* Merge */
	src = dest = mem;
	while ((l = *src++))
	{
		if (src - mem >= size - l) goto fail; // Paranoia
		memmove(dest, src, l);
		src += l; dest += l;
	}
	size = dest - mem; // Maybe realloc?

	/* Done */
	*res = mem;
	*len = size;
	return (0);

fail:	free(mem);
	return (-1); // Someone overwrote the file damaging it
}
#endif

/* Space enough to hold palette, or longest block + longest decoded sequence */
#define GIF_BUFSIZE (256 + 4096)
typedef struct {
	FILE *f;
	int ptr, end, tail;
	int lc0, lc, nxc, clear, cmask;
	int w, bits, prev;
	short nxcode[4096 + 1];
	unsigned char buf[GIF_BUFSIZE], cchar[4096 + 1];
} gifbuf;

static void resetlzw(gifbuf *gif)
{
	gif->nxc = gif->clear + 2; // First usable code
	gif->lc = gif->lc0 + 1; // Actual code size
	gif->cmask = (1 << gif->lc) - 1; // Actual code mask
	gif->prev = -1; // Previous code
}

static int initlzw(gifbuf *gif, FILE *fp)
{
	int i;

	gif->f = fp;
	gif->lc0 = i = getc(fp); // Min code size
	/* Enforce hard upper limit but allow wasteful encoding */
	if ((i == EOF) || (i > 11)) return (FALSE);
	gif->clear = i = 1 << i; // Clear code
	/* Initial 1-char codes */
	while (--i >= 0) gif->nxcode[i] = -1 , gif->cchar[i] = (unsigned char)i;
	resetlzw(gif);
	gif->w = gif->bits = 0; // Ready bits in shifter
	gif->ptr = gif->end = gif->tail = 0; // Ready data in buffer
	return (TRUE);
}

static int getlzw(unsigned char *dest, int cnt, gifbuf *gif)
{
	int l, c, cx, w, bits, lc, cmask, prev, nxc, tail = gif->tail;

	while (TRUE)
	{
		l = tail > cnt ? cnt : tail;
		cnt -= l;
		l = tail - l;
		while (tail > l) *dest++ = gif->buf[GIF_BUFSIZE - tail--];
		if (cnt <= 0) break; // No tail past this point
		w = gif->w;
		bits = gif->bits;
		lc = gif->lc;
		while (bits < lc)
		{
			if (gif->ptr >= gif->end)
			{
				gif->end = getblock(gif->buf, gif->f);
				if (gif->end <= 0) return (FALSE); // No data
				gif->ptr = 0;
			}
			w |= gif->buf[gif->ptr++] << bits;
			bits += 8;
		}
		cmask = gif->cmask;
		c = w & cmask;
		gif->w = w >> lc;
		gif->bits = bits - lc;
		if (c == gif->clear)
		{
			resetlzw(gif);
			continue;
		}
		if (c == gif->clear + 1) return (FALSE); // Premature EOI
		/* Update for next code */
		prev = gif->prev;
		gif->prev = c;
		nxc = gif->nxc;
		gif->nxcode[nxc] = prev;
		if ((prev >= 0) && (nxc < 4096))
		{
			if ((++gif->nxc > cmask) && (cmask < 4096 - 1))
				gif->cmask = (1 << ++gif->lc) - 1;
		}
		/* Decode this one */
		if (c > nxc) return (FALSE); // Broken code
		if ((c == nxc) && (prev < 0)) return (FALSE); // Too early
		for (cx = c; cx >= 0; cx = gif->nxcode[cx])
			gif->buf[GIF_BUFSIZE - ++tail] = gif->cchar[cx];
		gif->cchar[nxc] = gif->buf[GIF_BUFSIZE - tail];
		/* In case c == nxc, its char was garbage till now, so reread it */
		gif->buf[GIF_BUFSIZE - 1] = gif->cchar[c];
	}
	gif->tail = tail;
	return (TRUE);
}

static int load_gif_frame(FILE *fp, ls_settings *settings)
{
	/* GIF interlace pattern: Y0, DY, ... */
	static const unsigned char interlace[10] =
		{ 0, 1, 0, 8, 4, 8, 2, 4, 1, 2 };
	unsigned char hdr[GIF_IHDRLEN];
	gifbuf gif;
	int i, k, kx, n, w, h, dy, res;


	/* Read the header */
	if (fread(hdr, 1, GIF_IHDRLEN, fp) < GIF_IHDRLEN) return (-1);

	/* Get local palette if any */
	if (hdr[GIF_IBITS] & GIF_LPFLAG)
	{
		int cols = 2 << (hdr[GIF_IBITS] & 7);
		if (fread(gif.buf, 3, cols, fp) < cols) return (-1);
		rgb2pal(settings->pal, gif.buf, settings->colors = cols);
	}
	if (settings->colors < 0) return (-1); // No palette at all
	/* If palette is all we need */
	if ((settings->mode == FS_PALETTE_LOAD) ||
		(settings->mode == FS_PALETTE_DEF)) return (EXPLODE_FAILED);

	if (!initlzw(&gif, fp)) return (-1);

	/* Store actual image parameters */
	settings->x = GET16(hdr + GIF_IX);
	settings->y = GET16(hdr + GIF_IY);
	settings->width = w = GET16(hdr + GIF_IWIDTH);
	settings->height = h = GET16(hdr + GIF_IHEIGHT);
	settings->bpp = 1;

	if ((res = allocate_image(settings, CMASK_IMAGE))) return (res);
	res = FILE_LIB_ERROR;

	if (!settings->silent) ls_init("GIF", 0);

	if (hdr[GIF_IBITS] & GIF_ILFLAG) k = 2 , kx = 10; /* Interlace */
	else k = 0 , kx = 2;

	for (n = 0; k < kx; k += 2)
	{
		dy = interlace[k + 1];
		for (i = interlace[k]; i < h; n++ , i += dy)
		{
			if (!getlzw(settings->img[CHN_IMAGE] + i * w, w, &gif))
				goto fail;
			ls_progress(settings, n, 10);
		}
	}
	/* Skip data blocks till 0 */
	while ((i = getblock(NULL, fp)) > 0);
	if (!i) res = 1;
fail:	if (!settings->silent) progress_end();
	return (res);
}

static int load_gif_frames(char *file_name, ani_settings *ani)
{
	/* GIF disposal codes mapping */
	static const unsigned short gif_disposal[8] = {
		FM_DISP_LEAVE, FM_DISP_LEAVE, FM_DISP_REMOVE, FM_DISP_RESTORE,
		/* Handling (reserved) "4" same as "3" is what Mozilla does */
		FM_DISP_RESTORE, FM_DISP_LEAVE, FM_DISP_LEAVE, FM_DISP_LEAVE
	};
	unsigned char hdr[GIF_HDRLEN], buf[768];
	png_color w_pal[256];
	ani_status stat;
	ls_settings w_set, init_set;
	int l, id, disposal, bpp, res = -1;
	FILE *fp;

	if (!(fp = fopen(file_name, "rb"))) return (-1);
	memset(w_set.img, 0, sizeof(chanlist));

	/* Read the header */
	if (fread(hdr, 1, GIF_HDRLEN, fp) < GIF_HDRLEN) goto fail;

	/* Check signature */
	if (hdr[GIF_VER] == '9') hdr[GIF_VER] = '7'; // Does not matter anyway
	if (memcmp(hdr, GIF_ID, GIF_IDLEN)) goto fail;

	/* Init state structure */
	memset(&stat, 0, sizeof(stat));
	stat.mode = ani->mode;
	stat.defw = GET16(hdr + GIF_WIDTH);
	stat.defh = GET16(hdr + GIF_HEIGHT);
	stat.global_cols = -1;
	/* Get global palette */
	if (hdr[GIF_GPBITS] & GIF_GPFLAG)
	{
		int cols = 2 << (hdr[GIF_GPBITS] & 7);
		if (fread(buf, 3, cols, fp) < cols) goto fail;
		rgb2pal(stat.global_pal, buf, stat.global_cols = cols);
	}
	stat.blend = TRUE; // No other case in GIF

	/* Init temp container */
	init_set = ani->settings;
	init_set.colors = 0; // Nonzero will signal local palette
	init_set.pal = w_pal;
	init_set.xpm_trans = -1;
	init_set.gif_delay = 0;
	disposal = FM_DISP_LEAVE;

	/* Init frameset */
	if (stat.global_cols > 0) // Set default palette
	{
		res = FILE_MEM_ERROR;
		if (!(ani->fset.pal = malloc(SIZEOF_PALETTE))) goto fail;
		mem_pal_copy(ani->fset.pal, stat.global_pal);
	}

	/* Go through images */
	while (TRUE)
	{
		res = -1;
		id = getc(fp);
		if (!id) continue; // Extra end-blocks do happen sometimes
		if (id == ';') break; // Trailer block
		if (id == '!') // Extension block
		{
			if ((id = getc(fp)) == EOF) goto fail;
			if (id == 0xF9) // Graphics control - read it
			{
				if (getblock(buf, fp) < GIF_GC_LEN) goto fail;
				/* !!! In practice, Graphics Control Extension
				 * affects not only "the first block to follow"
				 * as docs say, but EVERY following block - WJ */
				init_set.xpm_trans = buf[GIF_GC_FLAGS] & GIF_GC_TFLAG ?
					buf[GIF_GC_TRANS] : -1;
				init_set.gif_delay = GET16(buf + GIF_GC_DELAY);
				disposal = gif_disposal[(buf[GIF_GC_FLAGS] >> GIF_GC_DISP) & 7];
			}
			while ((l = getblock(NULL, fp)) > 0); // Skip till end
			if (l < 0) goto fail;
		}
		else if (id == ',') // Image
		{
			res = FILE_TOO_LONG;
			if (!check_next_frame(&ani->fset, ani->settings.mode, TRUE))
				goto fail;
			w_set = init_set;
			res = load_gif_frame(fp, &w_set);
			if (res != 1) goto fail;
			/* Analyze how we can merge the frames */
			res = TOO_BIG;
			bpp = analyze_gif_frame(&stat, &w_set);
			if (bpp < 0) goto fail;

			/* Allocate a new frame */
			res = add_frame(ani, &stat, &w_set, bpp, disposal);
			if (res) goto fail;

			/* Do actual compositing, remember disposal method */
			composite_frame(&ani->fset, &stat, &w_set);
			mem_free_chanlist(w_set.img);
			memset(w_set.img, 0, sizeof(chanlist));

			/* Write out those frames worthy to be stored */
			res = done_frame(file_name, ani, FALSE);
			if (res != 1) goto fail;
		}
		else goto fail; // Garbage or EOF
	}
	/* Write out the final frame if not written before */
	res = done_frame(file_name, ani, TRUE);

fail:	mem_free_chanlist(w_set.img);
	fclose(fp);
	return (res);
}

static int load_gif(char *file_name, ls_settings *settings)
{
	unsigned char hdr[GIF_HDRLEN], buf[768];
	int trans = -1, delay = settings->gif_delay, frame = 0;
	int l, id, res = -1;
	FILE *fp;

	if (!(fp = fopen(file_name, "rb"))) return (-1);

	/* Read the header */
	if (fread(hdr, 1, GIF_HDRLEN, fp) < GIF_HDRLEN) goto fail;

	/* Check signature */
	if (hdr[GIF_VER] == '9') hdr[GIF_VER] = '7'; // Does not matter anyway
	if (memcmp(hdr, GIF_ID, GIF_IDLEN)) goto fail;

	/* Get global palette */
	settings->colors = -1;
	if (hdr[GIF_GPBITS] & GIF_GPFLAG)
	{
		int cols = 2 << (hdr[GIF_GPBITS] & 7);
		if (fread(buf, 3, cols, fp) < cols) goto fail;
		rgb2pal(settings->pal, buf, settings->colors = cols);
	}

	/* Go read the first image */
	while (TRUE)
	{
		res = frame ? FILE_LIB_ERROR : -1;
		id = getc(fp);
		if (!id) continue; // Extra end-blocks do happen sometimes
		if (id == ';') break; // Trailer block
		if (id == '!') // Extension block
		{
			if ((id = getc(fp)) == EOF) goto fail;
			if (id == 0xF9) // Graphics control - read it
			{
				if (getblock(buf, fp) < GIF_GC_LEN) goto fail;
				trans = buf[GIF_GC_FLAGS] & GIF_GC_TFLAG ?
					buf[GIF_GC_TRANS] : -1;
				delay = GET16(buf + GIF_GC_DELAY);
			}
#ifdef U_LCMS
			/* Yes GIF can have a color profile - imagine that */
			else if ((id == 0xFF) // Application extension
				&& !settings->icc_size // No need of it otherwise
				&& (getblock(buf, fp) >= GIF_AP_LEN) // Not broken
				&& !memcmp(buf, "ICCRGBG1012", GIF_AP_LEN)) // The right ID
			{
				l = getgifdata(fp, &settings->icc, &settings->icc_size);
				if (l < 0) goto fail;
				if (!l) continue; // No trailing blocks to skip
			}
#endif
			while ((l = getblock(NULL, fp)) > 0); // Skip till end
			if (l < 0) goto fail;
		}
		else if (id == ',') // Image
		{
			if (frame++) /* Multipage GIF - notify user */
			{
				res = FILE_HAS_FRAMES;
				goto fail;
			}
			settings->gif_delay = delay;
			settings->xpm_trans = trans;
			res = load_gif_frame(fp, settings);
			if (res != 1) goto fail;
		}
		else goto fail; // Garbage or EOF
	}
	if (frame) res = 1; // No images is fail
fail:	fclose(fp);
	return (res);
}

/* Space enough to hold palette and all headers, or longest block */
#define GIF_WBUFSIZE (768 + GIF_HDRLEN + (GIF_GC_LEN + 4) + (GIF_IHDRLEN + 2))

/* A cuckoo hash would take 4x less space, but need much more code */
#define GIF_CODESSIZE ((4096 * 2 * 16) * sizeof(short))
typedef struct {
	FILE *f;
	int cnt, nxmap;
	int lc0, lc, nxc, clear, nxc2;
	int w, bits, prev;
	short *codes;
	unsigned char buf[GIF_WBUFSIZE];
} gifcbuf;

static void resetclzw(gifcbuf *gif)
{
	/* Send clear code at current length */
	gif->w |= gif->clear << gif->bits;
	gif->bits += gif->lc;
	/* Reset parameters */
	gif->nxc = gif->clear + 2; // First usable code
	gif->lc = gif->lc0 + 1; // Actual code size
	gif->nxc2 = 1 << gif->lc; // For next code size
	memset(gif->codes, 0, GIF_CODESSIZE); // Maps
	gif->nxmap = 1; // First usable intermediate map
}

static void initclzw(gifcbuf *gif, int lc0, FILE *fp)
{
	if (lc0 < 2) lc0 = 2; // Minimum allowed
	gif->f = fp;
	gif->lc0 = lc0;
	fputc(lc0, fp);
	gif->clear = 1 << lc0; // Clear code
	gif->prev = -1; // No previous code
	gif->cnt = gif->w = gif->bits = 0; // No data yet
	gif->lc = gif->lc0 + 1; // Actual code size
	resetclzw(gif); // Initial clear
}

static void emitlzw(gifcbuf *gif, int c)
{
	int bits = gif->bits, w = gif->w | (c << bits);

	bits += gif->lc;
	while (bits >= 8)
	{
		gif->buf[++gif->cnt] = (unsigned char)w;
		w >>= 8;
		bits -= 8;
		if (gif->cnt >= 255)
		{
			gif->buf[0] = 255;
			fwrite(gif->buf, 1, 256, gif->f);
			gif->cnt = 0;
		}
	}
	gif->bits = bits;
	gif->w = w;
	/* Extend code size if needed */
	if (gif->nxc >= gif->nxc2) gif->nxc2 = 1 << ++gif->lc;
}

static void putlzw(gifcbuf *gif, unsigned char *src, int cnt)
{
	short *codes = gif->codes;
	int i, j, c, prev = gif->prev;

	while (cnt-- > 0)
	{
		c = *src++;
		if (prev < 0) /* Begin */
		{
			prev = c;
			continue;
		}
		/* Try compression */
		i = prev * 16 + (c >> 4) + 4096 * 16;
		j = codes[i] * 16 + (c & 0xF);
		j = codes[j];
		if (j) // Have match
		{
			prev = j - 4096;
			continue;
		}
		/* Emit the code */
		emitlzw(gif, prev);
		prev = c;
		/* Do a clear if needed */
		if (gif->nxc >= 4096 - 1)
		{
			resetclzw(gif);
			continue;
		}
		/* Add new code */
		if (!codes[i]) codes[i] = gif->nxmap++;
		j = codes[i] * 16 + (c & 0xF);
		codes[j] = gif->nxc++ + 4096;
	}
	gif->prev = prev;
}

static void donelzw(gifcbuf *gif) /* Flush */
{
	emitlzw(gif, gif->prev);
	emitlzw(gif, gif->clear + 1); // EOD
	if (gif->bits) gif->buf[++gif->cnt] = gif->w;
//	gif->w = gif->bits = 0;
	gif->buf[0] = gif->cnt;
	gif->buf[gif->cnt + 1] = 0; // Block terminator
	fwrite(gif->buf, 1, gif->cnt + 2, gif->f);
//	gif->cnt = 0;
}

static int save_gif(char *file_name, ls_settings *settings)
{
	gifcbuf gif;
	unsigned char *tmp;
	FILE *fp = NULL;
	int i, nc, ext = FALSE, w = settings->width, h = settings->height;


	/* GIF save must be on indexed image */
	if (settings->bpp != 1) return WRONG_FORMAT;

	gif.codes = malloc(GIF_CODESSIZE);
	if (!gif.codes) return (-1);

	if (!(fp = fopen(file_name, "wb")))
	{
		free(gif.codes);
		return (-1);
	}

	/* Get colormap size bits */
	nc = nlog2(settings->colors) - 1;
	if (nc < 0) nc = 0;

	/* Prepare header */
	tmp = gif.buf;
	memset(tmp, 0, GIF_HDRLEN);
	memcpy(tmp, GIF_ID, GIF_IDLEN);
	PUT16(tmp + GIF_WIDTH, w);
	PUT16(tmp + GIF_HEIGHT, h);
	tmp[GIF_GPBITS] = GIF_GPFLAG | GIF_8BPC | nc;
	tmp += GIF_HDRLEN;

	/* Prepare global palette */
	i = 2 << nc;
	pal2rgb(tmp, settings->pal, settings->colors, i);
	tmp += i * 3;

	/* Prepare extension */
	if (settings->xpm_trans >= 0)
	{
		ext = TRUE;
		*tmp++ = '!';	// Extension block
		*tmp++ = 0xF9;	// Graphics control
		*tmp++ = GIF_GC_LEN;
		memset(tmp, 0, GIF_GC_LEN + 1); // W/block terminator
		tmp[GIF_GC_FLAGS] = GIF_GC_TFLAG;
		tmp[GIF_GC_TRANS] = settings->xpm_trans;
		tmp += GIF_GC_LEN + 1;
	}

	/* Prepare image header */
	*tmp++ = ',';
	memset(tmp, 0, GIF_IHDRLEN);
	PUT16(tmp + GIF_IWIDTH, w);
	PUT16(tmp + GIF_IHEIGHT, h);
	tmp += GIF_IHDRLEN;

	if (ext) gif.buf[GIF_VER] = '9'; // If we use extension

	/* Write out all the headers */
	fwrite(gif.buf, 1, tmp - gif.buf, fp);

	if (!settings->silent) ls_init("GIF", 1);

	initclzw(&gif, nc + 1, fp); // "Min code size" = palette index bits
	for (i = 0; i < h; i++)
	{
		putlzw(&gif, settings->img[CHN_IMAGE] + i * w, w);
		ls_progress(settings, i, 20);
	}
	donelzw(&gif);
	fputc(';', fp); // Trailer block
	fclose(fp);

	if (!settings->silent) progress_end();

	free(gif.codes);
	return 0;
}

#ifdef NEED_CMYK
#ifdef U_LCMS
/* Guard against cmsHTRANSFORM changing into something overlong in the future */
typedef char cmsHTRANSFORM_Does_Not_Fit_Into_Pointer[2 * (sizeof(cmsHTRANSFORM) <= sizeof(char *)) - 1];

static int init_cmyk2rgb(ls_settings *settings, unsigned char *icc, int len,
	int inverted)
{
	cmsHPROFILE from, to;
	cmsHTRANSFORM how = NULL;

	from = cmsOpenProfileFromMem((void *)icc, len);
	if (!from) return (TRUE); // Unopenable now, unopenable ever
	to = cmsCreate_sRGBProfile();
	if (cmsGetColorSpace(from) == icSigCmykData)
		how = cmsCreateTransform(from, inverted ? TYPE_CMYK_8_REV :
			TYPE_CMYK_8, to, TYPE_RGB_8, INTENT_PERCEPTUAL, 0);
	if (from) cmsCloseProfile(from);
	cmsCloseProfile(to);
	if (!how) return (FALSE); // Better luck the next time

	settings->icc = (char *)how;
	settings->icc_size = -2;
	return (TRUE);
}

static void done_cmyk2rgb(ls_settings *settings)
{
	if (settings->icc_size != -2) return;
	cmsDeleteTransform((cmsHTRANSFORM)settings->icc);
	settings->icc = NULL;
	settings->icc_size = -1; // Not need profiles anymore
}

#else /* No LCMS */
#define done_cmyk2rgb(X)
#endif
#endif

static void cmyk2rgb(unsigned char *dest, unsigned char *src, int cnt,
	int inverted, ls_settings *settings)
{
	unsigned char xb;
	int j, k, r, g, b;

#ifdef U_LCMS
	/* Convert CMYK to RGB using LCMS if possible */
	if (settings->icc_size == -2)
	{
		cmsDoTransform((cmsHTRANSFORM)settings->icc, src, dest, cnt);
		return;
	}
#endif
	/* Simple CMYK->RGB conversion */
	xb = inverted ? 0 : 255;
	for (j = 0; j < cnt; j++ , src += 4 , dest += 3)
	{
		k = src[3] ^ xb;
		r = (src[0] ^ xb) * k;
		dest[0] = (r + (r >> 8) + 1) >> 8;
		g = (src[1] ^ xb) * k;
		dest[1] = (g + (g >> 8) + 1) >> 8;
		b = (src[2] ^ xb) * k;
		dest[2] = (b + (b >> 8) + 1) >> 8;
	}
}

#ifdef U_JPEG
struct my_error_mgr
{
	struct jpeg_error_mgr pub;	// "public" fields
	jmp_buf setjmp_buffer;		// for return to caller
};

typedef struct my_error_mgr *my_error_ptr;

METHODDEF(void) my_error_exit (j_common_ptr cinfo)
{
	my_error_ptr myerr = (my_error_ptr) cinfo->err;
	longjmp(myerr->setjmp_buffer, 1);
}
struct my_error_mgr jerr;

static int load_jpeg(char *file_name, ls_settings *settings)
{
	static int pr;
	struct jpeg_decompress_struct cinfo;
	unsigned char *memp, *memx = NULL;
	FILE *fp;
	int i, width, height, bpp, res = -1, inv = 0;
#ifdef U_LCMS
	unsigned char *icc = NULL;
#endif

	if ((fp = fopen(file_name, "rb")) == NULL) return (-1);

	pr = 0;
	jpeg_create_decompress(&cinfo);
	cinfo.err = jpeg_std_error(&jerr.pub);
	jerr.pub.error_exit = my_error_exit;
	if (setjmp(jerr.setjmp_buffer))
	{
		res = settings->width ? FILE_LIB_ERROR : -1;
		goto fail;
	}
	jpeg_stdio_src(&cinfo, fp);

#ifdef U_LCMS
	/* Request ICC profile aka APP2 data be preserved */
	if (!settings->icc_size)
		jpeg_save_markers(&cinfo, JPEG_APP0 + 2, 0xFFFF);
#endif

	jpeg_read_header(&cinfo, TRUE);
	jpeg_start_decompress(&cinfo);

	bpp = 3;
	switch (cinfo.out_color_space)
	{
	case JCS_RGB: break;
	case JCS_GRAYSCALE:
		set_gray(settings);
		bpp = 1;
		break;
	case JCS_CMYK:
		/* Photoshop writes CMYK data inverted */
		inv = cinfo.saw_Adobe_marker;
		if ((memx = malloc(cinfo.output_width * 4))) break;
		res = FILE_MEM_ERROR;
		// Fallthrough
	default: goto fail; /* Unsupported colorspace */
	}

	settings->width = width = cinfo.output_width;
	settings->height = height = cinfo.output_height;
	settings->bpp = bpp;
	if ((res = allocate_image(settings, CMASK_IMAGE))) goto fail;
	res = -1;
	pr = !settings->silent;

#ifdef U_LCMS
#define PARTHDR 14
	while (!settings->icc_size)
	{
		jpeg_saved_marker_ptr mk;
		unsigned char *tmp, *parts[256];
		int i, part, nparts = -1, icclen = 0, lparts[256];

		/* List parts */
		memset(parts, 0, sizeof(parts));
		for (mk = cinfo.marker_list; mk; mk = mk->next)
		{
			if ((mk->marker != JPEG_APP0 + 2) ||
				(mk->data_length < PARTHDR) ||
				strcmp(mk->data, "ICC_PROFILE")) continue;
			part = GETJOCTET(mk->data[13]);
			if (nparts < 0) nparts = part;
			if (nparts != part) break;
			part = GETJOCTET(mk->data[12]);
			if (!part-- || (part >= nparts) || parts[part]) break;
			parts[part] = (unsigned char *)(mk->data + PARTHDR);
			icclen += lparts[part] = mk->data_length - PARTHDR;
		}
		if (nparts < 0) break;

		icc = tmp = malloc(icclen);
		if (!icc) break;

		/* Assemble parts */
		for (i = 0; i < nparts; i++)
		{
			if (!parts[i]) break;
			memcpy(tmp, parts[i], lparts[i]);
			tmp += lparts[i];
		}
		if (i < nparts) break; // Sequence had a hole

		/* If profile is needed right now, for CMYK->RGB */
		if (memx && init_cmyk2rgb(settings, icc, icclen, inv))
			break; // Transform is ready, so drop the profile

		settings->icc = icc;
		settings->icc_size = icclen;
		icc = NULL; // Leave the profile be
		break;
	}
	free(icc);
#undef PARTHDR
#endif

	if (pr) ls_init("JPEG", 0);

	for (i = 0; i < height; i++)
	{
		memp = settings->img[CHN_IMAGE] + width * i * bpp;
		jpeg_read_scanlines(&cinfo, memx ? &memx : &memp, 1);
		if (memx) cmyk2rgb(memp, memx, width, inv, settings);
		ls_progress(settings, i, 20);
	}
	done_cmyk2rgb(settings);
	jpeg_finish_decompress(&cinfo);
	res = 1;

fail:	if (pr) progress_end();
	jpeg_destroy_decompress(&cinfo);
	fclose(fp);
	free(memx);
	return (res);
}

static int save_jpeg(char *file_name, ls_settings *settings)
{
	struct jpeg_compress_struct cinfo;
	JSAMPROW row_pointer;
	FILE *fp;
	int i;


	if (settings->bpp == 1) return WRONG_FORMAT;

	if ((fp = fopen(file_name, "wb")) == NULL) return -1;

	cinfo.err = jpeg_std_error(&jerr.pub);
	jerr.pub.error_exit = my_error_exit;
	if (setjmp(jerr.setjmp_buffer))
	{
		jpeg_destroy_compress(&cinfo);
		fclose(fp);
		return -1;
	}

	jpeg_create_compress(&cinfo);

	jpeg_stdio_dest( &cinfo, fp );
	cinfo.image_width = settings->width;
	cinfo.image_height = settings->height;
	cinfo.input_components = 3;
	cinfo.in_color_space = JCS_RGB;
	jpeg_set_defaults(&cinfo);
	jpeg_set_quality(&cinfo, settings->jpeg_quality, TRUE );
	jpeg_start_compress( &cinfo, TRUE );

	row_pointer = settings->img[CHN_IMAGE];
	if (!settings->silent) ls_init("JPEG", 1);
	for (i = 0; i < settings->height; i++ )
	{
		jpeg_write_scanlines(&cinfo, &row_pointer, 1);
		row_pointer += 3 * settings->width;
		ls_progress(settings, i, 20);
	}
	jpeg_finish_compress( &cinfo );

	if (!settings->silent) progress_end();

	jpeg_destroy_compress( &cinfo );
	fclose(fp);

	return 0;
}
#endif

#ifdef NEED_FILE2MEM
/* Read in the entire file, up to max size if given */
static int file2mem(char *file_name, unsigned char **where, size_t *len, size_t max)
{
	FILE *fp;
	unsigned char *buf;
	f_long l;
	int res = -1;

	if (!(fp = fopen(file_name, "rb"))) return (-1);
	fseek(fp, 0, SEEK_END);
	l = ftell(fp);
	/* Where a f_long is too short to hold the file's size, address space is
	 * too small to usefully hold the whole file anyway - WJ */
	/* And when a hard limit is set, it should be honored */
	if ((l > 0) && (!max || (l <= max)))
	{
		fseek(fp, 0, SEEK_SET);
		res = FILE_MEM_ERROR;
		if ((buf = malloc(l)))
		{
			res = -1;
			if (fread(buf, 1, l, fp) < l) free(buf);
			else *where = buf , *len = l , res = 0;
		}
	}
	fclose(fp);
	return (res);
}
#endif

#ifdef U_JP2

/* *** PREFACE ***
 * OpenJPEG 1.x is wasteful in the extreme, with memory overhead of about
 * 7 times the unpacked image size. So it can fail to handle even such
 * resolutions that fit into available memory with lots of room to spare.
 * Still, JasPer is an even worse memory hog, if a somewhat faster one.
 * Another thing - Linux builds of OpenJPEG cannot properly encode an opacity
 * channel (fixed in SVN on 06.11.09, revision 541)
 * And JP2 images with 4 channels, produced by OpenJPEG, cause JasPer
 * to die horribly.
 * Version 2.3.0 (04.10.17) was a sharp improvement, being twice faster than
 * JasPer when decompressing and less memory hungry (overhead of 5x size). When
 * compressing however, as of 2.3.1 still was about twice slower than JasPer.
 * Version 2.4.0 (28.12.20) fixed that, becoming about twice faster than JasPer
 * when compressing too.
 * Decompression speedup happened only for 64-bit builds, but on 32-bit with
 * multiple cores, multithreading still can let it overtake JasPer - WJ */

static int parse_opj(opj_image_t *image, ls_settings *settings)
{
	opj_image_comp_t *comp;
	unsigned char xtb[256], *dest;
	int i, j, k, w, h, w0, nc, step, shift;
	unsigned delta;
	int *src, cmask = CMASK_IMAGE, res;

	if (image->numcomps < 3) /* Guess this is paletted */
	{
		set_gray(settings);
		settings->bpp = 1;
	}
	else settings->bpp = 3;
	if ((nc = settings->bpp) < image->numcomps) nc++ , cmask = CMASK_RGBA;
	comp = image->comps;
	settings->width = w = (comp->w + (1 << comp->factor) - 1) >> comp->factor;
	settings->height = h = (comp->h + (1 << comp->factor) - 1) >> comp->factor;
	for (i = 1; i < nc; i++) /* Check if all components are the same size */
	{
		comp++;
		if ((w != (comp->w + (1 << comp->factor) - 1) >> comp->factor) ||
			(h != (comp->h + (1 << comp->factor) - 1) >> comp->factor))
			return (-1);
	}
	if ((res = allocate_image(settings, cmask))) return (res);

	/* Unpack data */
	for (i = 0 , comp = image->comps; i < nc; i++ , comp++)
	{
		if (i < settings->bpp) /* Image */
		{
			dest = settings->img[CHN_IMAGE] + i;
			step = settings->bpp;
		}
		else /* Alpha */
		{
			dest = settings->img[CHN_ALPHA];
			if (!dest) break; /* No alpha allocated */
			step = 1;
		}
		w0 = comp->w;
		delta = comp->sgnd ? 1U << (comp->prec - 1) : 0;
		shift = comp->prec > 8 ? comp->prec - 8 : 0;
		set_xlate(xtb, comp->prec - shift);
		for (j = 0; j < h; j++)
		{
			src = comp->data + j * w0;
			for (k = 0; k < w; k++)
			{
				*dest = xtb[(src[k] + delta) >> shift];
				dest += step;
			}
		}
	}

#ifdef U_LCMS
#if U_JP2 >= 2 /* 2.x */
	/* Extract ICC profile if it's of use */
	if (!settings->icc_size && image->icc_profile_buf &&
		(image->icc_profile_len < INT_MAX) &&
		(settings->icc = malloc(image->icc_profile_len)))
		memcpy(settings->icc, image->icc_profile_buf,
			settings->icc_size = image->icc_profile_len);
#endif
#endif
	return (1);
}

static opj_image_t *prepare_opj(ls_settings *settings)
{
	opj_image_cmptparm_t channels[4];
	opj_image_t *image;
	unsigned char *src;
	int i, j, k, nc, step;
	int *dest, w = settings->width, h = settings->height;

	nc = settings->img[CHN_ALPHA] ? 4 : 3;
	memset(channels, 0, sizeof(channels));
	for (i = 0; i < nc; i++)
	{
		channels[i].prec = channels[i].bpp = 8;
		channels[i].dx = channels[i].dy = 1;
		channels[i].w = w;
		channels[i].h = h;
	}

	image = opj_image_create(nc, channels,
#if U_JP2 < 2 /* 1.x */
		CLRSPC_SRGB);
#else /* 2.x */
		OPJ_CLRSPC_SRGB);
#endif
	if (!image) return (NULL);
	image->x0 = image->y0 = 0;
	image->x1 = w; image->y1 = h;

	/* Fill it */
	k = w * h;
	for (i = 0; i < nc; i++)
	{
		if (i < 3)
		{
			src = settings->img[CHN_IMAGE] + i;
			step = 3;
		}
		else
		{
			src = settings->img[CHN_ALPHA];
			step = 1;
		}
		dest = image->comps[i].data;
		for (j = 0; j < k; j++ , src += step) dest[j] = *src;
	}

	return (image);
}

#if U_JP2 < 2 /* 1.x */

static void stupid_callback(const char *msg, void *client_data)
{
}

static int load_jpeg2000(char *file_name, ls_settings *settings)
{
	opj_dparameters_t par;
	opj_dinfo_t *dinfo;
	opj_cio_t *cio = NULL;
	opj_image_t *image = NULL;
	opj_event_mgr_t useless_events; // !!! Silently made mandatory in v1.2
	unsigned char *buf = NULL;
	size_t l;
	int pr, res;


	/* Read in the entire file, provided its size fits into int */
	if ((res = file2mem(file_name, &buf, &l, INT_MAX))) return (res);

	/* Decompress it */
	dinfo = opj_create_decompress(settings->ftype == FT_J2K ? CODEC_J2K :
		CODEC_JP2);
	if (!dinfo) goto lfail;
	memset(&useless_events, 0, sizeof(useless_events));
	useless_events.error_handler = useless_events.warning_handler =
		useless_events.info_handler = stupid_callback;
	opj_set_event_mgr((opj_common_ptr)dinfo, &useless_events, stderr);
	opj_set_default_decoder_parameters(&par);
	opj_setup_decoder(dinfo, &par);
	cio = opj_cio_open((opj_common_ptr)dinfo, buf, l);
	if (!cio) goto lfail;
	if ((pr = !settings->silent)) ls_init("JPEG2000", 0);
	image = opj_decode(dinfo, cio);
	opj_cio_close(cio);
	opj_destroy_decompress(dinfo);
	free(buf);
	if (!image) goto ifail;
	
	/* Analyze what we got */
// !!! OpenJPEG 1.1.1 does *NOT* properly set image->color_space !!!
	res = parse_opj(image, settings);

ifail:	if (pr) progress_end();
	opj_image_destroy(image);
	return (res);
lfail:	opj_destroy_decompress(dinfo);
	free(buf);
	return (res);
}

static int save_jpeg2000(char *file_name, ls_settings *settings)
{
	opj_cparameters_t par;
	opj_cinfo_t *cinfo;
	opj_cio_t *cio = NULL;
	opj_image_t *image;
	opj_event_mgr_t useless_events; // !!! Silently made mandatory in v1.2
	FILE *fp;
	int k, res = -1;


	if (settings->bpp == 1) return WRONG_FORMAT;

	if ((fp = fopen(file_name, "wb")) == NULL) return -1;

	/* Create intermediate structure */
	image = prepare_opj(settings);
	if (!image) goto ffail;

	/* Compress it */
	if (!settings->silent) ls_init("JPEG2000", 1);
	cinfo = opj_create_compress(settings->ftype == FT_JP2 ? CODEC_JP2 :
		CODEC_J2K);
	if (!cinfo) goto fail;
	memset(&useless_events, 0, sizeof(useless_events));
	useless_events.error_handler = useless_events.warning_handler =
		useless_events.info_handler = stupid_callback;
	opj_set_event_mgr((opj_common_ptr)cinfo, &useless_events, stderr);
	opj_set_default_encoder_parameters(&par);
	par.tcp_numlayers = 1;
	par.tcp_rates[0] = settings->jp2_rate;
	par.cp_disto_alloc = 1;
	opj_setup_encoder(cinfo, &par, image);
	cio = opj_cio_open((opj_common_ptr)cinfo, NULL, 0);
	if (!cio) goto fail;
	if (!opj_encode(cinfo, cio, image, NULL)) goto fail;

	/* Write it */
	k = cio_tell(cio);
	if (fwrite(cio->buffer, 1, k, fp) == k) res = 0;

fail:	if (cio) opj_cio_close(cio);
	opj_destroy_compress(cinfo);
	opj_image_destroy(image);
	if (!settings->silent) progress_end();
ffail:	fclose(fp);
	return (res);
}

#else /* 2.x */

static int load_jpeg2000(char *file_name, ls_settings *settings)
{
	opj_dparameters_t par;
	opj_codec_t *dinfo;
	opj_stream_t *inp = NULL;
	opj_image_t *image = NULL;
	int i, pr, res = -1;
#if !OPJ_VERSION_MINOR /* 2.0.x */
	FILE *fp;
#endif


#if !OPJ_VERSION_MINOR /* 2.0.x */
	if ((fp = fopen(file_name, "rb")) == NULL) return (-1);
	if (!(inp = opj_stream_create_default_file_stream(fp, TRUE))) goto ffail;
#else /* 2.1+ */
	if (!(inp = opj_stream_create_default_file_stream(file_name, TRUE)))
		return (-1);
#endif

	/* Decompress it */
	dinfo = opj_create_decompress(settings->ftype == FT_J2K ? OPJ_CODEC_J2K :
		OPJ_CODEC_JP2);
	if (!dinfo) goto ffail;
	opj_set_default_decoder_parameters(&par);
	if (!opj_setup_decoder(dinfo, &par)) goto dfail;
#if defined(U_THREADS) && (OPJ_VERSION_MINOR >= 2) /* 2.2+ */
	/* Not much effect on 64 bit as of 2.3.0 - only 10% faster from a 2nd core
	 * But since 2.3.1, 1.3x faster with 2 cores, 1.7x with 4+ */
	opj_codec_set_threads(dinfo, helper_threads());
#endif
	if ((pr = !settings->silent)) ls_init("JPEG2000", 0);
	i = opj_read_header(inp, dinfo, &image) &&
		opj_decode(dinfo, inp, image) &&
		opj_end_decompress(dinfo, inp);
	opj_destroy_codec(dinfo);
	opj_stream_destroy(inp);
	if (!i) goto ifail;
	
	/* Parse what we got */
	if (image->color_space >= OPJ_CLRSPC_SYCC)
		goto ifail; // sYCC and CMYK - unsupported till seen in the wild
	res = parse_opj(image, settings);

ifail:	if (pr) progress_end();
	opj_image_destroy(image);
	return (res);
dfail:	opj_destroy_codec(dinfo);
ffail:	opj_stream_destroy(inp);
#if !OPJ_VERSION_MINOR /* 2.0.x */
	fclose(fp);
#endif
	return (res);
}

static int save_jpeg2000(char *file_name, ls_settings *settings)
{
	opj_cparameters_t par;
	opj_codec_t *cinfo;
	opj_stream_t *outp = NULL;
	opj_image_t *image;
	int res = -1;
#if !OPJ_VERSION_MINOR /* 2.0.x */
	FILE *fp;
#endif

	if (settings->bpp == 1) return WRONG_FORMAT;

#if !OPJ_VERSION_MINOR /* 2.0.x */
	if ((fp = fopen(file_name, "wb")) == NULL) return (-1);
	if (!(outp = opj_stream_create_default_file_stream(fp, FALSE))) goto ffail;
#else /* 2.1+ */
	if (!(outp = opj_stream_create_default_file_stream(file_name, FALSE)))
		return (-1);
#endif

	/* Create intermediate structure */
	image = prepare_opj(settings);
	if (!image) goto ffail;

	/* Compress it */
	if (!settings->silent) ls_init("JPEG2000", 1);
	cinfo = opj_create_compress(settings->ftype == FT_JP2 ? OPJ_CODEC_JP2 :
		OPJ_CODEC_J2K);
	if (!cinfo) goto fail;
	opj_set_default_encoder_parameters(&par);
	par.tcp_numlayers = 1;
	par.tcp_rates[0] = settings->jp2_rate;
	par.cp_disto_alloc = 1;
	opj_setup_encoder(cinfo, &par, image);
#if defined(U_THREADS) && (OPJ_VERSION_MINOR >= 4) /* 2.4+ */
	/* As of 2.4.0, 1.4x faster with 2 cores, 1.7x with 4+ */
	opj_codec_set_threads(cinfo, helper_threads());
#endif
	if (opj_start_compress(cinfo, image, outp) &&
		opj_encode(cinfo, outp) &&
		opj_end_compress(cinfo, outp)) res = 0;
fail:	opj_destroy_codec(cinfo);
	opj_image_destroy(image);
	if (!settings->silent) progress_end();
ffail:	opj_stream_destroy(outp);
#if !OPJ_VERSION_MINOR /* 2.0.x */
	fclose(fp);
#endif
	return (res);
}
#endif
#endif

#ifdef U_JASPER

/* *** PREFACE ***
 * JasPer is QUITE a memory waster, with peak memory usage nearly TEN times the
 * unpacked image size. But what is worse, its API is 99% undocumented.
 * And to add insult to injury, it reacts to some invalid JP2 files (4-channel
 * ones written by OpenJPEG) by abort()ing, instead of returning error - WJ */

static int jasper_init;

static int load_jpeg2000(char *file_name, ls_settings *settings)
{
	jas_image_t *img;
	jas_stream_t *inp;
	jas_matrix_t *mx;
	jas_seqent_t *src;
	char *fmt;
	unsigned char xtb[256], *dest;
	int nc, cspace, mode, slots[4];
	int bits, shift, chan, step;
	unsigned delta;
	int i, j, k, n, nx, w, h, bpp, pr = 0, res = -1;


	/* Init the dumb library */
	if (!jasper_init) jas_init();
	jasper_init = TRUE;
	/* Open the file */
	inp = jas_stream_fopen(file_name, "rb");
	if (!inp) return (-1);
	/* Validate format */
	fmt = jas_image_fmttostr(jas_image_getfmt(inp));
	if (!fmt || strcmp(fmt, settings->ftype == FT_JP2 ? "jp2" : "jpc"))
		goto ffail;

	/* Decode the file into a halfbaked pile of bytes */
	if ((pr = !settings->silent)) ls_init("JPEG2000", 0);
	img = jas_image_decode(inp, -1, NULL);
	jas_stream_close(inp);
	if (!img) goto dfail;
	/* Analyze the pile's contents */
	nc = jas_image_numcmpts(img);
	mode = jas_clrspc_fam(cspace = jas_image_clrspc(img));
	if (mode == JAS_CLRSPC_FAM_GRAY) bpp = 1;
	else if (mode == JAS_CLRSPC_FAM_RGB) bpp = 3;
	else goto ifail;
	if (bpp == 3)
	{
		slots[0] = jas_image_getcmptbytype(img,
			JAS_IMAGE_CT_COLOR(JAS_IMAGE_CT_RGB_R));
		slots[1] = jas_image_getcmptbytype(img,
			JAS_IMAGE_CT_COLOR(JAS_IMAGE_CT_RGB_G));
		slots[2] = jas_image_getcmptbytype(img,
			JAS_IMAGE_CT_COLOR(JAS_IMAGE_CT_RGB_B));
		if ((slots[1] < 0) | (slots[2] < 0)) goto ifail;
	}
	else
	{
		slots[0] = jas_image_getcmptbytype(img,
			JAS_IMAGE_CT_COLOR(JAS_IMAGE_CT_GRAY_Y));
		set_gray(settings);
	}
	if (slots[0] < 0) goto ifail;
	if (nc > bpp)
	{
		slots[bpp] = jas_image_getcmptbytype(img, JAS_IMAGE_CT_OPACITY);
/* !!! JasPer has a bug - it doesn't write out component definitions if color
 * channels are in natural order, thus losing the types of any extra components.
 * (See where variable "needcdef" in src/libjasper/jp2/jp2_enc.c gets unset.)
 * Then on reading, type will be replaced by component's ordinal number - WJ */
		if (slots[bpp] < 0) slots[bpp] = jas_image_getcmptbytype(img, bpp);
		/* Use an unlabeled extra component for alpha if no labeled one */
		if (slots[bpp] < 0)
			slots[bpp] = jas_image_getcmptbytype(img, JAS_IMAGE_CT_UNKNOWN);
		nc = bpp + (slots[bpp] >= 0); // Ignore extra channels if no alpha
	}
	w = jas_image_cmptwidth(img, slots[0]);
	h = jas_image_cmptheight(img, slots[0]);
	for (i = 1; i < nc; i++) /* Check if all components are the same size */
	{
		if ((jas_image_cmptwidth(img, slots[i]) != w) ||
			(jas_image_cmptheight(img, slots[i]) != h)) goto ifail;
	}

	/* Allocate "matrix" */
	res = FILE_MEM_ERROR;
	mx = jas_matrix_create(1, w);
	if (!mx) goto ifail;
	/* Allocate image */
	settings->width = w;
	settings->height = h;
	settings->bpp = bpp;
	if ((res = allocate_image(settings, nc > bpp ? CMASK_RGBA : CMASK_IMAGE)))
		goto mfail;
	if (!settings->img[CHN_ALPHA]) nc = bpp;
	res = 1;
#if U_LCMS
	/* JasPer implements CMS internally, but without lcms, it makes no sense
	 * to provide all the interface stuff for this one rare format - WJ */
	while (!settings->icc_size && (bpp == 3) && (cspace != JAS_CLRSPC_SRGB))
	{
		jas_cmprof_t *prof;
		jas_image_t *timg;

		res = FILE_LIB_ERROR;
		prof = jas_cmprof_createfromclrspc(JAS_CLRSPC_SRGB);
		if (!prof) break;
		timg = jas_image_chclrspc(img, prof, JAS_CMXFORM_INTENT_PER);
		jas_cmprof_destroy(prof);
		if (!timg) break;
		jas_image_destroy(img);
		img = timg;
		res = 1; // Success - further code is fail-proof
		break;
	}
#endif

	/* Unravel the ugly thing into proper format */
	nx = h * nc;
	for (i = n = 0; i < nc; i++)
	{
		if (i < bpp) /* Image */
		{
			dest = settings->img[CHN_IMAGE] + i;
			step = settings->bpp;
		}
		else /* Alpha */
		{
			dest = settings->img[CHN_ALPHA];
			step = 1;
		}
		chan = slots[i];
		bits = jas_image_cmptprec(img, chan);
		delta = jas_image_cmptsgnd(img, chan) ? 1U << (bits - 1) : 0;
		shift = bits > 8 ? bits - 8 : 0;
		set_xlate(xtb, bits - shift);
		for (j = 0; j < h; j++ , n++)
		{
			jas_image_readcmpt(img, chan, 0, j, w, 1, mx);
			src = jas_matrix_getref(mx, 0, 0);
			for (k = 0; k < w; k++)
			{
				*dest = xtb[(src[k] + delta) >> shift];
				dest += step;
			}
			if (pr && ((n * 10) % nx >= nx - 10))
				progress_update((float)n / nx);
		}
	}

mfail:	jas_matrix_destroy(mx);
ifail:	jas_image_destroy(img);
dfail:	if (pr) progress_end();
	return (res);
ffail:	jas_stream_close(inp);
	return (-1);
}

static int save_jpeg2000(char *file_name, ls_settings *settings)
{
	static const jas_image_cmpttype_t chans[4] = {
		JAS_IMAGE_CT_COLOR(JAS_IMAGE_CT_RGB_R),
		JAS_IMAGE_CT_COLOR(JAS_IMAGE_CT_RGB_G),
		JAS_IMAGE_CT_COLOR(JAS_IMAGE_CT_RGB_B),
		JAS_IMAGE_CT_OPACITY };
	jas_image_cmptparm_t cp[4];
	jas_image_t *img;
	jas_stream_t *outp;
	jas_matrix_t *mx;
	jas_seqent_t *dest;
	char buf[256], *opts = NULL;
	unsigned char *src;
	int w = settings->width, h = settings->height, res = -1;
	int i, j, k, n, nx, nc, step, pr;


	if (settings->bpp == 1) return WRONG_FORMAT;

	/* Init the dumb library */
	if (!jasper_init) jas_init();
	jasper_init = TRUE;
	/* Open the file */
	outp = jas_stream_fopen(file_name, "wb");
	if (!outp) return (-1);
	/* Setup component parameters */
	memset(cp, 0, sizeof(cp)); // Zero out all that needs zeroing
	cp[0].hstep = cp[0].vstep = 1;
	cp[0].width = w; cp[0].height = h;
	cp[0].prec = 8;
	cp[3] = cp[2] = cp[1] = cp[0];
	/* Create image structure */
	nc = 3 + !!settings->img[CHN_ALPHA];
	img = jas_image_create(nc, cp, JAS_CLRSPC_SRGB);
	if (!img) goto fail;
	/* Allocate "matrix" */
	mx = jas_matrix_create(1, w);
	if (!mx) goto fail2;

	if ((pr = !settings->silent)) ls_init("JPEG2000", 1);

	/* Fill image structure */
	nx = h * nc;
	nx += nx / 10 + 1; // Show "90% done" while compressing
	for (i = n = 0; i < nc; i++)
	{
	/* !!! The only workaround for JasPer losing extra components' types on
	 * write is to reorder the RGB components - but then, dumb readers, such
	 * as ones in Mozilla and GTK+, would read them in wrong order - WJ */
		jas_image_setcmpttype(img, i, chans[i]);
		if (i < 3) /* Image */
		{
			src = settings->img[CHN_IMAGE] + i;
			step = settings->bpp;
		}
		else /* Alpha */
		{
			src = settings->img[CHN_ALPHA];
			step = 1;
		}
		for (j = 0; j < h; j++ , n++)
		{
			dest = jas_matrix_getref(mx, 0, 0);
			for (k = 0; k < w; k++)
			{
				dest[k] = *src;
				src += step;
			}
			jas_image_writecmpt(img, i, 0, j, w, 1, mx);
			if (pr && ((n * 10) % nx >= nx - 10))
				if (progress_update((float)n / nx)) goto fail3;
		}
	}

	/* Compress it */
	if (pr) progress_update(0.9);
	if (settings->jp2_rate) // Lossless if NO "rate" option passed
		sprintf(opts = buf, "rate=%g", 1.0 / settings->jp2_rate);
	if (!jas_image_encode(img, outp, jas_image_strtofmt(
		settings->ftype == FT_JP2 ? "jp2" : "jpc"), opts)) res = 0;
	jas_stream_flush(outp);
	if (pr) progress_update(1.0);

fail3:	if (pr) progress_end();
	jas_matrix_destroy(mx);
fail2:	jas_image_destroy(img);
fail:	jas_stream_close(outp);
	return (res);
}
#endif

/* Slow-but-sure universal bitstream parsers; may read extra byte at the end */
static void stream_MSB(unsigned char *src, unsigned char *dest, int cnt,
	int bits, int bit0, int bitstep, int step)
{
	int i, j, v, mask = (1 << bits) - 1;

	for (i = 0; i < cnt; i++)
	{
		j = bit0 >> 3;
		v = (src[j] << 8) | src[j + 1];
		v >>= 16 - bits - (bit0 & 7);
		*dest = (unsigned char)(v & mask);
		bit0 += bitstep;
		dest += step;
	}
}

static void stream_LSB(unsigned char *src, unsigned char *dest, int cnt,
	int bits, int bit0, int bitstep, int step)
{
	int i, j, v, mask = (1 << bits) - 1;

	for (i = 0; i < cnt; i++)
	{
		j = bit0 >> 3;
		v = (src[j + 1] << 8) | src[j];
		v >>= bit0 & 7;
		*dest = (unsigned char)(v & mask);
		bit0 += bitstep;
		dest += step;
	}
}

static void pack_MSB(unsigned char *dest, unsigned char *src, int len, int bw)
{
	int i;

	memset(dest, 0, (len + 7) >> 3);
	for (i = 0; i < len; i++)
		dest[i >> 3] |= (*src++ == bw) << (~i & 7);
}

static void copy_bytes(unsigned char *dest, unsigned char *src, int len,
	int bpp, int step);

#ifdef U_TIFF

/* *** PREFACE ***
 * TIFF is a bitch, and libtiff is a joke. An unstable and buggy joke, at that.
 * It's a fact of life - and when some TIFFs don't load or are mangled, that
 * also is a fact of life. Installing latest libtiff may help - or not; sending
 * a bugreport with the offending file attached may help too - but again, it's
 * not guaranteed. But the common varieties of TIFF format should load OK. */

tiff_format tiff_formats[TIFF_MAX_TYPES] = {
	{ _("None"),	COMPRESSION_NONE, TIFFLAGS, XF_COMPT },
	{ "Group 3",	COMPRESSION_CCITTFAX3, FF_BW | TIFF0FLAGS, XF_COMPT },
	{ "Group 4",	COMPRESSION_CCITTFAX4, FF_BW | TIFF0FLAGS, XF_COMPT },
	{ "PackBits",	COMPRESSION_PACKBITS, TIFFLAGS, XF_COMPT },
	{ "LZW",	COMPRESSION_LZW, TIFFLAGS, XF_COMPT, 1 },
	{ "ZIP",	COMPRESSION_ADOBE_DEFLATE, TIFFLAGS, XF_COMPT | XF_COMPZT, 1 },
#ifdef COMPRESSION_LZMA
	{ "LZMA2",	COMPRESSION_LZMA, TIFFLAGS, XF_COMPT | XF_COMPLZ, 1 },
#endif
#ifdef COMPRESSION_ZSTD
	{ "ZSTD",	COMPRESSION_ZSTD, TIFFLAGS, XF_COMPT | XF_COMPZS, 1 },
#endif
	{ "JPEG",	COMPRESSION_JPEG, FF_RGB | TIFF0FLAGS, XF_COMPT | XF_COMPJ },
#ifdef COMPRESSION_WEBP
	{ "WebP",	COMPRESSION_WEBP, FF_RGB | FF_ALPHAR | TIFF0FLAGS, XF_COMPT | XF_COMPWT },
#endif
	{ NULL }
};

int tiff_lzma, tiff_zstd; /* FALSE by default */

void init_tiff_formats()	// Check what libtiff can handle
{
	tiff_format *src, *dest;

	/* Try all compiled-in formats */
	src = dest = tiff_formats + 1; // COMPRESSION_NONE is always there
	while (src->name)
	{
		if (TIFFIsCODECConfigured(src->id)) *dest++ = *src;
		src++;
	}
	/* Zero out extra slots */
	while (dest != src) *dest++ = *src;
	/* Set flag variables */
#ifdef COMPRESSION_LZMA
	tiff_lzma = TIFFIsCODECConfigured(COMPRESSION_LZMA);
#endif
#ifdef COMPRESSION_ZSTD
	tiff_zstd = TIFFIsCODECConfigured(COMPRESSION_ZSTD);
#endif
}

static int load_tiff_frame(TIFF *tif, ls_settings *settings)
{
	char cbuf[1024];
	uint16 bpsamp, sampp, xsamp, pmetric, planar, orient, sform;
	uint16 *sampinfo, *red16, *green16, *blue16;
	uint32 width, height, tw = 0, th = 0, rps = 0;
	uint32 *tr, *raster = NULL;
	unsigned char *tmp, *buf = NULL;
	int bpp = 3, cmask = CMASK_IMAGE, argb = FALSE, pr = FALSE;
	int i, j, mirror, res;


	/* Let's learn what we've got */
	TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &sampp);
	TIFFGetFieldDefaulted(tif, TIFFTAG_EXTRASAMPLES, &xsamp, &sampinfo);
	if (!TIFFGetField(tif, TIFFTAG_PHOTOMETRIC, &pmetric))
	{
		/* Defaults like in libtiff */
		if (sampp - xsamp == 1) pmetric = PHOTOMETRIC_MINISBLACK;
		else if (sampp - xsamp == 3) pmetric = PHOTOMETRIC_RGB;
		else return (-1);
	}
	TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &sform);
	TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width);
	TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height);
	TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bpsamp);
	TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &planar);
	planar = planar != PLANARCONFIG_CONTIG;
	TIFFGetFieldDefaulted(tif, TIFFTAG_ORIENTATION, &orient);
	switch (orient)
	{
	case ORIENTATION_TOPLEFT:
	case ORIENTATION_LEFTTOP: mirror = 0; break;
	case ORIENTATION_TOPRIGHT:
	case ORIENTATION_RIGHTTOP: mirror = 1; break;
	default:
	case ORIENTATION_BOTLEFT:
	case ORIENTATION_LEFTBOT: mirror = 2; break;
	case ORIENTATION_BOTRIGHT:
	case ORIENTATION_RIGHTBOT: mirror = 3; break;
	}
	if (TIFFIsTiled(tif))
	{
		TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tw);
		TIFFGetField(tif, TIFFTAG_TILELENGTH, &th);
	}
	else
	{
		TIFFGetFieldDefaulted(tif, TIFFTAG_ROWSPERSTRIP, &rps);
	}

	/* Extract position from it */
	settings->x = settings->y = 0;
	while (TRUE)
	{
		float xres, yres, dxu = 0, dyu = 0;

		if (!TIFFGetField(tif, TIFFTAG_XPOSITION, &dxu) &&
			!TIFFGetField(tif, TIFFTAG_YPOSITION, &dyu)) break;
		// Have position, now need resolution
		if (!TIFFGetField(tif, TIFFTAG_XRESOLUTION, &xres)) break;
		// X resolution we have, what about Y?
		yres = xres; // Default
		TIFFGetField(tif, TIFFTAG_YRESOLUTION, &yres);
		// Convert ResolutionUnits (whatever they are) to pixels
		settings->x = rint(dxu * xres);
		settings->y = rint(dyu * yres);
		break;
	}

	/* Let's decide how to store it */
	if ((width > MAX_WIDTH) || (height > MAX_HEIGHT)) return (TOO_BIG);
	settings->width = width;
	settings->height = height;
	if ((sform != SAMPLEFORMAT_UINT) && (sform != SAMPLEFORMAT_INT) &&
		(sform != SAMPLEFORMAT_VOID)) argb = TRUE;
	else	switch (pmetric)
		{
		case PHOTOMETRIC_PALETTE:
		{
			png_color *cp = settings->pal;
			int i, j, k, na = 0, nd = 1; /* Old palette format */

			if (bpsamp > 8)
			{
				argb = TRUE;
				break;
			}
			if (!TIFFGetField(tif, TIFFTAG_COLORMAP,
				&red16, &green16, &blue16)) return (-1);

			settings->colors = j = 1 << bpsamp;
			/* Analyze palette */
			for (k = i = 0; i < j; i++)
			{
				k |= red16[i] | green16[i] | blue16[i];
			}
			if (k > 255) na = 128 , nd = 257; /* New palette format */

			for (i = 0; i < j; i++ , cp++)
			{
				cp->red = (red16[i] + na) / nd;
				cp->green = (green16[i] + na) / nd;
				cp->blue = (blue16[i] + na) / nd;
			}
			/* If palette is all we need */
			if ((settings->mode == FS_PALETTE_LOAD) ||
				(settings->mode == FS_PALETTE_DEF))
				return (EXPLODE_FAILED);
			/* Fallthrough */
		}
		case PHOTOMETRIC_MINISWHITE:
		case PHOTOMETRIC_MINISBLACK:
			bpp = 1; break;
		case PHOTOMETRIC_RGB:
			break;
		case PHOTOMETRIC_SEPARATED:
			/* Leave non-CMYK separations to libtiff */
			if (sampp - xsamp == 4) break;
		default:
			argb = TRUE;
		}

	/* libtiff can't handle this and neither can we */
	if (argb && !TIFFRGBAImageOK(tif, cbuf)) return (-1);

	settings->bpp = bpp;
	/* Photoshop writes alpha as EXTRASAMPLE_UNSPECIFIED anyway */
	if (xsamp) cmask = CMASK_RGBA;

	/* !!! No alpha support for RGB mode yet */
	if (argb) cmask = CMASK_IMAGE;

	if ((res = allocate_image(settings, cmask))) return (res);
	res = -1;

#ifdef U_LCMS
#ifdef TIFFTAG_ICCPROFILE
	/* Extract ICC profile if it's of use */
	if (!settings->icc_size)
	{
		uint32 size;
		unsigned char *data;

		/* TIFFTAG_ICCPROFILE was broken beyond hope in libtiff 3.8.0
		 * (see libtiff 3.8.1+ changelog entry for 2006-01-04) */
		if (!strstr(TIFFGetVersion(), " 3.8.0") &&
			TIFFGetField(tif, TIFFTAG_ICCPROFILE, &size, &data) &&
			(size < INT_MAX) &&
			/* If profile is needed right now, for CMYK->RGB */
			!((pmetric == PHOTOMETRIC_SEPARATED) && !argb &&
				init_cmyk2rgb(settings, data, size, FALSE)) &&
			(settings->icc = malloc(size)))
		{
			settings->icc_size = size;
			memcpy(settings->icc, data, size);
		}
	}
#endif
#endif

	if ((pr = !settings->silent)) ls_init("TIFF", 0);

	/* Read it as ARGB if can't understand it ourselves */
	if (argb)
	{
		/* libtiff is too much of a moving target if finer control is
		 * needed, so let's trade memory for stability */
		raster = (uint32 *)_TIFFmalloc(width * height * sizeof(uint32));
		res = FILE_MEM_ERROR;
		if (!raster) goto fail2;
		res = FILE_LIB_ERROR;
		if (!TIFFReadRGBAImage(tif, width, height, raster, 0)) goto fail2;
		res = -1;

		/* Parse the RGB part only - alpha might be eaten by bugs */
		tr = raster;
		for (i = height - 1; i >= 0; i--)
		{
			tmp = settings->img[CHN_IMAGE] + width * i * bpp;
			j = width;
			while (j--)
			{
				tmp[0] = TIFFGetR(*tr);
				tmp[1] = TIFFGetG(*tr);
				tmp[2] = TIFFGetB(*tr);
				tmp += 3; tr++;
			}
			ls_progress(settings, height - i, 10);
		}

		_TIFFfree(raster);
		raster = NULL;

/* !!! Now it would be good to read in alpha ourselves - but not yet... */

		res = 1;
	}

	/* Read & interpret it ourselves */
	else
	{
		unsigned char xtable[256], *src, *tbuf = NULL;
		uint32 x0, y0, xstep = tw ? tw : width, ystep = th ? th : rps;
		int aalpha, tsz = 0, wbpp = bpp;
		int bpr, bits1, bit0, db, n, nx;
		int j, k, bsz, plane, nplanes;


		if (pmetric == PHOTOMETRIC_SEPARATED) // Needs temp buffer
			tsz = xstep * ystep * (wbpp = 4);
		nplanes = planar ? wbpp + !!settings->img[CHN_ALPHA] : 1;

		bsz = (tw ? TIFFTileSize(tif) : TIFFStripSize(tif)) + 1;
		bpr = tw ? TIFFTileRowSize(tif) : TIFFScanlineSize(tif);

		buf = _TIFFmalloc(bsz + tsz);
		res = FILE_MEM_ERROR;
		if (!buf) goto fail2;
		res = FILE_LIB_ERROR;
		if (tsz) tbuf = buf + bsz; // Temp buffer for CMYK->RGB

		/* Flag associated alpha */
		aalpha = settings->img[CHN_ALPHA] &&
			(pmetric != PHOTOMETRIC_PALETTE) &&
			(sampinfo[0] == EXTRASAMPLE_ASSOCALPHA);

		bits1 = bpsamp > 8 ? 8 : bpsamp;

		/* Setup greyscale palette */
		if ((bpp == 1) && (pmetric != PHOTOMETRIC_PALETTE))
		{
			/* Demultiplied values are 0..255 */
			j = aalpha ? 256 : 1 << bits1;
			settings->colors = j--;
			k = pmetric == PHOTOMETRIC_MINISBLACK ? 0 : j;
			mem_bw_pal(settings->pal, k, j ^ k);
		}

		/* !!! Assume 16-, 32- and 64-bit data follow machine's
		 * endianness, and everything else is packed big-endian way -
		 * like TIFF 6.0 spec says; but TIFF 5.0 and before specs said
		 * differently, so let's wait for examples to see if I'm right
		 * or not; as for 24- and 128-bit, even different libtiff
		 * versions handle them differently, so I leave them alone
		 * for now - WJ */

		bit0 = (G_BYTE_ORDER == G_LITTLE_ENDIAN) &&
			((bpsamp == 16) || (bpsamp == 32) ||
			(bpsamp == 64)) ? bpsamp - 8 : 0;
		db = (planar ? 1 : sampp) * bpsamp;

		/* Prepare to rescale what we've got */
		memset(xtable, 0, 256);
		set_xlate(xtable, bits1);

		/* Progress steps */
		nx = ((width + xstep - 1) / xstep) * nplanes * height;

		/* Read image tile by tile - considering strip a wide tile */
		for (n = y0 = 0; y0 < height; y0 += ystep)
		for (x0 = 0; x0 < width; x0 += xstep)
		for (plane = 0; plane < nplanes; plane++)
		{
			unsigned char *tmp, *tmpa;
			uint32 x, y, w, h, l;
			int i, k, dx, dxa, dy, dys;

			/* Read one piece */
			if (tw)
			{
				if (TIFFReadTile(tif, buf, x0, y0, 0, plane) < 0)
					goto fail2;
			}
			else
			{
				if (TIFFReadEncodedStrip(tif,
					TIFFComputeStrip(tif, y0, plane),
					buf, bsz) < 0) goto fail2;
			}

			/* Prepare decoding loops */
			if (mirror & 1) /* X mirror */
			{
				x = width - x0;
				w = x < xstep ? x : xstep;
				x -= w;
			}
			else
			{
				x = x0;
				w = x + xstep > width ? width - x : xstep;
			}
			if (mirror & 2) /* Y mirror */
			{
				y = height - y0;
				h = y < ystep ? y : ystep;
				y -= h;
			}
			else
			{
				y = y0;
				h = y + ystep > height ? height - y : ystep;
			}

			/* Prepare pointers */
			dx = dxa = 1; dy = width;
			i = y * width + x;
			tmp = tmpa = settings->img[CHN_ALPHA] + i;
			if (plane >= wbpp); // Alpha
			else if (tbuf) // CMYK
			{
				dx = 4; dy = w;
				tmp = tbuf + plane;
			}
			else // RGB/indexed
			{
				dx = bpp;
				tmp = settings->img[CHN_IMAGE] + plane + i * bpp;
			}
			dy *= dx; dys = bpr;
			src = buf;
			/* Account for horizontal mirroring */
			if (mirror & 1)
			{
				// Write bytes backward
				tmp += (w - 1) * dx; tmpa += w - 1;
				dx = -dx; dxa = -1;
			}
			/* Account for vertical mirroring */
			if (mirror & 2)
			{
				// Read rows backward
				src += (h - 1) * dys;
				dys = -dys;
			}

			/* Decode it */
			for (l = 0; l < h; l++ , n++ , src += dys , tmp += dy)
			{
				if (pr && ((n * 10) % nx >= nx - 10))
					progress_update((float)n / nx);

				stream_MSB(src, tmp, w, bits1, bit0, db, dx);
				if (planar) continue;
				for (k = 1; k < wbpp; k++)
				{
					stream_MSB(src, tmp + k, w, bits1,
						bit0 + bpsamp * k, db, dx);
				}
				if (settings->img[CHN_ALPHA])
				{
					stream_MSB(src, tmpa, w, bits1,
						bit0 + bpsamp * wbpp, db, dxa);
					tmpa += width;
				}
			}

			/* Convert CMYK to RGB if needed */
			if (!tbuf || (planar && (plane != 3))) continue;
			if (bits1 < 8)	// Rescale to 8-bit
				do_xlate(xtable, tbuf, w * h * 4);
			cmyk2rgb(tbuf, tbuf, w * h, FALSE, settings);
			src = tbuf;
			tmp = settings->img[CHN_IMAGE] + (y * width + x) * 3;
			w *= 3;
			for (l = 0; l < h; l++ , tmp += width * 3 , src += w)
				memcpy(tmp, src, w);
		}
		done_cmyk2rgb(settings);

		j = width * height;
		tmp = settings->img[CHN_IMAGE];
		src = settings->img[CHN_ALPHA];

		/* Unassociate alpha */
		if (aalpha)
		{
			if (wbpp > 3) // Converted from CMYK
			{
				unsigned char *img = tmp;
				int i, k, a;

				if (bits1 < 8) do_xlate(xtable, src, j);
				bits1 = 8; // No further rescaling needed

				/* Remove white background */
				for (i = 0; i < j; i++ , img += 3)
				{
					a = src[i] - 255;
					k = a + img[0];
					img[0] = k < 0 ? 0 : k;
					k = a + img[1];
					img[1] = k < 0 ? 0 : k;
					k = a + img[2];
					img[2] = k < 0 ? 0 : k;
				}
			}
			mem_demultiply(tmp, src, j, bpp);
			tmp = NULL; // Image is done
		}

		if (bits1 < 8)
		{
			/* Rescale alpha */
			if (src) do_xlate(xtable, src, j);
			/* Rescale RGB */
			if (tmp && (wbpp == 3)) do_xlate(xtable, tmp, j * 3);
		}
		res = 1;
	}

fail2:	if (pr) progress_end();
	if (raster) _TIFFfree(raster);
	if (buf) _TIFFfree(buf);
	return (res);
}

static int load_tiff_frames(char *file_name, ani_settings *ani)
{
	TIFF *tif;
	ls_settings w_set;
	int res;


	/* We don't want any echoing to the output */
	TIFFSetErrorHandler(NULL);
	TIFFSetWarningHandler(NULL);

	if (!(tif = TIFFOpen(file_name, "r"))) return (-1);

	while (TRUE)
	{
		res = FILE_TOO_LONG;
		if (!check_next_frame(&ani->fset, ani->settings.mode, FALSE))
			goto fail;
		w_set = ani->settings;
		w_set.gif_delay = -1; // Multipage
		res = load_tiff_frame(tif, &w_set);
		if (res != 1) goto fail;
		res = process_page_frame(file_name, ani, &w_set);
		if (res) goto fail;
		/* Try to get next frame */
		if (!TIFFReadDirectory(tif)) break;
	}
	res = 1;
fail:	TIFFClose(tif);
	return (res);
}

#ifndef TIFF_VERSION_BIG /* The ONLY useful way to detect libtiff 4.x vs 3.x */
#define tmsize_t tsize_t
#endif

static tmsize_t mTIFFread(thandle_t fd, void* buf, tmsize_t size)
{
	return mfread(buf, 1, size, (memFILE *)fd);
}

static tmsize_t mTIFFwrite(thandle_t fd, void* buf, tmsize_t size)
{
	return mfwrite(buf, 1, size, (memFILE *)fd);
}

static toff_t mTIFFlseek(thandle_t fd, toff_t off, int whence)
{
	return mfseek((memFILE *)fd, (f_long)off, whence) ? -1 : ((memFILE *)fd)->m.here;
}

static int mTIFFclose(thandle_t fd)
{
	return 0;
}

static toff_t mTIFFsize(thandle_t fd)
{
	return ((memFILE *)fd)->top;
}

static int mTIFFmap(thandle_t fd, void** base, toff_t* size)
{
	*base = ((memFILE *)fd)->m.buf;
	*size = ((memFILE *)fd)->top;
	return 1;
}

static void mTIFFunmap(thandle_t fd, void* base, toff_t size)
{
}

static int load_tiff(char *file_name, ls_settings *settings, memFILE *mf)
{
	TIFF *tif;
	int res;


	/* We don't want any echoing to the output */
	TIFFSetErrorHandler(NULL);
	TIFFSetWarningHandler(NULL);

	if (!mf) tif = TIFFOpen(file_name, "r");
	else tif = TIFFClientOpen("", "r", (void *)mf, mTIFFread, mTIFFwrite,
		mTIFFlseek, mTIFFclose, mTIFFsize, mTIFFmap, mTIFFunmap);
	if (!tif) return (-1);
	res = load_tiff_frame(tif, settings);
	if ((res == 1) && TIFFReadDirectory(tif)) res = FILE_HAS_FRAMES;
	TIFFClose(tif);
	return (res);
}

static int save_tiff(char *file_name, ls_settings *settings, memFILE *mf)
{
	unsigned char buf[MAX_WIDTH / 8], *src, *row = NULL;
	uint16 rgb[256 * 3];
	unsigned int tflags, sflags, xflags;
	int i, l, type, bw, af, pf, res = 0, pmetric = -1;
	int w = settings->width, h = settings->height, bpp = settings->bpp;
	TIFF *tif;


	/* Select output mode */
	sflags = FF_SAVE_MASK_FOR(*settings);
	type = settings->tiff_type;
	if (type < 0) type = bpp == 3 ? tiff_rtype : // RGB
		settings->colors <= 2 ?	tiff_btype : // BW
		tiff_itype; // Indexed
	if (settings->mode == FS_CLIPBOARD)
	{
		type = 0; // Uncompressed
		/* RGB for clipboard mask */
		if (settings->img[CHN_ALPHA]) sflags = FF_RGB , bpp = 3;
	}
	tflags = tiff_formats[type].flags;
	sflags &= tflags;
	bw = !(sflags & (FF_256 | FF_RGB));
	if (!sflags) return WRONG_FORMAT; // Paranoia

	af = settings->img[CHN_ALPHA] && (tflags & FF_ALPHA);

	/* Use 1-bit mode where possible */
	if (!bw && !af && (sflags & FF_BW))
	{
		/* No need of palette if the colors are full white and black */
		i = PNG_2_INT(settings->pal[0]);
		bw = (!i ? 0xFFFFFF : i == 0xFFFFFF ? 0 : -1) ==
			PNG_2_INT(settings->pal[1]) ? 1 : -1;
	}		

	/* !!! When using predictor, libtiff 3.8 modifies row buffer in-place */
	pf = tiff_predictor && !bw && tiff_formats[type].pflag;
	if (af || pf || (bpp > settings->bpp))
	{
		row = malloc(w * (bpp + af));
		if (!row) return -1;
	}

	TIFFSetErrorHandler(NULL);	// We don't want any echoing to the output
	TIFFSetWarningHandler(NULL);
	if (!mf) tif = TIFFOpen(file_name, "w");
	else tif = TIFFClientOpen("", "w", (void *)mf, mTIFFread, mTIFFwrite,
		mTIFFlseek, mTIFFclose, mTIFFsize, mTIFFmap, mTIFFunmap);
	if (!tif)
	{
		free(row);
		return -1;
	}

	/* Write regular tags */
	TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, w);
	TIFFSetField(tif, TIFFTAG_IMAGELENGTH, h);
	TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, bpp + af);
	TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bw ? 1 : 8);
	TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);

	/* Write compression-specific tags */
	TIFFSetField(tif, TIFFTAG_COMPRESSION, tiff_formats[type].id);
	xflags = tiff_formats[type].xflags;
	if (xflags & XF_COMPZT)
		TIFFSetField(tif, TIFFTAG_ZIPQUALITY, settings->png_compression);
#ifdef COMPRESSION_LZMA
	if (xflags & XF_COMPLZ)
		TIFFSetField(tif, TIFFTAG_LZMAPRESET, settings->lzma_preset);
#endif
#ifdef COMPRESSION_ZSTD
	if (xflags & XF_COMPZS)
		TIFFSetField(tif, TIFFTAG_ZSTD_LEVEL, settings->zstd_level);
#endif
#ifdef COMPRESSION_WEBP
	if (xflags & XF_COMPWT)
	{
		TIFFSetField(tif, TIFFTAG_WEBP_LEVEL, settings->webp_quality);
		// !!! libtiff 4.0.10 *FAILS* to do it losslessly despite trying
		if (settings->webp_quality == 100)
			TIFFSetField(tif, TIFFTAG_WEBP_LOSSLESS, 1);
	}
#endif
	if (xflags & XF_COMPJ)
	{
		TIFFSetField(tif, TIFFTAG_JPEGQUALITY, settings->jpeg_quality);
		TIFFSetField(tif, TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB);
		pmetric = PHOTOMETRIC_YCBCR;
	}
	if (pf) TIFFSetField(tif, TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL);

	if (bw > 0) pmetric = get_bw(settings) ? PHOTOMETRIC_MINISWHITE :
		PHOTOMETRIC_MINISBLACK;
	else if (bpp == 1)
	{
		pmetric = PHOTOMETRIC_PALETTE;
		memset(rgb, 0, sizeof(rgb));
		l = bw ? 2 : 256;
		for (i = 0; i < settings->colors; i++)
		{
			rgb[i] = settings->pal[i].red * 257;
			rgb[i + l] = settings->pal[i].green * 257;
			rgb[i + l * 2] = settings->pal[i].blue * 257;
		}
		TIFFSetField(tif, TIFFTAG_COLORMAP, rgb, rgb + l, rgb + l * 2);
	}
	else if (pmetric < 0) pmetric = PHOTOMETRIC_RGB;
	TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, pmetric);
	if (af)
	{
		rgb[0] = EXTRASAMPLE_UNASSALPHA;
		TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, 1, rgb);
	}

	/* Actually write the image */
	if (!settings->silent) ls_init("TIFF", 1);
	for (i = 0; i < h; i++)
	{
		src = settings->img[CHN_IMAGE] + w * i * settings->bpp;
		if (bw) /* Pack the bits */
		{
			pack_MSB(buf, src, w, 1);
			src = buf;
		}
		else if (row) /* Fill the buffer */
			src = prepare_row(row, settings, bpp + af, i);
		if (TIFFWriteScanline(tif, src, i, 0) == -1)
		{
			res = -1;
			break;
		}
		ls_progress(settings, i, 20);
	}
	TIFFClose(tif);

	if (!settings->silent) progress_end();

	free(row);
	return (res);
}
#endif

#ifdef U_WEBP

/* *** PREFACE ***
 * WebP Demux API is exceedingly cumbersome in case you don't feed the entire
 * file to it in one single memory block. Stepping through the RIFF container to
 * pick individual chunks is more manageable. */

/* Macro for little-endian tags (RIFF) */
#define TAG4(A,B,C,D) ((A) + ((B) << 8) + ((C) << 16) + ((D) << 24))

#define GET24(buf) (((buf)[2] << 16) + ((buf)[1] << 8) + (buf)[0])
#define PUT24(buf, v) (buf)[0] = (v) & 0xFF; (buf)[1] = ((v) >> 8) & 0xFF; \
	(buf)[2] = ((v) >> 16) & 0xFF;

/* !!! In documentation, bit fields are written in reverse; actual flag values
 * used by libwebp are as if bits are numbered right to left instead - WJ */

/* Macros for WEBP tags */
#define TAG4_RIFF TAG4('R', 'I', 'F', 'F')
#define TAG4_WEBP TAG4('W', 'E', 'B', 'P')
#define TAG4_VP8X TAG4('V', 'P', '8', 'X')
#define TAG4_ANIM TAG4('A', 'N', 'I', 'M')
#define TAG4_ANMF TAG4('A', 'N', 'M', 'F')
#define TAG4_VP8  TAG4('V', 'P', '8', ' ')
#define TAG4_VP8L TAG4('V', 'P', '8', 'L')
#define TAG4_ALPH TAG4('A', 'L', 'P', 'H')
#define TAG4_ICCP TAG4('I', 'C', 'C', 'P')
#define TAG4_EXIF TAG4('E', 'X', 'I', 'F')
#define TAG4_XMP  TAG4('X', 'M', 'P', ' ')

#define RIFF_TAGSIZE 4

/* RIFF block header */
#define RIFF_TAG    0 /* 32b */
#define RIFF_SIZE   4 /* 32b */
#define RIFF_HSIZE  8

/* WEBP header block (VP8X tag) */
#define VP8X_FLAGS  0 /*  8b */
#define VP8X_W1     4 /* 24b */
#define VP8X_H1     7 /* 24b */
#define VP8X_SIZE  10

/* VP8X flags (ignore on read) */
#define VP8XF_ANIM   2
#define VP8XF_ALPHA 16
#define VP8XF_ICC   32

/* ANIM block */
/* !!! Background color & alpha are ignored by vwebp */
#define ANIM_BKG_B  0 /* 8b */
#define ANIM_BKG_G  1 /* 8b */
#define ANIM_BKG_R  2 /* 8b */
#define ANIM_BKG_A  3 /* 8b */
#define ANIM_LOOP   4 /* 16b */
#define ANIM_SIZE   6

/* ANMF block header */
#define ANMF_X2     0 /* 24b */
#define ANMF_Y2     3 /* 24b */
#define ANMF_W1     6 /* 24b */
#define ANMF_H1     9 /* 24b */
#define ANMF_DELAY 12 /* 24b */
#define ANMF_FLAGS 15 /*  8b */
#define ANMF_HSIZE 16

/* ANMF flags */
#define ANMF_F_NOBLEND 2
#define ANMF_F_BKG     1 /* Dispose to background */

#define HAVE_IMG  0x01
#define HAVE_VP8X 0x02
#define HAVE_ANIM 0x04
#define HAVE_XTRA 0x08
#define HAVE_ALPH 0x10
#define HAVE_ANMF 0x20

typedef struct {
	FILE *f;
	unsigned len;	// File data left unparsed
	unsigned size;	// How much to read for this frame
	int blocks;
	unsigned char hdr[VP8X_SIZE], bkg[4];
	unsigned char anmf[ANMF_HSIZE + RIFF_TAGSIZE];
} webphead;

static int load_webp_frame(webphead *wp, ls_settings *settings)
{
	WebPDecoderConfig dconf;
	unsigned char *buf;
	int wh, bpp, wbpp = 3, cmask = CMASK_IMAGE, res = -1;
	

	if (!WebPInitDecoderConfig(&dconf)) return (-1); // Wrong lib version
	if (!(buf = malloc(wp->size))) return (FILE_MEM_ERROR);
	if (fread(buf, 1, wp->size, wp->f) != wp->size) goto fail;
	if (WebPGetFeatures((void *)buf, wp->size, &dconf.input) != VP8_STATUS_OK)
		goto fail;

	if (dconf.input.has_alpha) wbpp = 4 , cmask = CMASK_RGBA;
	wh = dconf.input.width * dconf.input.height;
	settings->width = dconf.input.width;
	settings->height = dconf.input.height;
	settings->bpp = wbpp;

	/* Get the extras from frame header */
	if (wp->blocks & HAVE_ANMF)
	{
		settings->x = GET24(wp->anmf + ANMF_X2) * 2;
		settings->y = GET24(wp->anmf + ANMF_Y2) * 2;
		/* Within mtPaint delays are 1/100s granular */
		settings->gif_delay = (GET24(wp->anmf + ANMF_DELAY) + 9) / 10; // Round up
	}

	if ((res = allocate_image(settings, cmask))) goto fail;

	bpp = settings->img[CHN_ALPHA] ? 4 : 3;
	dconf.output.colorspace = bpp > 3 ? MODE_RGBA : MODE_RGB;
	dconf.output.u.RGBA.rgba = settings->img[CHN_IMAGE];
	dconf.output.u.RGBA.stride = settings->width * bpp;
	dconf.output.u.RGBA.size = wh * bpp;
	dconf.output.is_external_memory = 1;

	if (!settings->silent) ls_init("WebP", 0);
	res = FILE_LIB_ERROR;
	if (WebPDecode(buf, wp->size, &dconf) == VP8_STATUS_OK)
	{
		if (bpp == 4) /* Separate out alpha from RGBA */
		{
			copy_bytes(settings->img[CHN_ALPHA],
				settings->img[CHN_IMAGE] + 3, wh, 1, 4);
			copy_bytes(settings->img[CHN_IMAGE],
				settings->img[CHN_IMAGE], wh, 3, 4);
		}
		if (wbpp > 3) /* Downsize image channel */
		{
			unsigned char *w = realloc(settings->img[CHN_IMAGE], wh * 3);
			if (w) settings->img[CHN_IMAGE] = w;
		}
		res = 1;
	}
	if (!settings->silent) progress_end();
	WebPFreeDecBuffer(&dconf.output);

fail:	free(buf);
	return (res);
}

static int webp_scan(webphead *wp, ls_settings *settings)
{
	unsigned char buf[256];
	FILE *fp = wp->f;
	unsigned tag, tl;
	f_long alph = -1;

	if (!settings) // Next-frame mode
	{
		/* If a regular image was first, believe there are no other frames */
		if (!(wp->blocks & HAVE_ANMF)) return (FALSE);

		wp->blocks &= ~(HAVE_ALPH | HAVE_IMG); // Prepare for new frame
	}

	/* Read block headers & see what we get */
	while ((wp->len >= RIFF_HSIZE) && (fread(buf, 1, RIFF_HSIZE, fp) == RIFF_HSIZE))
	{
		tag = GET32(buf);
		tl = GET32(buf + RIFF_SIZE);
		if (tl > 0xFFFFFFFFU - 1 - RIFF_HSIZE) break; // MAX_CHUNK_PAYLOAD
		if (tl >= F_LONG_MAX) break; // Limit for what can be handled
		tl += tl & 1; // Pad
		if (wp->len < tl + RIFF_HSIZE) break; // Does not fit in RIFF
		wp->len -= tl + RIFF_HSIZE;

		if (tag == TAG4_ANMF)
		{
			if (tl < ANMF_HSIZE + RIFF_HSIZE) break;
			if (fread(wp->anmf, 1, ANMF_HSIZE + RIFF_TAGSIZE, fp) !=
				ANMF_HSIZE + RIFF_TAGSIZE) break;
			fseek(fp, -RIFF_TAGSIZE, SEEK_CUR);
			wp->blocks |= HAVE_ANMF;
			tag = GET32(wp->anmf + ANMF_HSIZE);
			if (tag == TAG4_ALPH) wp->blocks |= HAVE_ALPH;
			else if ((tag != TAG4_VP8) && (tag != TAG4_VP8L)) break; // Damaged?
			wp->size = tl - ANMF_HSIZE;
			wp->blocks |= HAVE_IMG;
			break; // Done
		}
		/* Be extra accepting - skip duplicates of header chunks w/o failing */
		else if (!settings && ((tag == TAG4_VP8X) || (tag == TAG4_ANIM) ||
			(tag == TAG4_ICCP)));
		/* Fail on encountering image chunk where a frame should be */
		else if (!settings && ((tag == TAG4_VP8) || (tag == TAG4_VP8L) ||
			(tag == TAG4_ALPH))) break;
		else if (tag == TAG4_VP8X)
		{
			if (tl != VP8X_SIZE) break;
			if (fread(wp->hdr, 1, VP8X_SIZE, fp) != VP8X_SIZE) break;
			wp->blocks |= HAVE_VP8X;
			continue;
		}
		else if (tag == TAG4_ANIM)
		{
			if (tl != ANIM_SIZE) break;
			if (fread(buf, 1, ANIM_SIZE, fp) != ANIM_SIZE) break;
			/* Rearrange the bytes the sane way */
			wp->bkg[0] = buf[ANIM_BKG_R];
			wp->bkg[1] = buf[ANIM_BKG_G];
			wp->bkg[2] = buf[ANIM_BKG_B];
			wp->bkg[3] = buf[ANIM_BKG_A];
			wp->blocks |= HAVE_ANIM;
			continue;
		}
#ifdef U_LCMS
		else if (tag == TAG4_ICCP)
		{
			unsigned char *tmp = NULL;

			wp->blocks |= HAVE_XTRA;
			if (!settings->icc_size &&
				(tl < INT_MAX) && // For sanity (and icc_size size)
				(tmp = malloc(tl)) &&
				(fread(tmp, 1, tl, fp) == tl))
			{
				settings->icc = tmp;
				settings->icc_size = tl;
				continue;
			}
			if (tmp) /* Failed to read */
			{
				free(tmp);
				break;
			}
		}
#endif
		else if (tag == TAG4_ALPH)
		{
			wp->blocks |= HAVE_ALPH;
			alph = ftell(fp);
			if (alph < 0) break;
		}
		else if ((tag == TAG4_VP8) || (tag == TAG4_VP8L))
		{
			fseek(fp, -RIFF_HSIZE, SEEK_CUR);
			wp->size = tl + RIFF_HSIZE;
			if (alph > 0) // Need to start with alpha
			{
				f_long here = ftell(fp);
				if (here < 0) break; // Too long for us
				/* From ALPH header to this block's end */
				wp->size += here - alph + RIFF_HSIZE;
				fseek(fp, alph - RIFF_HSIZE, SEEK_SET);
			}
			wp->blocks |= HAVE_IMG;
			break;
		}
		else wp->blocks |= HAVE_XTRA;
		/* Default: skip (the rest of) tag data */
		if (tl && fseek(fp, tl, SEEK_CUR)) break;
	}
	return (wp->blocks & HAVE_IMG);
}

static int webp_behead(FILE *fp, webphead *wp, ls_settings *settings)
{
	unsigned char buf[RIFF_HSIZE + RIFF_TAGSIZE];
	unsigned tl;


	memset(wp, 0, sizeof(webphead));
	wp->f = fp;

	/* Read the RIFF header & check signature */
	if (fread(buf, 1, RIFF_HSIZE + RIFF_TAGSIZE, fp) < RIFF_HSIZE + RIFF_TAGSIZE)
		return (FALSE);
	if ((GET32(buf) != TAG4_RIFF) || (GET32(buf + RIFF_HSIZE) != TAG4_WEBP))
		return (FALSE);

	tl = GET32(buf + RIFF_SIZE);
	if (tl < RIFF_TAGSIZE + RIFF_HSIZE) return (FALSE);
	tl -= RIFF_TAGSIZE;
	tl += tl & 1; // Pad
	wp->len = tl;

	webp_scan(wp, settings);
	return (wp->blocks & HAVE_IMG);
}

static int load_webp_frames(char *file_name, ani_settings *ani)
{
	webphead wp;
	ani_status stat;
	ls_settings w_set, init_set;
	FILE *fp;
	int bpp, disposal, res = -1;


	if (!(fp = fopen(file_name, "rb"))) return (-1);
	memset(w_set.img, 0, sizeof(chanlist));

	/* Init temp container */
	init_set = ani->settings;
	if (!webp_behead(fp, &wp, &init_set)) goto fail;

	/* Need to have them to read them */
	if ((wp.blocks & (HAVE_VP8X | HAVE_ANMF)) != (HAVE_VP8X | HAVE_ANMF))
		goto fail;

	/* Init state structure */
	memset(&stat, 0, sizeof(stat));
	stat.mode = ani->mode;
	stat.defw = GET24(wp.hdr + VP8X_W1) + 1;
	stat.defh = GET24(wp.hdr + VP8X_H1) + 1;
	/* WebP has no palette of its own, use the default one */
	mem_pal_copy(stat.newpal, ani->settings.pal);
	stat.newcols = ani->settings.colors;
	stat.newtrans = -1; // No color-key transparency
	/* !!! vwebp ignores the value by default, and one example animation
	 * expects transparency despite background set to opaque white - WJ */
//	memcpy(stat.bkg, wp.bkg, 4); // RGBA background

	/* Go through images */
	while (TRUE)
	{
		res = FILE_TOO_LONG;
		if (!check_next_frame(&ani->fset, ani->settings.mode, TRUE))
			goto fail;
		w_set = init_set;
		res = load_webp_frame(&wp, &w_set);
		if (res != 1) goto fail;
		disposal = wp.anmf[ANMF_FLAGS] & ANMF_F_BKG ? FM_DISP_REMOVE :
			FM_DISP_LEAVE;
		delete_alpha(&w_set, 255);
		stat.blend = !(wp.anmf[ANMF_FLAGS] & ANMF_F_NOBLEND) && w_set.img[CHN_ALPHA];
		/* Analyze how we can merge the frames */
		res = TOO_BIG;
		bpp = analyze_rgba_frame(&stat, &w_set);
		if (bpp < 0) goto fail;

		/* Allocate a new frame */
		res = add_frame(ani, &stat, &w_set, bpp, disposal);
		if (res) goto fail;

		/* Do actual compositing, remember disposal method */
		composite_frame(&ani->fset, &stat, &w_set);
		mem_free_chanlist(w_set.img);
		memset(w_set.img, 0, sizeof(chanlist));

		/* Write out those frames worthy to be stored */
		res = done_frame(file_name, ani, FALSE);
		if (res != 1) goto fail;

		/* Step to the next frame, if any */
		if (!webp_scan(&wp, NULL)) break;
	}
	/* Write out the final frame if not written before */
	res = done_frame(file_name, ani, TRUE);

fail:	mem_free_chanlist(w_set.img);
	fclose(fp);
	return (res);
}

static int load_webp(char *file_name, ls_settings *settings)
{
	webphead wp;
	FILE *fp;
	int res = -1;


	if (!(fp = fopen(file_name, "rb"))) return (-1);
	if (webp_behead(fp, &wp, settings))
	{
		res = load_webp_frame(&wp, settings);
		if ((res == 1) && webp_scan(&wp, NULL)) res = FILE_HAS_FRAMES;
	}
	fclose(fp);
	return (res);
}

static int webp_fwrite(const uint8_t *data, size_t data_size, const WebPPicture *picture)
{
	return (fwrite(data, sizeof(uint8_t), data_size, picture->custom_ptr) == data_size);
}

static int webp_progress(int percent, const WebPPicture *picture)
{
	return (!progress_update((float)percent / 100));
}

char *webp_presets[] = { _("Lossless"), _("Default"), _("Picture"), _("Photo"),
	_("Drawing"), _("Icon"), _("Text"), NULL };

// !!! Min version 0.5.0

static int save_webp(char *file_name, ls_settings *settings)
{
	static const signed char presets[] = {
		WEBP_PRESET_DEFAULT, /* Lossless */
		WEBP_PRESET_DEFAULT,
		WEBP_PRESET_PICTURE,
		WEBP_PRESET_PHOTO,
		WEBP_PRESET_DRAWING,
		WEBP_PRESET_ICON,
		WEBP_PRESET_TEXT };
	WebPConfig conf;
	WebPPicture pic;
	FILE *fp;
	unsigned char *rgba;
	int wh, st, res = -1;

	if (settings->bpp == 1) return WRONG_FORMAT;

	if ((fp = fopen(file_name, "wb")) == NULL) return (-1);

	if (!WebPConfigPreset(&conf, presets[settings->webp_preset],
		settings->webp_quality)) goto ffail; // Lib failure
	if (!settings->webp_preset)
		WebPConfigLosslessPreset(&conf, settings->webp_compression);
	conf.exact = TRUE; // Preserve invisible parts

	/* Prepare intermediate container */
	if (!WebPPictureInit(&pic)) goto ffail; // Lib failure
	pic.use_argb = TRUE;
	pic.width = settings->width;
	pic.height = settings->height;
	wh = pic.width * pic.height;
	pic.writer = webp_fwrite;
	pic.custom_ptr = (void *)fp;
	if (!settings->silent) pic.progress_hook = webp_progress;
	if (settings->img[CHN_ALPHA]) /* Need RGBA */
	{
		rgba = malloc(wh * 4);
		if (!rgba) goto ffail;
		copy_bytes(rgba, settings->img[CHN_IMAGE], wh, 4, 3);
		copy_bytes(rgba + 3, settings->img[CHN_ALPHA], wh, 4, 1);
		st = WebPPictureImportRGBA(&pic, rgba, pic.width * 4);
		free(rgba);
	}
	/* RGB is enough */
	else st = WebPPictureImportRGB(&pic, settings->img[CHN_IMAGE], pic.width * 3);

	/* Do encode */
	if (st)
	{
		if (!settings->silent) ls_init("WebP", 1);
		if (WebPEncode(&conf, &pic)) res = 0;
		WebPPictureFree(&pic);
		if (!settings->silent) progress_end();
	}

ffail:	fclose(fp);
	return (res);
}
#endif

/* Version 2 fields */
#define BMP_FILESIZE  2		/* 32b */
#define BMP_XHOT      6		/* 16b */
#define BMP_YHOT      8		/* 16b */
#define BMP_DATAOFS  10		/* 32b */
#define BMP_HDR2SIZE 14		/* 32b */
#define BMP_WIDTH    18		/* s32b */
#define BMP_HEIGHT   22		/* s32b */
#define BMP_PLANES   26		/* 16b */
#define BMP_BPP      28		/* 16b */
#define BMP2_HSIZE   30
/* Version 3 fields */
#define BMP_COMPRESS 30		/* 32b */
#define BMP_DATASIZE 34		/* 32b */
#define BMP_XDPI     38		/* s32b */
#define BMP_YDPI     42		/* s32b */
#define BMP_COLORS   46		/* 32b */
#define BMP_ICOLORS  50		/* 32b */
#define BMP3_HSIZE   54
/* Version 4 fields */
#define BMP_RMASK    54		/* 32b */
#define BMP_GMASK    58		/* 32b */
#define BMP_BMASK    62		/* 32b */
#define BMP_AMASK    66		/* 32b */
#define BMP_CSPACE   70		/* 32b */
#define BMP4_HSIZE  122
/* Version 5 fields */
#define BMP_INTENT  122		/* 32b */
#define BMP_ICCOFS  126		/* 32b */
#define BMP_ICCSIZE 130		/* 32b */
#define BMP5_HSIZE  138
#define BMP_MAXHSIZE (BMP5_HSIZE + 256 * 4)

/* OS/2 1.x alternative fields */
#define OS2BMP_WIDTH  18	/* 16b */
#define OS2BMP_HEIGHT 20	/* 16b */
#define OS2BMP_PLANES 22	/* 16b */
#define OS2BMP_BPP    24	/* 16b */
#define OS2BMP_HSIZE  26

/* OS/2 2.x bitmap header is version 3 fields plus some extra */
#define OS2BMP2_HSIZE 78
/* Shortened header variant encountered in the wild */
#define OS2BMP2_HSIZE_S 38

/* OS/2 bitmap array fields */
#define OS2BA_HDRSIZE  2	/* 32b */
#define OS2BA_NEXT     6	/* 32b */
#define OS2BA_HSIZE   14

/* In OS/2 files, BMP_FILESIZE may instead contain header size */

/* Colorspace tags */
#define BMPCS_WIN   TAG4B('W', 'i', 'n', ' ')
#define BMPCS_SRGB  TAG4B('s', 'R', 'G', 'B')
#define BMPCS_LINK  TAG4B('L', 'I', 'N', 'K')
#define BMPCS_EMBED TAG4B('M', 'B', 'E', 'D')

static int load_bmp(char *file_name, ls_settings *settings, memFILE *mf)
{
	guint32 masks[4];
	unsigned char hdr[BMP5_HSIZE], xlat[256], *dest, *tmp, *buf = NULL;
	memFILE fake_mf;
	FILE *fp = NULL;
	unsigned l, ofs;
	int shifts[4], bpps[4];
	int def_alpha = FALSE, cmask = CMASK_IMAGE, comp = 0, ba = 0, rle = 0, res = -1;
	int i, j, k, n, ii, w, h, bpp, wbpp;
	int bl, rl, step, skip, dx, dy;


	if (!mf)
	{
		if (!(fp = fopen(file_name, "rb"))) return (-1);
		memset(mf = &fake_mf, 0, sizeof(fake_mf));
		fake_mf.file = fp;
	}

	/* Read the largest header */
	k = mfread(hdr, 1, BMP5_HSIZE, mf);

	/* Bitmap array? */
	if ((k > OS2BA_HSIZE) && (hdr[0] == 'B') && (hdr[1] == 'A'))
	{
		/* Just skip the header to go for 1st bitmap: no example files
		 * with more than one bitmap anyway */
		ba = OS2BA_HSIZE;
		memmove(hdr, hdr + ba, k -= ba);
	}
	/* Check general validity */
	if (k < OS2BMP_HSIZE) goto fail; /* Least supported header size */
	if ((hdr[0] != 'B') || (hdr[1] != 'M')) goto fail; /* Signature */
	l = GET32(hdr + BMP_HDR2SIZE);
	if (k - BMP_HDR2SIZE < l) goto fail;
	l += BMP_HDR2SIZE;
	if (ba && (l > OS2BMP2_HSIZE)) goto fail; /* Should not exist */
	ofs = GET32(hdr + BMP_DATAOFS);
	if (l + ba > ofs) goto fail; // Overlap
	if (ofs > F_LONG_MAX) goto fail; // Cannot handle this length

	/* Check format type: OS/2 or Windows */
	if (l == OS2BMP_HSIZE)
	{
		w = GET16(hdr + OS2BMP_WIDTH);
		h = GET16(hdr + OS2BMP_HEIGHT);
		bpp = GET16(hdr + OS2BMP_BPP);
	}
	else if (l >= BMP2_HSIZE)
	{
		w = GET32s(hdr + BMP_WIDTH);
		h = GET32s(hdr + BMP_HEIGHT);
		bpp = GET16(hdr + BMP_BPP);
	}
	else goto fail;

	/* Check format */
	if (l >= BMP3_HSIZE) comp = GET32(hdr + BMP_COMPRESS);
	/* !!! Some 8bpp OS/2 BMPs in the wild have compression not marked */
	if (!comp && (bpp == 8) && (h > 0) && (l == OS2BMP2_HSIZE_S))
	{
		unsigned fsize = GET32(hdr + BMP_DATASIZE);
		if (fsize && (fsize != w * h)) comp = 1;
	}
	/* Only 1, 4, 8, 16, 24 and 32 bpp allowed */
	rle = comp;
	switch (bpp)
	{
	case 1: if (comp) goto fail; /* No compression */
		break;
	case 4: if (comp && (comp != 2)) goto fail; /* RLE4 */
		break;
	case 8: if (comp && (comp != 1)) goto fail; /* RLE8 */
		break;
	case 24: if (comp == 4) /* RLE24 or JPEG */
		{
			/* If not definitely OS/2 header, consider it JPEG */
			if ((l != OS2BMP2_HSIZE) && (l != OS2BMP2_HSIZE_S)) goto fail;
			break;
		}
		// Fallthrough
	case 16: case 32:
		rle = 0;
		if (comp && (comp != 3)) goto fail; /* Bitfields */
		shifts[3] = bpps[3] = masks[3] = 0; /* No alpha by default */
		if (comp == 3)
		{
			/* V3-style bitfields? */
			if ((l == BMP3_HSIZE) && (ofs >= BMP_AMASK)) l = BMP_AMASK;
			if (l < BMP_AMASK) goto fail;
			masks[0] = GET32(hdr + BMP_RMASK);
			masks[1] = GET32(hdr + BMP_GMASK);
			masks[2] = GET32(hdr + BMP_BMASK);
			if (l >= BMP_AMASK + 4)
				masks[3] = GET32(hdr + BMP_AMASK);
			if (masks[3]) cmask = CMASK_RGBA;

			/* Convert masks into bit lengths and offsets */
			for (i = 0; i < 4; i++)
			{
				/* Bit length - just count bits */
				j = bitcount(masks[i]);
				/* Bit offset - add in bits _before_ mask */
				k = bitcount(masks[i] - 1) + 1;
				if (j > 8) j = 8;
				shifts[i] = k - j;
				bpps[i] = j;
			}
		}
		else if (bpp == 16)
		{
			shifts[0] = 10;
			shifts[1] = 5;
			shifts[2] = 0;
			bpps[0] = bpps[1] = bpps[2] = 5;
		}
		else
		{
			shifts[0] = 16;
			shifts[1] = 8;
			shifts[2] = 0;
			bpps[0] = bpps[1] = bpps[2] = 8;
			if (bpp == 32) /* Consider alpha present by default */
			{
				shifts[3] = 24;
				bpps[3] = 8;
				cmask = CMASK_RGBA;
				def_alpha = TRUE; /* Uncertain if alpha */
			}
		}
		break;
	default: goto fail;
	}
	if (rle && (h < 0)) goto fail; // Forbidden

	/* Load palette if needed */
	if (bpp < 16)
	{
		unsigned char tbuf[1024];
		unsigned n, j = 0;

		if (l >= BMP_COLORS + 4) j = GET32(hdr + BMP_COLORS);
		if (!j) j = 1 << bpp;

		n = ofs - l - ba;
		k = l < BMP2_HSIZE ? 3 : 4;
		/* !!! Some OS/2 2.x BMPs have 3-bpc palettes too */
		if ((l == OS2BMP2_HSIZE) && (n < j * 4) && (n >= j * 3)) k = 3;
		n /= k;
		if (n < j) j = n;
		if (!j) goto fail; /* Wrong palette size */
		if (j > 256) j = 256; /* Let overlarge palette be */
		settings->colors = j;
		mfseek(mf, l + ba, SEEK_SET);
		i = mfread(tbuf, 1, j * k, mf);
		if (i < j * k) goto fail; /* Cannot read palette */
		tmp = tbuf;
		for (i = 0; i < j; i++)
		{
			settings->pal[i].red = tmp[2];
			settings->pal[i].green = tmp[1];
			settings->pal[i].blue = tmp[0];
			tmp += k;
		}
		/* If palette is all we need */
		res = 1;
		if ((settings->mode == FS_PALETTE_LOAD) ||
			(settings->mode == FS_PALETTE_DEF)) goto fail;
	}
	res = -1;

	/* Allocate buffer and image */
	settings->width = w;
	settings->height = abs(h);
	settings->bpp = wbpp = bpp < 16 ? 1 : 3;
	rl = ((w * bpp + 31) >> 3) & ~3; /* Row data length */
	bl = rl; /* By default, only one row at a time */
	/* For RLE, load all compressed data at once */
	if (rle)
	{
		unsigned fsize = GET32(hdr + BMP_DATASIZE);
		if (fsize > INT_MAX - 1) goto fail;
		bl = fsize;
	}
	/* Sanity check */
	if (bl <= 0) goto fail;
	/* To accommodate bitparser's extra step */
	buf = malloc(bl + 1);
	res = FILE_MEM_ERROR;
	if (!buf) goto fail;
	if ((res = allocate_image(settings, cmask))) goto fail2;

#ifdef U_LCMS
	/* V5 can have embedded ICC profile */
	while (!settings->icc_size && (l == BMP5_HSIZE) &&
		(GET32(hdr + BMP_CSPACE) == BMPCS_EMBED))
	{
		unsigned char *icc = NULL;
		unsigned n, size = GET32(hdr + BMP_ICCSIZE), ofs = GET32(hdr + BMP_ICCOFS);

		if (!size || (size > INT_MAX)) break; // Avoid the totally crazy
		if (ofs > F_LONG_MAX - BMP_HDR2SIZE) break; // Too far
		if (mfseek(mf, ofs + BMP_HDR2SIZE, SEEK_SET)) break; // Cannot go there
		icc = malloc(size);
		if (!icc) break;
		n = mfread(icc, 1, size, mf);
		if (n != size) free(icc); // Failed
		else settings->icc_size = size , settings->icc = icc; // Got it
		break;
	}
#endif

	if (!settings->silent) ls_init("BMP", 0);

	mfseek(mf, ofs, SEEK_SET); /* Seek to data */
	if (h < 0) /* Prepare row loop */
	{
		step = 1;
		i = 0;
		h = -h;
	}
	else
	{
		step = -1;
		i = h - 1;
	}
	res = FILE_LIB_ERROR;

	if (!rle) /* No RLE */
	{
		for (n = 0; (i < h) && (i >= 0); n++ , i += step)
		{
			j = mfread(buf, 1, rl, mf);
			if (j < rl) goto fail3;
			dest = settings->img[CHN_IMAGE] + w * i * wbpp;
			if (bpp < 16) /* Indexed */
				stream_MSB(buf, dest, w, bpp, 0, bpp, 1);
			else /* RGB */
			{
				stream_LSB(buf, dest + 0, w, bpps[0],
					shifts[0], bpp, 3);
				stream_LSB(buf, dest + 1, w, bpps[1],
					shifts[1], bpp, 3);
				stream_LSB(buf, dest + 2, w, bpps[2],
					shifts[2], bpp, 3);
				if (settings->img[CHN_ALPHA])
					stream_LSB(buf, settings->img[CHN_ALPHA] +
						w * i, w, bpps[3], shifts[3], bpp, 1);
			}
			ls_progress(settings, n, 10);
		}

		/* Rescale shorter-than-byte RGBA components */
		if (bpp > 8)
		for (i = 0; i < 4; i++)
		{
			if (bpps[i] >= 8) continue;
			k = 3;
			if (i == 3)
			{
				tmp = settings->img[CHN_ALPHA];
				if (!tmp) continue;
				k = 1;
			}
			else tmp = settings->img[CHN_IMAGE] + i;
			set_xlate(xlat, bpps[i] + !bpps[i]); // Let 0-wide fields be
			n = w * h;
			for (j = 0; j < n; j++ , tmp += k) *tmp = xlat[*tmp];
		}

		res = 1;
	}
	else /* RLE - always bottom-up */
	{
		k = mfread(buf, 1, bl, mf);
		if (k < bl) goto fail3;
		memset(settings->img[CHN_IMAGE], 0, w * h * wbpp);
		skip = j = 0;

		dest = settings->img[CHN_IMAGE] + w * i * wbpp;
		for (tmp = buf; tmp - buf + 1 < k; )
		{
			/* Don't fail on out-of-bounds writes */
			if (*tmp) /* Fill block */
			{
				dx = n = *tmp;
				if (j + n > w) dx = j > w ? 0 : w - j;
				if (bpp == 24) /* 24-bit */
				{
					copy_run(dest + j * 3, tmp + 1, dx, 3, 0, TRUE);
					j += n; tmp += 4;
					continue;
				}
				if (bpp == 8) /* 8-bit */
				{
					memset(dest + j, tmp[1], dx);
					j += n; tmp += 2;
					continue;
				}
				for (ii = 0; ii < dx; ii++) /* 4-bit */
				{
					dest[j++] = tmp[1] >> 4;
					if (++ii >= dx) break;
					dest[j++] = tmp[1] & 0xF;
				}
				j += n - dx;
				tmp += 2;
				continue;
			}
			if (tmp[1] > 2) /* Copy block */
			{
				dx = n = tmp[1];
				if (j + n > w) dx = j > w ? 0 : w - j;
				tmp += 2;
				if (bpp == 24)
				{
					copy_run(dest + j * 3, tmp, dx, 3, 3, TRUE);
					j += n; tmp += (n * 3 + 1) & ~1;
					continue;
				}
				if (bpp == 8) /* 8-bit */
				{
					memcpy(dest + j, tmp, dx);
					j += n; tmp += (n + 1) & ~1;
					continue;
				}
				for (ii = 0; ii < dx; ii++) /* 4-bit */
				{
					dest[j++] = *tmp >> 4;
					if (++ii >= dx) break;
					dest[j++] = *tmp++ & 0xF;
				}
				j += n - dx;
				tmp += (((n + 3) & ~3) - (dx & ~1)) >> 1;
				continue;
			}
			if (tmp[1] == 2) /* Skip block */
			{
				dx = tmp[2] + j;
				dy = tmp[3];
				if (dx > w) goto fail3;
				if (dy > i) dx = 0 , dy = i + 1; // To the end
			}
			else /* End-of-something block */
			{
				dx = 0;
				dy = tmp[1] ? i + 1 : 1;
			}
			/* Transparency detected first time? */
			if (!skip && ((dy != 1) || dx || (j < w)))
			{
				if ((res = allocate_image(settings, CMASK_ALPHA)))
					goto fail3;
				res = FILE_LIB_ERROR;
				skip = 1;
				if (settings->img[CHN_ALPHA]) /* Got alpha */
				{
					memset(settings->img[CHN_ALPHA], 255, w * h);
					skip = 2;
				}
			}
			/* Row skip */
			for (ii = 0; ii < dy; ii++ , i--)
			{
				if ((skip > 1) && (j < w))
					memset(settings->img[CHN_ALPHA] + w * i + j,
						0, w - j);
				j = 0;
				ls_progress(settings, h - i - 1, 10);
			}
			/* Column skip */
			if (skip > 1) memset(settings->img[CHN_ALPHA] +
				w * i + j, 0, dx - j);
			j = dx;
			/* No more rows left */
			if (i < 0)
			{
				res = 1;
				break;
			}
			dest = settings->img[CHN_IMAGE] + w * i * wbpp;
			tmp += 2 + tmp[1];
		}
	}

	/* Delete all-zero "alpha" */
	if (def_alpha) delete_alpha(settings, 0);

fail3:	if (!settings->silent) progress_end();
fail2:	free(buf);
fail:	if (fp) fclose(fp);
	return (res);
}

/* Use BMP4 instead of BMP3 for images with alpha */
/* #define USE_BMP4 */ /* Most programs just use 32-bit RGB BMP3 for RGBA */

static int save_bmp(char *file_name, ls_settings *settings, memFILE *mf)
{
	unsigned char *buf, *tmp;
	memFILE fake_mf;
	FILE *fp = NULL;
	int i, j, ll, hsz0, hsz, dsz, fsz;
	int w = settings->width, h = settings->height, bpp = settings->bpp;

	i = w > BMP_MAXHSIZE / 4 ? w * 4 : BMP_MAXHSIZE;
	buf = malloc(i);
	if (!buf) return (-1);
	memset(buf, 0, i);

	if (!mf)
	{
		if (!(fp = fopen(file_name, "wb")))
		{
			free(buf);
			return (-1);
		}
		memset(mf = &fake_mf, 0, sizeof(fake_mf));
		fake_mf.file = fp;
	}

	/* Sizes of BMP parts */
	if (((settings->mode == FS_CLIPBOARD) || (bpp == 3)) &&
		settings->img[CHN_ALPHA]) bpp = 4;
	ll = (bpp * w + 3) & ~3;
	j = bpp == 1 ? settings->colors : 0;

#ifdef USE_BMP4
	hsz0 = bpp == 4 ? BMP4_HSIZE : BMP3_HSIZE;
#else
	hsz0 = BMP3_HSIZE;
#endif
	hsz = hsz0 + j * 4;
	dsz = ll * h;
	fsz = hsz + dsz;

	/* Prepare header */
	buf[0] = 'B'; buf[1] = 'M';
	PUT32(buf + BMP_FILESIZE, fsz);
	PUT32(buf + BMP_DATAOFS, hsz);
	i = hsz0 - BMP_HDR2SIZE;
	PUT32(buf + BMP_HDR2SIZE, i);
	PUT32(buf + BMP_WIDTH, w);
	PUT32(buf + BMP_HEIGHT, h);
	PUT16(buf + BMP_PLANES, 1);
	PUT16(buf + BMP_BPP, bpp * 8);
#ifdef USE_BMP4
	i = bpp == 4 ? 3 : 0; /* Bitfield "compression" / no compression */
	PUT32(buf + BMP_COMPRESS, i);
#else
	PUT32(buf + BMP_COMPRESS, 0); /* No compression */
#endif
	PUT32(buf + BMP_DATASIZE, dsz);
	PUT32(buf + BMP_COLORS, j);
	PUT32(buf + BMP_ICOLORS, j);
#ifdef USE_BMP4
	if (bpp == 4)
	{
		memset(buf + BMP_RMASK, 0, BMP4_HSIZE - BMP_RMASK);
		buf[BMP_RMASK + 2] = buf[BMP_GMASK + 1] = buf[BMP_BMASK + 0] =
			buf[BMP_AMASK + 3] = 0xFF; /* Masks for 8-bit BGRA */
		buf[BMP_CSPACE] = 1; /* Device-dependent RGB */
	}
#endif
	tmp = buf + hsz0;
	for (i = 0; i < j; i++ , tmp += 4)
	{
		tmp[0] = settings->pal[i].blue;
		tmp[1] = settings->pal[i].green;
		tmp[2] = settings->pal[i].red;
	}
	mfwrite(buf, 1, tmp - buf, mf);

	/* Write rows */
	if (!settings->silent) ls_init("BMP", 1);
	memset(buf + ll - 4, 0, 4);
	for (i = h - 1; i >= 0; i--)
	{
		prepare_row(buf, settings, bpp, i);
		mfwrite(buf, 1, ll, mf);
		ls_progress(settings, h - i, 20);
	}
	if (fp) fclose(fp);

	if (!settings->silent) progress_end();

	free(buf);
	return 0;
}

/* Partial ctype implementation for C locale;
 * space 1, digit 2, alpha 4, punctuation 8 */
static unsigned char ctypes[256] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	1, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 8, 8, 8, 8, 8, 8,
	8, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
	4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 8, 8, 8, 8, 4,
	8, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
	4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 8, 8, 8, 8, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
#define ISSPACE(x) (ctypes[(unsigned char)(x)] & 1)
#define ISALPHA(x) (ctypes[(unsigned char)(x)] & 4)
#define ISALNUM(x) (ctypes[(unsigned char)(x)] & 6)
#define ISCNTRL(x) (!ctypes[(unsigned char)(x)])
#define WHITESPACE "\t\n\v\f\r "

typedef struct {
	FILE *fp;
	char *buf, *ptr;
	int size, str, nl;
} cctx;

static void fsetC(cctx *ctx, FILE *fp, char *buf, int len)
{
	if (fp) /* Full init */
	{
		ctx->fp = fp;
		ctx->buf = ctx->ptr = buf;
		ctx->buf[0] = '\0';
		ctx->size = len;
		ctx->str = 0;
		ctx->nl = 1; // Not middle of line
	}
	else /* Switch buffers to a LARGER one */
	{
		int l = strlen(ctx->ptr);
		memmove(buf, ctx->ptr, l);
		buf[l] = '\0';
		ctx->buf = ctx->ptr = buf;
		ctx->size = len;
	}
}

/* Reads text and cuts out C-style comments */
static char *fgetsC(cctx *ctx)
{
	char *buf = ctx->buf;
	int i, l = 0, has_chars = 0, in_comment = 0, in_string = ctx->str;

	/* Keep unparsed tail if the line isn't yet terminated */
	if (!ctx->nl && ((l = strlen(ctx->ptr))))
	{
		memmove(buf, ctx->ptr, l);
		for (i = 0; i < l; i++) if (!ISSPACE(buf[i])) has_chars++;
	}
	ctx->ptr = buf;
	buf[l] = '\0';

	while (TRUE)
	{
		/* Read a line */
		if (!fgets(buf + l, ctx->size - l, ctx->fp)) return (NULL);

		/* Scan it for comments */
		i = l;
		l += strlen(buf + l);
		ctx->nl = l && (buf[l - 1] == '\n'); // Remember line termination
		for (; i < l; i++)
		{
			if (in_string)
			{
				/* Ignore backslash before quote, as libXpm does */
				if (buf[i] == '"') in_string = 0; /* Close a string */
				continue;
			}
			if (in_comment)
			{
				if ((buf[i] == '/') && i && (buf[i - 1] == '*'))
				{
					/* Replace comment by a single space */
					buf[in_comment - 1] = ' ';
					memcpy(buf + in_comment, buf + i + 1, l - i);
					l = in_comment + l - i - 1;
					i = in_comment - 1;
					in_comment = 0;
				}
				continue;
			}
			if (!ISSPACE(buf[i])) has_chars++;
			if (buf[i] == '"')
				in_string = 1; /* Open a string */
			else if ((buf[i] == '*') && i && (buf[i - 1] == '/'))
			{
				/* Open a comment */
				in_comment = i;
				has_chars -= 2;
			}
		}
		/* Fail unterminated strings, forbid continuations for simplicity */
		if (in_string && ctx->nl) return (NULL);

		/* Whitespace ending in unclosed comment - reduce it */
		if ((in_comment > 1) && !has_chars)
		{
			buf[0] = '/';
			buf[1] = '*';
			in_comment = 1;
		}

		/* Fail overlong lines ending in unclosed comment */
		if (in_comment >= ctx->size - 3) return (NULL);

		/* Continue reading till the comment closes */
		if (in_comment)
		{
			l = in_comment + 1;
			continue;
		}

		/* All line is whitespace - continue reading */
		if (!has_chars)
		{
			l = !l || ctx->nl ? 0 : 1; // Remember 1 leading space
			continue;
		}
		
		/* Remember if last string hasn't ended */
		ctx->str = in_string;
		return (buf);
	}
}

/* Gets next C string out of buffer */
static char *fstrC(cctx *ctx)
{
	char *s, *t, *w = ctx->ptr;

	/* String has to begin somewhere */
	while (!(s = strchr(w, '"')))
	{
		*(ctx->ptr = ctx->buf) = '\0';
		if (!(w = fgetsC(ctx))) return (NULL);
	}
	/* And to end on the same line */
	t = strchr(s + 1, '"');
	if (!t)
	{
		ctx->ptr = s;
		if (!(s = fgetsC(ctx))) return (NULL);
		t = strchr(s + 1, '"');
	}
	if (!t) return (NULL); // Overlong string
	/* Cut off the remainder */
	if (*++t)
	{
		*t++ = '\0'; // Ignoring whatever it was
		while (ISSPACE(*t)) t++;
	}
	/* Remember the tail */
	ctx->ptr = t;

	return (s);
}

/* Gets next C line out of buffer */
static char *flineC(cctx *ctx)
{
	/* Skip to end of this line */
	while (!ctx->nl)
	{
		*(ctx->ptr = ctx->buf) = '\0';
		if (!fgetsC(ctx)) return (NULL);
	}
	/* And read the next */
	return (fgetsC(ctx));
}

/* "One at a time" hash function */
static guint32 hashf(guint32 seed, char *key, int len)
{
	int i;

	for (i = 0; i < len; i++)
	{
		seed += key[i];
		seed += seed << 10;
		seed ^= seed >> 6;
	}
	seed += seed << 3;
	seed ^= seed >> 11;
	seed += seed << 15;
	return (seed);
} 

#define HASHSEED 0x811C9DC5
#define HASH_RND(X) ((X) * 0x10450405 + 1)
#define HSIZE 16384
#define HMASK 0x1FFF
/* For cuckoo hashing of 4096 items into 16384 slots */
#define MAXLOOP 39

/* This is the limit from libXPM */
#define XPM_MAXCOL 4096

/* Cuckoo hash of IDs for load or RGB triples for save */
typedef struct {
	short hash[HSIZE];
	char *keys;
	int step, cpp, cnt;
	guint32 seed;
} str_hash;

static int ch_find(str_hash *cuckoo, char *str)
{
	guint32 key;
	int k, idx, step = cuckoo->step, cpp = cuckoo->cpp;

	key = hashf(cuckoo->seed, str, cpp);
	k = (key & HMASK) * 2;
	while (TRUE)
	{
		idx = cuckoo->hash[k];
		if (idx && !memcmp(cuckoo->keys + (idx - 1) * step, str, cpp))
			return (idx);
		if (k & 1) return (0); /* Not found */
		k = ((key >> 16) & HMASK) * 2 + 1;
	}
}

static int ch_insert(str_hash *cuckoo, char *str)
{
	char *p, *keys;
	guint32 key;
	int i, j, k, n, idx, step, cpp;

	n = ch_find(cuckoo, str);
	if (n) return (n - 1);

	keys = cuckoo->keys;
	step = cuckoo->step; cpp = cuckoo->cpp;
	if (cuckoo->cnt >= XPM_MAXCOL) return (-1);
	p = keys + cuckoo->cnt++ * step;
	memcpy(p, str, cpp); p[cpp] = 0;

	for (n = cuckoo->cnt; n <= cuckoo->cnt; n++)
	{	
		idx = n;
		/* Normal cuckoo process */
		for (i = 0; i < MAXLOOP; i++)
		{
			key = hashf(cuckoo->seed, keys + (idx - 1) * step, cpp);
			key >>= (i & 1) << 4;
			j = (key & HMASK) * 2 + (i & 1);
			k = cuckoo->hash[j];
			cuckoo->hash[j] = idx;
			idx = k;
			if (!idx) break;
		}
		if (!idx) continue;
		/* Failed insertion - mutate seed */
		cuckoo->seed = HASH_RND(cuckoo->seed);
		memset(cuckoo->hash, 0, sizeof(short) * HSIZE);
		n = 1; /* Rehash everything */
	}
	return (cuckoo->cnt - 1);
}

#define XPM_COL_DEFS 5

#define BUCKET_SIZE 8

/* Comments are allowed where valid */
static int load_xpm(char *file_name, ls_settings *settings)
{
	static const char *cmodes[XPM_COL_DEFS] =
		{ "c", "g", "g4", "m", "s" };
	unsigned char *cbuf, *src, *dest, pal[XPM_MAXCOL * 3], *dst0 = pal;
	guint32 *slots;
	char lbuf[4096], *buf = lbuf, *bh = NULL;
	char ckeys[XPM_MAXCOL * 32], *cdefs[XPM_COL_DEFS], *r, *r2, *t;
	str_hash cuckoo;
	cctx ctx;
	FILE *fp;
	int uninit_(n), uninit_(nx), uninit_(nslots);
	int w, h, cols, cpp, hx, hy, res = -1, bpp = 1, trans = -1;
	int i, j, k, l, lsz = sizeof(lbuf), step = 3, pr = FALSE;


	if (!(fp = fopen(file_name, "r"))) return (-1);

	/* Read the header - accept XPM3 and nothing else */
	j = 0; fscanf(fp, " /* XPM */%n", &j);
	if (!j) goto fail;
	fsetC(&ctx, fp, lbuf, sizeof(lbuf)); /* Init reader */

	/* Skip right to the first string like libXpm does */
	if (!(r = fstrC(&ctx))) goto fail;

	/* Read the values section */
	i = sscanf(r + 1, "%d%d%d%d%d%d", &w, &h, &cols, &cpp, &hx, &hy);
	if (i == 4) hx = hy = -1;
	else if (i != 6) goto fail;
	/* Extension marker is ignored, as are extensions themselves */

	/* More than 16M colors or no colors at all aren't accepted */
	if ((cols < 1) || (cols > 0x1000000)) goto fail;
	/* Stupid chars per pixel values aren't either */
	if ((cpp < 1) || (cpp > 31)) goto fail;
	/* More than 4096 colors accepted only if 4 cpp or less */
	if ((cols > XPM_MAXCOL) && (cpp > 4)) goto fail;

	/* RGB image if more than 256 colors */
	if (cols > 256) bpp = 3;

	/* Store values */
	settings->width = w;
	settings->height = h;
	settings->bpp = bpp;
	if (bpp == 1) settings->colors = cols;
	settings->hot_x = hx;
	settings->hot_y = hy;
	settings->xpm_trans = -1;

	/* Allocate things early, to avoid reading huge color table and THEN
	 * failing for bad dimensions of / lack of memory for the image itself */
	if ((settings->mode != FS_PALETTE_LOAD) && (settings->mode != FS_PALETTE_DEF))
	{
		if ((res = allocate_image(settings, CMASK_IMAGE))) goto fail;
		/* Allocate row buffer */
		i = w * cpp + 4 + 1024;
		if (i > lsz) buf = malloc(lsz = i);
		res = FILE_MEM_ERROR;
		if (!buf) goto fail;
		fsetC(&ctx, NULL, buf, lsz); /* Switch buffers */

		/* Init bucketed hash */
		if (cols > XPM_MAXCOL)
		{
			nslots = (cols + BUCKET_SIZE - 1) / BUCKET_SIZE;
			bh = multialloc(MA_ALIGN_DEFAULT,
				&slots, (nslots + 1) * sizeof(*slots),
				&cbuf, cols * (4 + 3), NULL);
			if (!bh) goto fail2;

			dst0 = cbuf + 4;
			step = 4 + 3;
		}

		if ((pr = !settings->silent))
		{
			ls_init("XPM", 0);
			progress_update(0.0);
			/* A color seems to cost like about 2 pixels to load */
			n = (cols * 2) / w;
			nx = n + h;
		}
	}
	/* Too many colors do not a palette make */
	else if (bpp > 1) goto fail;

	/* Init cuckoo hash */
	if (!bh)
	{
		memset(&cuckoo, 0, sizeof(cuckoo));
		cuckoo.keys = ckeys;
		cuckoo.step = 32;
		cuckoo.cpp = cpp;
		cuckoo.seed = HASHSEED;
	}

	/* Read colormap */
	res = -1;
	for (i = 0 , dest = dst0; i < cols; i++ , dest += step)
	{
		if (!(r = fstrC(&ctx))) goto fail3;
		l = strlen(r);
		if (l < cpp + 4) goto fail3;
		r[l - 1] = '\0'; // Cut off closing quote

		/* Insert color into hash */
		if (bh) strncpy(dest - 4, r + 1, 4);
		else ch_insert(&cuckoo, r + 1);

		/* Parse color definitions */
		memset(cdefs, 0, sizeof(cdefs));
		r += cpp + 2;
		k = -1; r2 = NULL;
		while (TRUE)
		{
			while (ISSPACE(*r)) r++;
			if (!*r) break;
			t = r++;
			while (*r && !ISSPACE(*r)) r++;
			if (*r) *r++ = '\0';
			if (k < 0) /* Mode */
			{
				for (j = 0; j < XPM_COL_DEFS; j++)
				{
					if (!strcmp(t, cmodes[j])) break;
				}
				if (j < XPM_COL_DEFS) /* Key */
				{
					k = j; r2 = NULL;
					continue;
				}
			}
			if (!r2) /* Color name */
			{
				if (k < 0) goto fail3;
				cdefs[k] = r2 = t;
				k = -1;
			}
			else /* Add next part of name */
			{
				l = strlen(r2);
				r2[l] = ' ';
				memmove(r2 + l + 1, t, strlen(t) + 1);
			}
		}
		if (!r2) goto fail3; /* Key w/o name */

		/* Translate the best one */
		for (j = 0; j < XPM_COL_DEFS; j++)
		{
			int c;

			if (!cdefs[j]) continue;
			if (!strcasecmp(cdefs[j], "none")) /* Transparent */
			{
				trans = i;
				break;
			}
			c = parse_color(cdefs[j]);
			if (c < 0) continue;
			dest[0] = INT_2_R(c);
			dest[1] = INT_2_G(c);
			dest[2] = INT_2_B(c);
			break;
		}
		/* Not one understandable color */
		if (j >= XPM_COL_DEFS) goto fail3;
	}
	/* With bucketed hashing, half the work per color is not done yet */
	if (pr) progress_update((float)(bh ? n / 2 : n) / nx);

	/* Create palette */
	if (bpp == 1)
	{
		dest = dst0;
		for (i = 0; i < cols; i++ , dest += step)
		{
			settings->pal[i].red = dest[0];
			settings->pal[i].green = dest[1];
			settings->pal[i].blue = dest[2];
		}
		if (trans >= 0)
		{
			settings->xpm_trans = trans;
			settings->pal[trans].red = settings->pal[trans].green = 115;
			settings->pal[trans].blue = 0;
		}
		/* If palette is all we need */
		res = 1;
		if ((settings->mode == FS_PALETTE_LOAD) ||
			(settings->mode == FS_PALETTE_DEF)) goto fail3;
	}

	/* Find an unused color for transparency */
	else if (trans >= 0)
	{
		unsigned char cmap[XPM_MAXCOL / 8], *cc = cmap;
		int l = XPM_MAXCOL;

		if (bh) // Allocate color cube
		{
			l = 0x1000000;
			cc = malloc(l / 8);
			res = FILE_MEM_ERROR;
			if (!cc) goto fail3;
		}
		memset(cc, 0, l / 8);

		for (i = 0 , dest = dst0; i < cols; i++ , dest += step)
		{
			if (i == trans) continue;
			j = MEM_2_INT(dest, 0);
			if (j < l) cc[j >> 3] |= 1 << (j & 7);
		}
		/* Unused color IS there, as buffer has bits per total colors;
		 * if one is transparent, then a bit stays unset in there */
		dest = cc;
		while (*dest == 0xFF) dest++;
		j = (dest - cc) * 8;
		i = *dest;
		while (i & 1) j++ , i >>= 1;

		if (bh) free(cc);

		/* Store the result */	
		settings->rgb_trans = j;
		dest = dst0 + trans * step;
		dest[0] = INT_2_R(j);
		dest[1] = INT_2_G(j);
		dest[2] = INT_2_B(j);
	}

	if (bh) /* Sort the colors by their buckets */
	{
		unsigned char *w, *ww, tbuf[4 + 3];
		int ds, ins;

		/* First, count how many goes where */
		for (i = 0 , dest = cbuf; i < cols; i++ , dest += 4 + 3)
			(slots + 1)[hashf(HASHSEED, dest, cpp) % nslots]++;
		/* Then, prepare buckets' offsets */
		for (i = 0; i < nslots; i++) slots[i + 1] += slots[i];
		/* Now, starting from first, move colors where they belong */
		for (i = 0; i < cols; )
		{
			/* Color */
			w = cbuf + i * (4 + 3);
			/* Its home */
			ds = hashf(HASHSEED, w, cpp) % nslots;
			/* Its insertion point */
			ins = --slots[ds + 1];
			/* Already in place */
			if (ins <= i)
			{
				slots[ds + 1] = ++i; // Adjust
				continue;
			}
			/* Move it */
			ww = cbuf + ins * (4 + 3);
			memcpy(tbuf, ww, 4 + 3);
			memcpy(ww, w, 4 + 3);
			memcpy(w, tbuf, 4 + 3);
		}
		if (pr) progress_update((float)n / nx);
	}

	/* Now, read the image */
	res = FILE_LIB_ERROR;
	dest = settings->img[CHN_IMAGE];
	for (i = 0; i < h; i++)
	{
		if (!(r = fstrC(&ctx))) goto fail3;
		/* libXpm allows overlong strings */
		if (strlen(++r) < w * cpp + 1) goto fail3;
		for (j = 0; j < w; j++ , dest += bpp)
		{
			if (bh)
			{
				unsigned char *w;
				int ds, n;

				ds = hashf(HASHSEED, r, cpp) % nslots;
				k = slots[ds];
				w = cbuf + k * (4 + 3);
				n = slots[ds + 1];
				for (; k < n; k++ , w += 4 + 3)
					/* Trying to avoid function call */
					if ((*w == *r) && !memcmp(w, r, cpp)) break;
				k = k >= n ? 0 : k + 1;
			}
			else k = ch_find(&cuckoo, r);
			if (!k) goto fail3;
			r += cpp;
			if (bpp == 1) *dest = k - 1;
			else
			{
				src = dst0 + (k - 1) * step;
				dest[0] = src[0];
				dest[1] = src[1];
				dest[2] = src[2];
			}
		}
		if (pr && ((n++ * 10) % nx >= nx - 10))
			progress_update((float)n / nx);
	}
	res = 1;

fail3:	if (pr) progress_end();
	free(bh);
fail2:	if (buf != lbuf) free(buf);
fail:	fclose(fp);
	return (res);
}

static const char base64[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
	"!#$%&'()*,-.:;<=>?@[]^_`{|}~",
	hex[] = "0123456789ABCDEF";

/* Extract valid C identifier from filename */
static char *extract_ident(char *fname, int *len)
{
	char *tmp;
	int l;

	tmp = strrchr(fname, DIR_SEP);
	tmp = tmp ? tmp + 1 : fname;
	for (; *tmp && !ISALPHA(*tmp); tmp++);
	for (l = 0; (l < 256) && ISALNUM(tmp[l]); l++);
	*len = l;
	return (tmp);
}

#define CTABLE_SIZE (0x1000000 / 32)
#define CINDEX_SIZE (0x1000000 / 256)

/* Find color index using bitmap with sparse counters */
static int ct_index(int rgb, guint32 *ctable)
{
	guint32 bit = 1U << (rgb & 31), *cindex = ctable + CTABLE_SIZE;
	int n, d = rgb >> 5, m = d & 7;

	if (!(ctable[d] & bit)) return (-1); // Not found
	n = cindex[d >> 3];
	while (m > 0) n += bitcount(ctable[d - m--]);
	return (n + bitcount(ctable[d] & (bit - 1)));
}

static int save_xpm(char *file_name, ls_settings *settings)
{
	unsigned char rgbmem[XPM_MAXCOL * 4], *src;
	guint32 *cindex, *ctable = NULL;
	const char *ctb;
	char ws[5], tc[5] = "    ";
	char *buf, *tmp;
	str_hash cuckoo;
	FILE *fp;
	int bpp = settings->bpp, w = settings->width, h = settings->height;
	int uninit_(ccmask);
	int i, j, k, l, c, cpp, cols, trans = -1;


	tmp = extract_ident(file_name, &l);
	if (!l) return -1;

	/* Collect RGB colors */
	if (bpp == 3)
	{
		trans = settings->rgb_trans;

		/* Init hash */
		memset(&cuckoo, 0, sizeof(cuckoo));
		cuckoo.keys = rgbmem;
		cuckoo.step = 4;
		cuckoo.cpp = 3;
		cuckoo.seed = HASHSEED;

		j = w * h;
		src = settings->img[CHN_IMAGE];
		for (i = 0; i < j; i++ , src += 3)
		{
			if (ch_insert(&cuckoo, src) < 0)
				break; /* Too many colors for this mode */
		}

		if (i < j) /* Too many colors, collect & count 'em */
		{
			ctable = calloc(CTABLE_SIZE + CINDEX_SIZE, sizeof(*ctable));
			if (!ctable) return (-1); // No memory
			src = settings->img[CHN_IMAGE];
			for (i = 0; i < j; i++ , src += 3)
			{
				int n = MEM_2_INT(src, 0);
				ctable[n >> 5] |= 1U << (n & 31);
			}
			cindex = ctable + CTABLE_SIZE;
			for (i = cols = 0; i < CTABLE_SIZE; i++)
			{
				if (!(i & 7)) cindex[i >> 3] = cols;
				cols += bitcount(ctable[i]);
			}
			/* RGB to index */
			if (trans > -1) trans = ct_index(trans, ctable);
		}
		else /* Sensible number of colors, cuckoo hashing works */
		{
			cols = cuckoo.cnt;
			/* RGB to index */
			if (trans > -1)
			{
				char trgb[3];
				trgb[0] = INT_2_R(trans);
				trgb[1] = INT_2_G(trans);
				trgb[2] = INT_2_B(trans);
				trans = ch_find(&cuckoo, trgb) - 1;
			}
		}
	}

	/* Process indexed colors */
	else
	{
		cols = settings->colors;
		src = rgbmem;
		for (i = 0; i < cols; i++ , src += 4)
		{
			src[0] = settings->pal[i].red;
			src[1] = settings->pal[i].green;
			src[2] = settings->pal[i].blue;
		}
		trans = settings->xpm_trans;
	}

	cpp = cols > 92 * 92 * 92 ? 4 : cols > 92 * 92 ? 3 : cols > 92 ? 2 : 1;

	buf = malloc(w * cpp + 16); // Prepare row buffer
	if (!buf || !(fp = fopen(file_name, "w")))
	{
		free(buf);
		free(ctable);
		return -1;
	}

	if (!settings->silent) ls_init("XPM", 1);

	fprintf(fp, "/* XPM */\n" );
	fprintf(fp, "static char *%.*s_xpm[] = {\n", l, tmp);

	if ((settings->hot_x >= 0) && (settings->hot_y >= 0))
		fprintf(fp, "\"%d %d %d %d %d %d\",\n", w, h, cols, cpp,
			settings->hot_x, settings->hot_y);
	else fprintf(fp, "\"%d %d %d %d\",\n", w, h, cols, cpp);

	/* Create colortable */
	ctb = cols > 16 ? base64 : hex;
	tc[cpp] = '\0';
	if (ctable) // From bitmap
	{
		for (i = c = 0; i < CTABLE_SIZE; i++)
		{
			guint32 n = ctable[i];
			for (k = 0; n; k++ , n >>= 1)
			{
				if (!(n & 1)) continue;
				l = i * 32 + k; // Color

				/* Color ID */
				ws[0] = ctb[c % 92];
				ws[1] = ctb[(c / 92) % 92];
				ws[2] = ctb[(c / (92 * 92)) % 92];
				ws[3] = ctb[c / (92 * 92 * 92)];
				ws[cpp] = '\0';

				if (c == trans) // Transparency
					fprintf(fp, "\"%s\tc None\",\n", tc);
				else fprintf(fp, "\"%s\tc #%02X%02X%02X\",\n", ws,
					INT_2_R(l), INT_2_G(l), INT_2_B(l));

				c++;
			}
		}
	}
	else // From cuckoo's keys
	{
		ccmask = 255 >> cpp; // 63 for 2 cpp, 127 for 1
		for (i = 0; i < cols; i++)
		{
			if (i == trans)
			{
				fprintf(fp, "\"%s\tc None\",\n", tc);
				continue;
			}
			ws[0] = ctb[i & ccmask];
			ws[1] = ctb[i >> 6];
			ws[cpp] = '\0';
			src = rgbmem + i * 4;
			fprintf(fp, "\"%s\tc #%02X%02X%02X\",\n", ws,
				src[0], src[1], src[2]);
		}
	}

	w *= bpp;
	for (i = 0; i < h; i++)
	{
		src = settings->img[CHN_IMAGE] + i * w;
		tmp = buf;
		*tmp++ = '"';
		for (j = 0; j < w; j += bpp, tmp += cpp)
		{
			if (bpp == 1) k = src[j];
			else if (!ctable) k = ch_find(&cuckoo, src + j) - 1;
			else k = ct_index(MEM_2_INT(src, j), ctable);
			if (k == trans)
				tmp[0] = tmp[1] = tmp[2] = tmp[3] = ' ';
			else if (ctable)
			{
				tmp[0] = ctb[k % 92];
				tmp[1] = ctb[(k / 92) % 92];
				tmp[2] = ctb[(k / (92 * 92)) % 92];
				tmp[3] = ctb[k / (92 * 92 * 92)];
			}
			else // Backward compatible mapping
			{
				tmp[0] = ctb[k & ccmask];
				tmp[1] = ctb[k >> 6];
			}
		}
		strcpy(tmp, i < h - 1 ? "\",\n" : "\"\n};\n");
		fputs(buf, fp);
		ls_progress(settings, i, 10);
	}
	fclose(fp);

	if (!settings->silent) progress_end();

	free(buf);
	free(ctable);
	return 0;
}

static int load_xbm(char *file_name, ls_settings *settings)
{
	static const char XPMtext[] = "0123456789ABCDEFabcdef,} \t\n",
		XPMval[] = {
			0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
			10, 11, 12, 13, 14, 15, 16, 16, 16, 16, 16 };
	unsigned char ctb[256], *dest;
	char lbuf[4096], *r;
	cctx ctx;
	FILE *fp;
	int w , h, hx = -1, hy = -1, bpn = 16, res = -1;
	int i, j, k, c, v = 0;


	if (!(fp = fopen(file_name, "r"))) return (-1);

	/* Read & parse what serves as header to XBM */
	fsetC(&ctx, fp, lbuf, sizeof(lbuf)); /* Init reader */
	/* Width and height - required part in fixed order */
	if (!(r = flineC(&ctx))) goto fail;
	if (!sscanf(r, "#define %*s%n %d", &i, &w)) goto fail;
	if (strncmp(r + i - 5, "width", 5)) goto fail;
	if (!(r = flineC(&ctx))) goto fail;
	if (!sscanf(r, "#define %*s%n %d", &i, &h)) goto fail;
	if (strncmp(r + i - 6, "height", 6)) goto fail;
	/* Hotspot X and Y - optional part in fixed order */
	if (!(r = flineC(&ctx))) goto fail;
	if (sscanf(r, "#define %*s%n %d", &i, &hx))
	{
		if (strncmp(r + i - 5, "x_hot", 5)) goto fail;
		if (!(r = flineC(&ctx))) goto fail;
		if (!sscanf(r, "#define %*s%n %d", &i, &hy)) goto fail;
		if (strncmp(r + i - 5, "y_hot", 5)) goto fail;
		if (!(r = flineC(&ctx))) goto fail;
	}
	/* "Intro" string */
	j = 0; sscanf(r, " static short %*[^[]%n[] = {%n", &i, &j);
	if (!j)
	{
		bpn = 8; /* X11 format - 8-bit data */
		j = 0; sscanf(r, " static unsigned char %*[^[]%n[] = {%n", &i, &j);
		if (!j) sscanf(r, " static char %*[^[]%n[] = {%n", &i, &j);
		if (!j) goto fail;
	}
	if (strncmp(r + i - 4, "bits", 4)) goto fail;
// !!! For now, newline is required between "intro" and data

	/* Store values */
	settings->width = w;
	settings->height = h;
	settings->bpp = 1;
	settings->hot_x = hx;
	settings->hot_y = hy;
	/* Palette is white and black */
	set_bw(settings);

	/* Allocate image */
	if ((res = allocate_image(settings, CMASK_IMAGE))) goto fail;

	/* Prepare to read data */
	memset(ctb, 17, sizeof(ctb));
	for (i = 0; XPMtext[i]; i++)
	{
		ctb[(unsigned char)XPMtext[i]] = XPMval[i];
	}

	/* Now, read the image */
	if (!settings->silent) ls_init("XBM", 0);
	res = FILE_LIB_ERROR;
	dest = settings->img[CHN_IMAGE];
	for (i = 0; i < h; i++)
	{
		for (j = k = 0; j < w; j++ , k--)
		{
			if (!k) /* Get next value, the way X itself does */
			{
				v = 0;
				while (TRUE)
				{
					if ((c = getc(fp)) == EOF) goto fail2;
					c = ctb[c & 255];
					if (c < 16) /* Accept hex digits */
					{
						v = (v << 4) + c;
						k++;
					}
					/* Silently ignore out-of-place chars */
					else if (c > 16) continue;
					/* Stop on delimiters after digits */
					else if (k) break;
				}
				k = bpn;
			}
			*dest++ = v & 1;
			v >>= 1;
		}	
		ls_progress(settings, i, 10);
	}
	res = 1;

fail2:	if (!settings->silent) progress_end();
fail:	fclose(fp);
	return (res);
}

#define BPL 12 /* Bytes per line */
#define CPB 6  /* Chars per byte */
static int save_xbm(char *file_name, ls_settings *settings)
{
	unsigned char bw, *src;
	unsigned char row[MAX_WIDTH / 8];
	char buf[CPB * BPL + 16], *tmp;
	FILE *fp;
	int i, j, k, l, w = settings->width, h = settings->height;

	if ((settings->bpp != 1) || (settings->colors > 2)) return WRONG_FORMAT;

	/* Extract valid C identifier from name */
	tmp = extract_ident(file_name, &i);
	if (!i) return -1;

	if (!(fp = fopen(file_name, "w"))) return -1;

	fprintf(fp, "#define %.*s_width %i\n", i, tmp, w);
	fprintf(fp, "#define %.*s_height %i\n", i, tmp, h);
	if ((settings->hot_x >= 0) && (settings->hot_y >= 0))
	{
		fprintf(fp, "#define %.*s_x_hot %i\n", i, tmp, settings->hot_x);
		fprintf(fp, "#define %.*s_y_hot %i\n", i, tmp, settings->hot_y);
	}
	fprintf(fp, "static unsigned char %.*s_bits[] = {\n", i, tmp);

	if (!settings->silent) ls_init("XBM", 1);

	bw = get_bw(settings);
	j = k = (w + 7) >> 3; i = l = 0;
	while (TRUE)
	{
		if (j >= k)
		{
			if (i >= h) break;
			src = settings->img[CHN_IMAGE] + i * w;
			memset(row, 0, k);
			for (j = 0; j < w; j++)
			{
				if (src[j] == bw) row[j >> 3] |= 1 << (j & 7);
			}
			j = 0;
			ls_progress(settings, i, 10);
			i++;
		}
		for (; (l < BPL) && (j < k); l++ , j++)
		{
			tmp = buf + l * CPB;
			tmp[0] = ' '; tmp[1] = '0'; tmp[2] = 'x';
			tmp[3] = hex[row[j] >> 4]; tmp[4] = hex[row[j] & 0xF];
			tmp[5] = ',';
		}
		if ((l == BPL) && (j < k))
		{
			buf[BPL * CPB] = '\n'; buf[BPL * CPB + 1] = '\0';
			fputs(buf, fp);
			l = 0;
		}
	}
	strcpy(buf + l * CPB - 1, " };\n");
	fputs(buf, fp);
	fclose(fp);

	if (!settings->silent) progress_end();

	return 0;
}

/*
 * Those who don't understand PCX are condemned to reinvent it, poorly. :-)
 */

#define LSS_WIDTH   4 /* 16b */
#define LSS_HEIGHT  6 /* 16b */
#define LSS_PALETTE 8 /* 16 * 3 * 8b */
#define LSS_HSIZE   56

static int load_lss(char *file_name, ls_settings *settings)
{
	unsigned char hdr[LSS_HSIZE], *dest, *tmp, *buf = NULL;
	FILE *fp;
	f_long l;
	int i, j, k, w, h, bl, idx, last, cnt, res = -1;


	if (!(fp = fopen(file_name, "rb"))) return (-1);

	/* Read the header */
	k = fread(hdr, 1, LSS_HSIZE, fp);

	/* Check general validity */
	if (k < LSS_HSIZE) goto fail; /* Least supported header size */
	if (strncmp(hdr, "\x3D\xF3\x13\x14", 4)) goto fail; /* Signature */

	w = GET16(hdr + LSS_WIDTH);
	h = GET16(hdr + LSS_HEIGHT);
	settings->width = w;
	settings->height = h;
	settings->bpp = 1;
	settings->colors = 16;

	/* Read palette */
	tmp = hdr + LSS_PALETTE;
	for (i = 0; i < 16; i++)
	{
		settings->pal[i].red = tmp[0] << 2 | tmp[0] >> 4;
		settings->pal[i].green = tmp[1] << 2 | tmp[1] >> 4;
		settings->pal[i].blue = tmp[2] << 2 | tmp[2] >> 4;
		tmp += 3;
	}
	/* If palette is all we need */
	res = 1;
	if ((settings->mode == FS_PALETTE_LOAD) ||
		(settings->mode == FS_PALETTE_DEF)) goto fail;

	/* Load all image at once */
	fseek(fp, 0, SEEK_END);
	l = ftell(fp);
	if (l <= LSS_HSIZE) goto fail; /* Too large or too small */
	l -= LSS_HSIZE;
	fseek(fp, LSS_HSIZE, SEEK_SET);
	bl = (w * h * 3) >> 1; /* Cannot possibly be longer */
	if (bl > l) bl = l;
	buf = malloc(bl);
	res = FILE_MEM_ERROR;
	if (!buf) goto fail2;
	if ((res = allocate_image(settings, CMASK_IMAGE))) goto fail2;

	if (!settings->silent) ls_init("LSS16", 0);

	res = FILE_LIB_ERROR;
	j = fread(buf, 1, bl, fp);
	if (j < bl) goto fail3;

	dest = settings->img[CHN_IMAGE];
	idx = 0; bl += bl;
	for (i = 0; i < h; i++)
	{
		last = 0; idx = (idx + 1) & ~1;
		for (j = 0; j < w; )
		{
			if (idx >= bl) goto fail3;
			k = (buf[idx >> 1] >> ((idx & 1) << 2)) & 0xF; ++idx;
			if (k != last)
			{
				dest[j++] = last = k;
				continue;
			}
			if (idx >= bl) goto fail3;
			cnt = (buf[idx >> 1] >> ((idx & 1) << 2)) & 0xF; ++idx;
			if (!cnt)
			{
				if (idx >= bl) goto fail3;
				cnt = (buf[idx >> 1] >> ((idx & 1) << 2)) & 0xF; ++idx;
				if (idx >= bl) goto fail3;
				k = (buf[idx >> 1] >> ((idx & 1) << 2)) & 0xF; ++idx;
				cnt = (k << 4) + cnt + 16;
			}
			if (cnt > w - j) cnt = w - j;
			memset(dest + j, last, cnt);
			j += cnt;
		}
		dest += w;
	}
	res = 1;

fail3:	if (!settings->silent) progress_end();
fail2:	free(buf);
fail:	fclose(fp);
	return (res);
}

static int save_lss(char *file_name, ls_settings *settings)
{
	unsigned char *buf, *tmp, *src;
	FILE *fp;
	int i, j, uninit_(k), last, cnt, idx;
	int w = settings->width, h = settings->height;


	if ((settings->bpp != 1) || (settings->colors > 16)) return WRONG_FORMAT;

	i = w > LSS_HSIZE ? w : LSS_HSIZE;
	buf = malloc(i);
	if (!buf) return -1;
	memset(buf, 0, i);

	if (!(fp = fopen(file_name, "wb")))
	{
		free(buf);
		return -1;
	}

	/* Prepare header */
	buf[0] = 0x3D; buf[1] = 0xF3; buf[2] = 0x13; buf[3] = 0x14;
	PUT16(buf + LSS_WIDTH, w);
	PUT16(buf + LSS_HEIGHT, h);
	j = settings->colors > 16 ? 16 : settings->colors;
	tmp = buf + LSS_PALETTE;
	for (i = 0; i < j; i++)
	{
		tmp[0] = settings->pal[i].red >> 2;
		tmp[1] = settings->pal[i].green >> 2;
		tmp[2] = settings->pal[i].blue >> 2;
		tmp += 3;
	}
	fwrite(buf, 1, LSS_HSIZE, fp);

	/* Write rows */
	if (!settings->silent) ls_init("LSS16", 1);
	src = settings->img[CHN_IMAGE];
	for (i = 0; i < h; i++)
	{
		memset(buf, 0, w);
		last = cnt = idx = 0;
		for (j = 0; j < w; )
		{
			for (; j < w; j++)
			{
				k = *src++ & 0xF;
				if ((k != last) || (++cnt >= 255 + 16)) break;
			}
			if (cnt)
			{
				buf[idx >> 1] |= last << ((idx & 1) << 2); ++idx;
				if (cnt >= 16)
				{
					++idx; /* Insert zero */
					cnt -= 16;
					buf[idx >> 1] |= (cnt & 0xF) <<
						((idx & 1) << 2); ++idx;
					cnt >>= 4;
				}
				buf[idx >> 1] |= cnt << ((idx & 1) << 2); ++idx;
			}
			if (j++ >= w) break; /* Final repeat */
			cnt = 0;
			if (k == last) continue; /* Chain of repeats */
			buf[idx >> 1] |= k << ((idx & 1) << 2); ++idx;
			last = k;
		}			
		idx = (idx + 1) & ~1;
		fwrite(buf, 1, idx >> 1, fp);
		ls_progress(settings, i, 10);
	}
	fclose(fp);

	if (!settings->silent) progress_end();

	free(buf);
	return 0;
}

/* *** PREFACE ***
 * No other format has suffered so much at the hands of inept coders. With TGA,
 * exceptions are the rule, and files perfectly following the specification are
 * impossible to find. While I did my best to handle the format's perversions
 * that I'm aware of, there surely exist other kinds of weird TGAs that will
 * load wrong, or not at all. If you encounter one such, send a bugreport with
 * the file attached to it. */

/* TGA header */
#define TGA_IDLEN     0 /*  8b */
#define TGA_PALTYPE   1 /*  8b */
#define TGA_IMGTYPE   2 /*  8b */
#define TGA_PALSTART  3 /* 16b */
#define TGA_PALCOUNT  5 /* 16b */
#define TGA_PALBITS   7 /*  8b */
#define TGA_X0        8 /* 16b */
#define TGA_Y0       10 /* 16b */
#define TGA_WIDTH    12 /* 16b */
#define TGA_HEIGHT   14 /* 16b */
#define TGA_BPP      16 /*  8b */
#define TGA_DESC     17 /*  8b */
#define TGA_HSIZE    18

/* Image descriptor bits */
#define TGA_ALPHA 0x0F
#define TGA_R2L   0x10
#define TGA_T2B   0x20
#define TGA_IL    0xC0 /* Interleave mode - obsoleted in TGA 2.0 */

/* TGA footer */
#define TGA_EXTOFS 0 /* 32b */
#define TGA_DEVOFS 4 /* 32b */
#define TGA_SIGN   8
#define TGA_FSIZE  26

/* TGA extension area */
#define TGA_EXTLEN  0   /* 16b */
#define TGA_SOFTID  426 /* 41 bytes */
#define TGA_SOFTV   467 /* 16b */
#define TGA_ATYPE   494 /* 8b */
#define TGA_EXTSIZE 495

static void extend_bytes(unsigned char *dest, int len, int maxval)
{
	unsigned char tb[256];

	memset(tb, 255, 256);
	set_xlate_n(tb, maxval);
	do_xlate(tb, dest, len);
}

static int load_tga(char *file_name, ls_settings *settings)
{
	unsigned char hdr[TGA_HSIZE], ftr[TGA_FSIZE], ext[TGA_EXTSIZE];
	unsigned char pal[256 * 4], xlat5[32], xlat6[64], trans[256];
	unsigned char *buf = NULL, *dest, *dsta, *src = NULL, *srca = NULL;
	unsigned char *bstart, *bstop;
	FILE *fp;
	f_long fl;
	unsigned fofs;
	int i, k, w, h, bpp, ftype, ptype, ibpp, rbits, abits, itrans = FALSE;
	int rle, real_alpha = FALSE, assoc_alpha = FALSE, wmode = 0, res = -1;
	int iofs, buflen;
	int ix, ishift, imask, ax, ashift, amask;
	int start, xstep, xstepb, ystep, ccnt, rcnt, strl, y;


	if (!(fp = fopen(file_name, "rb"))) return (-1);

	/* Read the header */
	k = fread(hdr, 1, TGA_HSIZE, fp);
	if (k < TGA_HSIZE) goto fail;

	/* TGA has no signature as such - so check fields one by one */
	ftype = hdr[TGA_IMGTYPE];
	if (!(ftype & 3) || (ftype & 0xF4)) goto fail; /* Invalid type */
	/* Fail on interleave, because of lack of example files */
	if (hdr[TGA_DESC] & TGA_IL) goto fail;
	rle = ftype & 8;

	iofs = TGA_HSIZE + hdr[TGA_IDLEN];

	rbits = hdr[TGA_BPP];
	if (!rbits) goto fail; /* Zero bpp */
	abits = hdr[TGA_DESC] & TGA_ALPHA;
	if (abits > rbits) goto fail; /* Weird alpha */
	/* Workaround for a rather frequent bug */
	if (abits == rbits) abits = 0;
	ibpp = (rbits + 7) >> 3;
	rbits -= abits;

	set_xlate(xlat5, 5);
	ptype = hdr[TGA_PALTYPE];
	switch (ftype & 3)
	{
	case 1: /* Paletted */
	{
		int pbpp, i, j, k, l;
		png_color *pptr;

		if (ptype != 1) goto fail; /* Invalid palette */
		/* Don't want to bother with overlong palette without even
		 * having one example where such a thing exists - WJ */
		if (rbits > 8) goto fail;

		k = GET16(hdr + TGA_PALSTART);
		if (k >= 1 << rbits) goto fail; /* Weird palette start */
		j = GET16(hdr + TGA_PALCOUNT);
		if (!j || (k + j > 1 << rbits)) goto fail; /* Weird size */
		ptype = hdr[TGA_PALBITS];
		/* The options are quite limited here in practice */
		if (!ptype || (ptype > 32) || ((ptype & 7) && (ptype != 15)))
			goto fail;
		pbpp = (ptype + 7) >> 3;
		l = j * pbpp;

		/* Read the palette */
		fseek(fp, iofs, SEEK_SET);
		if (fread(pal + k * pbpp, 1, l, fp) != l) goto fail;
		iofs += l;

		/* Store the palette */
		settings->colors = j + k;
		memset(settings->pal, 0, 256 * 3);
		pptr = settings->pal + k;
		for (i = 0; i < l; i += pbpp , pptr++)
		{
			switch (pbpp)
			{
			case 1: /* 8-bit greyscale */
				pptr->red = pptr->green = pptr->blue = pal[i];
				break;
			case 2: /* 5:5:5 BGR */
				pptr->blue = xlat5[pal[i] & 0x1F];
				pptr->green = xlat5[(((pal[i + 1] << 8) +
					pal[i]) >> 5) & 0x1F];
				pptr->red = xlat5[(pal[i + 1] >> 2) & 0x1F];
				break;
			case 3: case 4: /* 8:8:8 BGR */
				pptr->blue = pal[i + 0];
				pptr->green = pal[i + 1];
				pptr->red = pal[i + 2];
				break;
			}
		}
		/* If palette is all we need */
		res = 1;
		if ((settings->mode == FS_PALETTE_LOAD) ||
			(settings->mode == FS_PALETTE_DEF)) goto fail;

		/* Assemble transparency table */
		memset(trans, 255, 256);
		if (ptype == 15)
		{
			int i, n, tr;

			for (i = n = 0; i < j; i++) n += pal[i + i + 1] & 0x80;
			/* Assume the less frequent value is transparent */
			tr = n >> 6 < j ? 0x80 : 0;
			for (i = 0; i < j; i++)
			{
				if ((pal[i + i + 1] & 0x80) == tr) trans[i + k] = 0;
			}
		}
		else if (ptype == 32)
		{
			for (i = 0; i < j; i++) trans[i + k] = pal[i * 4 + 3];
		}
		else break; /* Cannot have transparent color at all */

		/* If all alphas are identical, ignore them */
		itrans = !is_filled(trans + k, trans[k], j);
		break;
	}
	case 2: /* RGB */
		/* Options are very limited - and bugs abound. Presence or
		 * absence of attribute bits can't be relied upon. */
		switch (rbits)
		{
		case 16: /* 5:5:5 BGR or 5:6:5 BGR or 5:5:5:1 BGRA */
			if (abits) goto fail;
			if (tga_565)
			{
				set_xlate(xlat6, 6);
				wmode = 4;
				break;
			}
			rbits = 15;
			/* Fallthrough */
		case 15: /* 5:5:5 BGR or 5:5:5:1 BGRA */
			if (abits > 1) goto fail;
			abits = 1; /* Here it's unreliable to uselessness */
			wmode = 2;
			break;
		case 32: /* 8:8:8 BGR or 8:8:8:8 BGRA */
			if (abits) goto fail;
			rbits = 24; abits = 8;
			wmode = 6;
			break;
		case 24: /* 8:8:8 BGR or 8:8:8:8 BGRA */
			if (abits && (abits != 8)) goto fail;
			wmode = 6;
			break;
		default: goto fail;
		}
		break;
	case 3: /* Greyscale */
		/* Not enough examples - easier to handle all possibilities */
		/* Create palette */
		settings->colors = rbits > 8 ? 256 : 1 << rbits;
		mem_bw_pal(settings->pal, 0, settings->colors - 1);
		break;
	}
	/* Prepare for reading bitfields */
	i = abits > 8 ? abits - 8 : 0;
	abits -= i; i += rbits;
	ax = i >> 3;
	ashift = i & 7;
	amask = (1 << abits) - 1;
	i = rbits > 8 ? rbits - 8 : 0;
	rbits -= i;
	ix = i >> 3;
	ishift = i & 7;
	imask = (1 << rbits) - 1;

	/* Now read the footer if one is available */
	fseek(fp, 0, SEEK_END);
	fl = ftell(fp);
	while (fl >= iofs + TGA_FSIZE)
	{
		fseek(fp, fl - TGA_FSIZE, SEEK_SET);
		k = fread(ftr, 1, TGA_FSIZE, fp);
		if (k < TGA_FSIZE) break;
		if (strcmp(ftr + TGA_SIGN, "TRUEVISION-XFILE.")) break;
		fofs = GET32(ftr + TGA_EXTOFS);
		if ((fofs > F_LONG_MAX - TGA_EXTSIZE - TGA_FSIZE) ||
			(fofs < iofs) || (fofs + TGA_EXTSIZE + TGA_FSIZE > fl))
			break; /* Invalid location */
		fseek(fp, fofs, SEEK_SET);
		k = fread(ext, 1, TGA_EXTSIZE, fp);
		if ((k < TGA_EXTSIZE) ||
			/* !!! 3D Studio writes 494 into this field */
			(GET16(ext + TGA_EXTLEN) < TGA_EXTSIZE - 1))
			break; /* Invalid size */
		if ((ftype & 3) != 1) /* Premultiplied alpha? */
			assoc_alpha = ext[TGA_ATYPE] == 4;
		/* Can believe alpha bits contain alpha if this field says so */
		real_alpha |= assoc_alpha | (ext[TGA_ATYPE] == 3);
		break;
	}

	/* Allocate buffer and image */
	settings->width = w = GET16(hdr + TGA_WIDTH);
	settings->height = h = GET16(hdr + TGA_HEIGHT);
	settings->bpp = bpp = (ftype & 3) == 2 ? 3 : 1;
	buflen = ibpp * w;
	if (rle && (w < 129)) buflen = ibpp * 129;
	buf = malloc(buflen + 1); /* One extra byte for bitparser */
	res = FILE_MEM_ERROR;
	if (!buf) goto fail;
	if ((res = allocate_image(settings, abits ? CMASK_RGBA : CMASK_IMAGE)))
		goto fail2;
	/* Don't even try reading alpha if nowhere to store it */
	if (abits && settings->img[CHN_ALPHA]) wmode |= 1;
	res = -1;

	if (!settings->silent) ls_init("TGA", 0);

	fseek(fp, iofs, SEEK_SET); /* Seek to data */
	/* Prepare loops */
	start = 0; xstep = 1; ystep = 0;
	if (hdr[TGA_DESC] & TGA_R2L)
	{
		/* Right-to-left */
		start = w - 1;
		xstep = -1;
		ystep = 2 * w;
	}
	if (!(hdr[TGA_DESC] & TGA_T2B))
	{
		/* Bottom-to-top */
		start += (h - 1) * w;
		ystep -= 2 * w;
	}
	xstepb = xstep * bpp;
	res = FILE_LIB_ERROR;

	dest = settings->img[CHN_IMAGE] + start * bpp;
	dsta = settings->img[CHN_ALPHA] + start;
	y = ccnt = rcnt = 0;
	bstart = bstop = buf + buflen;
	strl = w;
	while (TRUE)
	{
		int j;

		j = bstop - bstart;
		if (j < ibpp)
		{
			if (bstop - buf < buflen) goto fail3; /* Truncated file */
			memcpy(buf, bstart, j);
			j += fread(buf + j, 1, buflen - j, fp);
			bstop = (bstart = buf) + j;
			if (!rle) /* Uncompressed */
			{
				if (j < buflen) goto fail3; /* Truncated file */
				rcnt = w; /* "Copy block" a row long */
			}
		}
		while (TRUE)
		{
			/* Read pixels */
			if (rcnt)
			{
				int l, n;

				l = rcnt < strl ? rcnt : strl;
				if (j < ibpp * l) l = j / ibpp;
				rcnt -= l; strl -= l;
				while (l--)
				{
					switch (wmode)
					{
					case 1: /* Generic alpha */
						*dsta = (((bstart[ax + 1] << 8) +
							bstart[ax]) >> ashift) & amask;
					case 0: /* Generic single channel */
						*dest = (((bstart[ix + 1] << 8) +
							bstart[ix]) >> ishift) & imask;
						break;
					case 3: /* One-bit alpha for 16 bpp */
						*dsta = bstart[1] >> 7;
					case 2: /* 5:5:5 BGR */
						n = (bstart[1] << 8) + bstart[0];
						dest[0] = xlat5[(n >> 10) & 0x1F];
						dest[1] = xlat5[(n >> 5) & 0x1F];
						dest[2] = xlat5[n & 0x1F];
						break;
					case 5: /* Cannot happen */
					case 4: /* 5:6:5 BGR */
						n = (bstart[1] << 8) + bstart[0];
						dest[0] = xlat5[n >> 11];
						dest[1] = xlat6[(n >> 5) & 0x3F];
						dest[2] = xlat5[n & 0x1F];
						break;
					case 7: /* One-byte alpha for 32 bpp */
						*dsta = bstart[3];
					case 6: /* 8:8:8 BGR */
						dest[0] = bstart[2];
						dest[1] = bstart[1];
						dest[2] = bstart[0];
						break;
					}
					dest += xstepb;
					dsta += xstep;
					bstart += ibpp;
				}
				if (!strl || rcnt) break; /* Row end or buffer end */
			}
			/* Copy pixels */
			if (ccnt)
			{
				int i, l;

				l = ccnt < strl ? ccnt : strl;
				ccnt -= l; strl -= l;
				for (i = 0; i < l; i++ , dest += xstepb)
				{
					dest[0] = src[0];
					if (bpp == 1) continue;
					dest[1] = src[1];
					dest[2] = src[2];
				}
				if (wmode & 1) memset(xstep < 0 ?
					dsta - l + 1 : dsta, *srca, l);
				dsta += xstep * l;
				if (!strl || ccnt) break; /* Row end or buffer end */
			}
			/* Read block header */
			j = bstop - bstart - 1;
			if (j < 0) break; /* Nothing in buffer */
			rcnt = *bstart++;
			if (rcnt > 0x7F) /* Repeat block - one read + some copies */
			{
				ccnt = rcnt & 0x7F;
				rcnt = 1;
				src = dest;
				srca = dsta;
			}
			else ++rcnt; /* Copy block - several reads */
		}
		if (strl) continue; /* It was buffer end */
		ls_progress(settings, y, 10);
		if (++y >= h) break; /* All done */
		dest += ystep * bpp;
		if (dsta) dsta += ystep;
		strl = w;
	}

	/* Check if alpha channel is valid */
	if (!real_alpha && settings->img[CHN_ALPHA])
		delete_alpha(settings, settings->img[CHN_ALPHA][0]);

	/* Check if alpha in 16-bpp BGRA is inverse */
	if (settings->img[CHN_ALPHA] && (wmode == 3) && !assoc_alpha)
	{
		unsigned char *timg, *talpha;
		int i, j = w * h, k = 0, l;

		timg = settings->img[CHN_IMAGE];
		talpha = settings->img[CHN_ALPHA];
		for (i = 0; i < j; i++)
		{
			l = 5;
			if (!(timg[0] | timg[1] | timg[2])) l = 1;
			else if ((timg[0] & timg[1] & timg[2]) == 255) l = 4;
			k |= l << talpha[i];
			if (k == 0xF) break; /* Colors independent of alpha */
			timg += 3;
		}
		/* If 0-covered parts more colorful than 1-covered, invert alpha */
		if ((k & 5) > ((k >> 1) & 5))
		{
			for (i = 0; i < j; i++) talpha[i] ^= 1;
		}
	}

	/* Rescale alpha */
	if (settings->img[CHN_ALPHA] && (abits < 8))
		extend_bytes(settings->img[CHN_ALPHA], w * h, (1 << abits) - 1);

	/* Unassociate alpha */
	if (settings->img[CHN_ALPHA] && assoc_alpha && (abits > 1))
	{
		mem_demultiply(settings->img[CHN_IMAGE],
			settings->img[CHN_ALPHA], w * h, bpp);
	}
	res = 0;

	/* Apply palette transparency */
	if (itrans) res = palette_trans(settings, trans);

	if (!res) res = 1;
fail3:	if (!settings->silent) progress_end();
fail2:	free(buf);
fail:	fclose(fp);
	return (res);
}

static int save_tga(char *file_name, ls_settings *settings)
{
	unsigned char hdr[TGA_HSIZE], ftr[TGA_FSIZE], pal[256 * 4];
	unsigned char *buf, *src, *srca, *dest;
	FILE *fp;
	int i, j, y0, y1, vstep, pcn, pbpp = 3;
	int w = settings->width, h = settings->height, bpp = settings->bpp;
	int rle = settings->tga_RLE;

	/* Indexed images not supposed to have alpha in TGA standard */
	if ((bpp == 3) && settings->img[CHN_ALPHA]) bpp = 4;
	i = w * bpp;
	if (rle) i += i + (w >> 7) + 3;
	buf = malloc(i);
	if (!buf) return -1;

	if (!(fp = fopen(file_name, "wb")))
	{
		free(buf);
		return -1;
	}

	/* Prepare header */
	memset(hdr, 0, TGA_HSIZE);
	switch (bpp)
	{
	case 1: /* Indexed */
		hdr[TGA_PALTYPE] = 1;
		hdr[TGA_IMGTYPE] = 1;
		PUT16(hdr + TGA_PALCOUNT, settings->colors);
		if ((settings->xpm_trans >= 0) &&
			(settings->xpm_trans < settings->colors)) pbpp = 4;
		hdr[TGA_PALBITS] = pbpp * 8;
		break;
	case 4: /* RGBA */
		hdr[TGA_DESC] = 8;
	case 3: /* RGB */
		hdr[TGA_IMGTYPE] = 2;
		break;
	}
	hdr[TGA_BPP] = bpp * 8;
	PUT16(hdr + TGA_WIDTH, w);
	PUT16(hdr + TGA_HEIGHT, h);
	if (rle) hdr[TGA_IMGTYPE] |= 8;
	if (!tga_defdir) hdr[TGA_DESC] |= TGA_T2B;
	fwrite(hdr, 1, TGA_HSIZE, fp);

	/* Write palette */
	if (bpp == 1)
	{
		dest = pal;
		for (i = 0; i < settings->colors; i++ , dest += pbpp)
		{
			dest[0] = settings->pal[i].blue;
			dest[1] = settings->pal[i].green;
			dest[2] = settings->pal[i].red;
			if (pbpp > 3) dest[3] = 255;
		}
		/* Mark transparent color */
		if (pbpp > 3) pal[settings->xpm_trans * 4 + 3] = 0;
		fwrite(pal, 1, dest - pal, fp);
	}

	/* Write rows */
	if (!settings->silent) ls_init("TGA", 1);
	if (tga_defdir)
	{
		y0 = h - 1; y1 = -1; vstep = -1;
	}
	else
	{
		y0 = 0; y1 = h; vstep = 1;
	}
	for (i = y0 , pcn = 0; i != y1; i += vstep , pcn++)
	{
		prepare_row(buf, settings, bpp, i); /* Fill uncompressed row */
		src = buf;
		dest = buf + w * bpp;
		if (rle) /* Compress */
		{
			unsigned char *tmp;
			int k, l;

			for (j = 1; j <= w; j++)
			{
				tmp = srca = src;
				src += bpp;
				/* Scan row for repeats */
				for (; j < w; j++ , src += bpp)
				{
					switch (bpp)
					{
					case 4: if (src[3] != srca[3]) break;
					case 3: if (src[2] != srca[2]) break;
					case 2: if (src[1] != srca[1]) break;
					case 1: if (src[0] != srca[0]) break;
					default: continue;
					}
					/* Useful repeat? */
					if (src - srca > bpp + 2) break;
					srca = src;
				}
				/* Avoid too-short repeats at row ends */
				if (src - srca <= bpp + 2) srca = src;
				/* Create copy blocks */
				for (k = (srca - tmp) / bpp; k > 0; k -= 128)
				{
					l = k > 128 ? 128 : k;
					*dest++ = l - 1;
					memcpy(dest, tmp, l *= bpp);
					dest += l; tmp += l;
				}
				/* Create repeat blocks */
				for (k = (src - srca) / bpp; k > 0; k -= 128)
				{
					l = k > 128 ? 128 : k;
					*dest++ = l + 127;
					memcpy(dest, srca, bpp);
					dest += bpp;
				}
			}
		}
		fwrite(src, 1, dest - src, fp);
		ls_progress(settings, pcn, 20);
	}

	/* Write footer */
	memcpy(ftr + TGA_SIGN, "TRUEVISION-XFILE.", TGA_FSIZE - TGA_SIGN);
	memset(ftr, 0, TGA_SIGN);
	fwrite(ftr, 1, TGA_FSIZE, fp);

	fclose(fp);

	if (!settings->silent) progress_end();

	free(buf);
	return 0;
}

/* PCX header */
#define PCX_ID        0 /*  8b */
#define PCX_VER       1 /*  8b */
#define PCX_ENC       2 /*  8b */
#define PCX_BPP       3 /*  8b */
#define PCX_X0        4 /* 16b */
#define PCX_Y0        6 /* 16b */
#define PCX_X1        8 /* 16b */
#define PCX_Y1       10 /* 16b */
#define PCX_HDPI     12 /* 16b */
#define PCX_VDPI     14 /* 16b */
#define PCX_PAL      16 /* 8b*3*16 */
#define PCX_NPLANES  65 /*  8b */
#define PCX_LINELEN  66 /* 16b */
#define PCX_PALTYPE  68 /* 16b */
#define PCX_HRES     70 /* 16b */
#define PCX_VRES     72 /* 16b */
#define PCX_HSIZE   128

#define PCX_BUFSIZE 16384 /* Bytes read at a time */

/* Default EGA/VGA palette */
static const png_color def_pal[16] = {
{0x00, 0x00, 0x00}, {0x00, 0x00, 0xAA}, {0x00, 0xAA, 0x00}, {0x00, 0xAA, 0xAA},
{0xAA, 0x00, 0x00}, {0xAA, 0x00, 0xAA}, {0xAA, 0x55, 0x00}, {0xAA, 0xAA, 0xAA},
{0x55, 0x55, 0x55}, {0x55, 0x55, 0xFF}, {0x55, 0xFF, 0x55}, {0x55, 0xFF, 0xFF},
{0xFF, 0x55, 0x55}, {0xFF, 0x55, 0xFF}, {0xFF, 0xFF, 0x55}, {0xFF, 0xFF, 0xFF},
};

static int load_pcx(char *file_name, ls_settings *settings)
{
	static const unsigned char planarconfig[9] = {
		0x11, /* BW */  0x12, /* 4c */ 0x21, /* 4c */ 0x31, /* 8c */
		0x41, /* 16c */ 0x14, /* 16c */ 0x18, /* 256c */
		0x38, /* RGB */	0x48  /* RGBA */ };
	unsigned char hdr[PCX_HSIZE], pbuf[769];
	unsigned char *buf, *row, *dest, *tmp;
	FILE *fp;
	int ver, bits, planes, ftype;
	int y, ccnt, bstart, bstop, strl, plane, cf;
	int w, h, cols, buflen, bpp = 3, res = -1;


	if (!(fp = fopen(file_name, "rb"))) return (-1);

	/* Read the header */
	if (fread(hdr, 1, PCX_HSIZE, fp) < PCX_HSIZE) goto fail;

	/* PCX has no real signature - so check fields one by one */
	if ((hdr[PCX_ID] != 10) || (hdr[PCX_ENC] > 1)) goto fail;
	ver = hdr[PCX_VER];
	if (ver > 5) goto fail;

	bits = hdr[PCX_BPP];
	planes = hdr[PCX_NPLANES];
	if ((bits == 24) && (planes == 1)) ftype = 7; /* Single-plane RGB */
	else if ((bits | planes) > 15) goto fail;
	else if ((tmp = memchr(planarconfig, (planes << 4) | bits, 9)))
		ftype = tmp - planarconfig;
	else goto fail;

	/* Prepare palette */
	if (ftype < 7)
	{
		bpp = 1;
		settings->colors = cols = 1 << (bits * planes);
		/* BW (0 is black) */
		if (cols == 2)
		{
			settings->pal[0] = def_pal[0];
			settings->pal[1] = def_pal[15];
		}
		/* Default 256-color palette - assumed greyscale */
		else if ((ver == 3) && (cols == 256)) set_gray(settings);
		/* Default 16-color palette */
		else if ((ver == 3) && (cols == 16))
			memcpy(settings->pal, def_pal, sizeof(def_pal));
	/* !!! CGA palette is evil: what the PCX spec describes is the way it
	 * was handled by PC Paintbrush 3.0, while 4.0 was using an entirely
	 * different, undocumented encoding for palette selection.
	 * The only seemingly sane way to differentiate the two is to look at
	 * paletteinfo field: zeroed in 3.0, set in 4.0+ - WJ */
		else if (cols == 4)
		{
			/* Bits 2:1:0 in index: color burst:palette:intensity */
			static const unsigned char cga_pals[8 * 3] = {
				2, 4, 6,  10, 12, 14,
				3, 5, 7,  11, 13, 15,
				3, 4, 7,  11, 12, 15,
				3, 4, 7,  11, 12, 15 };
			int i, idx = hdr[PCX_PAL + 3] >> 5; // PB 3.0

			if (GET16(hdr + PCX_PALTYPE)) // PB 4.0
			{
				/* Pick green palette if G>B in slot 1 */
				i = hdr[PCX_PAL + 5] >= hdr[PCX_PAL + 4];
				/* Pick bright palette if max(G,B) > 200 */
				idx = i * 2 + (hdr[PCX_PAL + 4 + i] > 200);
			}

			settings->pal[0] = def_pal[hdr[PCX_PAL] >> 4];
			for (i = 1 , idx *= 3; i < 4; i++)
				settings->pal[i] = def_pal[cga_pals[idx++]];
		}
		/* VGA palette - read from file */
		else if (cols == 256)
		{
			if ((fseek(fp, -769, SEEK_END) < 0) ||
				(fread(pbuf, 1, 769, fp) < 769) ||
				(pbuf[0] != 0x0C)) goto fail;
			rgb2pal(settings->pal, pbuf + 1, 256);
		}
		/* 8 or 16 colors - read from header */
		else rgb2pal(settings->pal, hdr + PCX_PAL, cols);

		/* If palette is all we need */
		res = 1;
		if ((settings->mode == FS_PALETTE_LOAD) ||
			(settings->mode == FS_PALETTE_DEF)) goto fail;
	}

	/* Allocate buffer and image */
	settings->width = w = GET16(hdr + PCX_X1) - GET16(hdr + PCX_X0) + 1;
	settings->height = h = GET16(hdr + PCX_Y1) - GET16(hdr + PCX_Y0) + 1;
	settings->bpp = bpp;
	buflen = GET16(hdr + PCX_LINELEN);
	res = -1;
	if (buflen < ((w * bits + 7) >> 3)) goto fail;
	/* To accommodate bitparser's extra step */
	buf = malloc(PCX_BUFSIZE + buflen + 1);
	res = FILE_MEM_ERROR;
	if (!buf) goto fail;
	row = buf + PCX_BUFSIZE;
	if ((res = allocate_image(settings, ftype > 7 ? CMASK_RGBA : CMASK_IMAGE)))
		goto fail2;

	/* Read and decode the file */
	if (!settings->silent) ls_init("PCX", 0);
	res = FILE_LIB_ERROR;
	fseek(fp, PCX_HSIZE, SEEK_SET);
	dest = settings->img[CHN_IMAGE];
	if (bits == 1) memset(dest, 0, w * h); // Write will be by OR
	y = plane = ccnt = 0;
	bstart = bstop = PCX_BUFSIZE;
	strl = buflen;
	cf = hdr[PCX_ENC] ? 0xC0 : 0x100; // Compressed, or not
	while (TRUE)
	{
		unsigned char v;

		/* Keep the buffer filled */
		if (bstart >= bstop)
		{
			bstart -= bstop;
			bstop = fread(buf, 1, PCX_BUFSIZE, fp);
			if (bstop <= bstart) goto fail3; /* Truncated file */
		}

		/* Decode data */
		v = buf[bstart];
		if (ccnt) /* Middle of a run */
		{
			int l = strl < ccnt ? strl : ccnt;
			memset(row + buflen - strl, v, l);
			strl -= l; ccnt -= l;
		}
		else if (v >= cf) /* Start of a run */
		{
			ccnt = v & 0x3F;
			bstart++;
		}
		else row[buflen - strl--] = v;
		bstart += !ccnt;
		if (strl) continue;

		/* Store a line */
		if (bits == 1) // N planes of 1-bit data (MSB first)
		{
			unsigned char uninit_(v), *tmp = row;
			int i, n = 7 - plane;

			for (i = 0; i < w; i++ , v += v)
			{
				if (!(i & 7)) v = *tmp++;
				dest[i] |= (v & 0x80) >> n;
			}
		}
		else if (bits == 24) // 1 plane of RGB
			memcpy(dest, row, w * 3);
		else if (plane < 3) // BPP planes of 2/4/8-bit data (MSB first)
			stream_MSB(row, dest + plane, w, bits, 0, bits, bpp);
		else if (settings->img[CHN_ALPHA]) // 8-bit alpha plane
			memcpy(settings->img[CHN_ALPHA] + y * w, row, w);

		if (++plane >= planes)
		{
			ls_progress(settings, y, 10);
			if (++y >= h) break;
			dest += w * bpp;
			plane = 0;
		}
		strl = buflen;
	}
	res = 1;

fail3:	if (!settings->silent) progress_end();
fail2:	free(buf);
fail:	fclose(fp);
	return (res);
}

static int save_pcx(char *file_name, ls_settings *settings)
{
	unsigned char *buf, *src, *dest;
	FILE *fp;
	int w = settings->width, h = settings->height, bpp = settings->bpp;
	int i, l, plane, cnt;


	/* Allocate buffer */
	i = w * 2; // Buffer one plane, with worst-case RLE expansion factor 2
	if (i < PCX_HSIZE) i = PCX_HSIZE;
	if (i < 769) i = 769; // For palette
	buf = calloc(1, i); // Zeroing out is for header
	if (!buf) return (-1);
	
	if (!(fp = fopen(file_name, "wb")))
	{
		free(buf);
		return (-1);
	}

	/* Prepare header */
	memcpy(buf, "\x0A\x05\x01\x08", 4); // Version 5 PCX, 8 bits/plane
	PUT16(buf + PCX_X1, w - 1);
	PUT16(buf + PCX_Y1, h - 1);
	PUT16(buf + PCX_HDPI, 300); // GIMP sets DPI to this value
	PUT16(buf + PCX_VDPI, 300);
	buf[PCX_NPLANES] = bpp;
	PUT16(buf + PCX_LINELEN, w);
	buf[PCX_PALTYPE] = 1;
	fwrite(buf, 1, PCX_HSIZE, fp);

	/* Compress & write pixel rows */
	if (!settings->silent) ls_init("PCX", 1);
	src = settings->img[CHN_IMAGE];
	for (i = 0; i < h; i++ , src += w * bpp)
	{
		for (plane = 0; plane < bpp; plane++)
		{
			unsigned char v, *tmp = src + plane;

			dest = buf; cnt = 0; l = w;
			while (l > 0)
			{
				v = *tmp; tmp += bpp; cnt++;
				if ((--l <= 0) || (cnt == 0x3F) || (v != *tmp))
				{
					if ((cnt > 1) || (v >= 0xC0))
						*dest++ = cnt | 0xC0;
					*dest++ = v; cnt = 0;
				}
			}
			fwrite(buf, 1, dest - buf, fp);
		}
		ls_progress(settings, i, 20);
	}

	/* Write palette */
	if (bpp == 1)
	{
		buf[0] = 0x0C;
		pal2rgb(buf + 1, settings->pal, settings->colors, 256);
		fwrite(buf, 1, 769, fp);
	}

	fclose(fp);

	if (!settings->silent) progress_end();

	free(buf);
	return (0);
}

/* *** PREFACE ***
 * LBM format has no one definitive documentation source, nor a good testsuite,
 * and while I found enough examples of some types, other ones I never observed
 * in the wild remain unsupported. If you encounter some such curiosity failing
 * to load or loading wrong, send a bugreport with the file attached to it. */

/* Macros for IFF tags; big-endian too */
#define TAG4B_FORM TAG4B('F', 'O', 'R', 'M')
#define TAG4B_ILBM TAG4B('I', 'L', 'B', 'M')
#define TAG4B_PBM  TAG4B('P', 'B', 'M', ' ')
#define TAG4B_BMHD TAG4B('B', 'M', 'H', 'D')
#define TAG4B_CMAP TAG4B('C', 'M', 'A', 'P')
#define TAG4B_GRAB TAG4B('G', 'R', 'A', 'B')
#define TAG4B_DEST TAG4B('D', 'E', 'S', 'T')
#define TAG4B_CAMG TAG4B('C', 'A', 'M', 'G')
#define TAG4B_BODY TAG4B('B', 'O', 'D', 'Y')
/* Multipalette tags */
#define TAG4B_SHAM TAG4B('S', 'H', 'A', 'M')
#define TAG4B_CTBL TAG4B('C', 'T', 'B', 'L')
#define TAG4B_PCHG TAG4B('P', 'C', 'H', 'G')

/* LBM header block (BMHD tag) */
#define BMHD_W     0 /* 16b */
#define BMHD_H     2 /* 16b */
#define BMHD_X0    4 /* 16b */
#define BMHD_Y0    6 /* 16b */
#define BMHD_BPP   8 /*  8b */
#define BMHD_MASK  9 /*  8b */
#define BMHD_COMP 10 /*  8b */
#define BMHD_PAD  11 /*  8b */
#define BMHD_TRAN 12 /* 16b */
#define BMHD_ASPX 14 /*  8b */
#define BMHD_ASPY 15 /*  8b */
#define BMHD_SCW  16 /* 16b */
#define BMHD_SCH  18 /* 16b */
#define BMHD_SIZE 20

/*  LBM DEST block */
#define DEST_DEPTH 0 /*  8b */
#define DEST_PAD   1 /*  8b */
#define DEST_PICK  2 /* 16b */
#define DEST_ONOFF 4 /* 16b */
#define DEST_MASK  6 /* 16b */
#define DEST_SIZE  8

/* PCHG block header */
#define PCHG_COMPR  0 /* 16b */
#define PCHG_FLAGS  2 /* 16b */
#define PCHG_START  4 /* 16b */
#define PCHG_COUNT  6 /* 16b */
#define PCHG_CHLIN  8 /* 16b */
#define PCHG_MINR  10 /* 16b */
#define PCHG_MAXR  12 /* 16b */
#define PCHG_MAXCH 14 /* 16b */
#define PCHG_TOTCH 16 /* 32b */
#define PCHG_HSIZE 20

#define HAVE_BMHD 1
#define HAVE_CMAP 2
#define HAVE_GRAB 4
#define HAVE_DEST 8

static int load_lbm(char *file_name, ls_settings *settings)
{
	static const unsigned char bitdepths[] =
		{ 1, 2, 3, 4, 5, 6, 7, 8, 21, 24, 32 };
	unsigned char hdr[BMHD_SIZE], dbuf[DEST_SIZE], pchdr[PCHG_HSIZE];
	unsigned char pbuf[768], wbuf[256];
	unsigned char *buf, *row, *dest, *mpp, *pr = NULL;
	FILE *fp;
	int y, ccnt, bstart, bstop, strl, np, ap, mp;
	f_long ctbl = 0, pchg = 0;
	unsigned tag, tl;
	int pstart = 0, ctbll = 0, pchgl = 0, pcnt = 0, sh2 = 0;
	int w, h, bpp, bits, mask, tbits, buflen, plen, half = 0, ham = 0;
	int pbm, palsize = 0, blocks = 0, hx = 0, hy = 0, res = -1;
	int i, j, l, p, pad, want_pal;


	if (!(fp = fopen(file_name, "rb"))) return (-1);

	/* Read the IFF header & check signature */
	if (fread(wbuf, 1, 12, fp) < 12) goto fail;
	if (GET32B(wbuf) != TAG4B_FORM) goto fail;
	tag = GET32B(wbuf + 8);
	if (!(pbm = tag == TAG4B_PBM) && !(tag == TAG4B_ILBM)) goto fail;

	/* Read block headers & see what we get */
	want_pal = (settings->mode == FS_PALETTE_LOAD) ||
		(settings->mode == FS_PALETTE_DEF);
	while (fread(wbuf, 1, 8, fp) == 8)
	{
		tag = GET32B(wbuf);
		tl = GET32B(wbuf + 4);
		if (tl >= INT_MAX) break; // Sanity check
		pad = tl & 1;
		if (tag == TAG4B_BMHD)
		{
			if (tl != BMHD_SIZE) break;
			if (fread(hdr, 1, BMHD_SIZE, fp) != BMHD_SIZE) break;
			blocks |= HAVE_BMHD;
			continue;
		}
		else if (tag == TAG4B_CMAP)
		{
			/* Allow palette being too long */
			palsize = tl > 768 ? 768 : tl;
			if (fread(pbuf, 1, palsize, fp) != palsize) break;
			blocks |= HAVE_CMAP;
			tl -= palsize;
			/* If palette is all we need; hope there's only one */
			if (want_pal)
			{
				res = 1;
				break;
			}
			// Fallthrough
		}
		else if (tag == TAG4B_GRAB)
		{
			if ((tl != 4) || (fread(wbuf, 1, 4, fp) != 4)) break;
			blocks |= HAVE_GRAB;
			hx = GET16B(wbuf);
			hy = GET16B(wbuf + 2);
			continue;
		}
		else if (tag == TAG4B_DEST)
		{
			if (tl != DEST_SIZE) break;
			if (fread(dbuf, 1, DEST_SIZE, fp) != DEST_SIZE) break;
			blocks |= HAVE_DEST;
			continue;
		}
		else if (tag == TAG4B_CAMG)
		{
			if ((tl != 4) || (fread(wbuf, 1, 4, fp) != 4)) break;
			tag = GET32B(wbuf);
			half = tag & 0x80;
			ham = tag & 0x800;
			continue;
		}
		else if ((tag == TAG4B_SHAM) || (tag == TAG4B_CTBL))
		{
			ctbl = ftell(fp);
			ctbll = tl;
			// SHAM has "version" word at the beginning
			if (tag == TAG4B_SHAM)
			{
				if (tl < 2) break;
				ctbl += 2 , ctbll -= 2;
			}
		}
		else if (tag == TAG4B_PCHG)
		{
			if ((tl < PCHG_HSIZE) ||
				(fread(pchdr, 1, PCHG_HSIZE, fp) != PCHG_HSIZE)) break;
			pchg = ftell(fp);
			pchgl = tl -= PCHG_HSIZE;
		}
		else if (tag == TAG4B_BODY)
		{
			/* Palette & header must be before body */
			if (!want_pal && (blocks & HAVE_BMHD)) res = 0;
			break;
		}
		/* Default: skip (the rest of) tag data */
		tl += pad;
		if (tl && fseek(fp, tl, SEEK_CUR)) break;
	}
	if (res < 0) goto fail;

	/* Parse bitplanes */
	tbits = !(blocks & HAVE_BMHD) ? 0 : // Palette may happen before header
		blocks & HAVE_DEST ? dbuf[DEST_DEPTH] : hdr[BMHD_BPP];

	/* Prepare palette */
	if (blocks & HAVE_CMAP)
	{
		/* Corrective multipliers to counteract dumb shift */
		static const unsigned char mult[8] =
			{ 128, 128, 130, 132, 136, 146, 170, 255 };

		/* Limit palette to actual bitplanes */
		l = palsize / 3;
		if (tbits && (tbits < 9))
		{
			i = tbits;
			if (ham) i = i > 6 ? 6 : 4;
			else if (half && (i > 5)) i = 5;
			i = 1 << i;
			if (l > i) l = i;
		}
		/* Detect and correct palettes where 6..1-bit color was shifted
		 * left by 2..7 without replicating high bits into low */
		l *= 3;
		for (i = 0 , j = 0x80; i < l; i++) j |= pbuf[i];
		for (i = 0; !(j & 1); i++) j >>= 1;
		for (j = mult[i] , i = 0; i < l; i++)
			pbuf[i] = (pbuf[i] * j) >> 7;
		/* Apply half-brite mode */
		if (half && (l <= 32 * 3))
		{
			memset(pbuf + l, 0, 32 * 3 - l);
			for (i = 0; i < l; i++)
				pbuf[i + 32 * 3] = pbuf[i] >> 1;
			l += 32 * 3;
		}
		/* Store the result */
		rgb2pal(settings->pal, pbuf, settings->colors = l / 3);
	}
	if (want_pal) goto fail;

	/* Check sanity */
	res = -1;
	if (hdr[BMHD_COMP] > 1) goto fail; // Unknown compression type
	bits = hdr[BMHD_BPP];
	if (!memchr(bitdepths, bits, sizeof(bitdepths))) goto fail;
	if (ham)
	{
 		if ((bits < 5) || (bits > 8)) goto fail;
		// No reason for grayscale HAM to exist
		if (!(blocks & HAVE_CMAP)) goto fail;
		ham = bits > 6 ? 6 : 4; // Shift value
	}
	if (ctbl)
	{
		h = GET16B(hdr + BMHD_H);
		sh2 = ctbll == (h >> 1) * 32;
		if (!sh2 && (ctbll != h * 32)) goto fail; // Size must match
		pchg = pchgl = 0; // If both present, simpler is better
		if (bits > (ham ? 6 : 4)) goto fail;
	}
	if (pchg)
	{
		/* No examples of anything but uncompressed 12-bit PCHG blocks,
		 * so no reason to waste code supporting anything else */
		if (GET16B(pchdr + PCHG_COMPR)) goto fail;
		if (GET16B(pchdr + PCHG_FLAGS) != 1) goto fail;
		if (bits > (half || ham ? 6 : 5)) goto fail;
		pstart = GET16B(pchdr + PCHG_START);
		pcnt = GET16B(pchdr + PCHG_COUNT);

	}
	mask = hdr[BMHD_MASK] == 1;
	if (pbm && (mask || ham || ctbl || pchg || (bits != 8)))
		goto fail; // Not compatible

	/* DEST block if any */
	if (blocks & HAVE_DEST)
	{
		unsigned skip, setv, v;

		/* For simplicity, as no one ever saw files w/DEST anyway */
		if ((tbits < bits) || (tbits > 8) || ham) goto fail;
		/* Make a lookup table for remapping bits after the fact;
		 * ignore planeMask in hope it masks only planeOnOff */
		skip = ((1 << tbits) - 1) & ~GET16B(dbuf + DEST_PICK); // Skipmask
		setv = skip & GET16B(dbuf + DEST_ONOFF); // Setmask
		for (v = i = 0; !(i >> bits); i++)
		{
			wbuf[i] = v | setv;
			v = (v + skip + 1) & ~skip; // Increment across gaps
		}
	}
	/* 21-bit RGB */
	else if (bits == 21)
	{
		set_xlate(wbuf, 7);
		blocks |= HAVE_DEST; // !!! Let same xlate do either thing
	}

	/* Make greyscale palette if needed */
	if ((tbits <= 8) && !(blocks & HAVE_CMAP))
		mem_bw_pal(settings->pal, 0, (settings->colors = 1 << tbits) - 1);

	/* Transparent color - nearly always a glitch, rarely a real thing */
	if (!lbm_untrans && (hdr[BMHD_MASK] > 1))
	{
		j = GET16B(hdr + BMHD_TRAN);
		if (j < settings->colors) settings->xpm_trans = j;
	}

	if (blocks & HAVE_GRAB) settings->hot_x = hx , settings->hot_y = hy;

	/* Allocate buffer and image */
	settings->width = w = GET16B(hdr + BMHD_W);
	settings->height = h = GET16B(hdr + BMHD_H);
	plen = ctbll + pchgl;
	settings->bpp = bpp = ham || plen || (bits > 8) ? 3 : 1;
	buflen = pbm ? w + (w & 1) : ((w + 15) >> 4) * 2 * (bits + mask);
	buf = multialloc(MA_ALIGN_DEFAULT, &buf, PCX_BUFSIZE, &row, buflen,
		&mpp, plen, NULL);
	res = FILE_MEM_ERROR;
	if (!buf) goto fail;
	i = bits == 32 ? CMASK_RGBA : CMASK_IMAGE;
	if (mask) i |= CMASK_FOR(lbm_mask);
	if ((res = allocate_image(settings, i))) goto fail2;
	if (!pbm) // Prepare for writes by OR
	{
		memset(settings->img[CHN_IMAGE], 0, w * h * bpp);
		if (settings->img[CHN_ALPHA])
			memset(settings->img[CHN_ALPHA], 0, w * h);
		if ((i & ~CMASK_RGBA) && settings->img[lbm_mask])
			memset(settings->img[lbm_mask], 0, w * h);
	}

	/* Load color change table if any */
	if (plen)
	{
		f_long b = ftell(fp);
		if (fseek(fp, ctbl + pchg, SEEK_SET) ||
			(fread(mpp, 1, plen, fp) != plen)) goto fail2;
		fseek(fp, b, SEEK_SET);
		if (!ham) ham = 8; // Use same decoding loop in mode 0
		pr = mpp + ((pcnt + 31) >> 5) * 4;
	}

	/* Read and decode the file */
	if (!settings->silent) ls_init("LBM", 0);
	res = FILE_LIB_ERROR;
	ap = bits > 24 ? 24 : -1; // First alpha plane
	if (!settings->img[CHN_ALPHA]) ap = -1; // No alpha
	mp = bits; // Mask plane
	if (!mask || !lbm_mask || !settings->img[lbm_mask] ||
		((lbm_mask == CHN_ALPHA) && (ap > 0))) mp = -1;
	np = mp > 0 ? bits + 1 : (ap > 0) || (bits < 24) ? bits : 24; // Planes to read
	y = ccnt = 0;
	if (!hdr[BMHD_COMP]) ccnt = buflen * h; // Uncompressed is file-sized copy run
	bstart = bstop = PCX_BUFSIZE;
	strl = buflen;
	while (TRUE)
	{
		/* Keep the buffer filled */
		if (bstart >= bstop)
		{
			bstart -= bstop;
			bstop = fread(buf, 1, PCX_BUFSIZE, fp);
			if (bstop <= bstart) goto fail3; /* Truncated file */
		}

		/* Decode data */
		if (ccnt < 0) /* Middle of a repeat run */
		{
			int l = strl + ccnt < 0 ? strl : -ccnt;
			memset(row + buflen - strl, buf[bstart], l);
			strl -= l; bstart += !(ccnt += l);
		}
		else if (ccnt > 0) /* Middle of a copy run */
		{
			int l = strl < ccnt ? strl : ccnt;
			if (l > bstop - bstart) l = bstop - bstart;
			memcpy(row + buflen - strl, buf + bstart, l);
			strl -= l; ccnt -= l; bstart += l;
		}
		else /* Start of a run */
		{
			ccnt = buf[bstart];
			ccnt += ccnt < 128 ? 1 : -257;
			bstart++;
		}
		if (strl) continue;

		/* Store a line */
		p = y * w;
		dest = settings->img[CHN_IMAGE] + p * bpp;
		if (pbm) memcpy(dest, row, w);
		while (!pbm)
		{
			unsigned char *dsta = NULL, *dstm = NULL;
			unsigned char uninit_(v), *tmp, *dp;
			int i, n, plane, step = bpp;

			if (ap > 0) dsta = settings->img[CHN_ALPHA] + p;
			if (mp > 0) dstm = settings->img[lbm_mask] + p;
			for (plane = 0; plane < np; plane++)
			{
				tmp = row + ((w + 15) >> 4) * 2 * plane;
				if (bits == 21)
					dp = dest + plane % 3 , n = 1 + plane / 3;
				else dp = dest + (plane >> 3) , n = 7 - (plane & 7);
				if (plane == mp) dp = dstm , step = 1; // Mask
				else if (plane >= 24) dp = dsta , step = 1; // Alpha
				if (!dp) continue; // Skipping alpha till mask
				for (i = 0; i < w; i++ , v += v , dp += step)
				{
					if (!(i & 7)) v = *tmp++;
					*dp |= (v & 0x80) >> n;
				}
			}

			if (!ham) break;

			/* Multipalette, simpler kind */
			if (ctbl && !(y & sh2))
			{
				unsigned char *dest = pbuf;
				for (i = 0; i < 16; i++ , pr += 2 , dest += 3)
				{
					int v = GET16B(pr);
					dest[0] = ((v >> 8) & 0xF) * 0x11;
					dest[1] = ((v >> 4) & 0xF) * 0x11;
					dest[2] = (v & 0xF) * 0x11;
				}
			}
			/* Multipalette, complex kind */
			while (pchg && (y >= pstart) && (y < pstart + pcnt))
			{
				unsigned char *dest;
				int n, n16, v, i, j;
				i = y - pstart;
				j = (i >> 5) * 4;
				if (!((GET32B(mpp + j) >> (~i & 0x1F)) & 1))
					break; // Nothing to do for this line
				n16 = pr[1]; // Colors 16-31 for this many
				n = pr[0] + n16; // Total indices
				pr += 2;
				while (n-- > 0)
				{
					v = GET16B(pr);
					pr += 2;
					dest = pbuf + (n < n16) * 16 * 3 + (v >> 12) * 3;
					dest[0] = ((v >> 8) & 0xF) * 0x11;
					dest[1] = ((v >> 4) & 0xF) * 0x11;
					dest[2] = (v & 0xF) * 0x11;
				}
				if (half) for (i = 0; i < 32 * 3; i++)
					pbuf[i + 32 * 3] = pbuf[i] >> 1;
				break;
			}

			/* Recode the row */
			/* !!! Start with palette color 0 as amigaos.net says and
			 * GrafX2 does, not RGB 0 as ilbmtopnm does */
			tmp = pbuf;
			dp = dest;
			for (i = 0; i < w; i++ , dp += 3)
			{
				n = (v = *dp) >> ham;
				if (!n) tmp = pbuf + v * 3; // Palette color
				dp[0] = tmp[0];
				dp[1] = tmp[1];
				dp[2] = tmp[2];
				tmp = dp;
				if (!n) continue;
				v ^= n << ham;
				n ^= (n >> 1) ^ 3; // 0BRG -> RGB
				/* !!! In HAM8, preserve low 2 bits as Amiga docs
				 * say and ilbmtopnm does; but in HAM6, put value
				 * into lower & upper bits like GrafX2 and unlike
				 * ilbmtopnm: those old Amigas did not HAVE any
				 * color bits beyond the 4 */
				dp[n] = ham == 4 ? v + (v << 4) :
					(v << 2) + (dp[n] & 3);
			}
			break;
		}
		ls_progress(settings, y, 10);
		if (++y >= h) break;
		strl = buflen;
	}
	res = 1;

	/* Finalize DEST or 21-bit */
	if (blocks & HAVE_DEST) do_xlate(wbuf, settings->img[CHN_IMAGE], w * h * bpp);
	/* Finalize mask */
	if (mp < 0); // No mask
	else if (is_filled(settings->img[lbm_mask], settings->img[lbm_mask][0], w * h))
		deallocate_image(settings, CMASK_FOR(lbm_mask)); // Useless mask
	else
	{
		memset(wbuf + 1, 255, 255); // Nonzero means fully opaque
		wbuf[0] = 0;
		do_xlate(wbuf, settings->img[lbm_mask], w * h);
	}

fail3:	if (!settings->silent) progress_end();
fail2:	free(buf);
fail:	fclose(fp);
	return (res);
}

static int save_lbm(char *file_name, ls_settings *settings)
{
	unsigned char *buf, *wb, *src, *dest;
	FILE *fp;
	f_long bstart, fend;
	unsigned l;
	int w = settings->width, h = settings->height, bpp = settings->bpp;
	int pbm = settings->lbm_pbm && (bpp == 1), comp = !!settings->lbm_pack;
	int i, j, np1, rl, plane, st, cnt, np = 0, mask = 0;

	/* Count bitplanes */
	if (!pbm)
	{
		mask = settings->img[lbm_mask] ? lbm_mask : 0;
		if (bpp == 1) // Planes to hold indexed color
		{
			i = settings->colors - 1;
			if (i > 15) np = 4 , i >>= 4;
			if (i > 3) np += 2 , i >>= 2;
			if (i > 1) np++ , i >>= 1;
			np += i;
		}
		else np = settings->img[CHN_ALPHA] ? 32 : 24; // RGBA/RGB
		if ((np == 32) && (mask == CHN_ALPHA)) mask = 0; // No need
	}

	/* Allocate buffer */
	rl = pbm ? w + (w & 1) : ((w + 15) >> 4) * 2; // One plane
	i = rl + (rl + 127) / 128; // Worst-case RLE expansion
	if (!pbm) i *= np + !!mask; // Buffer all planes
	i += comp * rl; // Uncompressed source
	if (i < 8 + 768) i = 8 + 768; // For CMAP & header
	buf = calloc(1, i); // Zeroing out is for header
	if (!buf) return (-1);
	wb = buf + comp * rl; // Compressed data go here
	
	if (!(fp = fopen(file_name, "wb")))
	{
		free(buf);
		return (-1);
	}

	/* Prepare header */
	memcpy(buf, "FORM\0\0\0\0", 8);
	memcpy(buf + 8, pbm ? "PBM " : "ILBM", 4);
	memcpy(buf + 12, "BMHD", 4);
	PUT32B(buf + 16, BMHD_SIZE);
	PUT16B(buf + 20 + BMHD_W, w);
	PUT16B(buf + 20 + BMHD_H, h);
	buf[20 + BMHD_BPP] = pbm ? 8 : np;
	buf[20 + BMHD_MASK] = mask ? 1 : 0;
	buf[20 + BMHD_COMP] = comp;
	if (!mask && (settings->xpm_trans >= 0))
	{
		buf[20 + BMHD_MASK] = 2;
		PUT16B(buf + 20 + BMHD_TRAN, settings->xpm_trans);
	}
	buf[20 + BMHD_ASPX] = buf[20 + BMHD_ASPY] = 1;
	/* Leave page size unset */
//	PUT16B(buf + 20 + BMHD_W, w);
//	PUT16B(buf + 20 + BMHD_H, h);
	fwrite(buf, 1, 20 + BMHD_SIZE, fp);

	/* Palette (none for RGB/RGBA, to avoid confusing readers) */
	if (bpp == 1)
	{
		memcpy(buf, "CMAP", 4);
		i = settings->colors * 3;
		i += i & 1; // Align the size itself, as in every example observed
		PUT32B(buf + 4, i);
		pal2rgb(buf + 8, settings->pal, settings->colors, 256);
		fwrite(buf, 1, 8 + i, fp);
	}

	/* Anchor point */
	if ((settings->hot_x >= 0) && (settings->hot_y >= 0))
	{
		memcpy(buf, "GRAB", 4);
		PUT32B(buf + 4, 4);
		PUT16B(buf + 8, settings->hot_x);
		PUT16B(buf + 10, settings->hot_y);
		fwrite(buf, 1, 8 + 4, fp);
	}

	/* Compress & write pixel rows */
	if (!settings->silent) ls_init("LBM", 1);
	fwrite("BODY\0\0\0\0", 1, 8, fp);
	bstart = ftell(fp);
	np1 = np + (pbm || mask); // Total planes
	for (i = 0; i < h; i++)
	{
		src = settings->img[CHN_IMAGE] + w * bpp * i;
		dest = wb;
		for (plane = 0; plane < np1; plane++)
		{
			unsigned char v, *d, *s = src + (plane >> 3);
			int n = plane & 7, step = bpp;

			d = comp ? buf : dest;
			if (pbm)
			{
				/* Copy indexed row */
				memcpy(d, src, w);
				d += w;
			}
			else
			{
				/* Extract a bitplane */
				if (plane >= 24) // Alpha
					s = settings->img[CHN_ALPHA] + w * i , step = 1;
				if (plane == np) // Mask - threshold at 128
					s = settings->img[mask] + w * i , step = 1 , n = 7;
				for (j = v = 0; j < w; j++ , s += step)
				{
					v |= ((*s >> n) & 1) << (~j & 7);
					if (~j & 7) continue;
					*d++ = v;
					v = 0;
				}
				if (w & 7) *d++ = v;
			}
			if ((d - buf) & 1) *d++ = 0; // Align

			if (!comp)
			{
				dest = d;
				continue;
			}

			/* Compress a bitplane */
#define FILL 1
#define EMIT 2
#define STOP 4
#define NFIL 8
			s = buf;
			st = cnt = 0;
			while (TRUE)
			{
				if (d - s <= 0) st |= EMIT + STOP;
				else if (cnt == 128) st |= EMIT;
				else if (st & FILL)
				{
					if (s[0] != *(s - 1)) st = EMIT + FILL;
				}
				else if ((d - s > 1) && (s[0] == s[1]))
				{
				/* Code pairs as repeats only when NOT following
				 * a copy block; code triples as repeats always */
					if (!cnt || ((d - s > 2) && (s[0] == s[2])))
						st = EMIT + NFIL;
				}
				if (!(st & EMIT))
				{
					s++ , cnt++;
					continue;
				}
				if (st & FILL)
				{
					*dest++ = 257 - cnt;
					*dest++ = *(s - 1);
				}
				else if (cnt)
				{
					*dest++ = cnt - 1;
					memcpy(dest, s - cnt, cnt);
					dest += cnt;
				}
				if (st & STOP) break;
				if (st & NFIL)
				{
					s += cnt = 2;
					st = FILL;
				}
				else st = cnt = 0;
			}
#undef FILL
#undef EMIT
#undef STOP
#undef NFIL
		}
		fwrite(wb, 1, dest - wb, fp);
		ls_progress(settings, i, 20);
	}

	/* Align last block & write sizes */
	fend = ftell(fp);
	l = fend - bstart;
	if (l & 1) fwrite("", 1, 1, fp); // Padding
	PUT32B(buf, l);
	fseek(fp, bstart - 4, SEEK_SET);
	fwrite(buf, 1, 4, fp);
	l = fend - 8;
	l += l & 1; // Aligned
	PUT32B(buf, l);
	fseek(fp, 4, SEEK_SET);
	fwrite(buf, 1, 4, fp);
	fclose(fp);

	if (!settings->silent) progress_end();

	free(buf);
	return (0);
}

typedef void (*cvt_func)(unsigned char *dest, unsigned char *src, int len,
	int bpp, int step, int maxval);

static void convert_16b(unsigned char *dest, unsigned char *src, int len,
	int bpp, int step, int maxval)
{
	int i, v, m = maxval * 2;

	if (!(step -= bpp)) bpp *= len , len = 1;
	step *= 2;
	while (len-- > 0)
	{
		i = bpp;
		while (i--)
		{
			v = (src[0] << 8) + src[1];
			src += 2;
			*dest++ = (v * (255 * 2) + maxval) / m;
		}
		src += step;
	}	
}

static void copy_bytes(unsigned char *dest, unsigned char *src, int len,
	int bpp, int step)
{
	int i, dd = 0;

	if (!(step -= bpp)) bpp *= len , len = 1;
	else if (step < 0) bpp -= dd = -step , step = 0;
	while (len-- > 0)
	{
		i = bpp;
		while (i--) *dest++ = *src++;
		src += step; dest += dd;
	}	
}

static int check_next_pnm(FILE *fp, char id)
{
	char buf[2];

	if (fread(buf, 2, 1, fp))
	{
		fseek(fp, -2, SEEK_CUR);
		if ((buf[0] == 'P') && (buf[1] == id)) return (FILE_HAS_FRAMES);
	}
	return (1);
}

/* Parse PAM header */
static char *pam_behead(memFILE *mf, int whdm[4])
{
	char wbuf[2048];
	char *t1, *t2, *tail, *res = NULL;
	int i, n, l, flag = 0;

	/* Read header, check for basic PAM */
	if (!mfgets(wbuf, sizeof(wbuf), mf) || strncmp(wbuf, "P7", 2))
		return (NULL);
	while (TRUE)
	{
		if (!mfgets(wbuf, sizeof(wbuf), mf)) break;
		if (!wbuf[0] || (wbuf[0] == '#')) continue; // Empty line or comment
		t1 = wbuf + strspn(wbuf, WHITESPACE);
		l = strcspn(t1, WHITESPACE);
		t2 = t1 + l + strspn(t1 + l, WHITESPACE);
		t1[l] = '\0';
		if (!strcmp(t1, "ENDHDR"))
		{
			if (flag < 0x0F) break; // Incomplete header
			return (res ? res : strdup("")); // TUPLTYPE is optional
		}
		if (!*t2) break; // There must be something but whitespace
		tail = t2 + strcspn(t2, WHITESPACE);
		if (!strcmp(t1, "TUPLTYPE"))
		{
			if (res) continue; // Only first value matters
			while (TRUE)
			{
				t1 = tail + strspn(tail, WHITESPACE);
				if (!*t1) break;
				tail = t1 + strcspn(t1, WHITESPACE);
			}
			// Preserve value for caller
			*tail = '\0';
			res = strdup(t2);
			continue;
		}
		// Other fields are numeric
		*tail = '\0';
		i = strtol(t2, &tail, 10);
		if (*tail) break;
		if (i < 1) break; // Must be at least 1

		if (!strcmp(t1, "WIDTH")) n = 0;
		else if (!strcmp(t1, "HEIGHT")) n = 1;
		else if (!strcmp(t1, "DEPTH")) n = 2;
		else if (!strcmp(t1, "MAXVAL")) n = 3;
		else break; // Unknown IDs not allowed

		whdm[n] = i;
		n = 1 << n;
		if (flag & n) break; // No duplicate entries
		flag |= n;
	}
	free(res);
	return (NULL);
}

/* PAM loader does not support nonstandard types "GRAYSCALEFP" and "RGBFP",
 * because handling format variations which aren't found in the wild
 * is a waste of code - WJ */

static int load_pam_frame(FILE *fp, ls_settings *settings)
{
	static const char *typenames[] = {
		"BLACKANDWHITE", "BLACKANDWHITE_ALPHA",
		"GRAYSCALE", "GRAYSCALE_ALPHA",
		"RGB", "RGB_ALPHA",
		"CMYK", "CMYK_ALPHA", NULL };
	static const char depths[] = { 1, 2, 1, 2, 3, 4, 4, 5 };
	memFILE fake_mf;
	cvt_func cvt_stream;
	char *t1;
	unsigned char *dest, *buf = NULL;
	int maxval, w, h, depth, ftype = -1;
	int i, j, ll, bpp, trans, vl, res, whdm[4];


	/* Read header */
	memset(&fake_mf, 0, sizeof(fake_mf));
	fake_mf.file = fp;
	if (!(t1 = pam_behead(&fake_mf, whdm))) return (-1);
	/* Compare TUPLTYPE to list of known ones */
	if (*t1) for (i = 0; typenames[i]; i++)
	{
		if (strcmp(t1, typenames[i])) continue;
		ftype = i;
		break;
	}
	free(t1); // No use anymore
	w = whdm[0]; h = whdm[1]; depth = whdm[2]; maxval = whdm[3];
	/* Interpret unknown content as RGB or grayscale */
	if (ftype < 0) ftype = depth >= 3 ? 4 : 2;

	/* Validate */
	if ((depth < depths[ftype]) || (depth > 16) || (maxval > 65535))
		return (-1);
	bpp = ftype < 4 ? 1 : 3;
	trans = ftype & 1;
	vl = maxval < 256 ? 1 : 2;
	ll = w * depth * vl;
	/* !!! ImageMagick writes BLACKANDWHITE as GRAYSCALE */
	if ((ftype < 2) && (maxval > 1)) ftype += 2;
	if (ftype < 2) set_bw(settings); // BW
	else if (bpp == 1) set_gray(settings); // Grayscale

	/* Allocate row buffer if cannot read directly into image */
	if (trans || (vl > 1) || (bpp != depth))
	{
		buf = malloc(ll);
		if (!buf) return (FILE_MEM_ERROR);
	}

	/* Allocate image */
	settings->width = w;
	settings->height = h;
	settings->bpp = bpp;
	res = allocate_image(settings, trans ? CMASK_RGBA : CMASK_IMAGE);
	if (res) goto fail;

	/* Read the image */
	if (!settings->silent) ls_init("PAM", 0);
	res = FILE_LIB_ERROR;
	cvt_stream = vl > 1 ? convert_16b : (cvt_func)copy_bytes;
	for (i = 0; i < h; i++)
	{
		dest = buf ? buf : settings->img[CHN_IMAGE] + ll * i;
		j = fread(dest, 1, ll, fp);
		if (j < ll) goto fail2;
		ls_progress(settings, i, 10);

		if (!buf) continue; // Nothing else to do here
		if (settings->img[CHN_ALPHA]) // Have alpha - parse it
		{
			cvt_stream(settings->img[CHN_ALPHA] + w * i,
				buf + depths[ftype] * vl - vl, w, 1, depth, maxval);
		}
		dest = settings->img[CHN_IMAGE] + w * bpp * i;
		if (ftype >= 6) // CMYK
		{
			cvt_stream(buf, buf, w, 4, depth, maxval);
			if (maxval < 255) extend_bytes(buf, w * 4, maxval);
			cmyk2rgb(dest, buf, w, FALSE, settings);
		}
		else cvt_stream(dest, buf, w, bpp, depth, maxval);
	}

	/* Check for next frame */
	res = check_next_pnm(fp, '7');

fail2:	if (maxval < 255) // Extend what we've read
	{
		j = w * h;
		if (settings->img[CHN_ALPHA])
			extend_bytes(settings->img[CHN_ALPHA], j, maxval);
		j *= bpp;
		dest = settings->img[CHN_IMAGE];
		if (ftype >= 6); // CMYK is done already
		else if (ftype > 1) extend_bytes(dest, j, maxval);
		else // Convert BW from 1-is-white to 1-is-black
		{
			for (i = 0; i < j; i++ , dest++) *dest = !*dest;
		}
	}
	if (!settings->silent) progress_end();

fail:	free(buf);
	return (res);
}

#define PNM_BUFSIZE 4096
typedef struct {
	FILE *f;
	int ptr, end, eof, comment;
	char buf[PNM_BUFSIZE + 2];
} pnmbuf;

/* What PBM documentation says is NOT what Netpbm actually does; skipping a
 * comment in file header, it does not consume the newline after it - WJ */
static void pnm_skip_comment(pnmbuf *pnm)
{
	pnm->comment = !pnm->buf[pnm->ptr += strcspn(pnm->buf + pnm->ptr, "\r\n")];
}

static char *pnm_gets(pnmbuf *pnm, int data)
{
	int k, l;

	while (TRUE)
	{
		while (pnm->ptr < pnm->end)
		{
			l = pnm->ptr + strspn(pnm->buf + pnm->ptr, WHITESPACE);
			if (pnm->buf[l] == '#')
			{
				if (data) return (NULL);
				pnm->ptr = l;
				pnm_skip_comment(pnm);
				continue;
			}
			k = l + strcspn(pnm->buf + l, WHITESPACE "#");
			if (pnm->buf[k] || pnm->eof)
			{
				pnm->ptr = k + 1;
				if (pnm->buf[k] == '#')
				{
					if (data) return (NULL);
					pnm_skip_comment(pnm);
				}
				pnm->buf[k] = '\0';
				return (pnm->buf + l);
			}
			memmove(pnm->buf, pnm->buf + l, pnm->end -= l);
			pnm->ptr = 0;
			break;
		}
		if (pnm->eof) return (NULL);
		if (pnm->ptr >= pnm->end) pnm->ptr = pnm->end = 0;
		l = PNM_BUFSIZE - pnm->end;
		if (l <= 0) return (NULL); // A "token" of 4096 chars means failure
		pnm->end += k = fread(pnm->buf + pnm->end, 1, l, pnm->f);
		pnm->eof = k < l;
		if (pnm->comment) pnm_skip_comment(pnm);
	}
}

static int pnm_endhdr(pnmbuf *pnm, int plain)
{
	while (pnm->comment)
	{
		pnm_skip_comment(pnm);
		if (!pnm->comment) break;
		if (pnm->eof) return (FALSE);
		pnm->end = fread(pnm->buf, 1, PNM_BUFSIZE, pnm->f);
		pnm->eof = pnm->end < PNM_BUFSIZE;
	}
	/* Last whitespace in header already got consumed while parsing */

	/* Buffer will remain in use in plain mode */
	if (!plain && (pnm->ptr < pnm->end))
		fseek(pnm->f, pnm->ptr - pnm->end, SEEK_CUR);
	return (TRUE);
}

static int load_pnm_frame(FILE *fp, ls_settings *settings)
{
	pnmbuf pnm;
	char *s, *tail;
	unsigned char *dest;
	int i, l, m, w, h, bpp, maxval, plain, mode, fid, res;


	/* Identify*/
	memset(&pnm, 0, sizeof(pnm));
	pnm.f = fp;
	fid = settings->ftype == FT_PBM ? 0 : settings->ftype == FT_PGM ? 1 : 2;
	if (!(s = pnm_gets(&pnm, FALSE))) return (-1);
	if ((s[0] != 'P') || ((s[1] != fid + '1') && (s[1] != fid + '4')))
		 return (-1);
	plain = s[1] < '4';

	/* Read header */
	if (!(s = pnm_gets(&pnm, FALSE))) return (-1);
	w = strtol(s, &tail, 10);
	if (*tail) return (-1);
	if (!(s = pnm_gets(&pnm, FALSE))) return (-1);
	h = strtol(s, &tail, 10);
	if (*tail) return (-1);
	bpp = maxval = 1;
	if (settings->ftype == FT_PBM) set_bw(settings);
	else
	{
		if (!(s = pnm_gets(&pnm, FALSE))) return (-1);
		maxval = strtol(s, &tail, 10);
		if (*tail) return (-1);
		if ((maxval <= 0) || (maxval > 65535)) return (-1);
		if (settings->ftype == FT_PGM) set_gray(settings);
		else bpp = 3;
	}
	if (!pnm_endhdr(&pnm, plain)) return (-1);

	/* Store values */
	settings->width = w;
	settings->height = h;
	settings->bpp = bpp;

	/* Allocate image */
	if ((res = allocate_image(settings, CMASK_IMAGE))) return (res);

	/* Now, read the image */
	mode = settings->ftype == FT_PBM ? plain /* 0 and 1 */ :
		plain ? 2 : maxval < 255 ? 3 : maxval > 255 ? 4 : 5;
	s = "";
	if (!settings->silent) ls_init("PNM", 0);
	res = FILE_LIB_ERROR;
	l = w * bpp;
	m = maxval * 2;
	for (i = 0; i < h; i++)
	{
		dest = settings->img[CHN_IMAGE] + l * i;
		switch (mode)
		{
		case 0: /* Raw packed bits */
		{
#if PNM_BUFSIZE * 8 < MAX_WIDTH
#error "Buffer too small to read PBM row all at once"
#endif
			int i, j, k;
			unsigned char *tp = pnm.buf;

			k = (w + 7) >> 3;
			j = fread(tp, 1, k, fp);
			for (i = 0; i < w; i++)
				*dest++ = (tp[i >> 3] >> (~i & 7)) & 1;
			if (j < k) goto fail2;
			break;
		}
		case 3: /* Raw byte values - extend later */
		case 5: /* Raw 0..255 values - trivial */
			if (fread(dest, 1, l, fp) < l) goto fail2;
			break;
		case 1: /* Chars "0" and "1" */
		{
			int i;
			unsigned char ch;

			for (i = 0; i < l; i++)
			{
				if (!s[0] && !(s = pnm_gets(&pnm, TRUE)))
					goto fail2;
				ch = *s++ - '0';
				if (ch > 1) goto fail2;
				*dest++ = ch;
			}
			break;
		}
		case 2: /* Integers in ASCII */
		{
			int i, n;

			for (i = 0; i < l; i++)
			{
				if (!(s = pnm_gets(&pnm, TRUE))) goto fail2;
				n = strtol(s, &tail, 10);
				if (*tail) goto fail2;
				if ((n < 0) || (n > maxval)) goto fail2;
				n = (n * (255 * 2) + maxval) / m;
				*dest++ = n;
			}
			break;
		}
		case 4: /* Raw ushorts in MSB order */
		{
			int i, j, k, ll;

			for (ll = l * 2; ll > 0; ll -= k)
			{
				k = PNM_BUFSIZE < ll ? PNM_BUFSIZE : ll;
				j = fread(pnm.buf, 1, k, fp);
				i = j >> 1;
				convert_16b(dest, pnm.buf, i, 1, 1, maxval);
				dest += i;
				if (j < k) goto fail2;
			}
			break;
		}
		}
		ls_progress(settings, i, 10);
	}
	res = 1;

	/* Check for next frame */
	if (!plain) res = check_next_pnm(fp, fid + '4');

fail2:	if (mode == 3) // Extend what we've read
		extend_bytes(settings->img[CHN_IMAGE], l * h, maxval);
	if (!settings->silent) progress_end();

	return (res);
}

static int load_pnm_frames(char *file_name, ani_settings *ani)
{
	FILE *fp;
	ls_settings w_set;
	int res, is_pam = ani->settings.ftype == FT_PAM, next = TRUE;


	if (!(fp = fopen(file_name, "rb"))) return (-1);
	while (next)
	{
		res = FILE_TOO_LONG;
		if (!check_next_frame(&ani->fset, ani->settings.mode, FALSE))
			goto fail;
		w_set = ani->settings;
		w_set.gif_delay = -1; // Multipage
		res = (is_pam ? load_pam_frame : load_pnm_frame)(fp, &w_set);
		next = res == FILE_HAS_FRAMES;
		if ((res != 1) && !next) goto fail;
		res = process_page_frame(file_name, ani, &w_set);
		if (res) goto fail;
	}
	res = 1;
fail:	fclose(fp);
	return (res);
}

static int load_pnm(char *file_name, ls_settings *settings)
{
	FILE *fp;
	int res;

	if (!(fp = fopen(file_name, "rb"))) return (-1);
	res = (settings->ftype == FT_PAM ? load_pam_frame :
		load_pnm_frame)(fp, settings);
	fclose(fp);
	return (res);
}

static int save_pbm(char *file_name, ls_settings *settings)
{
	unsigned char buf[MAX_WIDTH / 8], bw, *src;
	FILE *fp;
	int i, l, w = settings->width, h = settings->height;


	if ((settings->bpp != 1) || (settings->colors > 2)) return WRONG_FORMAT;

	if (!(fp = fopen(file_name, "wb"))) return (-1);

	if (!settings->silent) ls_init("PBM", 1);
	fprintf(fp, "P4\n%d %d\n", w, h);

	bw = get_bw(settings);

	/* Write rows */
	src = settings->img[CHN_IMAGE];
	l = (w + 7) >> 3;
	for (i = 0; i < h; i++)
	{
		pack_MSB(buf, src, w, bw);
		src += w;
		fwrite(buf, l, 1, fp);
		ls_progress(settings, i, 20);
	}
	fclose(fp);

	if (!settings->silent) progress_end();

	return (0);
}

static int save_ppm(char *file_name, ls_settings *settings)
{
	FILE *fp;
	int i, l, m, w = settings->width, h = settings->height;


	if (settings->bpp != 3) return WRONG_FORMAT;

	if (!(fp = fopen(file_name, "wb"))) return (-1);

	if (!settings->silent) ls_init("PPM", 1);
	fprintf(fp, "P6\n%d %d\n255\n", w, h);

	/* Write rows */
	m = (l = w * 3) * h;
	// Write entire file at once if no progressbar
	if (settings->silent) l = m;
	for (i = 0; m > 0; m -= l , i++)
	{
		fwrite(settings->img[CHN_IMAGE] + l * i, l, 1, fp);
		ls_progress(settings, i, 20);
	}
	fclose(fp);

	if (!settings->silent) progress_end();

	return (0);
}

static int save_pam(char *file_name, ls_settings *settings)
{
	unsigned char xv, xa, *dest, *src, *srca, *buf = NULL;
	FILE *fp;
	int ibpp = settings->bpp, w = settings->width, h = settings->height;
	int i, j, bpp;


	if ((ibpp != 3) && (settings->colors > 2)) return WRONG_FORMAT;

	bpp = ibpp + !!settings->img[CHN_ALPHA];
	/* For BW: image XOR 1 if white is 0, alpha AND 1 */
	xv = 0; xa = 255;
	if (ibpp == 1) xv = get_bw(settings) , xa = 1;
	if (bpp != 3) // BW needs inversion, and alpha, interlacing
	{
		buf = malloc(w * bpp);
		if (!buf) return (-1);
	}

	if (!(fp = fopen(file_name, "wb")))
	{
		free(buf);
		return (-1);
	}

	if (!settings->silent) ls_init("PAM", 1);
	fprintf(fp, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH %d\nMAXVAL %d\n"
		"TUPLTYPE %s%s\nENDHDR\n", w, h, bpp, ibpp == 1 ? 1 : 255,
		ibpp == 1 ? "BLACKANDWHITE" : "RGB", bpp > ibpp ? "_ALPHA" : "");

	for (i = 0; i < h; i++)
	{
		src = settings->img[CHN_IMAGE] + i * w * ibpp;
		if ((dest = buf))
		{
			srca = NULL;
			if (settings->img[CHN_ALPHA])
				srca = settings->img[CHN_ALPHA] + i * w;
			for (j = 0; j < w; j++)
			{
				*dest++ = *src++ ^ xv;
				if (ibpp > 1)
				{
					*dest++ = *src++;
					*dest++ = *src++;
				}
				if (srca) *dest++ = *srca++ & xa;
			}
			src = buf;
		}
		fwrite(src, 1, w * bpp, fp);
		ls_progress(settings, i, 20);
	}
	fclose(fp);

	if (!settings->silent) progress_end();
	free(buf);

	return (0);
}

/* *** PREFACE ***
 * PMM is mtPaint's own format, extending the PAM format in a compatible way;
 * PAM tools from Netpbm can easily split a PMM file into regular PAM files, or
 * build it back from these. Some extra values are stored inside the "TUPLTYPE"
 * fields, because Netpbm tools do NOT preserve comments but don't much care
 * about TUPLTYPE. Other things, like palette, are stored as separate
 * pseudo-images preceding the bitmap, with their own TUPLTYPE and extra values.
 * When building a PMM file out of PAMs by hand, just write out the PMM_ID1
 * string, below, before the first PAM file - WJ */

# define PMM_ID1 "P7\n#MTPAINT#"

typedef struct {
	char *next; // the rest of string
	char *tag; // last found tag
	int val; // its value if any
} tagline;

/* Parse a tag out of string */
static int nexttag(tagline *iter, int split_under)
{
	char *s, *tail, *str = iter->next;
	int l, n, res = 1;

	iter->tag = str;
	if (!str || !*str) return (0); // Empty
	l = strcspn(str, "_=" WHITESPACE + !split_under);
	if (!l) return (0); // No tag here - format violation
	s = str + l;
	if (*s == '=') /* NAME=VALUE */
	{
		n = strtol(++s, &tail, 10);
		if ((tail == s) || (*tail && !strchr(WHITESPACE, *tail)))
			return (0); // Unparsable or no value - format violation
		iter->val = n;
		s = tail;
		res = 2; // Have value
	}
	else s += (*s == '_'); /* NAME_ */

	iter->next = s + strspn(s, WHITESPACE);
	str[l] = '\0';

	return (res); // Parsed another tag
}

/* Interpret known value tags, ignore unknown ones */
static void readtags(tagline *tl, ls_settings *settings, int bpp)
{
	static const char *tags[] = { "TRANS", "DELAY", "X", "Y", NULL };
	int j, i = 2;

	// Skip channel tags if no extra channels
	if (!bpp) while ((i = nexttag(tl, FALSE)) == 1);

	while (i == 2)
	{
		for (j = 0; tags[j] && strcmp(tl->tag, tags[j]); j++);
		i = tl->val;

		switch (j)
		{
		case 0: // Transparent color
			/* Invalid value - ignore */
			if (i < -1) break;
			/* No transparency - disable */
			if (i == -1) settings->xpm_trans =
				settings->rgb_trans = -1;
			/* Indexed transparency */
			else if (bpp < 3)
			{
				if (i < settings->colors) settings->xpm_trans = i;
			}
			/* RGB transparency */
			else if (i <= 0xFFFFFF)
			{
				int j = settings->xpm_trans;
				png_color *p = settings->pal + j;
				// Only if differs from indexed
				if ((j < 0) || (PNG_2_INT(*p) != i))
					settings->rgb_trans = i;
			}
			break;
		case 1: // Anim delay, in 0.01 sec
			if (i >= 0) settings->gif_delay = i;
			break;
		case 2: // X offset
			settings->x = i;
			break;
		case 3: // Y offset
			settings->y = i;
			break;
		// !!! No other parameters yet
		}
		i = nexttag(tl, FALSE);
	}
}

static int load_pmm_frame(memFILE *mf, ls_settings *settings)
{
	/* !!! INDEXED is at index 1, RGB at index 3 to use index as BPP */
	static const char *blocks[] = { "TAGS", "INDEXED", "PALETTE", "RGB", NULL };
	tagline tl;
	unsigned char *dest, *buf = NULL;
	char *ttype = NULL;
	int w, h, depth, rgbpp, cmask = CMASK_IMAGE;
	int i, j, l, res, whdm[4], slots[NUM_CHANNELS];

	while (TRUE)
	{
		res = -1;
		free(ttype);
		if (!(tl.next = ttype = pam_behead(mf, whdm))) break;
		if (whdm[3] > 255) break; // 16-bit values not allowed
		depth = whdm[2];
		if (depth > 16) break; // Depth limited to sane values
		w = whdm[0]; h = whdm[1];

		/* Parse out type tag */
		j = -1;
		if (nexttag(&tl, TRUE) == 1)
		{
			for (j = 0; blocks[j] && strcmp(blocks[j], tl.tag); j++);
			if (!blocks[j]) j = -1;
		}

		if (!j) readtags(&tl, settings, 0); /* TAGS */

		if (j <= 0) /* !!! IGNORE anything unrecognized & skip "TAGS" */
		{
			mfseek(mf, w * h * depth, SEEK_CUR);
			continue;
		}

		if (j == 2) /* PALETTE */
		{
			unsigned char pbuf[256 * 16], *tp = pbuf;

			/* Validate */
			if ((depth < 3) || (w < 2) || (w > 256) || (h != 1)) break;
			settings->colors = w;
			settings->xpm_trans = settings->rgb_trans = -1; // Default

			/* Skip channel tags, interpret value tags */
			readtags(&tl, settings, 0);

			if (mfread(pbuf, depth, w, mf) != w) break; // Failed
			/* Store palette */
			extend_bytes(tp, w * depth, whdm[3]);
			for (i = 0; i < w; i++)
			{
				settings->pal[i].red = tp[0];
				settings->pal[i].green = tp[1];
				settings->pal[i].blue = tp[2];
				tp += depth;
			}
			/* If palette is all we need */
			res = EXPLODE_FAILED;
			if ((settings->mode == FS_PALETTE_LOAD) ||
				(settings->mode == FS_PALETTE_DEF)) break;
			continue;
		}

		/* Got an image bitmap */
		// !!! Only slots 1 & 3 fall through to here
		rgbpp = j;
		/* Add up extra channels */
		memset(slots, 0, sizeof(slots));
		while ((i = nexttag(&tl, FALSE)) == 1)
		{
			if (!strcmp(tl.tag, "ALPHA")) i = CHN_ALPHA;
			else if (!strcmp(tl.tag, "SELECTION")) i = CHN_SEL;
			else if (!strcmp(tl.tag, "MASK")) i = CHN_MASK;
			else // Unknown channel - skip
			{
				j++;
				continue;
			}
			slots[i] = j++;
			cmask |= CMASK_FOR(i);
		}
		if (j > depth) break; // Cannot be

		/* Interpret value tags */
		if (i == 2) readtags(&tl, settings, rgbpp);

		l = w * depth;
		/* Allocate row buffer if cannot read directly into image */
		if (rgbpp != depth)
		{
			res = FILE_MEM_ERROR;
			if (!(buf = malloc(l))) break;
		}
		/* Allocate image */
		settings->width = w;
		settings->height = h;
		settings->bpp = rgbpp;
		if ((res = allocate_image(settings, cmask))) break;

		/* Read the image */
		if (!settings->silent) ls_init("* PMM *", 0);
		res = FILE_LIB_ERROR;
		for (i = 0; i < h; i++)
		{
			dest = settings->img[CHN_IMAGE] + w * rgbpp * i;
			if (!mfread(buf ? buf : dest, l, 1, mf)) goto fail;
			ls_progress(settings, i, 10);
			if (!buf) continue; // Nothing else to do here

			copy_bytes(dest, buf, w, rgbpp, depth);
			for (j = CHN_ALPHA; j < NUM_CHANNELS; j++)
				if (settings->img[j]) copy_bytes(
					settings->img[j] + w * i,
					buf + slots[j], w, 1, depth);
		}

		/* Extend what we've read */
		if (whdm[3] < 255)
		{
			i = w * h * rgbpp;
			for (j = CHN_IMAGE; j < NUM_CHANNELS; j++)
			{
				if (settings->img[j]) extend_bytes(
					settings->img[j], i, whdm[3]);
				i = w * h;
			}
		}

		/* Check for next frame */
		res = 1;
		if (mfread(ttype, 2, 1, mf)) // it was no shorter than "RGB"
		{
			mfseek(mf, -2, SEEK_CUR);
			if (!strncmp(ttype, "P7", 2)) res = FILE_HAS_FRAMES;
		}

fail:		if (!settings->silent) progress_end();
		break;
	}
	free(buf);
	free(ttype);
	return (res);
}

static int load_pmm_frames(char *file_name, ani_settings *ani, memFILE *mf)
{
	memFILE fake_mf;
	FILE *fp = NULL;
	ls_settings w_set, init_set;
	int res, next;


	if (!mf)
	{
		if (!(fp = fopen(file_name, "rb"))) return (-1);
		memset(mf = &fake_mf, 0, sizeof(fake_mf));
		fake_mf.file = fp;
	}
	init_set = ani->settings;
	init_set.gif_delay = -1; // Multipage by default
	while (TRUE)
	{
		w_set = init_set;
		res = load_pmm_frame(mf, &w_set);
		next = res == FILE_HAS_FRAMES;
		if ((res != 1) && !next) break;
		/* !!! RGB transparency may modify the palette */
		map_rgb_trans(&w_set);
		if ((res = process_page_frame(file_name, ani, &w_set))) break;
		res = 1;
		if (!next) break;
		res = FILE_TOO_LONG;
		if (!check_next_frame(&ani->fset, ani->settings.mode,
			w_set.gif_delay >= 0)) break;
		/* Update initial values */
		init_set.colors = w_set.colors; // Palettes are inheritable
		init_set.xpm_trans = w_set.xpm_trans;
		init_set.rgb_trans = w_set.rgb_trans;
		init_set.gif_delay = w_set.gif_delay;
	}
	fclose(fp);
	return (res);
}

static int load_pmm(char *file_name, ls_settings *settings, memFILE *mf)
{
	memFILE fake_mf;
	FILE *fp = NULL;
	int res;

	if (!mf)
	{
		if (!(fp = fopen(file_name, "rb"))) return (-1);
		memset(mf = &fake_mf, 0, sizeof(fake_mf));
		fake_mf.file = fp;
	}
	res = load_pmm_frame(mf, settings);
	if (fp) fclose(fp);
	return (res);
}

static int save_pmm(char *file_name, ls_settings *settings, memFILE *mf)
{
	unsigned char *dest, *src, *buf = NULL;
	unsigned char sbuf[768];
	memFILE fake_mf;
	FILE *fp = NULL;
	int rgbpp = settings->bpp, w = settings->width, h = settings->height;
	int i, k, bpp;


	for (i = bpp = 0; i < NUM_CHANNELS; i++) bpp += !!settings->img[i];
	bpp += rgbpp - 1;
	/* Allocate row buffer if needed */
	if ((bpp != rgbpp) && (settings->mode != FS_PALETTE_SAVE))
	{
		buf = malloc(w * bpp);
		if (!buf) return (-1);
	}

	if (!mf)
	{
		if (!(fp = fopen(file_name, "wb")))
		{
			free(buf);
			return (-1);
		}
		memset(mf = &fake_mf, 0, sizeof(fake_mf));
		fake_mf.file = fp;
	}

	if (!settings->silent) ls_init("* PMM *", 1);

	/* First, write palette */
	if (settings->pal)
	{
		mfputs(PMM_ID1 "\n", mf);
		snprintf(sbuf, sizeof(sbuf), "WIDTH %d\n", settings->colors);
		mfputs(sbuf, mf);
		// Extra data for palette: transparent index if any
		sbuf[0] = '\0';
		if (settings->xpm_trans >= 0) snprintf(sbuf, sizeof(sbuf),
			" TRANS=%d", settings->xpm_trans);
		mfputss(mf, "HEIGHT 1\nDEPTH 3\nMAXVAL 255\nTUPLTYPE PALETTE",
			sbuf, "\nENDHDR\n", NULL);
		pal2rgb(sbuf, settings->pal, settings->colors, 0);
		mfwrite(sbuf, 1, settings->colors * 3, mf);
	}
	/* All done if only writing palette */
	if (settings->mode == FS_PALETTE_SAVE) goto done;

	/* Now, write image bitmap */
	mfputs(PMM_ID1 "\n", mf);
	snprintf(sbuf, sizeof(sbuf), "WIDTH %d\nHEIGHT %d\nDEPTH %d\n",
		w, h, bpp);
	mfputss(mf, sbuf, "MAXVAL 255\nTUPLTYPE ",
		rgbpp > 1 ? "RGB" : "INDEXED",
		settings->img[CHN_ALPHA] ? "_ALPHA" : "",
		settings->img[CHN_SEL] ? " SELECTION" : "",
		settings->img[CHN_MASK] ? " MASK" : "",
		"\nENDHDR\n", NULL);

	for (i = 0; i < h; i++)
	{
		src = settings->img[CHN_IMAGE] + i * w * rgbpp;
		if ((dest = buf))
		{
			copy_bytes(dest, src, w, bpp, rgbpp);
			dest += rgbpp;
			for (k = CHN_ALPHA; k < NUM_CHANNELS; k++)
				if (settings->img[k]) copy_bytes(dest++,
					settings->img[k] + i * w, w, bpp, 1);
			src = buf;
		}
		mfwrite(src, 1, w * bpp, mf);
		ls_progress(settings, i, 20);
	}
done:	if (fp) fclose(fp);

	if (!settings->silent) progress_end();

	free(buf);
	return 0;
}

/* Put screenshots and X pixmaps on an equal footing with regular files */

#ifdef HAVE_PIXMAPS

static int save_pixmap(ls_settings *settings, memFILE *mf)
{
	pixmap_info p;
	unsigned char *src, *dest, *sel, *buf = NULL;
	int i, j, l, w = settings->width, h = settings->height;

	/* !!! Pixmap export used only for FS_CLIPBOARD, where the case of
	 * selection without alpha is already prevented */
	if ((settings->bpp == 1) || settings->img[CHN_ALPHA])
	{
		buf = malloc(w * 3);
		if (!buf) return (-1);
	}

	if (!export_pixmap(&p, w, h))
	{
		free(buf);
		return (-1);
	}

	/* Plain RGB - copy it whole */
	if (!buf) pixmap_put_rows(&p, settings->img[CHN_IMAGE], 0, h);

	/* Something else - render & copy row by row */
	else
	{
		l = w * settings->bpp;
		for (i = 0; i < h; i++)
		{
			src = settings->img[CHN_IMAGE] + l * i;
			if (settings->bpp == 3) memcpy(buf, src, l);
			else do_convert_rgb(0, 1, w, buf, src, settings->pal);
			/* There is no way to send alpha to XPaint, so I use
			 * alpha (and selection if any) to blend image with
			 * white and send the result - WJ */
			if (settings->img[CHN_ALPHA])
			{
				src = settings->img[CHN_ALPHA] + w * i;
				sel = settings->img[CHN_SEL] ?
					settings->img[CHN_SEL] + w * i : NULL;
				dest = buf;
				for (j = 0; j < w; j++)
				{
					int ii, jj, k = *src++;

					if (sel)
					{
						k *= *sel++;
						k = (k + (k >> 8) + 1) >> 8;
					}
					for (ii = 0; ii < 3; ii++)
					{
						jj = 255 * 255 + (*dest - 255) * k;
						*dest++ = (jj + (jj >> 8) + 1) >> 8;
					}
				}
			}
			pixmap_put_rows(&p, buf, i, 1);
		}
		free(buf);
	}

	*(XID_type *)mf->m.buf = p.xid;
	mf->top = sizeof(XID_type);
	return (0);
}

#else /* Pixmap export fails by definition in absence of X */
#define save_pixmap(A,B) (-1)
#endif

static int load_pixmap(ls_settings *settings, memFILE *mf)
{
	pixmap_info p;
	int res = -1;

	if (import_pixmap(&p, mf ? (void *)mf->m.buf : NULL)) // !mf == screenshot
	{
		settings->width = p.w;
		settings->height = p.h;
		settings->bpp = 3;
		res = allocate_image(settings, CMASK_IMAGE);
		if (!res) res = pixmap_get_rows(&p,
			settings->img[CHN_IMAGE], 0, p.h) ? 1 : -1;
		drop_pixmap(&p);
	}
	return (res);
}

/* Handle SVG import using gdk-pixbuf */

#if (GDK_PIXBUF_MAJOR > 2) || ((GDK_PIXBUF_MAJOR == 2) && (GDK_PIXBUF_MINOR >= 4))

#define MAY_HANDLE_SVG

static int svg_check = -1;

static int svg_supported()
{
	GSList *tmp, *ff;
	int i, res = FALSE;

	ff = gdk_pixbuf_get_formats();
	for (tmp = ff; tmp; tmp = tmp->next)
	{
		gchar **mime = gdk_pixbuf_format_get_mime_types(tmp->data);

		for (i = 0; mime[i]; i++)
		{
			res |= strstr(mime[i], "image/svg") == mime[i];
		}
		g_strfreev(mime);
		if (res) break;
	} 
	g_slist_free(ff);
	return (res);
}

static int load_svg(char *file_name, ls_settings *settings)
{
	GdkPixbuf *pbuf;
	GError *err = NULL;
	guchar *src;
	unsigned char *dest, *dsta;
	int i, j, w, h, bpp, cmask, skip, res = -1;


#if (GDK_PIXBUF_MAJOR == 2) && (GDK_PIXBUF_MINOR < 8)
	/* 2.4 can constrain size only while preserving aspect ratio;
	 * 2.6 can constrain size fully, but not partially */
	if (settings->req_w && settings->req_h)
		pbuf = gdk_pixbuf_new_from_file_at_scale(file_name,
			settings->req_w, settings->req_h, FALSE, &err);
	else pbuf = gdk_pixbuf_new_from_file(file_name, &err);
#else
	/* 2.8+ is full-featured */
	pbuf = gdk_pixbuf_new_from_file_at_scale(file_name,
		settings->req_w ? settings->req_w : -1,
		settings->req_h ? settings->req_h : -1,
		!(settings->req_w && settings->req_h), &err);
#endif
	if (!pbuf)
	{
		if ((err->domain == GDK_PIXBUF_ERROR) &&
			(err->code == GDK_PIXBUF_ERROR_INSUFFICIENT_MEMORY))
			res = FILE_MEM_ERROR;
		g_error_free(err);
		return (res);
	}
	/* Prevent images loading wrong in case gdk-pixbuf ever starts using
	 * something other than 8-bit RGB/RGBA without me noticing - WJ */
	if (gdk_pixbuf_get_bits_per_sample(pbuf) != 8) goto fail;

	bpp = gdk_pixbuf_get_n_channels(pbuf);
	if (bpp == 4) cmask = CMASK_RGBA;
	else if (bpp == 3) cmask = CMASK_IMAGE;
	else goto fail;
	settings->width = w = gdk_pixbuf_get_width(pbuf);
	settings->height = h = gdk_pixbuf_get_height(pbuf);
	settings->bpp = 3;
	if ((res = allocate_image(settings, cmask))) goto fail;

	skip = gdk_pixbuf_get_rowstride(pbuf) - w * bpp;
	src = gdk_pixbuf_get_pixels(pbuf);
	dest = settings->img[CHN_IMAGE];
	dsta = settings->img[CHN_ALPHA];
	for (i = 0; i < h; i++ , src += skip)
	for (j = 0; j < w; j++ , src += bpp , dest += 3)
	{
		dest[0] = src[0];
		dest[1] = src[1];
		dest[2] = src[2];
		if (dsta) *dsta++ = src[3];
	}
	res = 1;

	/* Delete all-set "alpha" */
	delete_alpha(settings, 255);

fail:	g_object_unref(pbuf);
	return (res);
}

#endif

/* Handle SVG import using rsvg-convert */

static int import_svg(char *file_name, ls_settings *settings)
{
	da_settings ds;
	char buf[PATHBUF];
	int res = -1;

	if (!get_tempname(buf, file_name, FT_PNG)) return (-1);
	memset(&ds, 0, sizeof(ds));
	ds.sname = file_name;
	ds.dname = buf;
	ds.width = settings->req_w;
	ds.height = settings->req_h;
	if (!run_def_action_x(DA_SVG_CONVERT, &ds))
		res = load_png(buf, settings, NULL, FALSE);
	unlink(buf);

	/* Delete all-set "alpha" */
	if (res == 1) delete_alpha(settings, 255);

	return (res);
}

/* Handle textual palette file formats - GIMP's GPL and mtPaint's own TXT */

static void to_pal(png_color *c, int *rgb)
{
	c->red = rgb[0] < 0 ? 0 : rgb[0] > 255 ? 255 : rgb[0];
	c->green = rgb[1] < 0 ? 0 : rgb[1] > 255 ? 255 : rgb[1];
	c->blue = rgb[2] < 0 ? 0 : rgb[2] > 255 ? 255 : rgb[2];
}

static int load_txtpal(char *file_name, ls_settings *settings)
{
	char lbuf[4096];
	FILE *fp;
	png_color *c = settings->pal;
	int i, rgb[3], n = 0, res = -1;


	if (!(fp = fopen(file_name, "r"))) return (-1);
	if (!fgets(lbuf, 4096, fp)) goto fail;
	if (settings->ftype == FT_GPL)
	{
		if (strstr(lbuf, "GIMP Palette") != lbuf) goto fail;
		while (fgets(lbuf, 4096, fp) && (n < 256))
		{
			/* Just ignore invalid/unknown lines */
			if (sscanf(lbuf, "%d %d %d", rgb + 0, rgb + 1, rgb + 2) != 3)
				continue;
			to_pal(c++, rgb);
			n++;
		}
	}
	else
	{
		if (sscanf(lbuf, "%i", &n) != 1) goto fail;
		/* No further validation of anything at all */
		n = n < 2 ? 2 : n > 256 ? 256 : n;
		for (i = 0; i < n; i++)
		{
			fscanf(fp, "%i,%i,%i\n", rgb + 0, rgb + 1, rgb + 2);
			to_pal(c++, rgb);
		}
	}
	settings->colors = n;
	if (n > 0) res = 1;

fail:	fclose(fp);
	return (res);
}

static int save_txtpal(char *file_name, ls_settings *settings)		
{
	FILE *fp;
	char *tpl;
	png_color *cp;
	int i, l, n = settings->colors;

	if ((fp = fopen(file_name, "w")) == NULL) return (-1);

	if (settings->ftype == FT_GPL)	// .gpl file
	{
		tpl = extract_ident(file_name, &l);
		if (!l) tpl = "mtPaint" , l = strlen("mtPaint");
		fprintf(fp, "GIMP Palette\nName: %.*s\nColumns: 16\n#\n", l, tpl);
		tpl = "%3i %3i %3i\tUntitled\n";
	}
	else // .txt file
	{
		fprintf(fp, "%i\n", n);
		tpl = "%i,%i,%i\n";
	}

	cp = settings->pal;
	for (i = 0; i < n; i++ , cp++)
		fprintf(fp, tpl, cp->red, cp->green, cp->blue);

	fclose(fp);
	return (0);
}

/* Handle raw palette file formats - 6-bit PAL and 8-bit ACT */

static int load_rawpal(char *file_name, ls_settings *settings)
{
	unsigned char buf[769], xlat[256], *tp;
	FILE *fp;
	char *stop;
	int i, l, ftype;


	memset(buf, 0, sizeof(buf));
	if (!(fp = fopen(file_name, "rb"))) return (-1);
	l = fread(buf, 1, 769, fp);
	fclose(fp);
	if (!l || (l > 768) || (l % 3)) return (-1); // Wrong size
	l /= 3;

	/* !!! Filetype in ls_settings is ignored */
	ftype = FT_NONE;
	if ((stop = strrchr(file_name, '.')))
	{
		if (!strcasecmp(stop + 1, "act"))
		{
			if (l != 256) return (-1);
			ftype = FT_ACT;
		}
		else if (!strcasecmp(stop + 1, "pal"))
			ftype = FT_PAL;
	}
	if (l < 256) ftype = FT_PAL;

	if (ftype != FT_ACT) // Default to 6-bit
	{
		set_xlate(xlat, 6);
		for (i = 64; i < 255; i++) xlat[i] = xlat[i - 64];
	}
	else set_xlate(xlat, 8); // 1:1

	for (i = 0 , tp = buf; i < l; i++)
	{
		settings->pal[i].red = xlat[tp[0]];
		settings->pal[i].green = xlat[tp[1]];
		settings->pal[i].blue = xlat[tp[2]];
		tp += 3;
	}
	settings->colors = l;

	return (1);
}

static int save_rawpal(char *file_name, ls_settings *settings)		
{
	FILE *fp;
	unsigned char buf[768], xlat[256], *tp;
	png_color *cp;
	int i, n = settings->colors;

	if (!(fp = fopen(file_name, "wb"))) return (-1);

	memset(buf, 0, 768);
	if (settings->ftype == FT_PAL) // 6-bit
		for (i = 0; i < 256; i++)
			xlat[i] = (63 * 2 * i + 255) / (255 * 2);
	else for (i = 0; i < 256; i++) xlat[i] = i; // 8-bit ACT

	cp = settings->pal;
	for (i = 0 , tp = buf; i < n; i++ , cp++)
	{
		tp[0] = xlat[cp->red];
		tp[1] = xlat[cp->green];
		tp[2] = xlat[cp->blue];
		tp += 3;
	}
	if (settings->ftype != FT_PAL) n = 256;
	i = fwrite(buf, n * 3, 1, fp);
	fclose(fp);

	return (i ? 0 : -1);
}

static int save_image_x(char *file_name, ls_settings *settings, memFILE *mf)
{
	ls_settings setw = *settings; // Make a copy to safely modify
	png_color greypal[256];
	int res;

	/* Prepare to handle clipboard export */
	if (setw.mode != FS_CLIPBOARD); // not export
	else if (setw.ftype & FTM_EXTEND) setw.mode = FS_CLIP_FILE; // to mtPaint
	else if (setw.img[CHN_SEL] && !setw.img[CHN_ALPHA])
	{
		/* Pass clipboard mask as alpha if there is no alpha already */
		setw.img[CHN_ALPHA] = setw.img[CHN_SEL];
		setw.img[CHN_SEL] = NULL;
	}
	setw.ftype &= FTM_FTYPE;

	/* Be silent if only writing palette */
	if (setw.mode == FS_PALETTE_SAVE) setw.silent = TRUE;

	/* Provide a grayscale palette if needed */
	if ((setw.bpp == 1) && !setw.pal)
		mem_bw_pal(setw.pal = greypal, 0, 255);

	/* Validate transparent color (for now, forbid out-of-palette RGB
	 * transparency altogether) */
	if (setw.colors && (setw.xpm_trans >= setw.colors))
		setw.xpm_trans = setw.rgb_trans = -1;

	switch (setw.ftype)
	{
	default:
	case FT_PNG: res = save_png(file_name, &setw, mf); break;
	case FT_GIF: res = save_gif(file_name, &setw); break;
#ifdef U_JPEG
	case FT_JPEG: res = save_jpeg(file_name, &setw); break;
#endif
#ifdef HANDLE_JP2
	case FT_JP2:
	case FT_J2K: res = save_jpeg2000(file_name, &setw); break;
#endif
#ifdef U_TIFF
	case FT_TIFF: res = save_tiff(file_name, &setw, mf); break;
#endif
#ifdef U_WEBP
	case FT_WEBP: res = save_webp(file_name, &setw); break;
#endif
	case FT_BMP: res = save_bmp(file_name, &setw, mf); break;
	case FT_XPM: res = save_xpm(file_name, &setw); break;
	case FT_XBM: res = save_xbm(file_name, &setw); break;
	case FT_LSS: res = save_lss(file_name, &setw); break;
	case FT_TGA: res = save_tga(file_name, &setw); break;
	case FT_PCX: res = save_pcx(file_name, &setw); break;
	case FT_LBM: res = save_lbm(file_name, &setw); break;
	case FT_PBM: res = save_pbm(file_name, &setw); break;
	case FT_PPM: res = save_ppm(file_name, &setw); break;
	case FT_PAM: res = save_pam(file_name, &setw); break;
	case FT_PMM: res = save_pmm(file_name, &setw, mf); break;
	case FT_PIXMAP: res = save_pixmap(&setw, mf); break;
	/* Palette files */
	case FT_GPL:
	case FT_TXT: res = save_txtpal(file_name, &setw); break;
	case FT_PAL:
	case FT_ACT: res = save_rawpal(file_name, &setw); break;
	}

	return (res);
}

int save_image(char *file_name, ls_settings *settings)
{
	return (save_image_x(file_name, settings, NULL));
}

int save_mem_image(unsigned char **buf, int *len, ls_settings *settings)
{
	memFILE mf;
	int res;

	memset(&mf, 0, sizeof(mf));
	if ((settings->ftype & FTM_FTYPE) == FT_PIXMAP)
		mf.m.buf = malloc(sizeof(XID_type)); // Expect to know type here
	else if (!(file_formats[settings->ftype & FTM_FTYPE].flags & FF_WMEM))
		return (-1);
	else mf.m.buf = malloc(mf.m.size = 0x4000 - 64);
	/* Be silent when saving to memory */
	settings->silent = TRUE;
	res = save_image_x(NULL, settings, &mf);
	if (res) free(mf.m.buf);
	else *buf = mf.m.buf , *len = mf.top;
	return (res);
}

static void store_image_extras(image_info *image, image_state *state,
	ls_settings *settings)
{
#if U_LCMS
	/* Apply ICC profile */
	while (settings->icc_size > 0)
	{
		cmsHPROFILE from, to;
		cmsHTRANSFORM how = NULL;
		int l = settings->icc_size - sizeof(icHeader);
		unsigned char *iccdata = settings->icc + sizeof(icHeader);

		/* Do nothing if the profile seems to be the default sRGB one */
		if ((l == 3016) && (hashf(HASHSEED, iccdata, l) == 0xBA0A8E52UL) &&
			(hashf(HASH_RND(HASHSEED), iccdata, l) == 0x94C42C77UL)) break;

		from = cmsOpenProfileFromMem((void *)settings->icc,
			settings->icc_size);
		to = cmsCreate_sRGBProfile();
		if (from && (cmsGetColorSpace(from) == icSigRgbData))
			how = cmsCreateTransform(from, TYPE_RGB_8,
				to, TYPE_RGB_8, INTENT_PERCEPTUAL, 0);
		if (how && (settings->bpp == 1)) /* For GIF: apply to palette */
		{
			unsigned char tm[256 * 3];
			int l = settings->colors;

			pal2rgb(tm, settings->pal, l, 0);
			cmsDoTransform(how, tm, tm, l);
			rgb2pal(settings->pal, tm, l);

			cmsDeleteTransform(how);
		}
		else if (how)
		{
			unsigned char *img = settings->img[CHN_IMAGE];
			size_t l = settings->width, sz = l * settings->height;
			int i, j;

			if (!settings->silent)
				progress_init(_("Applying colour profile"), 1);
			else if (sz < UINT_MAX) l = sz;
			j = sz / l;
			for (i = 0; i < j; i++ , img += l * 3)
			{
				if (!settings->silent && ((i * 20) % j >= j - 20))
					if (progress_update((float)i / j)) break;
				cmsDoTransform(how, img, img, l);
			}
			progress_end();
			cmsDeleteTransform(how);
		}
		if (from) cmsCloseProfile(from);
		cmsCloseProfile(to);
		break;
	}
#endif
// !!! Changing any values is frequently harmful in this mode, so don't do it
	if (settings->mode == FS_CHANNEL_LOAD) return;

	/* Stuff RGB transparency into color 255 */
	map_rgb_trans(settings);

	/* Accept vars which make sense */
	state->xbm_hot_x = settings->hot_x;
	state->xbm_hot_y = settings->hot_y;
	if (settings->gif_delay > 0) preserved_gif_delay = settings->gif_delay;

	/* Accept palette */
	image->trans = settings->xpm_trans;
	mem_pal_copy(image->pal, settings->pal);
	image->cols = settings->colors;
}

static int load_image_x(char *file_name, memFILE *mf, int mode, int ftype,
	int rw, int rh)
{
	layer_image *lim = NULL;
	png_color pal[256];
	ls_settings settings;
	int i, tr, res, res0, undo = ftype & FTM_UNDO;


	/* Clipboard import - from mtPaint, or from something other? */
	if ((mode == FS_CLIPBOARD) && (ftype & FTM_EXTEND)) mode = FS_CLIP_FILE;
	ftype &= FTM_FTYPE;

	/* Prepare layer slot */
	if (mode == FS_LAYER_LOAD)
	{
		lim = layer_table[layers_total].image;
		if (!lim) lim = layer_table[layers_total].image =
			alloc_layer(0, 0, 1, 0, NULL);
		else if (layers_total) mem_free_image(&lim->image_, FREE_IMAGE);
		if (!lim) return (FILE_MEM_ERROR);
	}

	/* Fit scalable image into channel */
	if (mode == FS_CHANNEL_LOAD) rw = mem_width , rh = mem_height;

	init_ls_settings(&settings, NULL);
	settings.req_w = rw;
	settings.req_h = rh;
	/* Preset delay to -1, to detect animations by its changing */
	settings.gif_delay = -1;
#ifdef U_LCMS
	/* Set size to -1 when we don't want color profile */
	if (!apply_icc || ((mode == FS_CHANNEL_LOAD) ? (MEM_BPP != 3) :
		(mode != FS_PNG_LOAD) && (mode != FS_LAYER_LOAD)))
		settings.icc_size = -1;
#endif
	/* 0th layer load is just an image load */
	if ((mode == FS_LAYER_LOAD) && !layers_total) mode = FS_PNG_LOAD;
	settings.mode = mode;
	settings.ftype = ftype;
	settings.pal = pal;
	/* Clear hotspot & transparency */
	settings.hot_x = settings.hot_y = -1;
	settings.xpm_trans = settings.rgb_trans = -1;
	/* Be silent if working from memory */
	if (mf) settings.silent = TRUE;

	/* !!! Use default palette - for now */
	mem_pal_copy(pal, mem_pal_def);
	settings.colors = mem_pal_def_i;

	switch (ftype)
	{
	default:
	case FT_PNG: res0 = load_png(file_name, &settings, mf, FALSE); break;
	case FT_GIF: res0 = load_gif(file_name, &settings); break;
#ifdef U_JPEG
	case FT_JPEG: res0 = load_jpeg(file_name, &settings); break;
#endif
#ifdef HANDLE_JP2
	case FT_JP2:
	case FT_J2K: res0 = load_jpeg2000(file_name, &settings); break;
#endif
#ifdef U_TIFF
	case FT_TIFF: res0 = load_tiff(file_name, &settings, mf); break;
#endif
#ifdef U_WEBP
	case FT_WEBP: res0 = load_webp(file_name, &settings); break;
#endif
	case FT_BMP: res0 = load_bmp(file_name, &settings, mf); break;
	case FT_XPM: res0 = load_xpm(file_name, &settings); break;
	case FT_XBM: res0 = load_xbm(file_name, &settings); break;
	case FT_LSS: res0 = load_lss(file_name, &settings); break;
	case FT_TGA: res0 = load_tga(file_name, &settings); break;
	case FT_PCX: res0 = load_pcx(file_name, &settings); break;
	case FT_LBM: res0 = load_lbm(file_name, &settings); break;
	case FT_PBM:
	case FT_PGM:
	case FT_PPM:
	case FT_PAM: res0 = load_pnm(file_name, &settings); break;
	case FT_PMM: res0 = load_pmm(file_name, &settings, mf); break;
	case FT_PIXMAP: res0 = load_pixmap(&settings, mf); break;
	case FT_SVG:
#ifdef MAY_HANDLE_SVG
		if (svg_check < 0) svg_check = svg_supported();
		if (svg_check) res0 = load_svg(file_name, &settings);
		else
#endif
		res0 = import_svg(file_name, &settings); break;
	/* Palette files */
	case FT_GPL:
	case FT_TXT: res0 = load_txtpal(file_name, &settings); break;
	case FT_PAL:
	case FT_ACT: res0 = load_rawpal(file_name, &settings); break;
	}

	/* Consider animated GIF a success */
	res = res0 == FILE_HAS_FRAMES ? 1 : res0;
	/* Ignore frames beyond first if in-memory (imported clipboard) */
	if (mf) res0 = res;

	switch (mode)
	{
	case FS_PNG_LOAD: /* Image */
		/* Success, or lib failure with single image - commit load */
		if ((res == 1) || (!lim && (res == FILE_LIB_ERROR)))
		{
			if (!mem_img[CHN_IMAGE] || !undo)
				mem_new(settings.width, settings.height,
					settings.bpp, 0);
			else undo_next_core(UC_DELETE, settings.width,
				settings.height, settings.bpp, CMASK_ALL);
			memcpy(mem_img, settings.img, sizeof(chanlist));
			store_image_extras(&mem_image, &mem_state, &settings);
			update_undo(&mem_image);
			mem_undo_prepare();
			if (lim) layer_copy_from_main(0);
			/* Report whether the file is animated or multipage */
			res = res0;
			if ((res == FILE_HAS_FRAMES) &&
				/* If file contains frame delay value... */
				((settings.gif_delay >= 0) ||
				/* ...or it cannot be multipage at all... */
				!(file_formats[ftype].flags & FF_LAYER)))
				res = FILE_HAS_ANIM; /* ...then it's animated */
		}
		/* Failure */
		else
		{
			mem_free_chanlist(settings.img);
			/* If loader managed to delete image before failing */
			if (!mem_img[CHN_IMAGE]) create_default_image();
		}
		break;
	case FS_CLIPBOARD: /* Imported clipboard */
		if ((res == 1) && mem_clip_alpha && !mem_clip_mask)
		{
			/* "Alpha" likely means clipboard mask here */
			mem_clip_mask = mem_clip_alpha;
			mem_clip_alpha = NULL;
			memcpy(settings.img, mem_clip.img, sizeof(chanlist));
		}
		/* Fallthrough */
	case FS_CLIP_FILE: /* Clipboard */
		/* Convert color transparency to alpha */
		tr = settings.bpp == 3 ? settings.rgb_trans : settings.xpm_trans;
		if ((res == 1) && (tr >= 0))
		{
			/* Add alpha channel if no alpha yet */
			if (!settings.img[CHN_ALPHA])
			{
				i = settings.width * settings.height;
				/* !!! Create committed */
				mem_clip_alpha = malloc(i);
				if (mem_clip_alpha)
				{
					settings.img[CHN_ALPHA] = mem_clip_alpha;
					memset(mem_clip_alpha, 255, i);
				}
			}
			if (!settings.img[CHN_ALPHA]) res = FILE_MEM_ERROR;
			else mem_mask_colors(settings.img[CHN_ALPHA],
				settings.img[CHN_IMAGE], 0, settings.width,
				settings.height, settings.bpp, tr, tr);
		}
		/* Success - accept data */
		if (res == 1); /* !!! Clipboard data committed already */
		/* Failure needing rollback */
		else if (settings.img[CHN_IMAGE])
		{
			/* !!! Too late to restore previous clipboard */
			mem_free_image(&mem_clip, FREE_ALL);
		}
		break;
	case FS_CHANNEL_LOAD:
		/* Success - commit load */
		if (res == 1)
		{
			/* Add frame & stuff data into it */
			undo_next_core(UC_DELETE, mem_width, mem_height, mem_img_bpp,
				CMASK_CURR);
			mem_img[mem_channel] = settings.img[CHN_IMAGE];
			update_undo(&mem_image);
			if (mem_channel == CHN_IMAGE)
				store_image_extras(&mem_image, &mem_state, &settings);
			mem_undo_prepare();
		}
		/* Failure */
		else free(settings.img[CHN_IMAGE]);
		break;
	case FS_LAYER_LOAD: /* Layer */
		/* Success - commit load */
		if (res == 1)
		{
			mem_alloc_image(0, &lim->image_, settings.width,
				settings.height, settings.bpp, 0, NULL);
			memcpy(lim->image_.img, settings.img, sizeof(chanlist));
			store_image_extras(&lim->image_, &lim->state_, &settings);
			update_undo(&lim->image_);
		}
		/* Failure */
		else mem_free_chanlist(settings.img);
		break;
	case FS_PATTERN_LOAD:
		/* Success - rebuild patterns */
		if (res == 1) set_patterns(&settings);
		free(settings.img[CHN_IMAGE]);
		break;
	case FS_PALETTE_LOAD:
	case FS_PALETTE_DEF:
		/* Drop image channels if any */
		mem_free_chanlist(settings.img);
		/* This "failure" in this context serves as shortcut */
		if (res == EXPLODE_FAILED) res = 1;
		/* In case of image format, retry as raw palette */
		if ((res != 1) && (file_formats[ftype].flags & FF_IMAGE))
			res = load_rawpal(file_name, &settings);
		/* Utter failure - do nothing */
		if ((res != 1) || (settings.colors <= 0));
		/* Replace default palette */
		else if (mode == FS_PALETTE_DEF)
		{
			mem_pal_copy(mem_pal_def, pal);
			mem_pal_def_i = settings.colors;
		}
		/* Change current palette */
		else
		{
			mem_undo_next(UNDO_PAL);
			mem_pal_copy(mem_pal, pal);
			mem_cols = settings.colors;
		}
		break;
	}
	free(settings.icc);
	return (res);
}

int load_image(char *file_name, int mode, int ftype)
{
	return (load_image_x(file_name, NULL, mode, ftype, 0, 0));
}

int load_mem_image(unsigned char *buf, int len, int mode, int ftype)
{
	memFILE mf;

	if (((ftype & FTM_FTYPE) != FT_PIXMAP) /* Special case */ &&
		!(file_formats[ftype & FTM_FTYPE].flags & FF_RMEM)) return (-1);
	memset(&mf, 0, sizeof(mf));
	mf.m.buf = buf; mf.top = mf.m.size = len;
	return (load_image_x(NULL, &mf, mode, ftype, 0, 0));
}

int load_image_scale(char *file_name, int mode, int ftype, int w, int h)
{
	return (load_image_x(file_name, NULL, mode, ftype, w, h));
}

// !!! The only allowed modes for now are FS_LAYER_LOAD and FS_EXPLODE_FRAMES
// !!! Load from memblock is not supported yet
static int load_frames_x(ani_settings *ani, int ani_mode, char *file_name,
	int mode, int ftype)
{
	png_color pal[256];


	ftype &= FTM_FTYPE;
	ani->mode = ani_mode;
	init_ls_settings(&ani->settings, NULL);
#ifdef U_LCMS
	/* Set size to -1 when we don't want color profile */
	/* if (!apply_icc) */ ani->settings.icc_size = -1; // !!! Disable for now
#endif
	ani->settings.mode = mode;
	ani->settings.ftype = ftype;
	ani->settings.pal = pal;
	/* Clear hotspot & transparency */
	ani->settings.hot_x = ani->settings.hot_y = -1;
	ani->settings.xpm_trans = ani->settings.rgb_trans = -1;
	/* No load progressbar when exploding frames */
	if (mode == FS_EXPLODE_FRAMES) ani->settings.silent = TRUE;

	/* !!! Use default palette - for now */
	mem_pal_copy(pal, mem_pal_def);
	ani->settings.colors = mem_pal_def_i;

	switch (ftype)
	{
	case FT_PNG: return (load_apng_frames(file_name, ani));
	case FT_GIF: return (load_gif_frames(file_name, ani));
#ifdef U_TIFF
	case FT_TIFF: return (load_tiff_frames(file_name, ani));
#endif
#ifdef U_WEBP
	case FT_WEBP: return (load_webp_frames(file_name, ani));
#endif
	case FT_PBM:
	case FT_PGM:
	case FT_PPM:
	case FT_PAM: return (load_pnm_frames(file_name, ani));
	case FT_PMM: return (load_pmm_frames(file_name, ani, NULL));
	}
	return (-1);
}

int load_frameset(frameset *frames, int ani_mode, char *file_name, int mode,
	int ftype)
{
	ani_settings ani;
	int res;


	memset(&ani, 0, sizeof(ani_settings));
	res = load_frames_x(&ani, ani_mode, file_name, mode, ftype);

	/* Treat out-of-memory error as fatal, to avoid worse things later */
	if ((res == FILE_MEM_ERROR) || !ani.fset.cnt)
		mem_free_frames(&ani.fset);
	/* Pass too-many-frames error along */
	else if (res == FILE_TOO_LONG);
	/* Consider all other errors partial failures */
	else if (res != 1) res = FILE_LIB_ERROR;

	/* Just pass the frameset to the outside, for now */
	*frames = ani.fset;
	return (res);
}

/* Write out the last frame to indexed sequence, and delete it */
static int write_out_frame(char *file_name, ani_settings *ani, ls_settings *f_set)
{
	ls_settings w_set;
	image_frame *frame = ani->fset.frames + ani->fset.cnt - 1;
	char new_name[PATHBUF + 32], *tmp;
	int n, deftype = ani->desttype, res;


	/* Show progress, for unknown final count */
	n = nextpow2(ani->cnt);
	if (n < 16) n = 16;
	progress_update((float)ani->cnt / n);

	tmp = strrchr(file_name, DIR_SEP);
	if (!tmp) tmp = file_name;
	else tmp++;
	file_in_dir(new_name, ani->destdir, tmp, PATHBUF);
	tmp = new_name + strlen(new_name);
	sprintf(tmp, ".%03d", ani->cnt);

	if (f_set) w_set = *f_set;
	else
	{
		init_ls_settings(&w_set, NULL);
		memcpy(w_set.img, frame->img, sizeof(chanlist));
		w_set.width = frame->width;
		w_set.height = frame->height;
		w_set.pal = frame->pal ? frame->pal : ani->fset.pal;
		w_set.bpp = frame->bpp;
		w_set.colors = frame->cols;
		w_set.xpm_trans = frame->trans;
	}
	w_set.ftype = deftype;
	w_set.silent = TRUE;
	if (!(file_formats[deftype].flags & FF_SAVE_MASK_FOR(w_set)))
	{
		w_set.ftype = FT_PNG;
		ani->miss++;
	}
	w_set.mode = ani->mode; // Only FS_EXPLODE_FRAMES for now

	res = ani->error = save_image(new_name, &w_set);
	if (!res) ani->cnt++;

	if (f_set) // Delete
	{
		mem_free_chanlist(f_set->img);
		memset(f_set->img, 0, sizeof(chanlist));
	}
	// Set for deletion
	else frame->flags |= FM_NUKE;
	return (res);
}

static void warn_miss(int miss, int total, int ftype)
{
	char *txt = g_strdup_printf(
		__("%d out of %d frames could not be saved as %s - saved as PNG instead"),
		miss, total, file_formats[ftype].name);
	alert_box(_("Warning"), txt, "", NULL); // Not an error
	g_free(txt);
}

int explode_frames(char *dest_path, int ani_mode, char *file_name, int ftype,
	int desttype)
{
	ani_settings ani;
	int res;


	memset(&ani, 0, sizeof(ani_settings));
	ani.desttype = desttype;
	ani.destdir = dest_path;

	progress_init(_("Explode frames"), 0);
	progress_update(0.0);
	res = load_frames_x(&ani, ani_mode, file_name, FS_EXPLODE_FRAMES, ftype);
	progress_update(1.0);
	if (res == 1); // Everything went OK
	else if (res == FILE_MEM_ERROR); // Report memory problem
	else if (ani.error) // Sequence write failure - soft or hard?
		res = ani.cnt ? FILE_EXP_BREAK : EXPLODE_FAILED;
	else if (ani.cnt) // Failed to read some middle frame
		res = FILE_LIB_ERROR;
	mem_free_frames(&ani.fset);
	progress_end();

	if (ani.miss && (res == 1))
		warn_miss(ani.miss, ani.cnt, ani.desttype & FTM_FTYPE);

	return (res);
}

int export_undo(char *file_name, ls_settings *settings)
{
	char new_name[PATHBUF + 32];
	int start = mem_undo_done, res = 0, lenny, i, j;
	int deftype = settings->ftype, miss = 0;

	strncpy(new_name, file_name, PATHBUF);
	lenny = strlen( file_name );

	ls_init("UNDO", 1);
	settings->silent = TRUE;

	for (j = 0; j < 2; j++)
	{
		for (i = 1; i <= start + 1; i++)
		{
			if (!res && (!j ^ (settings->mode == FS_EXPORT_UNDO)))
			{
				progress_update((float)i / (start + 1));
				settings->ftype = deftype;
				if (!(file_formats[deftype].flags & FF_SAVE_MASK))
				{
					settings->ftype = FT_PNG;
					miss++;
				}
				sprintf(new_name + lenny, "%03i.%s", i,
					file_formats[settings->ftype].ext);
				memcpy(settings->img, mem_img, sizeof(chanlist));
				settings->pal = mem_pal;
				settings->width = mem_width;
				settings->height = mem_height;
				settings->bpp = mem_img_bpp;
				settings->colors = mem_cols;
				res = save_image(new_name, settings);
			}
			if (!j) /* Goto first image */
			{
				if (mem_undo_done > 0) mem_do_undo(FALSE);
			}
			else if (mem_undo_done < start) mem_do_undo(TRUE);
		}
	}

	progress_end();

	if (miss && !res) warn_miss(miss, mem_undo_done, deftype);

	return (res);
}

int export_ascii ( char *file_name )
{
	char ch[16] = " .,:;+=itIYVXRBM";
	int i, j;
	unsigned char pix;
	FILE *fp;

	if ((fp = fopen(file_name, "w")) == NULL) return -1;

	for ( j=0; j<mem_height; j++ )
	{
		for ( i=0; i<mem_width; i++ )
		{
			pix = mem_img[CHN_IMAGE][ i + mem_width*j ];
			fprintf(fp, "%c", ch[pix % 16]);
		}
		fprintf(fp, "\n");
	}
	fclose(fp);

	return 0;
}

static int do_detect_format(char *name, FILE *fp)
{
	unsigned char buf[66], *stop;
	int i;

	i = fread(buf, 1, 64, fp);
	buf[64] = '\0';

	/* Check all unambiguous signatures */
	if (!memcmp(buf, "\x89PNG", 4)) return (FT_PNG);
	if (!memcmp(buf, "GIF8", 4)) return (FT_GIF);
	if (!memcmp(buf, "\xFF\xD8", 2))
#ifdef U_JPEG
		return (FT_JPEG);
#else
		return (FT_NONE);
#endif
	if (!memcmp(buf, "\0\0\0\x0C\x6A\x50\x20\x20\x0D\x0A\x87\x0A", 12))
#ifdef HANDLE_JP2
		return (FT_JP2);
#else
		return (FT_NONE);
#endif
	if (!memcmp(buf, "\xFF\x4F", 2))
#ifdef HANDLE_JP2
		return (FT_J2K);
#else
		return (FT_NONE);
#endif
	if (!memcmp(buf, "II", 2) || !memcmp(buf, "MM", 2))
#ifdef U_TIFF
		return (FT_TIFF);
#else
		return (FT_NONE);
#endif
	if (!memcmp(buf, "RIFF", 4) && !memcmp(buf + 8, "WEBP", 4))
#ifdef U_WEBP
		return (FT_WEBP);
#else
		return (FT_NONE);
#endif
	if (!memcmp(buf, "FORM", 4) && (!memcmp(buf + 8, "ILBM", 4) ||
		!memcmp(buf + 8, "PBM ", 4))) return (FT_LBM);
	if (!memcmp(buf, "BM", 2) || !memcmp(buf, "BA", 2)) return (FT_BMP);

	if (!memcmp(buf, "\x3D\xF3\x13\x14", 4)) return (FT_LSS);

	if (!memcmp(buf, PMM_ID1, 12)) return (FT_PMM);

	if (!memcmp(buf, "P7", 2)) return (FT_PAM);
	if ((buf[0] == 'P') && (buf[1] >= '1') && (buf[1] <= '6'))
	{
		static const unsigned char pnms[3] = { FT_PBM, FT_PGM, FT_PPM };
		return (pnms[(buf[1] - '1') % 3]);
	}

	if (!memcmp(buf, "GIMP Palette", strlen("GIMP Palette"))) return (FT_GPL);

	/* Check layers signature and version */
	if (!memcmp(buf, LAYERS_HEADER, strlen(LAYERS_HEADER)))
	{
		stop = strchr(buf, '\n');
		if (!stop || (stop - buf > 32)) return (FT_NONE);
		i = atoi(++stop);
		if (i == 1) return (FT_LAYERS1);
/* !!! Not implemented yet */
//		if (i == 2) return (FT_LAYERS2);
		return (FT_NONE);
	}

	/* Assume generic GZIP is SVGZ */
	if (!memcmp(buf, "\x1F\x8B", 2)) return (FT_SVG);
	/* Assume generic XML is SVG */
	i = 0; sscanf(buf, " <?xml %n", &i);
	if (!i) sscanf(buf, " <svg%n", &i);
	if (!i) sscanf(buf, " <!DOCTYPE svg%n", &i);
	if (i) return (FT_SVG);

	/* Discern PCX from TGA */
	while (buf[0] == 10)
	{
		if (buf[1] > 5) break;
		if (buf[1] > 1) return (FT_PCX);
		if (buf[2] != 1) break; // Uncompressed PCX is nonstandard
		/* Ambiguity - look at name as a last resort
		 * Bias to PCX - TGAs usually have 0th byte = 0 */
		stop = strrchr(name, '.');
		if (!stop) return (FT_PCX);
		if (!strcasecmp(stop + 1, "tga")) break;
		return (FT_PCX);
	}

	/* Check if this is TGA */
	if ((buf[1] < 2) && (buf[2] < 12) && ((1 << buf[2]) & 0x0E0F))
		return (FT_TGA);

	/* Simple check for "txt" palette format */
	if ((sscanf(buf, "%i", &i) == 1) && (i > 0) && (i <= 256)) return (FT_TXT);

	/* Simple check for XPM */
	stop = strstr(buf, "XPM");
	if (stop)
	{
		i = stop - buf;
		stop = strchr(buf, '\n');
		if (!stop || (stop - buf > i)) return (FT_XPM);
	}
	/* Check possibility of XBM by absence of control chars */
	for (i = 0; buf[i] && (buf[i] != '\n'); i++)
	{
		if (ISCNTRL(buf[i])) return (FT_NONE);
	}
	return (FT_XBM);
}

int detect_file_format(char *name, int need_palette)
{
	FILE *fp;
	int i, f;

	if (!(fp = fopen(name, "rb"))) return (-1);
	i = do_detect_format(name, fp);
	f = file_formats[i].flags;
	if (need_palette)
	{
		/* Check for PAL/ACT raw format */
		if (!(f & (FF_16 | FF_256 | FF_PALETTE)))
		{
			f_long l;
			fseek(fp, 0, SEEK_END);
			l = ftell(fp);
			i = (l > 0) && (l <= 768) && !(l % 3) ? FT_PAL : FT_NONE;
		}
	}
	else if (!(f & (FF_IMAGE | FF_LAYER))) i = FT_NONE;
	fclose(fp);
	return (i);
}

// Can this file be opened for reading?

/* 0 = readable, -1 = not exists, 1 = error, 2 = a directory */
int valid_file(char *filename)
{
	FILE *fp = fopen(filename, "r");
	if (!fp) return (errno == ENOENT ? -1 : 1);
	else
	{
		struct stat buf;
		int d = fstat(fileno(fp), &buf) ? 1 : S_ISDIR(buf.st_mode) ? 2 : 0;
		fclose(fp);
		return (d);
	}
}
