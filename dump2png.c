/*
 * dump2png	Visualize file data as a png.  Intended for memory dumps.
 *
 * This tool is an experiment, intended to characterize the memory usage of
 * large process core dumps.  It does by converting each byte to a colored
 * pixel, and building an image from these line by line.  For serious core
 * dump analysis, look for other tools that read the metadata and structure
 * from the dump.
 *
 * USAGE: See: ./dump2png --help
 *
 * BUILD: gcc -O3 -lm -lpng -o dump2png dump2png.c	# requires libpng
 *
 * By default, the least significant bit is masked, so that the image can't
 * be converted back to the input file, to avoid inadvertent privacy leaks.
 * Use -M to avoid masking, or increase BYTE_MASK to mask more bits.
 *
 * SEE ALSO: ImageMagick, which has similar functionality to the "gray" and
 * "rgb" palettes.
 *
 * Copyright 2012 Joyent, Inc.  All rights reserved.
 * Copyright 2012 Brendan Gregg.  All rights reserved.
 *
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 *
 * 30-Apr-2012	Brendan Gregg	Created this.
 */

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <math.h>
#include <png.h>
#include <sys/types.h>
#include <sys/stat.h>

static void
usage(int full)
{
	printf("USAGE: dump2png [-HM] [-w width] [-h height_max]\n"
	    "                [-p palette] [-o outfile.png]\n"
	    "                [-k skip_factor] [-s seek_bytes]\n"
	    "                [-z zoom_factor] file\n\n"
	    "                [--help]\t# for full help\n\n"
	    "palette types: gray, gray16b, gray16l, gray32b, gray32l,\n"
	    "               hues, hues6, fhues, color, color16, color32, rgb,\n"
	    "               dvi, x86 (default).\n");
	if (!full)
		exit(1);
	printf("\n\t-H            \tdon't autoscale height\n"
	    "\t-M            \tdon't mask least significant bit\n"
	    "\t-k skip_factor\tskips horiz lines; eg, 3 means show 1 out of 3\n"
	    "\t-s seek_bytes\tthe byte offset of the infile to begin reading\n"
	    "\t-z zoom_factor\taverages multiple bytes; eg, 16 avgs 16 as 1\n"
	    "\t-z palette\tpalette type for colorization:\n\n"
	    "\tgray\t\tgrayscale, per byte\n"
	    "\tgray16b\t\tgrayscale, per short (big-endian)\n"
	    "\tgray16l\t\tgrayscale, per short (little-endian)\n"
	    "\tgray32b\t\tgrayscale, per long (big-endian)\n"
	    "\tgray32l\t\tgrayscale, per long (little-endian)\n"
	    "\thues\t\tmap to 3 hue ranges (rgb), per byte (zoom safe)\n"
	    "\thues6\t\tmap to 6 hue ranges (rgbcmy), per byte\n"
	    "\tfhues\t\tmap to 3 full hue ranges (rgb), per byte (zoom safe)\n"
	    "\tcolor\t\tfull colorized scale, per byte\n"
	    "\tcolor16\t\tfull colorized scale, per short (16-bit)\n"
	    "\tcolor32\t\tfull colorized scale, per long (32-bit)\n"
	    "\trgb\t\ttreat 3 sequential bytes as RGB\n"
	    "\tdvi\t\tuse RGB to convey differential, value, integral\n"
	    "\tx86\t\tgrayscale with some (9) color indicators:\n\n"
	    "\t    green = common english chars: 'e', 't', 'a'\n"
	    "\t    red = common x86 instructions: movl, call, testl\n"
	    "\t    blue = binary values: 0x01, 0x02, 0x03\n");
	exit(1);
}

typedef enum {
	GRAY = 0,
	GRAY16B,
	GRAY32B,
	GRAY16L,
	GRAY32L,
	HUES,
	HUES6,
	FHUES,
	COLOR,
	COLOR16,
	COLOR32,
	RGB,
	DVI,
	X86
} palette_t;

static palette_t atopal(const char *opt);
static int pal2chrs(palette_t pal);
static int doimage(int infile, FILE *outfile, int width, int height,
    palette_t pal, int skip, int zoom, int mask);

