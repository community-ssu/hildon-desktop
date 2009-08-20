
#ifndef __HD_ATOM_H__
#define __HD_ATOM_H__

#include <X11/Xlib.h>
#include <X11/Xatom.h>          /* for XA_ATOM etc */

typedef enum HdAtoms
{
  HD_ATOM_HILDON_APP_KILLABLE = 0,
  HD_ATOM_HILDON_ABLE_TO_HIBERNATE,

  HD_ATOM_HILDON_HOME_VIEW,
  HD_ATOM_HILDON_STACKABLE_WINDOW,
  HD_ATOM_HILDON_NON_COMPOSITED_WINDOW,

  HD_ATOM_HILDON_WM_WINDOW_TYPE_HOME_APPLET,
  HD_ATOM_HILDON_WM_WINDOW_TYPE_APP_MENU,
  HD_ATOM_HILDON_WM_WINDOW_TYPE_STATUS_AREA,
  HD_ATOM_HILDON_WM_WINDOW_TYPE_STATUS_MENU,
  HD_ATOM_HILDON_WM_WINDOW_TYPE_ANIMATION_ACTOR,
  HD_ATOM_HILDON_WM_WINDOW_TYPE_REMOTE_TEXTURE,

  HD_ATOM_HILDON_NOTIFICATION_TYPE,
  HD_ATOM_HILDON_NOTIFICATION_TYPE_BANNER,
  HD_ATOM_HILDON_NOTIFICATION_TYPE_INFO,
  HD_ATOM_HILDON_NOTIFICATION_TYPE_CONFIRMATION,
  HD_ATOM_NOTIFICATION_THREAD,

  HD_ATOM_HILDON_INCOMING_EVENT_NOTIFICATION_DESTINATION,
  HD_ATOM_HILDON_INCOMING_EVENT_NOTIFICATION_MESSAGE,
  HD_ATOM_HILDON_INCOMING_EVENT_NOTIFICATION_SUMMARY,
  HD_ATOM_HILDON_INCOMING_EVENT_NOTIFICATION_COUNT,
  HD_ATOM_HILDON_INCOMING_EVENT_NOTIFICATION_TIME,
  HD_ATOM_HILDON_INCOMING_EVENT_NOTIFICATION_ICON,

  HD_ATOM_HILDON_CLIENT_MESSAGE_PAN,
  HD_ATOM_HILDON_CLIENT_MESSAGE_SHOW_SETTINGS,

  HD_ATOM_HILDON_APPLET_ID,
  HD_ATOM_HILDON_APPLET_SETTINGS,
  HD_ATOM_HILDON_APPLET_SHOW_SETTINGS,
  HD_ATOM_HILDON_APPLET_ON_CURRENT_DESKTOP,

  HD_ATOM_HILDON_WM_WINDOW_PROGRESS_INDICATOR,

  HD_ATOM_WM_WINDOW_ROLE,

  HD_ATOM_WM_PORTRAIT_OK,
  HD_ATOM_WM_PORTRAIT_REQUESTED,

  HD_ATOM_HILDON_DO_NOT_DISTURB,
  HD_ATOM_HILDON_DO_NOT_DISTURB_OVERRIDE,

  HD_ATOM_UTF8_STRING,

  HD_ATOM_HILDON_ANIMATION_CLIENT_MESSAGE_SHOW,
  HD_ATOM_HILDON_ANIMATION_CLIENT_MESSAGE_POSITION,
  HD_ATOM_HILDON_ANIMATION_CLIENT_MESSAGE_ROTATION,
  HD_ATOM_HILDON_ANIMATION_CLIENT_MESSAGE_SCALE,
  HD_ATOM_HILDON_ANIMATION_CLIENT_MESSAGE_ANCHOR,
  HD_ATOM_HILDON_ANIMATION_CLIENT_MESSAGE_PARENT,
  HD_ATOM_HILDON_ANIMATION_CLIENT_READY,

  HD_ATOM_HILDON_TEXTURE_CLIENT_MESSAGE_SHM,
  HD_ATOM_HILDON_TEXTURE_CLIENT_MESSAGE_DAMAGE,
  HD_ATOM_HILDON_TEXTURE_CLIENT_MESSAGE_SHOW,
  HD_ATOM_HILDON_TEXTURE_CLIENT_MESSAGE_POSITION,
  HD_ATOM_HILDON_TEXTURE_CLIENT_MESSAGE_OFFSET,
  HD_ATOM_HILDON_TEXTURE_CLIENT_MESSAGE_SCALE,
  HD_ATOM_HILDON_TEXTURE_CLIENT_MESSAGE_PARENT,
  HD_ATOM_HILDON_TEXTURE_CLIENT_READY,

  HD_ATOM_HILDON_LOADING_SCREENSHOT,

  HD_ATOM_RANDR_CONNECTOR_TYPE,
  HD_ATOM_RANDR_CONNECTOR_TYPE_PANEL,

  HD_ATOM_DELETE_TEMPORARIES,

  HD_ATOM_OMAP_VIDEO_OVERLAY,

  HD_ATOM_MAEMO_ROTATION_TRANSITION,

  _HD_ATOM_LAST
} HdAtoms;

void
hd_atoms_init (Display * xdpy, Atom * atoms);

#endif
