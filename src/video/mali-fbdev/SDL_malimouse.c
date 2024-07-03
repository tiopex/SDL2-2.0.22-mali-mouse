/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2022 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include "../../SDL_internal.h"

#if SDL_VIDEO_DRIVER_MALI

#include "SDL_malivideo.h"
#include "SDL_malimouse.h"
#include "SDL_egl.h"
#include "SDL_opengl.h"
#include "SDL_opengles2.h"

#include "../../events/SDL_mouse_c.h"
#include "../../events/default_cursor.h"

#include "../SDL_pixels_c.h"

static SDL_Cursor *MALI_CreateDefaultCursor(void);
static SDL_Cursor *MALI_CreateCursor(SDL_Surface * surface, int hot_x, int hot_y);
static int MALI_ShowCursor(SDL_Cursor * cursor);
static void MALI_MoveCursor(SDL_Cursor * cursor);
static void MALI_FreeCursor(SDL_Cursor * cursor);
static void MALI_WarpMouse(SDL_Window * window, int x, int y);
static int MALI_WarpMouseGlobal(int x, int y);

/**************************************************************************************/
/* BEFORE CODING ANYTHING MOUSE/CURSOR RELATED, REMEMBER THIS.                        */
/* How does SDL manage cursors internally? First, mouse =! cursor. The mouse can have */
/* many cursors in mouse->cursors.                                                    */
/* -SDL tells us to create a cursor with MALI_CreateCursor(). It can create many    */
/*  cursosr with this, not only one.                                                  */
/* -SDL stores those cursors in a cursors array, in mouse->cursors.                   */
/* -Whenever it wants (or the programmer wants) takes a cursor from that array        */
/*  and shows it on screen with MALI_ShowCursor().                                  */
/*  MALI_ShowCursor() simply shows or hides the cursor it receives: it does NOT     */
/*  mind if it's mouse->cur_cursor, etc.                                              */
/* -If MALI_ShowCursor() returns succesfully, that cursor becomes mouse->cur_cursor */
/*  and mouse->cursor_shown is 1.                                                     */
/**************************************************************************************/

static SDL_Cursor *
MALI_CreateDefaultCursor(void)
{
	return SDL_CreateCursor(default_cdata, default_cmask, DEFAULT_CWIDTH, DEFAULT_CHEIGHT, DEFAULT_CHOTX, DEFAULT_CHOTY);
}

/* Given a display's driverdata, destroy the cursor BO for it.
   To be called from MALI_DestroyWindow(), as that's where we
   destroy the driverdata for the window's display. */

/* This is only for freeing the SDL_cursor.*/
static void
MALI_FreeCursor(SDL_Cursor * cursor)
{
	MALI_CursorData *curdata;

	/* Even if the cursor is not ours, free it. */
	if (cursor) {
		curdata = (MALI_CursorData *) cursor->driverdata;
		/* Free cursor buffer */
		if (curdata->buffer) {
			SDL_free(curdata->buffer);
			curdata->buffer = NULL;
		}
		/* Free cursor itself */
		if (cursor->driverdata) {
			SDL_free(cursor->driverdata);
		}
		SDL_free(cursor);
	}
}

/* This simply gets the cursor soft-buffer ready.
   We don't copy it to a GBO BO until ShowCursor() because the cusor GBM BO (living
   in dispata) is destroyed and recreated when we recreate windows, etc. */
static SDL_Cursor *
MALI_CreateCursor(SDL_Surface * surface, int hot_x, int hot_y)
{
	MALI_CursorData *curdata;
	SDL_Cursor *cursor, *ret;

	curdata = NULL;
	ret = NULL;

	cursor = (SDL_Cursor *) SDL_calloc(1, sizeof(*cursor));
	if (!cursor) {
		SDL_OutOfMemory();
		goto cleanup;
	}
	curdata = (MALI_CursorData *) SDL_calloc(1, sizeof(*curdata));
	if (!curdata) {
		SDL_OutOfMemory();
		goto cleanup;
	}

	/* hox_x and hot_y are the coordinates of the "tip of the cursor" from it's base. */
	curdata->hot_x = hot_x;
	curdata->hot_y = hot_y;
	curdata->w = surface->w;
	curdata->h = surface->h;
	curdata->buffer = NULL;

	/* Configure the cursor buffer info.
	   This buffer has the original size of the cursor surface we are given. */
	curdata->buffer_pitch = surface->w;
	curdata->buffer_size = surface->w * surface->h * 4;
	curdata->buffer = (uint32_t*)SDL_malloc(curdata->buffer_size);

	if (!curdata->buffer) {
		SDL_OutOfMemory();
		goto cleanup;
	}

	/* All code below assumes ARGB8888 format for the cursor surface,
	   like other backends do. Also, the GBM BO pixels have to be
	   alpha-premultiplied, but the SDL surface we receive has
	   straight-alpha pixels, so we always have to convert. */
	SDL_PremultiplyAlpha(surface->w, surface->h,
						 surface->format->format, surface->pixels, surface->pitch,
						 SDL_PIXELFORMAT_ARGB8888, curdata->buffer, surface->w * 4);

	cursor->driverdata = curdata;

	ret = cursor;

	cleanup:
	if (ret == NULL) {
		if (curdata) {
			if (curdata->buffer) {
				SDL_free(curdata->buffer);
			}
			SDL_free(curdata);
		}
		if (cursor) {
			SDL_free(cursor);
		}
	}

	return ret;
}

