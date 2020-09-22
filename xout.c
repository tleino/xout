/*
 * xout - X11 input forwarder
 * Copyright (c) 2020 Tommi M. Leino <tleino@me.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <X11/Xmu/WinUtil.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XKBrules.h>
#include <unistd.h>
#include <X11/keysym.h>
#include <unistd.h>
#include <err.h>
#include <errno.h>

static Window create_select_window(Display *, GC gc);
static GC create_text_gc(Display *, const char *);
static Window change_target(Display *, Window, GC, Cursor);
static Window get_window_at_cursor(Display *, Cursor);
static KeySym wait_key(Display *);
static void store_cursor_xy(Display *, int *, int *);
static void restore_cursor_xy(Display *, int, int);
static KeySym grab(Display *, Cursor, Window, int, KeySym *);
static void delta_from_center(Display *, int, int, int *, int *);
static void warp_center(Display *dpy);
static void forward_str(Display *, XEvent *, const char *, Window);
static void forward_xmotion(Display *, XEvent *, Window);
static void forward_xkey(Display *, XEvent *, Window);
static void forward_xbutton(Display *, XEvent *, Window);
static void update_mapping(Display *, XEvent *);
static void forward_mapping(Display *, Window);

int
main(int argc, char **argv)
{
	Display *dpy;
	char *denv;
	Cursor cursor;			/* Stores empty cursor */
	Cursor crosshair;
	Pixmap pixmap;			/* For empty cursor */
	const char data[] = { 0 };	/* Empty cursor */
	XColor color = { 0 };		/* Cursor color */
	Window target = None, selwin;
	int x, y, xkbmaj, xkbmin, xkb_op, xkb_event, xkb_error;
	KeySym toggle_forward_key, change_target_key, keysym, break_keys[2];
	GC gc;

#ifdef __OpenBSD__
	if (pledge("stdio rpath prot_exec dns unix inet", NULL) != 0)
		err(1, "pledge");
#endif

	/* Establish X11 connection */
	if ((denv = getenv("DISPLAY")) == NULL && errno != 0)
		err(1, "getenv");
	if ((dpy = XOpenDisplay(denv)) == NULL) {
		if (denv == NULL)
			errx(1, "X11 connection failed; "
			    "DISPLAY environment variable not set?");
		else
			errx(1, "failed X11 connection to '%s'", denv);
	}

#ifdef __OpenBSD__
	if (pledge("stdio rpath prot_exec", NULL) != 0)
		err(1, "pledge");
#endif

	/*
	 * We use XKB extension because XKeycodeToKeysym is deprecated,
	 * even though there would be no big harm.
	 * 
	 * We also need XKB for using the undocumented XkbRF function
	 * for getting a human-readable layout name so that we can forward
	 * it to remote for running setxkbmap - for simplicity, not a
	 * complete solution because this does not obviously send custom
	 * mappings but custom mappings can be supported by also having
	 * non-adhoc custom mapping in the remote, named under a proper
	 * layout for it.
	 */
	xkbmaj = XkbMajorVersion;
	xkbmin = XkbMinorVersion;
	if (XkbLibraryVersion(&xkbmaj, &xkbmin) == False)
		errx(1, "trouble with XKB extension; needed %d.%d got %d.%d",
		    XkbMajorVersion, XkbMinorVersion, xkbmaj, xkbmin);
	if (XkbQueryExtension(dpy, &xkb_op, &xkb_event, &xkb_error,
	    &xkbmaj, &xkbmin) == False)
		errx(1, "trouble with XKB extension");

	/* Create empty cursor */
	pixmap = XCreateBitmapFromData(dpy, RootWindow(dpy, 0), data, 1, 1);
	cursor = XCreatePixmapCursor(dpy, pixmap, pixmap, &color,
	    &color, 0, 0);
	XFreePixmap(dpy, pixmap);
	if (cursor == None)
		errx(1, "failed to create empty cursor");

	/* Create crosshair cursor */
	crosshair = XCreateFontCursor(dpy, XC_crosshair);
	if (crosshair == None)
		errx(1, "failed to create crosshair cursor");

	if ((toggle_forward_key = XStringToKeysym("Super_R")) == NoSymbol)
		errx(1, "no KeySym for toggle-forward key");
	if ((change_target_key = XStringToKeysym("Menu")) == NoSymbol)
		errx(1, "no KeySym for change-target key");
	XGrabKey(dpy, XKeysymToKeycode(dpy, toggle_forward_key), 0,
	    RootWindow(dpy, 0), 0, 0, 1);
	XGrabKey(dpy, XKeysymToKeycode(dpy, change_target_key), 0,
	    RootWindow(dpy, 0), 0, 0, 1);

	gc = create_text_gc(dpy, "fixed");
	selwin = create_select_window(dpy, gc);
	XSync(dpy, False);