int
main(int argc, char *argv[])
{
	char *infilename, *outfilename = "dump2png.png";
	extern char *optarg;
	extern int optind, optopt;
	struct stat filestat;
	int infile, opt, width, height, skip, zoom, chrs, mask, hscale = 1;
	palette_t pal;
	off_t seek;
	FILE *outfile;

	/* defaults */
	width = 1024 * 1;
	height = 1024 * 10;
	zoom = skip = 1;
	seek = 0;
	mask = 1;
	pal = X86;

	if (argc < 2 || strcmp(argv[1], "--help") == 0)
		usage(argc >= 2);

	while ((opt = getopt(argc, argv, "HMh:k:o:p:s:w:z:?")) != EOF) {
		switch (opt) {
			case 'H':
				hscale = 0;
				break;
			case 'h':
				height = atoi(optarg);
				break;
			case 'k':
				skip = atoi(optarg);
				break;
			case 'M':
				mask = 0;
				break;
			case 'o':
				outfilename = optarg;
				break;
			case 'p':
				pal = atopal(optarg);
				break;
			case 's':
				seek = atoi(optarg);
				break;
			case 'w':
				width = atoi(optarg);
				break;
			case 'z':
				zoom = atoi(optarg);
				break;
			case '?':
				usage(0);
		}
	}

	if (width == 0 || height == 0 || skip == 0 || zoom == 0)
		usage(0);
	if (optind + 1 != argc)
		usage(0);
	infilename = argv[optind];

	if (stat(infilename, &filestat) != 0) {
		perror("Can't access infile");
		return (2);
	}

	chrs = pal2chrs(pal);
	int fullheight = ceil((float)(filestat.st_size /
	    (zoom * skip * chrs)) / width);

	if (fullheight > height) {
		printf("Truncating height: showing %llu of %llu bytes. ",
		    (unsigned long long)width * height * zoom * skip * chrs,
		    (unsigned long long)filestat.st_size);
		printf("Use -h to allow larger heights.\n");
	} else {
		if (hscale) {
			height = fullheight;
		}
	}

	printf("Output image: height:%d, width:%d\n", height, width);

	if ((infile = open(infilename, O_RDONLY)) < 0) {
		fprintf(stderr, "Can't read %s", infilename);
		exit(2);
	}

	if (seek && lseek(infile, seek, SEEK_SET) == -1) {
		perror("Seek failed");
		exit(2);
	}

	outfile = fopen(outfilename, "wb");
	if (outfile == NULL) {
		fprintf(stderr, "ERROR: Could not write to %s\n", outfilename);
		exit(2);
	}

	printf("Writing %s...\n", outfilename);
	int result = doimage(infile, outfile, width, height, pal, skip, zoom,
	    mask);
	close(infile);
	fclose(outfile);

	return (result);
}

static palette_t
atopal(const char *opt)
{
	if (strcmp(opt, "gray") == 0)
		return (GRAY);
	if (strcmp(opt, "gray16b") == 0)
		return (GRAY16B);
	if (strcmp(opt, "gray32b") == 0)
		return (GRAY32B);
	if (strcmp(opt, "gray16l") == 0)
		return (GRAY16L);
	if (strcmp(opt, "gray32l") == 0)
		return (GRAY32L);
	if (strcmp(opt, "hues") == 0)
		return (HUES);
	if (strcmp(opt, "hues6") == 0)
		return (HUES6);
	if (strcmp(opt, "fhues") == 0)
		return (FHUES);
	if (strcmp(opt, "color") == 0)
		return (COLOR);
	if (strcmp(opt, "color16") == 0)
		return (COLOR16);
	if (strcmp(opt, "color32") == 0)
		return (COLOR32);
	if (strcmp(opt, "rgb") == 0)
		return (RGB);
	if (strcmp(opt, "dvi") == 0)
		return (DVI);
	if (strcmp(opt, "x86") == 0)
		return (X86);
	fprintf(stderr, "invalid palette. See USAGE (--help).\n");
	exit(3);
}

static int
pal2chrs(palette_t pal)
{
	switch (pal) {
		case RGB:
			return (3);
		case GRAY16B:
		case GRAY16L:
		case COLOR16:
			return (2);
		case GRAY32B:
		case GRAY32L:
		case COLOR32:
			return (4);
		default:
			return (1);
	}
}

inline void
map_hues(png_byte *ptr, unsigned char val)
{
	int v = val * 3;
	if (v < 256) {
		ptr[0] = v; ptr[1] = 0; ptr[2] = 0;
	} else if (v < 512) {
		ptr[0] = 0; ptr[1] = v % 256; ptr[2] = 0;
	} else {
		ptr[0] = 0; ptr[1] = 0; ptr[2] = v % 256;
	}
}

