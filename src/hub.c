#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <pango/pangocairo.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "hub.h"


typedef struct HubCtrlState {
  int shouldShowAll;
  int shouldQuit;
} HubCtrlState;

typedef struct Hub {
  pthread_mutex_t lock;
  pthread_cond_t cond;
  HubCtrlState ctrlState;
} Hub;


struct MwmHints {
  unsigned long flags;
  unsigned long functions;
  unsigned long decorations;
  long input_mode;
  unsigned long status;
};

enum {
  MWM_HINTS_FUNCTIONS = (1L << 0),
  MWM_HINTS_DECORATIONS =  (1L << 1),

  MWM_FUNC_ALL = (1L << 0),
  MWM_FUNC_RESIZE = (1L << 1),
  MWM_FUNC_MOVE = (1L << 2),
  MWM_FUNC_MINIMIZE = (1L << 3),
  MWM_FUNC_MAXIMIZE = (1L << 4),
  MWM_FUNC_CLOSE = (1L << 5)
};


Hub* newHub()
{
  Hub* hub = malloc(sizeof(Hub));
  pthread_mutex_init(&hub->lock, 0);
  pthread_cond_init(&hub->cond, 0);
  hub->ctrlState.shouldShowAll = 0;
  hub->ctrlState.shouldQuit = 0;
	return hub;
}

void freeHub(Hub* hub)
{
  pthread_cond_destroy(&hub->cond);
  pthread_mutex_destroy(&hub->lock);
	free(hub);
}

