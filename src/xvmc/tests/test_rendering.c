#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <error.h>
#include "testlib.h"

#define BLOCK_WIDTH			8
#define BLOCK_HEIGHT			8
#define BLOCK_SIZE			(BLOCK_WIDTH * BLOCK_HEIGHT)
#define MACROBLOCK_WIDTH		16
#define MACROBLOCK_HEIGHT		16
#define MACROBLOCK_WIDTH_IN_BLOCKS	(MACROBLOCK_WIDTH / BLOCK_WIDTH)
#define MACROBLOCK_HEIGHT_IN_BLOCKS	(MACROBLOCK_HEIGHT / BLOCK_HEIGHT)
#define BLOCKS_PER_MACROBLOCK		6

#define INPUT_WIDTH			16
#define INPUT_HEIGHT			16
#define INPUT_WIDTH_IN_MACROBLOCKS	(INPUT_WIDTH / MACROBLOCK_WIDTH)
#define INPUT_HEIGHT_IN_MACROBLOCKS	(INPUT_HEIGHT / MACROBLOCK_HEIGHT)
#define NUM_MACROBLOCKS			(INPUT_WIDTH_IN_MACROBLOCKS * INPUT_HEIGHT_IN_MACROBLOCKS)

#define DEFAULT_OUTPUT_WIDTH		INPUT_WIDTH
#define DEFAULT_OUTPUT_HEIGHT		INPUT_HEIGHT
#define DEFAULT_ACCEPTABLE_ERR		0.01

void ParseArgs(int argc, char **argv, unsigned int *output_width, unsigned int *output_height, double *acceptable_error, int *prompt)
{
	int fail = 0;
	int i;

	*output_width = DEFAULT_OUTPUT_WIDTH;
	*output_height = DEFAULT_OUTPUT_WIDTH;
	*acceptable_error = DEFAULT_ACCEPTABLE_ERR;
	*prompt = 1;

	for (i = 1; i < argc && !fail; ++i)
	{
		if (!strcmp(argv[i], "-w"))
		{
			if (sscanf(argv[++i], "%u", output_width) != 1)
				fail = 1;
		}
		else if (!strcmp(argv[i], "-h"))
		{
			if (sscanf(argv[++i], "%u", output_height) != 1)
				fail = 1;
		}
		else if (!strcmp(argv[i], "-e"))
		{
			if (sscanf(argv[++i], "%lf", acceptable_error) != 1)
				fail = 1;
		}
		else if (strcmp(argv[i], "-n"))
			*prompt = 0;
		else
			fail = 1;
	}

	if (fail)
		error
		(
			1, 0,
			"Bad argument.\n"
			"\n"
			"Usage: %s [options]\n"
			"\t-w <width>\tOutput width\n"
			"\t-h <height>\tOutput height\n"
			"\t-e <error>\tAcceptable margin of error per pixel, from 0 to 1\n"
			"\t-n\tDon't prompt for quit\n",
			argv[0]
		);
}

void Gradient(short *block, unsigned int start, unsigned int stop, int horizontal)
{
	unsigned int x, y;
	unsigned int range = stop - start;

	if (horizontal)
	{
		for (y = 0; y < BLOCK_HEIGHT; ++y)
			for (x = 0; x < BLOCK_WIDTH; ++x)
				block[y * BLOCK_WIDTH + x] = (short)(start + range * (x / (float)(BLOCK_WIDTH - 1)));
	}
	else
	{
		for (y = 0; y < BLOCK_HEIGHT; ++y)
			for (x = 0; x < BLOCK_WIDTH; ++x)
				block[y * BLOCK_WIDTH + x] = (short)(start + range * (y / (float)(BLOCK_HEIGHT - 1)));
	}
}

