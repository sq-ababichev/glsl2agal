/*
 * Copyright (C) 2009 Francisco Jerez.
 * All Rights Reserved.
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

#include "nouveau_driver.h"
#include "nouveau_context.h"
#include "nouveau_util.h"
#include "nouveau_class.h"
#include "nv04_driver.h"

#define COMBINER_SHIFT(in)						\
	(NV04_MULTITEX_TRIANGLE_COMBINE_COLOR_ARGUMENT##in##_SHIFT	\
	 - NV04_MULTITEX_TRIANGLE_COMBINE_COLOR_ARGUMENT0_SHIFT)
#define COMBINER_SOURCE(reg)					\
	NV04_MULTITEX_TRIANGLE_COMBINE_COLOR_ARGUMENT0_##reg
#define COMBINER_INVERT					\
	NV04_MULTITEX_TRIANGLE_COMBINE_COLOR_INVERSE0
#define COMBINER_ALPHA					\
	NV04_MULTITEX_TRIANGLE_COMBINE_COLOR_ALPHA0

struct combiner_state {
	int unit;
	GLboolean alpha;

	/* GL state */
	GLenum mode;
	GLenum *source;
	GLenum *operand;
	GLuint logscale;

	/* Derived HW state */
	uint32_t hw;
};

#define __INIT_COMBINER_ALPHA_A GL_TRUE
#define __INIT_COMBINER_ALPHA_RGB GL_FALSE

/* Initialize a combiner_state struct from the texture unit
 * context. */
#define INIT_COMBINER(chan, rc, i) do {				\
		struct gl_tex_env_combine_state *c =		\
			ctx->Texture.Unit[i]._CurrentCombine;	\
		(rc)->alpha = __INIT_COMBINER_ALPHA_##chan;	\
		(rc)->unit = i;					\
		(rc)->mode = c->Mode##chan;			\
		(rc)->source = c->Source##chan;			\
		(rc)->operand = c->Operand##chan;		\
		(rc)->logscale = c->ScaleShift##chan;		\
		(rc)->hw = 0;					\
	} while (0)

/* Get the combiner source for the specified EXT_texture_env_combine
 * argument. */
static uint32_t
get_arg_source(struct combiner_state *rc, int arg)
{
	switch (rc->source[arg]) {
	case GL_TEXTURE:
		return rc->unit ? COMBINER_SOURCE(TEXTURE1) :
			COMBINER_SOURCE(TEXTURE0);

	case GL_TEXTURE0:
		return COMBINER_SOURCE(TEXTURE0);

	case GL_TEXTURE1:
		return COMBINER_SOURCE(TEXTURE1);

	case GL_CONSTANT:
		return COMBINER_SOURCE(CONSTANT);

	case GL_PRIMARY_COLOR:
		return COMBINER_SOURCE(PRIMARY_COLOR);

	case GL_PREVIOUS:
		return rc->unit ? COMBINER_SOURCE(PREVIOUS) :
			COMBINER_SOURCE(PRIMARY_COLOR);

	default:
		assert(0);
	}
}

/* Get the (possibly inverted) combiner input mapping for the
 * specified argument. */
#define INVERT 0x1

static uint32_t
get_arg_mapping(struct combiner_state *rc, int arg, int flags)
{
	int map = 0;

	switch (rc->operand[arg]) {
	case GL_SRC_COLOR:
	case GL_ONE_MINUS_SRC_COLOR:
		break;

	case GL_SRC_ALPHA:
	case GL_ONE_MINUS_SRC_ALPHA:
		map |= rc->alpha ? 0 : COMBINER_ALPHA;
		break;
	}

	switch (rc->operand[arg]) {
	case GL_SRC_COLOR:
	case GL_SRC_ALPHA:
		map |= flags & INVERT ? COMBINER_INVERT : 0;
		break;

	case GL_ONE_MINUS_SRC_COLOR:
	case GL_ONE_MINUS_SRC_ALPHA:
		map |= flags & INVERT ? 0 : COMBINER_INVERT;
		break;
	}

	return map;
}

/* Bind the combiner input <in> to the combiner source <src>,
 * possibly inverted. */
#define INPUT_SRC(rc, in, src, flags)					\
	(rc)->hw |= ((flags & INVERT ? COMBINER_INVERT : 0) |		\
		   COMBINER_SOURCE(src)) << COMBINER_SHIFT(in)

/* Bind the combiner input <in> to the EXT_texture_env_combine
 * argument <arg>, possibly inverted. */
#define INPUT_ARG(rc, in, arg, flags)					\
	(rc)->hw |= (get_arg_source(rc, arg) |				\
		     get_arg_mapping(rc, arg, flags)) << COMBINER_SHIFT(in)