#if 0
	target = change_target(dpy, selwin, gc, crosshair);
#endif

#ifdef __OpenBSD__
	if (pledge("stdio", NULL) != 0)
		err(1, "pledge");
#endif

	keysym = None;
	for (;;) {
		if (keysym == change_target_key)
			target = change_target(dpy, selwin, gc, crosshair);
		else {
			keysym = wait_key(dpy);
			if (keysym == change_target_key)
				target = change_target(dpy, selwin, gc,
				    crosshair);
		}

		if (target == None) {
			keysym = None;
			continue;
		}

		store_cursor_xy(dpy, &x, &y);

		warp_center(dpy);

		forward_mapping(dpy, target);

		break_keys[0] = toggle_forward_key;
		break_keys[1] = change_target_key;
		keysym = grab(dpy, cursor, target, 2, break_keys);

		restore_cursor_xy(dpy, x, y);
	}

	XFreeCursor(dpy, cursor);
	XFreeCursor(dpy, crosshair);
	return 0;
}

static Window
change_target(Display *dpy, Window selwin, GC gc, Cursor crosshair)
{
	static const char *s =
	    "Select target window for event forwarding";
	Window target;

	XMapRaised(dpy, selwin);
	XDrawString(dpy, selwin, gc, 8, 15, s, strlen(s));
	XSync(dpy, False);
	target = get_window_at_cursor(dpy, crosshair);
	XUnmapWindow(dpy, selwin);

	return target;
}

static KeySym
wait_key(Display *dpy)
{
	XEvent e;

	for (;;) {
		XNextEvent(dpy, &e);
		switch (e.type) {
		case KeyRelease:
			return XkbKeycodeToKeysym(dpy,
			    e.xkey.keycode, 0,
			    e.xkey.state & ShiftMask ? 1 : 0);
		case MappingNotify:
			update_mapping(dpy, &e);
			break;
		}
	}

	return NoSymbol;
}

static GC
create_text_gc(Display *dpy, const char *font)
{
	GC gc;
	XGCValues v;
	int mask;

	v.foreground = WhitePixel(dpy, 0);
	v.background = BlackPixel(dpy, 0);
	v.font = XLoadFont(dpy, font);
	mask = GCForeground | GCBackground | GCFont;
	gc = XCreateGC(dpy, RootWindow(dpy, 0), mask, &v);

	return gc;
}

static Window
create_select_window(Display *dpy, GC gc)
{
	XSetWindowAttributes a;
	int mask, w, h, x, y;
	Window win;

	a.override_redirect = True;
	a.background_pixel = 43434343;
	mask = CWOverrideRedirect | CWBackPixel;

	w = DisplayWidth(dpy, 0) / 6;
	h = 40;
	x = (DisplayWidth(dpy, 0) - w) / 2;
	y = (DisplayHeight(dpy, 0) - h) / 2;

	win = XCreateWindow(dpy, RootWindow(dpy, 0), x, y, w, h, 0,
	    CopyFromParent,InputOutput, CopyFromParent, mask, &a);

	return win;
}

static Window
get_window_at_cursor(Display *dpy, Cursor crosshair)
{
	Window root;
	Window retwin = None;
	const int mask = ButtonReleaseMask;

	root = RootWindow(dpy, 0);
	if (XGrabPointer(dpy, root, False, mask,
	    GrabModeSync, GrabModeAsync, None, crosshair, CurrentTime) !=
	    GrabSuccess)
		errx(1, "unable to grab cursor");

	while (retwin == None) {
		XEvent e;

		XAllowEvents(dpy, SyncPointer, CurrentTime);
		XWindowEvent(dpy, root, mask, &e);
		switch (e.type) {
		case ButtonRelease:
			retwin = e.xbutton.subwindow;
			break;
		}
	}

	XUngrabPointer(dpy, CurrentTime);
	XSync(dpy, 0);
	return XmuClientWindow(dpy, retwin);
}