int runHub(Hub* hub)
{
  int status;
  Display* xDisplay = XOpenDisplay(0);
  if (xDisplay == 0) {
    printf("Failed to connect to X. Terminating.\n");
    return 1;
  }
  int xScreen = DefaultScreen(xDisplay);
  Window xRoot = RootWindow(xDisplay, xScreen);
  int width = 100;
  int height = 100;
  XVisualInfo xVisualInfo;
  status = XMatchVisualInfo(xDisplay,
                            xScreen,
                            32,
                            TrueColor,
                            &xVisualInfo);
  if (status == 0) {
    printf("No 32-bit visual available from X. Terminating.\n");
    return 1;
  }
  XSetWindowAttributes xAttributes;
  xAttributes.background_pixmap = None;
  xAttributes.border_pixel = 0;
  xAttributes.colormap = XCreateColormap(xDisplay,
                                         xRoot,
                                         xVisualInfo.visual,
                                         AllocNone);
  xAttributes.event_mask = ExposureMask;
  int xAttributeMask = CWBackPixmap | CWBorderPixel | CWColormap | CWEventMask;
  Window xWindow = XCreateWindow(xDisplay,
                                 xRoot,
                                 0, 0,
                                 width, height,
                                 0,
                                 xVisualInfo.depth,
                                 InputOutput,
                                 xVisualInfo.visual,
                                 xAttributeMask,
                                 &xAttributes);

  // Make the window floating
  // I'd prefer "_NET_WM_WINDOW_TYPE_NOTIFICATION", but i3 still tiles that
  Atom windowType = XInternAtom(xDisplay, "_NET_WM_WINDOW_TYPE", 0);
  Atom windowTypeDesktop = XInternAtom(xDisplay, "_NET_WM_WINDOW_TYPE_UTILITY", 0);
  XChangeProperty(xDisplay,
                  xWindow,
                  windowType,
                  XA_ATOM,
                  32,
                  PropModeReplace,
                  (unsigned char*)&windowTypeDesktop,
                  1);

  // Remove the window decorations
  Atom motifHintsType = XInternAtom(xDisplay, "_MOTIF_WM_HINTS", 0);
  struct MwmHints motifHints;
  motifHints.flags = MWM_HINTS_DECORATIONS;
  motifHints.decorations = 0;
  XChangeProperty(xDisplay,
                  xWindow,
                  motifHintsType,
                  XA_ATOM,
                  32,
                  PropModeReplace,
                  (unsigned char*)&motifHints,
                  5);

  XMapWindow(xDisplay, xWindow);
  XSelectInput(xDisplay, xWindow, ExposureMask | KeyPressMask);

  // Create cairo surface
  cairo_surface_t* cs = cairo_xlib_surface_create(xDisplay,
                                                  xWindow,
                                                  xVisualInfo.visual,
                                                  width, height);
  cairo_xlib_surface_set_size(cs, width, height); // Is this really needed?

  // Create cairo context
  cairo_t* cr = cairo_create(cs);

  // Load font
  PangoFontDescription* fontInfo = pango_font_description_new();
  pango_font_description_set_family(fontInfo, "monaco");
  pango_font_description_set_weight(fontInfo, PANGO_WEIGHT_NORMAL);
  pango_font_description_set_absolute_size(fontInfo, 32 * PANGO_SCALE);

  PangoLayout* fontLayout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(fontLayout, fontInfo);
  pango_layout_set_text(fontLayout, "Good", -1);

  while (1) {

    // Save state
    pthread_mutex_lock(&hub->lock);
    HubCtrlState ctrlState = hub->ctrlState;
    pthread_mutex_unlock(&hub->lock);

    // Quit if requested
    if (ctrlState.shouldQuit != 0) {
      break;
    }

    // Clear the screen if hiding all
    if (ctrlState.shouldShowAll == 0) {
      cairo_save(cr);
      cairo_set_source_rgba(cr, 0, 0, 0, 0);
      cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
      cairo_paint(cr);
      cairo_restore(cr);

      // Draw the screen if showing all
    } else {
      cairo_save(cr);
      cairo_set_source_rgba(cr, 1.0, 0.0, 0.0, 0.5);
      cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
      cairo_paint(cr);
      cairo_restore(cr);

      cairo_set_source_rgb(cr, 0.0, 0.0, 1.0);
      cairo_move_to(cr, 10, 10);
      pango_cairo_show_layout(cr, fontLayout);
    }

    // Wait for a message if we're hidden. This is purely to save resources
    // instead of busy waiting until we either have to quit or show the modules
    // again.
    if (ctrlState.shouldShowAll == 0) {
      pthread_mutex_lock(&hub->lock);
      pthread_cond_wait(&hub->cond, &hub->lock);
      pthread_mutex_unlock(&hub->lock);
      // We don't need to worry about spurious wake-ups. The worst that will
      // happen is we will run through the while loop again and then go back to
      // waiting.
    }

    // TEMP: Delete this stuff
    // Display on expose events
    XEvent e;
    XNextEvent(xDisplay, &e);
    if (e.type == Expose) {
      //XWindowAttributes xWinAttr;
      //int width, height;
      //XGetWindowAttributes(xDisplay, xWindow, &xWinAttr);
      //width = xWinAttr.width;
      //height = xWinAttr.height;

    }
    if (e.type == KeyPress) {
      break;
    }
  }

  g_object_unref(fontLayout);
  pango_font_description_free(fontInfo);
  cairo_destroy(cr);
  cairo_surface_destroy(cs);
  XUnmapWindow(xDisplay, xWindow);
  XDestroyWindow(xDisplay, xWindow);
  XCloseDisplay(xDisplay);
  return 0;
}

void stopHub(Hub* hub)
{
  pthread_mutex_lock(&hub->lock);
  if (hub->ctrlState.shouldQuit == 0) {
    hub->ctrlState.shouldQuit = 1;
    pthread_cond_signal(&hub->cond);
  }
  pthread_mutex_unlock(&hub->lock);
}

void showHubModules(Hub* hub)
{
  pthread_mutex_lock(&hub->lock);
  if (hub->ctrlState.shouldShowAll == 0) {
    hub->ctrlState.shouldShowAll = 1;
    pthread_cond_signal(&hub->cond);
  }
  pthread_mutex_unlock(&hub->lock);
}

void hideHubModules(Hub* hub)
{
  pthread_mutex_lock(&hub->lock);
  if (hub->ctrlState.shouldShowAll != 0) {
    hub->ctrlState.shouldShowAll = 0;
    // Do NOT signal condition. The hub will never be waiting for a message if
    // it is currently showing a module.
    //pthread_cond_signal(&hub->cond);
  }
  pthread_mutex_unlock(&hub->lock);
}