#define UNSIGNED_OP(rc)							\
	(rc)->hw |= ((rc)->logscale ?					\
		     NV04_MULTITEX_TRIANGLE_COMBINE_COLOR_MAP_SCALE2 :	\
		     NV04_MULTITEX_TRIANGLE_COMBINE_COLOR_MAP_IDENTITY)
#define SIGNED_OP(rc)							\
	(rc)->hw |= ((rc)->logscale ?					\
		     NV04_MULTITEX_TRIANGLE_COMBINE_COLOR_MAP_BIAS_SCALE2 : \
		     NV04_MULTITEX_TRIANGLE_COMBINE_COLOR_MAP_BIAS)

static void
setup_combiner(struct combiner_state *rc)
{
	switch (rc->mode) {
	case GL_REPLACE:
		INPUT_ARG(rc, 0, 0, 0);
		INPUT_SRC(rc, 1, ZERO, INVERT);
		INPUT_SRC(rc, 2, ZERO, 0);
		INPUT_SRC(rc, 3, ZERO, 0);
		UNSIGNED_OP(rc);
		break;

	case GL_MODULATE:
		INPUT_ARG(rc, 0, 0, 0);
		INPUT_ARG(rc, 1, 1, 0);
		INPUT_SRC(rc, 2, ZERO, 0);
		INPUT_SRC(rc, 3, ZERO, 0);
		UNSIGNED_OP(rc);
		break;

	case GL_ADD:
		INPUT_ARG(rc, 0, 0, 0);
		INPUT_SRC(rc, 1, ZERO, INVERT);
		INPUT_ARG(rc, 2, 1, 0);
		INPUT_SRC(rc, 3, ZERO, INVERT);
		UNSIGNED_OP(rc);
		break;

	case GL_INTERPOLATE:
		INPUT_ARG(rc, 0, 0, 0);
		INPUT_ARG(rc, 1, 2, 0);
		INPUT_ARG(rc, 2, 1, 0);
		INPUT_ARG(rc, 3, 2, INVERT);
		UNSIGNED_OP(rc);
		break;

	case GL_ADD_SIGNED:
		INPUT_ARG(rc, 0, 0, 0);
		INPUT_SRC(rc, 1, ZERO, INVERT);
		INPUT_ARG(rc, 2, 1, 0);
		INPUT_SRC(rc, 3, ZERO, INVERT);
		SIGNED_OP(rc);
		break;

	default:
		assert(0);
	}
}

void
nv04_emit_tex_env(GLcontext *ctx, int emit)
{
	const int i = emit - NOUVEAU_STATE_TEX_ENV0;
	struct nouveau_channel *chan = context_chan(ctx);
	struct nouveau_grobj *fahrenheit = nv04_context_engine(ctx);
	struct combiner_state rc_a = {}, rc_c = {};

	if (!nv04_mtex_engine(fahrenheit)) {
		context_dirty(ctx, BLEND);
		return;
	}

	/* Compute the new combiner state. */
	if (ctx->Texture.Unit[i]._ReallyEnabled) {
		INIT_COMBINER(A, &rc_a, i);
		setup_combiner(&rc_a);

		INIT_COMBINER(RGB, &rc_c, i);
		setup_combiner(&rc_c);

	} else {
		if (i == 0) {
			INPUT_SRC(&rc_a, 0, PRIMARY_COLOR, 0);
			INPUT_SRC(&rc_c, 0, PRIMARY_COLOR, 0);
		} else {
			INPUT_SRC(&rc_a, 0, PREVIOUS, 0);
			INPUT_SRC(&rc_c, 0, PREVIOUS, 0);
		}

		INPUT_SRC(&rc_a, 1, ZERO, INVERT);
		INPUT_SRC(&rc_c, 1, ZERO, INVERT);
		INPUT_SRC(&rc_a, 2, ZERO, 0);
		INPUT_SRC(&rc_c, 2, ZERO, 0);
		INPUT_SRC(&rc_a, 3, ZERO, 0);
		INPUT_SRC(&rc_c, 3, ZERO, 0);

		UNSIGNED_OP(&rc_a);
		UNSIGNED_OP(&rc_c);
	}

	/* Write the register combiner state out to the hardware. */
	BEGIN_RING(chan, fahrenheit,
		   NV04_MULTITEX_TRIANGLE_COMBINE_ALPHA(i), 2);
	OUT_RING(chan, rc_a.hw);
	OUT_RING(chan, rc_c.hw);

	BEGIN_RING(chan, fahrenheit,
		   NV04_MULTITEX_TRIANGLE_COMBINE_FACTOR, 1);
	OUT_RING(chan, pack_rgba_f(MESA_FORMAT_ARGB8888,
				   ctx->Texture.Unit[0].EnvColor));
}