static void
store_cursor_xy(Display *dpy, int *x, int *y)
{
	XButtonEvent xb;

	XQueryPointer(dpy, RootWindow(dpy, 0),
	    &xb.root, &xb.window,
	    &xb.x_root, &xb.y_root, &xb.x, &xb.y,
	    &xb.state);

	*x = xb.x_root;
	*y = xb.y_root;
}

static void
restore_cursor_xy(Display *dpy, int x, int y)
{
	XWarpPointer(dpy, None, RootWindow(dpy, 0), 0, 0, 0, 0, x, y);
	XSync(dpy, False);
}

static KeySym
grab(Display *dpy, Cursor cursor, Window target, int nbreak, KeySym *breaks)
{
	XEvent e;
	XKeyEvent *ke;
	int grabbed, npress, i, ignore;
	KeySym keysym;

	XSelectInput(dpy, RootWindow(dpy, 0),
	    PointerMotionMask |
	    ButtonPressMask |
	    ButtonReleaseMask |
	    KeyPressMask |
	    KeyReleaseMask);
	XSync(dpy, 0);

	XGrabPointer(dpy, RootWindow(dpy, 0), False,
	    PointerMotionMask |
	    ButtonReleaseMask |
	    ButtonPressMask,
	    GrabModeAsync, GrabModeAsync, None, cursor, CurrentTime);

	XGrabKeyboard(dpy, RootWindow(dpy, 0), False,
	    GrabModeAsync, GrabModeAsync, CurrentTime);

	grabbed = 1;
	npress = 0;
	while (grabbed) {
		XNextEvent(dpy, &e);
		switch (e.type) {
		case KeyPress:
			npress++;
			/* FALLTHROUGH */
		case KeyRelease:
			ke = &e.xkey;

			keysym = XkbKeycodeToKeysym(dpy,
			    ke->keycode, 0,
			    ke->state & ShiftMask ? 1 : 0);
			ignore = 0;
			for (i = 0; i < nbreak; i++) {
				if (keysym != breaks[i])
					continue;
				ignore = 1;
				if (e.type == KeyRelease)
					grabbed = 0;
			}
			if (ignore == 1)
				break;

			/*
			 * We wish to avoid a situation where we have
			 * received KeyRelease but no KeyPress for it,
			 * which means we're e.g. entering after a
			 * break which would leave us into an inconsistent
			 * state.
			 */
			if (e.type == KeyPress || npress > 0) {
				forward_xkey(dpy, &e, target);
				if (e.type == KeyRelease)
					npress--;
			} else if (npress < 1)
				warnx("skipped sending keyrelease (npress=%d)", npress);
			break;
		case ButtonPress:
		case ButtonRelease:
			forward_xbutton(dpy, &e, target);
			break;
		case MotionNotify:
			forward_xmotion(dpy, &e, target);
			break;
		case MappingNotify:
			update_mapping(dpy, &e);
			forward_mapping(dpy, target);
			break;
		}
	}

	XUngrabPointer(dpy, CurrentTime);
	XUngrabKeyboard(dpy, CurrentTime);

	XSelectInput(dpy, RootWindow(dpy, 0), None);
	XSync(dpy, False);

	return keysym;
}

static void
forward_str(Display *dpy, XEvent *e, const char *s, Window target)
{
	const char *p;

	for (p = s; *p != '\0'; p++) {
		if (!isalnum(*p) && *p != ' ' && *p != '\r' && *p != '-')
			errx(1, "attempted to send bogus control string "
			    "(*p=%02x)", *p);

		/*
		 * TODO: We're reusing XEvent because we couldn't figure
		 *       out how to initialize XEvent from scratch without
		 *       things breaking up.
		 */
		if (iscntrl(*p))
			e->xkey.keycode = XKeysymToKeycode(dpy, 0xff00 + *p);
		else
			e->xkey.keycode = XKeysymToKeycode(dpy, *p);

		e->xkey.window = target;
		e->xkey.subwindow = target;
		if (isupper(*p))
			e->xkey.state = ShiftMask;
		else
			e->xkey.state = 0;

		e->xkey.type = KeyPress;
		e->xkey.time = CurrentTime;
		XSendEvent(dpy, target, False, KeyPressMask, e);

/*
 * Disabling KeyRelease speeds things up a bit. Enable it if
 * if some program absolutely needs it for some reason.
 */
#if 0
		e->xkey.type = KeyRelease;
		e->xkey.time = CurrentTime;
		XSendEvent(dpy, target, False, KeyReleaseMask, e);
#endif
	}
}