inline void
map_fhues(png_byte *ptr, unsigned char val)
{
	int v = val * 6;
	if (v < 256) {
		ptr[0] = v; ptr[1] = 0; ptr[2] = 0;
	} else if (v < 256 * 2) {
		ptr[0] = 255; ptr[1] = v % 256; ptr[2] = v % 256;
	} else if (v < 256 * 3) {
		ptr[0] = 0; ptr[1] = v % 256; ptr[2] = 0;
	} else if (v < 256 * 4) {
		ptr[0] = v % 256; ptr[1] = 255; ptr[2] = v % 256;
	} else if (v < 256 * 5) {
		ptr[0] = 0; ptr[1] = 0; ptr[2] = v % 256;
	} else {
		ptr[0] = v % 256; ptr[1] = v % 256; ptr[2] = 255;
	}
}

inline void
map_hues6(png_byte *ptr, unsigned char val)
{
	int v = val * 6;
	if (v < 256) {
		ptr[0] = v; ptr[1] = 0; ptr[2] = 0;
	} else if (v < 256 * 2) {
		ptr[0] = 0; ptr[1] = v % 256; ptr[2] = 0;
	} else if (v < 256 * 3) {
		ptr[0] = 0; ptr[1] = 0; ptr[2] = v % 256;
	} else if (v < 256 * 4) {
		ptr[0] = 0; ptr[1] = v % 256; ptr[2] = v % 256;
	} else if (v < 256 * 5) {
		ptr[0] = v % 256; ptr[1] = 0; ptr[2] = v % 256;
	} else {
		ptr[0] = v % 256; ptr[1] = v % 256; ptr[2] = 0;
	}
}

inline void
map_color16(png_byte *ptr, unsigned short val)
{
	ptr[0] = (val & 0xfc00) >> 8;
	ptr[1] = (val & 0x03c0) >> 2;
	ptr[2] = (val & 0x001f) << 3;
}

inline void
map_color32(png_byte *ptr, unsigned long val)
{
	ptr[0] = (val & 0xff000000) >> 24;
	ptr[1] = (val & 0x001fe000) >> 13;
	ptr[2] = (val & 0x000001fe) >> 1;
}

inline unsigned char
c2v_binary(unsigned char c)
{
	switch (c) {
		case 0x01: return (0xff);
		case 0x02: return (0xcf);
		case 0x03: return (0xaf);
	}
	return (0);
}

inline unsigned char
c2v_english(char c)
{
	switch (c) {
		case 'e': return (0xff);
		case 't': return (0xcf);
		case 'a': return (0xaf);
	}
	return (0);
}

inline unsigned char
c2v_x86(unsigned char c)
{
	switch (c) {
		case 0x8b: return (0xff);	/* movl */
		case 0xe8: return (0xcf);	/* call */
		case 0x85: return (0xaf);	/* testl */
	}
	return (0);
}

static void
map_x86(unsigned char *rgb, unsigned char c)
{
	rgb[0] = rgb[1] = rgb[2] = 0;

	rgb[0] = c2v_x86(c);
	rgb[1] = c2v_english(c);
	rgb[2] = c2v_binary(c);

	/* default to grayscale */
	if ((rgb[0] + rgb[1] + rgb[2]) == 0) {
		rgb[0] = rgb[1] = rgb[2] = c;
	}
}

#define	BYTE_MASK	0xfe

