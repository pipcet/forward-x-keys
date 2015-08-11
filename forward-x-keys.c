/*
 * Author Email: pipcet@gmail.com
 * GitHub URL: http://github.com/pipcet/forward-x-keys
 * Licence: GNU LGPL v3
 */

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <X11/Intrinsic.h>
#include <X11/Core.h>

/* grab X keys globally and forward them as though they had
 * happened in specific windows. */

/* usage fxk <window id> <subwindow id> <key1> <key2> ...
 *
 * xdotool works for finding the window/subwindow ids (xwininfo also
 * works, particularly for interactive use). If the application is not
 * responding to the forwarded keys, it's likely to ignore send_key
 * events; in that case, the hack at
 * http://www.semicomplete.com/blog/tags/xsendevent might work.
 *
 * Surprisingly, this code does appear to work with Firefox!
 *
 * Apparently the magic number to make this work with xtightvncviewer is 4:
 * id=$(($(xdotool search --name vnc)+4)); ./forward-x-keys $id $id F20 F21 F22 F23 F24 F25
 * successfully forwards the relevant function keys to the vnc viewer.
 */

/* This code needs a little explanation:
 *
 * we allow two modes of operation: pretending our hotkey is an
 * ordinary key, or pretending our hotkey is a modifier key.
 *
 * When the hot key is pressed, then released, we switch focus to our
 * hotkeyed application, forward it the hot key, and leave things at
 * that.
 *
 * When the hot key is pressed, another key (or several) is pressed,
 * and the hot key is then released, we switch focus only for the
 * duration that the hot key is pressed, then restore our original
 * input focus and window stacking order. That way, you don't have to
 * interrupt your workflow more than necessary when briefly triggering
 * another application, but you can still see a menu produced by your
 * hot key and react to it.
 *
 * However, things are actually implemented by grabbing the keyboard
 * when the hot key is pressed to detect further key presses, the
 * first of which determines whether we act as a modifier or as an
 * ordinary key. For extra fun, we restore the stacking order when
 * we're done acting as a modifier. After reversing the array Xlib
 * gives us, obviously, since XQueryTree returns windows bottom-first
 * and XRestackWindows wants them top-first.
 *
 * Trouble with repeating keys? Try xkbset -r <keycode>. When using
 * this with xtightvncviewer, it's necessary to do that on the X
 * server running xtightvncviewer, not the one running x11vnc.
 */

/*
 * TODO:
 *  - parse modifiers rather than always passing in 0 for the modifier bits
 *  - allow switching to an application without forwarding the hotkey in modifier mode.
 *  - handle windows appearing or disappearing while our modifier is pressed gracefully (i.e. at all)
 */

static void grabxkey(Display *d, Window w, unsigned int modifiers, const char *string)
{
  KeySym keysym;
  KeyCode *keycodes;
  Cardinal nkeycodes;
  Cardinal i;

  keysym = XStringToKeysym(string);
  if (keysym == NoSymbol) {
    fprintf(stderr, "NoSymbol for %s, exiting\n", string);
    exit(1);
  }
  XtKeysymToKeycodeList(d, keysym, &keycodes, &nkeycodes);
  for (i=0; i<nkeycodes; i++) {
    XGrabKey(d, keycodes[i], modifiers, w, False, GrabModeAsync, GrabModeAsync);
  }
}

enum state {
  forward_some_keys,
  as_modifier,
  magic_key_held,
};

enum discard {
  discard_never,
  discard_modifier,
  discard_always,
};

static void reverse_array(Window *array, size_t n)
{
  size_t i;

  for (i=0; i<n/2; i++) {
    Window tmp = array[n-i-1];
    array[n-i-1] = array[i];
    array[i] = tmp;
  }
}