static void
forward_xkey(Display *dpy, XEvent *e, Window target)
{
	XKeyEvent *ke = &e->xkey;
	char s[16];
	KeySym sym;

	sym = XkbKeycodeToKeysym(dpy, ke->keycode, 0,
	    ke->state & ShiftMask ? 1 : 0);
	if (snprintf(s, sizeof(s), "%c %lu\r", ke->type == KeyPress ?
	    'k' : 'K', sym) >= sizeof(s)) {
		warnx("xkey forward did not fit %lu bytes", sizeof(s));
		return;
	}
	forward_str(dpy, e, s, target);
}

static void
forward_xbutton(Display *dpy, XEvent *e, Window target)
{
	XButtonEvent *be = &e->xbutton;
	char s[16];

	if (snprintf(s, sizeof(s), "%c %u %u\r", be->type == ButtonPress ?
	    'b' : 'B', be->state, be->button) >=
	    sizeof(s)) {
		warnx("xbutton forward did not fit %lu bytes", sizeof(s));
		return;
	}
	forward_str(dpy, e, s, target);
}

static void
delta_from_center(Display *dpy, int x, int y, int *dx, int *dy)
{
	int cx, cy;

	cx = DisplayWidth(dpy, DefaultScreen(dpy)) / 2;
	cy = DisplayHeight(dpy, DefaultScreen(dpy)) / 2;

	*dx = cx - x;
	*dy = cy - y;
}

static void
warp_center(Display *dpy)
{
	int cx, cy;

	cx = DisplayWidth(dpy, DefaultScreen(dpy)) / 2;
	cy = DisplayHeight(dpy, DefaultScreen(dpy)) / 2;

	XWarpPointer(dpy, None, RootWindow(dpy, 0), 0, 0, 0, 0, cx, cy);
}

static void
forward_xmotion(Display *dpy, XEvent *e, Window target)
{
	XMotionEvent *me;
	static XButtonEvent xb;		/* Last pointer status */
	int dx, dy;			/* Delta to last */
	char s[16];

	/* First time around, we need to fetch pointer status. */
	if (xb.root == None) {
		xb.root = RootWindow(dpy, 0);
		xb.x_root = DisplayWidth(dpy, DefaultScreen(dpy)) / 2;
		xb.y_root = DisplayHeight(dpy, DefaultScreen(dpy)) / 2;
	}

	/* Calculate delta. */
	me = &e->xmotion;
	delta_from_center(dpy, me->x_root, me->y_root, &dx, &dy);

	/* Let's skip forwarding if we have nothing to do. */
	if (dx == 0 && dy == 0)
		return;

	/* Warp pointer back to original location. */
	warp_center(dpy);

	/* Construct the forward string. */
	if (snprintf(s, sizeof(s), "m %d %d\r", dx, dy) >= sizeof(s)) {
		warnx("xmotion forward did not fit %lu bytes", sizeof(s));
		return;
	}

	/* Forward. */
	forward_str(dpy, e, s, target);
}

static void
update_mapping(Display *dpy, XEvent *e)
{
	if (e->xmapping.request == MappingKeyboard)
		XRefreshKeyboardMapping(&e->xmapping);
}

#ifndef DFLT_XKB_LAYOUT
#define DFLT_XKB_LAYOUT "us"
#endif

static void
forward_mapping(Display *dpy, Window target)
{
	XkbRF_VarDefsRec vd;
	char *tmp;
	XEvent e = { 0 };
	char s[32];

	tmp = NULL;
	if (!XkbRF_GetNamesProp(dpy, &tmp, &vd) || tmp == NULL) {
		warnx("couldn't interpret %s", _XKB_RF_NAMES_PROP_ATOM);
		vd.layout = DFLT_XKB_LAYOUT;
	}

	if (snprintf(s, sizeof(s), "l %s\r", vd.layout) >= sizeof(s)) {
		warnx("layout forward did not fit %lu bytes", sizeof(s));
		return;
	}
	forward_str(dpy, &e, s, target);
}