int main(int argc, char **argv)
{
	unsigned int		output_width;
	unsigned int		output_height;
	double			acceptable_error;
	int			prompt;
	Display			*display;
	Window			root, window;
	const unsigned int	mc_types[2] = {XVMC_MOCOMP | XVMC_MPEG_2, XVMC_IDCT | XVMC_MPEG_2};
	XvPortID		port_num;
	int			surface_type_id;
	unsigned int		is_overlay, intra_unsigned;
	int			colorkey;
	XvMCContext		context;
	XvMCSurface		surface;
	XvMCBlockArray		block_array;
	XvMCMacroBlockArray	mb_array;
	int			mbx, mby, bx, by;
	XvMCMacroBlock		*mb;
	short			*blocks;
	int			quit = 0;

	ParseArgs(argc, argv, &output_width, &output_height, &acceptable_error, &prompt);

	display = XOpenDisplay(NULL);

	if (!GetPort
	(
		display,
		INPUT_WIDTH,
		INPUT_HEIGHT,
		XVMC_CHROMA_FORMAT_420,
    		mc_types,
    		2,
    		&port_num,
    		&surface_type_id,
    		&is_overlay,
    		&intra_unsigned
	))
	{
		XCloseDisplay(display);
		error(1, 0, "Error, unable to find a good port.\n");
	}

	if (is_overlay)
	{
		Atom xv_colorkey = XInternAtom(display, "XV_COLORKEY", 0);
		XvGetPortAttribute(display, port_num, xv_colorkey, &colorkey);
	}

	root = XDefaultRootWindow(display);
	window = XCreateSimpleWindow(display, root, 0, 0, output_width, output_height, 0, 0, colorkey);

	assert(XvMCCreateContext(display, port_num, surface_type_id, INPUT_WIDTH, INPUT_HEIGHT, XVMC_DIRECT, &context) == Success);
	assert(XvMCCreateSurface(display, &context, &surface) == Success);
	assert(XvMCCreateBlocks(display, &context, NUM_MACROBLOCKS * BLOCKS_PER_MACROBLOCK, &block_array) == Success);
	assert(XvMCCreateMacroBlocks(display, &context, NUM_MACROBLOCKS, &mb_array) == Success);

	mb = mb_array.macro_blocks;
	blocks = block_array.blocks;

	for (mby = 0; mby < INPUT_HEIGHT_IN_MACROBLOCKS; ++mby)
		for (mbx = 0; mbx < INPUT_WIDTH_IN_MACROBLOCKS; ++mbx)
		{
			mb->x = mbx;
			mb->y = mby;
			mb->macroblock_type = XVMC_MB_TYPE_INTRA;
			/*mb->motion_type = ;*/
			/*mb->motion_vertical_field_select = ;*/
			mb->dct_type = XVMC_DCT_TYPE_FRAME;
			/*mb->PMV[0][0][0] = ;
			mb->PMV[0][0][1] = ;
			mb->PMV[0][1][0] = ;
			mb->PMV[0][1][1] = ;
			mb->PMV[1][0][0] = ;
			mb->PMV[1][0][1] = ;
			mb->PMV[1][1][0] = ;
			mb->PMV[1][1][1] = ;*/
			mb->index = (mby * INPUT_WIDTH_IN_MACROBLOCKS + mbx) * BLOCKS_PER_MACROBLOCK;
			mb->coded_block_pattern = 0x3F;

			mb++;

			for (by = 0; by < MACROBLOCK_HEIGHT_IN_BLOCKS; ++by)
				for (bx = 0; bx < MACROBLOCK_WIDTH_IN_BLOCKS; ++bx)
				{
					const int start = 16, stop = 235, range = stop - start;

					Gradient
					(
						blocks,
						(short)(start + range * ((mbx * MACROBLOCK_WIDTH + bx * BLOCK_WIDTH) / (float)(INPUT_WIDTH - 1))),
						(short)(start + range * ((mbx * MACROBLOCK_WIDTH + bx * BLOCK_WIDTH + BLOCK_WIDTH - 1) / (float)(INPUT_WIDTH - 1))),
						1
					);

					blocks += BLOCK_SIZE;
				}

			for (by = 0; by < MACROBLOCK_HEIGHT_IN_BLOCKS / 2; ++by)
				for (bx = 0; bx < MACROBLOCK_WIDTH_IN_BLOCKS / 2; ++bx)
				{
					const int start = 16, stop = 240, range = stop - start;

					Gradient
					(
						blocks,
						(short)(start + range * ((mbx * MACROBLOCK_WIDTH + bx * BLOCK_WIDTH) / (float)(INPUT_WIDTH - 1))),
						(short)(start + range * ((mbx * MACROBLOCK_WIDTH + bx * BLOCK_WIDTH + BLOCK_WIDTH - 1) / (float)(INPUT_WIDTH - 1))),
						1
					);

					blocks += BLOCK_SIZE;

					Gradient
					(
						blocks,
						(short)(start + range * ((mbx * MACROBLOCK_WIDTH + bx * BLOCK_WIDTH) / (float)(INPUT_WIDTH - 1))),
						(short)(start + range * ((mbx * MACROBLOCK_WIDTH + bx * BLOCK_WIDTH + BLOCK_WIDTH - 1) / (float)(INPUT_WIDTH - 1))),
						1
					);

					blocks += BLOCK_SIZE;
				}
		}

	XSelectInput(display, window, ExposureMask | KeyPressMask);
	XMapWindow(display, window);
	XSync(display, 0);

	/* Test NULL context */
	assert(XvMCRenderSurface(display, NULL, XVMC_FRAME_PICTURE, &surface, NULL, NULL, 0, NUM_MACROBLOCKS, 0, &mb_array, &block_array) == XvMCBadContext);
	/* Test NULL surface */
	assert(XvMCRenderSurface(display, &context, XVMC_FRAME_PICTURE, NULL, NULL, NULL, 0, NUM_MACROBLOCKS, 0, &mb_array, &block_array) == XvMCBadSurface);
	/* Test bad picture structure */
	assert(XvMCRenderSurface(display, &context, 0, &surface, NULL, NULL, 0, NUM_MACROBLOCKS, 0, &mb_array, &block_array) == BadValue);
	/* Test valid params */
	assert(XvMCRenderSurface(display, &context, XVMC_FRAME_PICTURE, &surface, NULL, NULL, 0, NUM_MACROBLOCKS, 0, &mb_array, &block_array) == Success);

	/* Test NULL surface */
	assert(XvMCPutSurface(display, NULL, window, 0, 0, INPUT_WIDTH, INPUT_HEIGHT, 0, 0, output_width, output_height, XVMC_FRAME_PICTURE) == XvMCBadSurface);
	/* Test bad window */
	/* XXX: X halts with a bad drawable for some reason, doesn't return BadDrawable as expected */
	/*assert(XvMCPutSurface(display, &surface, 0, 0, 0, width, height, 0, 0, width, height, XVMC_FRAME_PICTURE) == BadDrawable);*/

	if (prompt)
	{
		puts("Press any button to quit...");

		while (!quit)
		{
			if (XPending(display) > 0)
			{
				XEvent event;

				XNextEvent(display, &event);

				switch (event.type)
				{
					case Expose:
					{
						/* Test valid params */
						assert
						(
							XvMCPutSurface
							(
								display, &surface, window,
								0, 0, INPUT_WIDTH, INPUT_HEIGHT,
								0, 0, output_width, output_height,
								XVMC_FRAME_PICTURE
							) == Success
						);
						break;
					}
					case KeyPress:
					{
						quit = 1;
						break;
					}
				}
			}
		}
	}

	assert(XvMCDestroyBlocks(display, &block_array) == Success);
	assert(XvMCDestroyMacroBlocks(display, &mb_array) == Success);
	assert(XvMCDestroySurface(display, &surface) == Success);
	assert(XvMCDestroyContext(display, &context) == Success);

	XvUngrabPort(display, port_num, CurrentTime);
	XDestroyWindow(display, window);
	XCloseDisplay(display);

	return 0;
}