static int
doimage(int infile, FILE *outfile, int width, int height, palette_t pal,
    int skip, int zoom, int mask)
{
	png_text pngtitle;
	png_structp pngstruct;
	png_infop pnginfo;
	png_bytep pngbyte;
	unsigned char last, rgb[3], *inbuf;
	int in, xx, x, y, z, chrs, i = 0, code = 1;
	unsigned long sum[3];

	/* setup png */
	pngbyte = (png_bytep)malloc(width * skip * zoom * sizeof (png_byte) *
	    3);
	pngstruct = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL,
	    NULL);
	pnginfo = png_create_info_struct(pngstruct);
	chrs = pal2chrs(pal);
	inbuf = (char *)malloc(width * skip * zoom * chrs);

	if (pngbyte == NULL | pngstruct == NULL | pnginfo == NULL |
	    inbuf == NULL) {
		perror("Out of memory");
		goto out;
	}

	if (setjmp(png_jmpbuf(pngstruct))) {
		perror("Error during png creation");
		goto out;
	}

	png_init_io(pngstruct, outfile);
	png_set_IHDR(pngstruct, pnginfo, width, height, 8, PNG_COLOR_TYPE_RGB,
	    PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE,
	    PNG_FILTER_TYPE_BASE);

	pngtitle.compression = PNG_TEXT_COMPRESSION_NONE;
	pngtitle.key = "Title";
	pngtitle.text = "dump2png";
	png_set_text(pngstruct, pnginfo, &pngtitle, 1);

	png_write_info(pngstruct, pnginfo);

	/*
	 * Read data and convert to png image.  x tracks the destination pixel
	 * x offset.  xx tracks the offset in the input buffer, which can step
	 * at a faster rate when it's necessary to combine multiple bytes into
	 * one pixel (with zoom or certain palettes).
	 */
	for (y = 0; y < height; y++) {
		in = read(infile, inbuf, width * chrs * skip * zoom);

		for (x = 0, xx = 0; x < width; x++) {
			if (xx + chrs > in) {
				(&pngbyte[x * 3])[0] = 0;
				(&pngbyte[x * 3])[1] = 0;
				(&pngbyte[x * 3])[2] = 0;
				continue;
			}

			sum[0] = sum[1] = sum[2] = 0;

			for (z = 0; z < zoom; z++, xx++) {
				switch (pal) {
					case GRAY:
						rgb[0] = inbuf[xx];
						rgb[1] = inbuf[xx];
						rgb[2] = inbuf[xx];
						break;
					/*
					 * Gray 16|32 skip bytes and map
					 * significant byte to grayscale.
					 */
					case GRAY16B:
						rgb[0] = inbuf[xx];
						rgb[1] = inbuf[xx];
						rgb[2] = inbuf[xx++];
						break;
					case GRAY32B:
						rgb[0] = inbuf[xx];
						rgb[1] = inbuf[xx];
						rgb[2] = inbuf[xx];
						xx += 3;
						break;
					case GRAY16L:
						rgb[0] = inbuf[++xx];
						rgb[1] = inbuf[xx];
						rgb[2] = inbuf[xx];
						break;
					case GRAY32L:
						xx += 3;
						rgb[0] = inbuf[xx];
						rgb[1] = inbuf[xx];
						rgb[2] = inbuf[xx];
						break;
					case HUES:
						map_hues(&rgb[0], inbuf[xx]);
						break;
					case HUES6:
						map_hues6(&rgb[0], inbuf[xx]);
						break;
					case FHUES:
						map_fhues(&rgb[0], inbuf[xx]);
						break;
					/*
					 * Color palettes mask and shifts bits
					 * into RGB
					 */
					case COLOR:
						rgb[0] = inbuf[xx] & 0xe0;
						rgb[1] = (inbuf[xx] & 0x1c) <<
						    3;
						rgb[2] = (inbuf[xx] & 0x03) <<
						    6;
						break;
					case COLOR16:
						map_color16(&rgb[0],
						    inbuf[xx++] +
						    (inbuf[xx] << 8));
						break;
					case COLOR32:
						map_color32(&rgb[0],
						    inbuf[xx++] +
						    (inbuf[xx++] << 8) +
						    (inbuf[xx++] << 16) +
						    (inbuf[xx] << 24));
						break;
					/*
					 * RGB uses sequential bytes for RGB
					 */
					case RGB:
						rgb[0] = inbuf[xx++];
						rgb[1] = inbuf[xx++];
						rgb[2] = inbuf[xx];
						break;
					case X86:
						map_x86(&rgb[0], inbuf[xx]);
						break;
					case DVI:
						rgb[0] = abs(inbuf[xx] - last);
						rgb[1] = inbuf[xx];
						rgb[2] = (inbuf[xx] + last) / 2;
						break;
					default:
						fprintf(stderr, "palette?\n");
						goto out;
				}

				if (zoom > 1) {
					sum[0] += rgb[0];
					sum[1] += rgb[1];
					sum[2] += rgb[2];
				}
			}

			if (zoom > 1) {
				rgb[0] = sum[0] / zoom;
				rgb[1] = sum[1] / zoom;
				rgb[2] = sum[2] / zoom;
			}

			if (mask) {
				rgb[0] = rgb[0] & BYTE_MASK;
				rgb[1] = rgb[1] & BYTE_MASK;
				rgb[2] = rgb[2] & BYTE_MASK;
			}

			(&pngbyte[x * 3])[0] = rgb[0];
			(&pngbyte[x * 3])[1] = rgb[1];
			(&pngbyte[x * 3])[2] = rgb[2];

			last = inbuf[x];
		}
		png_write_row(pngstruct, pngbyte);
	}

	png_write_end(pngstruct, NULL);
	code = 0;

out:
	if (pnginfo != NULL)
		png_free_data(pngstruct, pnginfo, PNG_FREE_ALL, -1);
	if (pngstruct != NULL)
		png_destroy_write_struct(&pngstruct, (png_infopp) NULL);
	if (pngbyte != NULL)
		free(pngbyte);

	return (code);
}