int main(int argc, char **argv)
{
  Display *d;
  Window rw, w, sw;
  Window dummy;
  int dummy_int;
  char **arg;
  XEvent ev, trigger_event;
  Window fw;
  int revert_to;
  unsigned trigger_keycode;
  Window *stacking_order = NULL;
  unsigned stacking_order_n = 0;
  enum state state = forward_some_keys;
  enum discard discard = discard_never;
  unsigned int modifiers;

  if (argc < 3) {
    fprintf(stderr, "usage: %s <window id> <subwindow id> <keysym1> <keysym2> ...\n",
            argv[0]);
    exit(1);
  }

  d = XtOpenDisplay(XtCreateApplicationContext(), NULL, NULL, NULL, NULL, 0, &argc, argv);

  if (!d) {
    fprintf(stderr, "%s: failed to open X display at %s, is $DISPLAY set?\n",
            argv[0], getenv("DISPLAY"));
    exit(1);
  }

  w = (Window)(strtoul(argv[1], NULL, 0));
  sw = (Window)(strtoul(argv[2], NULL, 0));

  rw = RootWindow(d, DefaultScreen(d));

  XQueryPointer(d, rw, &dummy, &dummy, &dummy_int, &dummy_int, &dummy_int, &dummy_int, &modifiers);

  arg = argv+3;

  if (strcmp(arg[0], "--discard=always") == 0) {
    discard = discard_always;
    arg++;
  } else if (strcmp(arg[0], "--discard=modifier") == 0) {
    discard = discard_modifier;
    arg++;
  } else if (strcmp(arg[0], "--discard=never") == 0) {
    /* for completeness, this is the default behaviour */
    discard = discard_never;
    arg++;
  };

  for (; *arg; arg++)
    grabxkey(d, rw, modifiers, *arg);

#if 0 /* there's no good way to know when to terminate, so don't daemonize for now */
  if ((ret = fork()) != 0) {
    if (ret < 0) {
      fprintf(stderr, "%s: couldn't fork\n",
              argv[0]);
      exit(1);
    }

    exit(0);
  }
#endif

  while (XNextEvent(d, &ev) == 0) {
    int do_send = 1;

    if (ev.type != KeyPress && ev.type != KeyRelease)
      continue;

    /* Try to catch auto-repeat keys and discard them. This doesn't
     * actually work a hundred percent of the time. See
     * http://stackoverflow.com/questions/2100654/ignore-auto-repeat-in-x11-applications. */
    if (ev.type == KeyRelease) {
      XEvent discard;
      Bool predicate(Display *d, XEvent *event, XPointer arg) {
        if (event->type == KeyPress &&
            event->xkey.time == ev.xkey.time && /* ha ha, you'd think this would work */
            event->xkey.keycode == ev.xkey.keycode)
          return True;
        return False;
      }

      XSync(d, False);
      XQueryPointer(d, rw, &dummy, &dummy, &dummy_int, &dummy_int, &dummy_int, &dummy_int,
                    &modifiers);
      XSync(d, False);

      if (XCheckIfEvent(d, &discard, predicate, NULL) == True)
        continue;
    }

    switch (state) {
    case forward_some_keys:
      if (ev.type == KeyPress) {
        XGrabKeyboard(d, rw, True, GrabModeAsync, GrabModeAsync, CurrentTime);
        trigger_keycode = ev.xkey.keycode;
        state = magic_key_held;

        if (stacking_order) {
          XFree(stacking_order);
          stacking_order = NULL;
        }
        XQueryTree(d, rw, &dummy, &dummy, &stacking_order, &stacking_order_n);
        /* XQueryTree returns windows bottom-first, but
         * XRestackWindows expects them top-first. No, really, I'm not
         * making this up. */
        if (stacking_order)
          reverse_array(stacking_order, stacking_order_n);
        XRaiseWindow(d, w);
        XGetInputFocus(d, &fw, &revert_to);
        XSetInputFocus(d, sw, RevertToNone, CurrentTime);
        if (discard == discard_always)
          do_send = 0;
        else if (discard == discard_modifier) {
          trigger_event = ev;
          do_send = 0;
        }
      } else {
        do_send = 0;
      }
      break;

    case magic_key_held:
      if (ev.type == KeyPress) {
        if (ev.xkey.keycode != trigger_keycode) {
          state = as_modifier;
        } else {
          do_send = 0;
        }
      } else {
        if (ev.xkey.keycode == trigger_keycode) {
          if (discard == discard_always) {
            do_send = 0;
          } else if (discard == discard_modifier) {
            XSendEvent(d, w, True, KeyPressMask|KeyReleaseMask, &trigger_event);
          }
          XUngrabKeyboard(d, CurrentTime);
          state = forward_some_keys;
        } else {
          state = as_modifier;
          do_send = 0;
        }
      }
      break;

    case as_modifier:
      if (ev.type == KeyRelease && ev.xkey.keycode == trigger_keycode) {
        XSetInputFocus(d, fw, revert_to, CurrentTime);
        if (stacking_order)
          XRestackWindows(d, stacking_order, stacking_order_n);
        XUngrabKeyboard(d, CurrentTime);
        state = forward_some_keys;
      }
      break;
    }
    if (do_send) {
      ev.xkey.window = w;
      ev.xkey.subwindow = sw;
      XSendEvent(d, w, True, KeyPressMask|KeyReleaseMask, &ev);
    }
  }

  return 0;
}