/* Show the specified cursor, or hide if cursor is NULL or has no focus. */
static int
MALI_ShowCursor(SDL_Cursor * cursor)
{
	SDL_VideoDisplay *display;
	SDL_Window *window;
	SDL_Mouse *mouse;

	int ret = 0;

	/* Get the mouse focused window, if any. */

	mouse = SDL_GetMouse();
	if (!mouse) {
		return SDL_SetError("No mouse.");
	}

	window = mouse->focus;

	if (!window || !cursor) {

		/* If no window is focused by mouse or cursor is NULL,
		   since we have no window (no mouse->focus) and hence
		   we have no display, we simply hide mouse on all displays.
		   This happens on video quit, where we get here after
		   the mouse focus has been unset, yet SDL wants to
		   restore the system default cursor (makes no sense here). */

	} else {

		display = SDL_GetDisplayForWindow(window);

		if (display) {

			if (cursor) {
				/* Dump the cursor to the display DRM cursor BO so it becomes visible
				   on that display. */
				ret = SDL_ShowCursor(1);

			} else {
				/* Hide the cursor on that display. */
				ret =SDL_ShowCursor(0);
			}
		}
	}

	return ret;
}

/* Warp the mouse to (x,y) */
static void
MALI_WarpMouse(SDL_Window * window, int x, int y)
{
	/* Only one global/fullscreen window is supported */
	MALI_WarpMouseGlobal(x, y);
}

/* Warp the mouse to (x,y) */
static int
MALI_WarpMouseGlobal(int x, int y)
{
	SDL_Mouse *mouse = SDL_GetMouse();

	if (mouse && mouse->cur_cursor && mouse->focus) {

		SDL_Window *window = mouse->focus;

		/* Update internal mouse position. */
		SDL_SendMouseMotion(mouse->focus, mouse->mouseID, 0, x, y);

		/* And now update the cursor graphic position on screen. */
		SDL_WarpMouseInWindow(window, x, y);

	} else {
		return SDL_SetError("No mouse or current cursor.");
	}
	return 0;
}

void
MALI_InitMouse(_THIS, SDL_VideoDisplay *display)
{
	SDL_Mouse *mouse = SDL_GetMouse();

	mouse->CreateCursor = MALI_CreateCursor;
	mouse->ShowCursor = MALI_ShowCursor;
	mouse->MoveCursor = MALI_MoveCursor;
	mouse->FreeCursor = MALI_FreeCursor;
	mouse->WarpMouse = MALI_WarpMouse;
	mouse->WarpMouseGlobal = MALI_WarpMouseGlobal;

	/* Only create the default cursor for this display if we haven't done so before,
	   we don't want several cursors to be created for the same display. */
		SDL_SetDefaultCursor(MALI_CreateDefaultCursor());
}

void
MALI_QuitMouse(_THIS)
{
	/* TODO: ? */
}

/* This is called when a mouse motion event occurs */
static void
MALI_MoveCursor(SDL_Cursor * cursor)
{
	SDL_Mouse *mouse = SDL_GetMouse();


	/* We must NOT call SDL_SendMouseMotion() here or we will enter recursivity!
	   That's why we move the cursor graphic ONLY. */
	if (mouse && mouse->cur_cursor && mouse->focus) {

		SDL_Window *window = mouse->focus;

		//SDL_WarpMouseInWindow(window, mouse->x, mouse->y);
	}
}

#endif /* SDL_VIDEO_DRIVER_KMSDRM */

/* vi: set ts=4 sw=4 expandtab: */
