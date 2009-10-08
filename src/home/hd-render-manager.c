/*
 * This file is part of hildon-desktop
 *
 * Copyright (C) 2008 Nokia Corporation. All rights reserved.
 *
 * Author:  Gordon Williams <gordon.williams@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "hd-render-manager.h"

#include <clutter/clutter.h>

#include "tidy/tidy-blur-group.h"

#include "hd-comp-mgr.h"
#include "hd-home.h"
#include "hd-switcher.h"
#include "hd-launcher.h"
#include "hd-task-navigator.h"
#include "hd-transition.h"
#include "hd-wm.h"
#include "hd-util.h"
#include "hd-title-bar.h"

#include <matchbox/core/mb-wm.h>
#include <matchbox/theme-engines/mb-wm-theme.h>

#include <sys/time.h>

/* This is to dump debug information to the console to help see whether the
 * order of clutter actors matches that of matchbox. */
#define STACKING_DEBUG 0

/* And this one is to help debugging visibility-related problems
 * ie. when stacking is all right but but you cannot see what you want. */
#if 0
# define VISIBILITY       g_debug
#else
# define VISIBILITY(...)  /* NOP */
#endif
/* ------------------------------------------------------------------------- */
#define I_(str) (g_intern_static_string ((str)))

GType
hd_render_manager_state_get_type (void)
{
  static GType gtype = 0;

  if (G_UNLIKELY (gtype == 0))
    {
      static GEnumValue values[] = {
        { HDRM_STATE_UNDEFINED,      "HDRM_STATE_UNDEFINED",      "Undefined" },
        { HDRM_STATE_HOME,           "HDRM_STATE_HOME",           "Home" },
        { HDRM_STATE_HOME_EDIT,      "HDRM_STATE_HOME_EDIT",      "Home edit" },
        { HDRM_STATE_HOME_EDIT_DLG,  "HDRM_STATE_HOME_EDIT_DLG",  "Home edit dialog" },
        { HDRM_STATE_HOME_PORTRAIT,  "HDRM_STATE_HOME_PORTRAIT",  "Home in portrait mode" },
        { HDRM_STATE_APP,            "HDRM_STATE_APP",            "Application" },
        { HDRM_STATE_APP_PORTRAIT,   "HDRM_STATE_APP_PORTRAIT",   "Application in portrait mode" },
        { HDRM_STATE_TASK_NAV,       "HDRM_STATE_TASK_NAV",       "Task switcher" },
        { HDRM_STATE_LAUNCHER,       "HDRM_STATE_LAUNCHER",       "Task launcher" },
        { HDRM_STATE_NON_COMPOSITED, "HDRM_STATE_NON_COMPOSITED", "Non-composited" },
        { HDRM_STATE_NON_COMP_PORT, "HDRM_STATE_NON_COMP_PORT", "Non-composited portrait" },
        { HDRM_STATE_LOADING,        "HDRM_STATE_LOADING",        "Loading" },
        { HDRM_STATE_LOADING_SUBWIN, "HDRM_STATE_LOADING_SUBWIN", "Loading Subwindow" },
        { 0, NULL, NULL }
      };

      gtype = g_enum_register_static (I_("HdRenderManagerStateType"), values);
    }

  return gtype;
}
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */

G_DEFINE_TYPE (HdRenderManager, hd_render_manager, TIDY_TYPE_CACHED_GROUP);
#define HD_RENDER_MANAGER_GET_PRIVATE(obj) \
                (G_TYPE_INSTANCE_GET_PRIVATE ((obj), \
                HD_TYPE_RENDER_MANAGER, HdRenderManagerPrivate))

/* The HdRenderManager singleton */
static HdRenderManager *the_render_manager = NULL;

/* HdRenderManager properties */
enum
{
  PROP_0,
  PROP_STATE
};
/* ------------------------------------------------------------------------- */

typedef enum
{
  HDRM_BLUR_NONE = 0,
  HDRM_BLUR_HOME = 1,
  HDRM_SHOW_TASK_NAV = 2, /* Used to fade out/fade in task nav */
  HDRM_BLUR_BACKGROUND = 4, /* like BLUR_HOME, but for dialogs, etc */
  HDRM_ZOOM_FOR_LAUNCHER = 16, /* zoom task_nav for launchre */
  HDRM_ZOOM_FOR_LAUNCHER_SUBMENU = 32, /* ...for submenu */
  HDRM_ZOOM_FOR_HOME = 64, /* for home */
  HDRM_ZOOM_FOR_TASK_NAV = 128, /* zoom home for task_nav */
  HDRM_SHOW_APPLETS = 256, /* Used to fade out/fade in applets */
} HDRMBlurEnum;

enum
{
  TRANSITION_COMPLETE,
  LAST_SIGNAL
};
static guint signals[LAST_SIGNAL] = { 0, };

/* ------------------------------------------------------------------------- */

/*
 *
 * HDRM ---> home_blur         ---> home
 *       |                                         --> home_get_front (!STATE_HOME_FRONT)
 *       |                      --> apps (not app_top)
 *       |                      --> blur_front (STATE_BLUR_BUTTONS)
 *       |                                        ---> home_get_front (STATE_HOME_FRONT)
 *       |                                        ---> loading_image (STATE_LOADING)
 *       |                                         --> title_bar *       |
 *       |                                               ---> title_bar::foreground (!HDTB_VIS_FOREGROUND)
 *       |                                               ---> status_area
 *       |
 *       --> blur_front (!STATE_BLUR_BUTTONS)
 *       |
 *       --> task_nav
 *       |
 *       --> launcher
 *       |
 *       --> app_top           ---> dialogs
 *       |
 *       --> front             ---> status_menu
 *                             ---> title_bar::foreground (HDTB_VIS_FOREGROUND)
 *
 */

typedef struct _Range {
  float a, b, current;
} Range;

struct _HdRenderManagerPrivate {
  gboolean      disposed;
  HDRMStateEnum state;
  HDRMStateEnum previous_state;

  /* The input blocker is added with hd_render_manager_add_input_blocker.
   * It grabs the whole screen's input until either a window appears or
   * a timeout expires. */
  gboolean      has_input_blocker;
  guint         has_input_blocker_timeout;

  TidyBlurGroup *home_blur;
  ClutterGroup  *app_top;
  ClutterGroup  *front;
  ClutterGroup  *blur_front;
  HdTitleBar    *title_bar;

  /* external */
  HdCompMgr            *comp_mgr;
  HdTaskNavigator      *task_nav;
  HdHome               *home;
  ClutterActor         *status_area;
  MBWindowManagerClient *status_area_client;
  ClutterActor         *status_menu;
  ClutterActor         *operator;
  ClutterActor         *loading_image;
  ClutterActor         *loading_image_parent;

  /* these are current, from + to variables for doing the blurring animations */
  Range         home_radius;
  Range         home_zoom;
  Range         home_brightness;
  Range         home_saturation;
  Range         task_nav_opacity;
  Range         task_nav_zoom;
  Range         applets_opacity;
  Range         applets_zoom;

  HDRMBlurEnum  current_blur;

  ClutterTimeline    *timeline_blur;
  /* Timeline works by signals, so we get horrible flicker if we ask it if it
   * is playing right after saying _start() - so we have a boolean to figure
   * out for ourselves */
  gboolean            timeline_playing;

  gboolean            in_set_state;

  /* When a sync is queued, this contains the id of the source
   * (added with g_idle_add) */
  gint                idle_sync_source;
  /* If we are currently executing a sync, set this to avoid loops */
  gboolean            in_idle_sync;
};

/* ------------------------------------------------------------------------- */
static void
stage_allocation_changed(ClutterActor *actor, GParamSpec *unused,
                         ClutterActor *stage);
static gboolean
hd_render_manager_captured_event_cb (ClutterActor     *actor,
                                     ClutterEvent     *event,
                                     gpointer *data);

static void
on_timeline_blur_new_frame(ClutterTimeline *timeline,
                           gint frame_num, gpointer data);
static void
on_timeline_blur_completed(ClutterTimeline *timeline, gpointer data);

static void
hd_render_manager_sync_clutter_before(void);
static void
hd_render_manager_sync_clutter_after(void);

static const char *
hd_render_manager_state_str(HDRMStateEnum state);
static void
hd_render_manager_set_visibilities(void);

static void
hd_render_manager_get_property (GObject    *object,
                                guint       property_id,
                                GValue     *value,
                                GParamSpec *pspec);
static void
hd_render_manager_set_property (GObject      *gobject,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec);

static
gboolean hd_render_manager_should_ignore_cm_client(MBWMCompMgrClutterClient *cm_client);
static
gboolean hd_render_manager_should_ignore_actor(ClutterActor *actor);
static void hd_render_manager_update_blur_state(void);
/* ------------------------------------------------------------------------- */
/* -------------------------------------------------------------  RANGE      */
/* ------------------------------------------------------------------------- */
static inline void range_set(Range *range, float val)
{
  range->a = range->b = range->current = val;
}
static inline void range_interpolate(Range *range, float n)
{
  range->current = (range->a*(1-n)) + range->b*n;
}
static inline void range_next(Range *range, float x)
{
  range->a = range->current;
  range->b = x;
}
static inline gboolean range_equal(Range *range)
{
  return range->a == range->b;
}

/* ------------------------------------------------------------------------- */
/* -------------------------------------------------------------    INIT     */
/* ------------------------------------------------------------------------- */

HdRenderManager *hd_render_manager_create (HdCompMgr *hdcompmgr,
                                           HdLauncher *launcher,
                                           HdHome *home,
                                           HdTaskNavigator *task_nav
                                           )
{
  HdRenderManagerPrivate *priv;

  g_assert(the_render_manager == NULL);

  the_render_manager = HD_RENDER_MANAGER(g_object_ref (
        g_object_new (HD_TYPE_RENDER_MANAGER, NULL)));
  priv = the_render_manager->priv;

  priv->disposed = FALSE;
  priv->comp_mgr = hdcompmgr;

  /* Task switcher widget: anchor it at the centre so it is zoomed in
   * the middle when blurred. */
  priv->task_nav = task_nav;
  clutter_actor_set_visibility_detect(CLUTTER_ACTOR(priv->task_nav), FALSE);
  clutter_actor_set_position(CLUTTER_ACTOR(priv->task_nav), 0, 0);
  clutter_actor_set_size (CLUTTER_ACTOR(priv->task_nav),
                          HD_COMP_MGR_LANDSCAPE_WIDTH,
                          HD_COMP_MGR_LANDSCAPE_HEIGHT);
  clutter_container_add_actor(CLUTTER_CONTAINER(the_render_manager),
                              CLUTTER_ACTOR(priv->task_nav));
  clutter_actor_move_anchor_point_from_gravity(CLUTTER_ACTOR(priv->task_nav),
                                               CLUTTER_GRAVITY_CENTER);

  /* Add the launcher widget. */
  clutter_container_add_actor(CLUTTER_CONTAINER(the_render_manager),
                              CLUTTER_ACTOR(launcher));

  /* These must be below tasw and talu. */
  clutter_actor_lower_bottom (CLUTTER_ACTOR (priv->app_top));
  clutter_actor_lower_bottom (CLUTTER_ACTOR (priv->front));

  /* HdHome */
  priv->home = home;
  g_signal_connect_swapped(clutter_stage_get_default(), "notify::allocation",
                           G_CALLBACK(stage_allocation_changed), priv->home);
  clutter_container_add_actor(CLUTTER_CONTAINER(priv->home_blur),
                              CLUTTER_ACTOR(priv->home));

  /* Edit button */
  clutter_container_add_actor(CLUTTER_CONTAINER(priv->blur_front),
                              hd_home_get_edit_button(priv->home));

  /* Operator */
  hd_render_manager_set_operator(hd_home_get_operator(priv->home));
  clutter_actor_reparent(priv->operator, CLUTTER_ACTOR(priv->blur_front));

  /* HdTitleBar */
  priv->title_bar = g_object_new(HD_TYPE_TITLE_BAR, NULL);
  g_signal_connect_swapped(clutter_stage_get_default(), "notify::allocation",
                           G_CALLBACK(stage_allocation_changed), priv->title_bar);
  clutter_container_add_actor(CLUTTER_CONTAINER(priv->blur_front),
                              CLUTTER_ACTOR(priv->title_bar));

  return the_render_manager;
}

HdRenderManager *
hd_render_manager_get (void)
{
  return the_render_manager;
}

static void
hd_render_manager_finalize (GObject *gobject)
{
  HdRenderManagerPrivate *priv = HD_RENDER_MANAGER_GET_PRIVATE(gobject);
  g_object_unref(priv->home);
  g_object_unref(priv->task_nav);
  g_object_unref(priv->title_bar);
  G_OBJECT_CLASS (hd_render_manager_parent_class)->finalize (gobject);
}

static void
hd_render_manager_dispose (GObject *gobject)
{
  HdRenderManagerPrivate *priv = HD_RENDER_MANAGER_GET_PRIVATE(gobject);
  /* We run dispose only once. */
  if (priv->disposed)
    return;

  priv->disposed = TRUE;
  G_OBJECT_CLASS(hd_render_manager_parent_class)->dispose(gobject);
}


static void
hd_render_manager_class_init (HdRenderManagerClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GParamSpec *pspec;

  g_type_class_add_private (klass, sizeof (HdRenderManagerPrivate));

  gobject_class->get_property = hd_render_manager_get_property;
  gobject_class->set_property = hd_render_manager_set_property;
  gobject_class->finalize     = hd_render_manager_finalize;
  gobject_class->dispose      = hd_render_manager_dispose;

  signals[TRANSITION_COMPLETE] =
        g_signal_new ("transition-complete",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);



  pspec = g_param_spec_enum ("state",
                             "State", "Render manager state",
                             HD_TYPE_RENDER_MANAGER_STATE,
                             HDRM_STATE_UNDEFINED,
                             G_PARAM_READABLE    |
                             G_PARAM_WRITABLE    |
                             G_PARAM_STATIC_NICK |
                             G_PARAM_STATIC_NAME |
                             G_PARAM_STATIC_BLURB);
  g_object_class_install_property (gobject_class, PROP_STATE, pspec);
}

static void
hd_render_manager_init (HdRenderManager *self)
{
  ClutterActor *stage;
  HdRenderManagerPrivate *priv;

  stage = clutter_stage_get_default();

  self->priv = priv = HD_RENDER_MANAGER_GET_PRIVATE (self);
  clutter_actor_set_name(CLUTTER_ACTOR(self), "HdRenderManager");
  g_signal_connect_swapped(stage, "notify::allocation",
                           G_CALLBACK(stage_allocation_changed), self);
  /* Add a callback we can use to capture events when we need to block
   * input with has_input_blocker */
  g_signal_connect (clutter_stage_get_default(),
                    "captured-event",
                    G_CALLBACK (hd_render_manager_captured_event_cb),
                    self);

  priv->state = HDRM_STATE_UNDEFINED;
  priv->previous_state = HDRM_STATE_UNDEFINED;
  priv->current_blur = HDRM_BLUR_NONE;

  priv->home_blur = TIDY_BLUR_GROUP(tidy_blur_group_new());
  clutter_actor_set_name(CLUTTER_ACTOR(priv->home_blur),
                         "HdRenderManager:home_blur");
  clutter_actor_set_visibility_detect(CLUTTER_ACTOR(priv->home_blur), FALSE);
  tidy_blur_group_set_use_alpha(CLUTTER_ACTOR(priv->home_blur), FALSE);
  tidy_blur_group_set_use_mirror(CLUTTER_ACTOR(priv->home_blur), TRUE);
  g_signal_connect_swapped(stage, "notify::allocation",
                           G_CALLBACK(stage_allocation_changed), priv->home_blur);
  clutter_container_add_actor(CLUTTER_CONTAINER(self),
                              CLUTTER_ACTOR(priv->home_blur));

  priv->app_top = CLUTTER_GROUP(clutter_group_new());
  clutter_actor_set_name(CLUTTER_ACTOR(priv->app_top),
                         "HdRenderManager:app_top");
  g_signal_connect_swapped(stage, "notify::allocation",
                           G_CALLBACK(stage_allocation_changed), priv->app_top);
  clutter_actor_set_visibility_detect(CLUTTER_ACTOR(priv->app_top), FALSE);
  clutter_container_add_actor(CLUTTER_CONTAINER(self),
                              CLUTTER_ACTOR(priv->app_top));

  priv->front = CLUTTER_GROUP(clutter_group_new());
  clutter_actor_set_name(CLUTTER_ACTOR(priv->front),
                         "HdRenderManager:front");
  g_signal_connect_swapped(stage, "notify::allocation",
                           G_CALLBACK(stage_allocation_changed), priv->front);
  clutter_actor_set_visibility_detect(CLUTTER_ACTOR(priv->front), FALSE);
  clutter_container_add_actor(CLUTTER_CONTAINER(self),
                              CLUTTER_ACTOR(priv->front));

  priv->blur_front = CLUTTER_GROUP(clutter_group_new());
  clutter_actor_set_name(CLUTTER_ACTOR(priv->blur_front),
                         "HdRenderManager:blur_front");
  g_signal_connect_swapped(stage, "notify::allocation",
                           G_CALLBACK(stage_allocation_changed), priv->blur_front);
  clutter_actor_set_visibility_detect(CLUTTER_ACTOR(priv->blur_front), FALSE);
  g_object_set(priv->blur_front, "show_on_set_parent", FALSE, NULL);
  clutter_container_add_actor(CLUTTER_CONTAINER(priv->home_blur),
                              CLUTTER_ACTOR(priv->blur_front));

  /* Animation stuff */
  range_set(&priv->home_radius, 0);
  range_set(&priv->home_zoom, 1);
  range_set(&priv->home_saturation, 1);
  range_set(&priv->home_brightness, 1);
  range_set(&priv->task_nav_opacity, 0);
  range_set(&priv->task_nav_zoom, 1);
  range_set(&priv->applets_opacity, 0);
  range_set(&priv->applets_zoom, 1);

  priv->timeline_blur = clutter_timeline_new_for_duration(250);
  g_signal_connect (priv->timeline_blur, "new-frame",
                    G_CALLBACK (on_timeline_blur_new_frame), self);
  g_signal_connect (priv->timeline_blur, "completed",
                      G_CALLBACK (on_timeline_blur_completed), self);
  priv->timeline_playing = FALSE;

  priv->in_set_state = FALSE;
}

/* ------------------------------------------------------------------------- */
/* -------------------------------------------------------------  CALLBACK   */
/* ------------------------------------------------------------------------- */

/* Resize @actor to the current screen dimensions.
 * Also can be used to set @actor's initial size. */
static void
stage_allocation_changed(ClutterActor *actor, GParamSpec *unused,
                         ClutterActor *stage)
{
  clutter_actor_set_size(actor,
                         hd_comp_mgr_get_current_screen_width (),
                         hd_comp_mgr_get_current_screen_height ());
}

static void
on_timeline_blur_new_frame(ClutterTimeline *timeline,
                           gint frame_num, gpointer data)
{
  HdRenderManagerPrivate *priv;
  float amt;
  gint task_opacity, applets_opacity;
  ClutterActor *home_front;

  priv = the_render_manager->priv;

  amt = frame_num / (float)clutter_timeline_get_n_frames(timeline);

  range_interpolate(&priv->home_radius, amt);
  range_interpolate(&priv->home_zoom, amt);
  range_interpolate(&priv->home_saturation, amt);
  range_interpolate(&priv->home_brightness, amt);
  range_interpolate(&priv->task_nav_opacity, amt);
  range_interpolate(&priv->task_nav_zoom, amt);
  range_interpolate(&priv->applets_opacity, amt);
  range_interpolate(&priv->applets_zoom, amt);

  tidy_blur_group_set_blur      (CLUTTER_ACTOR(priv->home_blur),
                                 priv->home_radius.current);
  tidy_blur_group_set_saturation(CLUTTER_ACTOR(priv->home_blur),
                                 priv->home_saturation.current);
  tidy_blur_group_set_brightness(CLUTTER_ACTOR(priv->home_blur),
                                 priv->home_brightness.current);
  tidy_blur_group_set_zoom(CLUTTER_ACTOR(priv->home_blur),
                                 priv->home_zoom.current);

  task_opacity = priv->task_nav_opacity.current*255;
  clutter_actor_set_opacity(CLUTTER_ACTOR(priv->task_nav), task_opacity);
  if (task_opacity==0)
    {
      clutter_actor_hide(CLUTTER_ACTOR(priv->task_nav));
      if (priv->loading_image)
        {
          clutter_actor_reparent (priv->loading_image,
                                  CLUTTER_ACTOR (priv->blur_front));
          clutter_actor_set_size(priv->loading_image,
                                 hd_comp_mgr_get_current_screen_width (),
                                 hd_comp_mgr_get_current_screen_height ()
                                 - HD_COMP_MGR_TOP_MARGIN);
          clutter_actor_set_position(priv->loading_image, 0,
               /* We use priv->loading_image_parent to determine if we
                * got loading_image from the switcher.
                * TODO: Make this less of a nasty hack.
                */
               (priv->loading_image_parent ? 0 : HD_COMP_MGR_TOP_MARGIN));
          clutter_actor_show (priv->loading_image);
        }
    }
  else
    clutter_actor_show(CLUTTER_ACTOR(priv->task_nav));
  clutter_actor_set_scale(CLUTTER_ACTOR(priv->task_nav),
                          priv->task_nav_zoom.current,
                          priv->task_nav_zoom.current);

  home_front = hd_home_get_front (priv->home);
  applets_opacity = priv->applets_opacity.current*255;
  clutter_actor_set_opacity(CLUTTER_ACTOR(home_front), applets_opacity);
  if (applets_opacity==0)
    clutter_actor_hide(CLUTTER_ACTOR(home_front));
  else
    clutter_actor_show(CLUTTER_ACTOR(home_front));
  /* Set the scale of the home_front group. Also set its position
   * so it appears to zoom from the centre. We can't set the anchor point
   * from the gravity as this breaks home view panning */
  clutter_actor_set_scale(CLUTTER_ACTOR(home_front),
                          priv->applets_zoom.current,
                          priv->applets_zoom.current);
  clutter_actor_set_anchor_point(CLUTTER_ACTOR(home_front),
      -HD_COMP_MGR_LANDSCAPE_WIDTH  * (1-priv->applets_zoom.current) / 2,
      -HD_COMP_MGR_LANDSCAPE_HEIGHT * (1-priv->applets_zoom.current) / 2);
}

static void
on_timeline_blur_completed (ClutterTimeline *timeline, gpointer data)
{
  HdRenderManagerPrivate *priv = the_render_manager->priv;

  priv->timeline_playing = FALSE;
  hd_comp_mgr_set_effect_running(priv->comp_mgr, FALSE);

  g_signal_emit (the_render_manager, signals[TRANSITION_COMPLETE], 0);

  /* to trigger a change after the transition */
  hd_render_manager_sync_clutter_after();
}

/* ------------------------------------------------------------------------- */
/* -------------------------------------------------------------    PRIVATE  */
/* ------------------------------------------------------------------------- */

static
void hd_render_manager_set_blur (HDRMBlurEnum blur)
{
  HdRenderManagerPrivate *priv;
  gboolean blur_home;
  gint zoom_home = 0;

  priv = the_render_manager->priv;

  if (priv->timeline_playing)
    {
      clutter_timeline_stop(priv->timeline_blur);
      hd_comp_mgr_set_effect_running(priv->comp_mgr, FALSE);
    }

  priv->current_blur = blur;

  /* If we were going to transition to not blurring but didn't get there,
   * make sure we set blur=0 anyway in order to *force* the blurring to
   * recalculate. The first call to on_timeline_blur_new_frame will set
   * the correct value anyway. */
  if (priv->home_radius.b == 0 &&
      priv->home_radius.current != 0)
    tidy_blur_group_set_blur(
      CLUTTER_ACTOR(the_render_manager->priv->home_blur), 0);

  range_next(&priv->home_radius, 0);
  range_next(&priv->home_saturation, 1);
  range_next(&priv->home_brightness, 1);
  range_next(&priv->home_zoom, 1);
  range_next(&priv->task_nav_opacity, 0);
  range_next(&priv->task_nav_zoom, 1);
  range_next(&priv->applets_opacity, 0);
  range_next(&priv->applets_zoom, 1);


  /* work out how much we need to zoom various things */
  zoom_home += (blur & HDRM_ZOOM_FOR_LAUNCHER) ? 1 : 0;
  zoom_home += (blur & HDRM_ZOOM_FOR_LAUNCHER_SUBMENU) ? 1 : 0;
  zoom_home += (blur & HDRM_ZOOM_FOR_TASK_NAV) ? 1 : 0;

  blur_home = blur & (HDRM_BLUR_BACKGROUND | HDRM_BLUR_HOME);

  if (blur_home)
    {
      priv->home_saturation.b =
              hd_transition_get_double("home","saturation", 1);
      priv->home_brightness.b =
              hd_transition_get_double("home","brightness", 1);
      priv->home_radius.b =
              hd_transition_get_double("home",
                  (zoom_home)?"radius_more":"radius", 8);
    }

  if (zoom_home)
    {
      float zoom =
              hd_transition_get_double("home", "zoom", 1);
      /* We zoom the home multiple different levels */
      priv->home_zoom.b = 1 - (1-zoom)*(zoom_home+1);
      /* However applets are either zoomed or not (because they are
       * faded out in every zoom level apart from the first) */
      priv->applets_zoom.b = hd_transition_get_double("home", "zoom_applets", 1);
    }

  if (blur & HDRM_SHOW_TASK_NAV)
    {
      priv->task_nav_opacity.b = 1;
    }
  if (blur & HDRM_ZOOM_FOR_HOME)
    priv->task_nav_zoom.b = hd_transition_get_double("task_nav",
                                                     "zoom_for_home", 1);
  else if (blur & HDRM_ZOOM_FOR_LAUNCHER)
    priv->task_nav_zoom.b = hd_transition_get_double("task_nav", "zoom", 1);
  else if (blur & HDRM_ZOOM_FOR_LAUNCHER_SUBMENU)
    {
      priv->task_nav_zoom.b = hd_transition_get_double("task_nav", "zoom", 1);
      priv->task_nav_zoom.b = 1 - 2*(1-priv->task_nav_zoom.b);
    }

  if (blur & HDRM_SHOW_APPLETS)
    {
      /* Set .a here because we want to show applets immediately
       * (because we'll fade back from the blurred (non-appletted) image
       * at the same time, and fading applets too makes them appear to
       * just flick up). Fading out is fine though, so we want to do that
       * slowly.
       */
      priv->applets_opacity.a = 1;
      priv->applets_opacity.b = 1;
    }

  /* Just make sure that we set everything up correctly - even if the
   * ranges are the same, we may have changed 'a' and 'b' together
   * (see applets_opacity). Otherwise it is possible to get a frame
   * of flicker if clutter renders before the timeline. */
  on_timeline_blur_new_frame(priv->timeline_blur, 0, NULL);

  /* no point animating if everything is already right */
  if (range_equal(&priv->home_radius) &&
      range_equal(&priv->home_saturation) &&
      range_equal(&priv->home_brightness) &&
      range_equal(&priv->home_zoom) &&
      range_equal(&priv->task_nav_opacity) &&
      range_equal(&priv->task_nav_zoom) &&
      range_equal(&priv->applets_opacity))
    {
      hd_render_manager_sync_clutter_after();
      return;
    }

  hd_comp_mgr_set_effect_running(priv->comp_mgr, TRUE);
  /* Set duration here so we reload from the file every time */
  clutter_timeline_set_duration(priv->timeline_blur,
      hd_transition_get_int("blur", "duration", 250));
  clutter_timeline_start(priv->timeline_blur);
  priv->timeline_playing = TRUE;
}

/* This is for the task navigator when it zooms into a thumbnail.
 * The background is already zoomed and blurred now leave all
 * home_blur-zooming behind (especially HDRM_ZOOM_FOR_TASK_NAV). */
void
hd_render_manager_unzoom_background()
{
  hd_render_manager_set_blur (HDRM_BLUR_HOME | HDRM_SHOW_TASK_NAV);
}

/* Checks the whole tree for visibility */
gboolean hd_render_manager_actor_is_visible(ClutterActor *actor)
{
  ClutterActor *stage;

  if (!actor)
    return FALSE;

  stage = clutter_actor_get_stage(actor);
  for (;actor;actor=clutter_actor_get_parent(actor))
    if (actor!=stage && !CLUTTER_ACTOR_IS_VISIBLE(actor))
      return FALSE;

  return TRUE;
}

void
hd_render_manager_set_input_viewport()
{
  ClutterGeometry geom[4];
  int geom_count = 0;
  HdRenderManagerPrivate *priv = the_render_manager->priv;

  if (!STATE_NEED_WHOLE_SCREEN_INPUT(priv->state) &&
      !priv->has_input_blocker)
    {
      /* Now look at what buttons we have showing, and add each visible button X
       * to the X input viewport. */
      /* LEFT button */
      if (hd_title_bar_get_state(priv->title_bar) & HDTB_VIS_BTN_LEFT_MASK)
        {
          ClutterGeometry *g = &geom[geom_count++];
          g->width = hd_title_bar_get_button_width(priv->title_bar);
          g->height = HD_COMP_MGR_TOP_MARGIN;
          g->x = 0;
          g->y = 0;
        }

      /* RIGHT button: We have to ignore this in app mode, because matchbox
       * wants to pick it up from X */
      if ((hd_title_bar_get_state(priv->title_bar) & HDTB_VIS_BTN_RIGHT_MASK) &&
          !STATE_IS_APP(priv->state))
        {
          ClutterGeometry *g = &geom[geom_count++];
          g->width = hd_title_bar_get_button_width(priv->title_bar);
          g->height = HD_COMP_MGR_TOP_MARGIN;
          g->x = hd_comp_mgr_get_current_screen_width() - g->width;
          g->y = 0;
        }

      /* Edit button... */
      if (hd_render_manager_actor_is_visible(hd_home_get_edit_button(priv->home)))
        {
          clutter_actor_get_geometry(hd_home_get_edit_button(priv->home),
                                     &geom[geom_count++]);
        }

      /* Block status area?  If so refer to the client geometry,
       * because we might be right after a place_titlebar_elements()
       * which could just have moved it. */
      if (hd_render_manager_actor_is_visible(priv->status_area) &&
          (STATE_IS_PORTRAIT (priv->state)
           /* also in the case of "dialog blur": */
           || (priv->state == HDRM_STATE_APP
                /* FIXME: the following check does not work when there are
                 * two levels of dialogs */
                && (priv->current_blur & (HDRM_BLUR_BACKGROUND|HDRM_BLUR_HOME))
              )
           ))
        {
          g_assert(priv->status_area_client);
          const MBGeometry *src = &priv->status_area_client->frame_geometry;
          ClutterGeometry *dst = &geom[geom_count++];
          dst->x = src->x;
          dst->y = src->y;
          dst->width  = src->width;
          dst->height = src->height;
        }
    }
  else
    {
      /* g_warning ("%s: get the whole screen!", __func__); */
      geom[0].x = 0;
      geom[0].y = 0;
      geom[0].width = hd_comp_mgr_get_current_screen_width ();
      geom[0].height = hd_comp_mgr_get_current_screen_height ();
      geom_count = 1;
    }

  hd_comp_mgr_setup_input_viewport(priv->comp_mgr, geom, geom_count);
}

/* The syncing with clutter that is done before a transition */
static
void hd_render_manager_sync_clutter_before ()
{
  HdRenderManagerPrivate *priv = the_render_manager->priv;

  HdTitleBarVisEnum btn_state = hd_title_bar_get_state(priv->title_bar) &
    ~(HDTB_VIS_BTN_LEFT_MASK | HDTB_VIS_FULL_WIDTH |
      HDTB_VIS_BTN_RIGHT_MASK | HDTB_VIS_FOREGROUND |
      HDTB_VIS_SMALL_BUTTONS);
  HDRMBlurEnum blur = 0;
  gboolean blurred_changed = FALSE;

  if (STATE_SHOW_APPLETS(priv->state))
    blur |= HDRM_SHOW_APPLETS;

  switch (priv->state)
    {
      case HDRM_STATE_UNDEFINED:
        g_error("%s: NEVER supposed to be in HDRM_STATE_UNDEFINED", __func__);
	return;
      case HDRM_STATE_HOME:
        blur |=  HDRM_ZOOM_FOR_HOME;
      case HDRM_STATE_HOME_PORTRAIT: /* Fallen truth */
        if (hd_task_navigator_is_empty())
          btn_state |= HDTB_VIS_BTN_LAUNCHER;
        else
          btn_state |= HDTB_VIS_BTN_SWITCHER;
        clutter_actor_show(CLUTTER_ACTOR(priv->home));
        hd_home_update_layout (priv->home);
        break;
      case HDRM_STATE_HOME_EDIT:
        blur |= HDRM_BLUR_HOME; /* fall through intentionally */
      case HDRM_STATE_HOME_EDIT_DLG:
        clutter_actor_show(CLUTTER_ACTOR(priv->home));
        hd_home_update_layout (priv->home);
        break;
      case HDRM_STATE_LOADING: /* fall through intentionally */
      case HDRM_STATE_LOADING_SUBWIN:
        if (hd_task_navigator_is_empty())
          btn_state |= HDTB_VIS_BTN_LAUNCHER;
        else
          btn_state |= HDTB_VIS_BTN_SWITCHER;
        clutter_actor_show(CLUTTER_ACTOR(priv->home));
        /* Fixed NB#140723 - Task Launcher background should not un-blur
         *                   and un-dim when launching new app */
        if (priv->previous_state==HDRM_STATE_LAUNCHER)
          blur |= HDRM_BLUR_HOME;
        break;
      case HDRM_STATE_APP:
      case HDRM_STATE_APP_PORTRAIT:
        btn_state |= HDTB_VIS_BTN_SWITCHER;
        clutter_actor_hide(CLUTTER_ACTOR(priv->home));
        break;
      case HDRM_STATE_TASK_NAV:
        btn_state |= HDTB_VIS_BTN_LAUNCHER;
        clutter_actor_show(CLUTTER_ACTOR(priv->home));
        blur |=  HDRM_BLUR_HOME |
                 HDRM_ZOOM_FOR_TASK_NAV |
                 HDRM_SHOW_TASK_NAV;
        break;
      case HDRM_STATE_LAUNCHER:
        clutter_actor_show(CLUTTER_ACTOR(priv->home));
        blur |=
            HDRM_BLUR_HOME |
            HDRM_ZOOM_FOR_LAUNCHER |
            ((priv->previous_state==HDRM_STATE_TASK_NAV)?
                HDRM_ZOOM_FOR_TASK_NAV : 0);
        break;
      case HDRM_STATE_NON_COMPOSITED:
      case HDRM_STATE_NON_COMP_PORT:
        clutter_actor_hide(CLUTTER_ACTOR(priv->home));
        break;
    }

  clutter_actor_show(CLUTTER_ACTOR(priv->home_blur));
  clutter_actor_show(CLUTTER_ACTOR(priv->app_top));
  clutter_actor_show(CLUTTER_ACTOR(priv->front));
  clutter_actor_raise_top(CLUTTER_ACTOR(priv->app_top));
  clutter_actor_raise_top(CLUTTER_ACTOR(priv->front));

  if (STATE_IS_PORTRAIT(priv->state))
    btn_state |= HDTB_VIS_SMALL_BUTTONS;

  if (STATE_SHOW_OPERATOR(priv->state))
    clutter_actor_show(priv->operator);
  else
    clutter_actor_hide(priv->operator);

  if (priv->status_area)
    {
      if (STATE_SHOW_STATUS_AREA(priv->state))
        {
          clutter_actor_show(priv->status_area);
          clutter_actor_raise_top(priv->status_area);
        }
      else
        clutter_actor_hide(priv->status_area);
    }

  if (STATE_TOOLBAR_FOREGROUND(priv->state))
    btn_state |= HDTB_VIS_FOREGROUND;

  if (priv->status_menu)
    clutter_actor_raise_top(CLUTTER_ACTOR(priv->status_menu));

  if (!STATE_BLUR_BUTTONS(priv->state) &&
      clutter_actor_get_parent(CLUTTER_ACTOR(priv->blur_front)) !=
              CLUTTER_ACTOR(the_render_manager))
    {
      /* raise the blur_front out of the blur group so we can still
       * see it unblurred */
      clutter_actor_reparent(CLUTTER_ACTOR(priv->blur_front),
                             CLUTTER_ACTOR(the_render_manager));
      /* lower this below task_nav (see the ordering comments at the top) */
      clutter_actor_lower(CLUTTER_ACTOR(priv->blur_front),
                          CLUTTER_ACTOR(priv->task_nav));
      blurred_changed = TRUE;
    }

  /* Move the applets out to the front if required */
  {
    ClutterActor *home_front = hd_home_get_front (priv->home);
    if (STATE_HOME_FRONT (priv->state))
      {
        if (clutter_actor_get_parent(home_front) !=
            CLUTTER_ACTOR (priv->blur_front))
          {
            clutter_actor_reparent(home_front, CLUTTER_ACTOR (priv->blur_front));
            blurred_changed = TRUE;
          }
        clutter_actor_lower_bottom (home_front);
      }
    else if (clutter_actor_get_parent(home_front) !=
             CLUTTER_ACTOR (priv->home))
      {
        clutter_actor_reparent(home_front, CLUTTER_ACTOR (priv->home));
        blurred_changed = TRUE;
      }
  }

  hd_title_bar_set_state(priv->title_bar, btn_state);
  /* hd_render_manager_place_titlebar_elements calls hd_title_bar_update()
   * as well.
   * TODO: (gw) Do we *really* need to call this so often? Most likely there
   * is some corner case that we could fix some other way. Now we call this
   * inside set_visibilities after we change status area visibility, it might
   * not be needed. */
  hd_render_manager_place_titlebar_elements();

  /* Make sure we hide the edit button if it's not required */
  if (priv->state != HDRM_STATE_HOME)
    hd_home_hide_edit_button(priv->home);

  /* We need to check visibilities after we change between app state and not,
   * because currently we move status area right out of the way if we don't
   * think it's visible (to make it non-clickable). A fullscreen app->home
   * transition tends to leave the status area off the screen. NB#112996 */
  if (STATE_IS_APP(priv->state) != STATE_IS_APP(priv->previous_state) ||
      priv->previous_state == HDRM_STATE_NON_COMPOSITED ||
      priv->previous_state == HDRM_STATE_NON_COMP_PORT)
    hd_render_manager_queue_sync();

  /* Now look at what buttons we have showing, and add each visible button X
   * to the X input viewport. FIXME: Do we need this now HdTitleBar does it? */
  hd_render_manager_set_input_viewport();

  /* as soon as we start a transition, set out left-hand button to be
   * not pressed (used when home->switcher causes change of button style) */
  hd_title_bar_left_pressed(priv->title_bar, FALSE);

  /* Do set_blur here, as this sets the initial amounts of blurring, and we
   * want to have visibilities the way we want them when we do it */
  hd_render_manager_set_blur(blur);

  /* We have to call blurred_changed *after* set_blur, so we know
   * whether we are blurring in or out. (If blurring out, we don't
   * want to update the blurring immediately). */
  if (blurred_changed)
    hd_render_manager_blurred_changed();
}

/* The syncing with clutter that is done after a transition ends */
static
void hd_render_manager_sync_clutter_after ()
{
  HdRenderManagerPrivate *priv = the_render_manager->priv;

  if (STATE_BLUR_BUTTONS(priv->state) &&
      clutter_actor_get_parent(CLUTTER_ACTOR(priv->blur_front)) !=
                                CLUTTER_ACTOR(priv->home_blur))
    {
      /* raise the blur_front to the top of the home_blur group so
       * we still see the apps */
      clutter_actor_reparent(CLUTTER_ACTOR(priv->blur_front),
                             CLUTTER_ACTOR(priv->home_blur));
      hd_render_manager_blurred_changed();
    }

  /* The launcher transition should hide the launcher, so we shouldn't
   * need this.
  if (priv->state != HDRM_STATE_LAUNCHER)
    clutter_actor_hide(CLUTTER_ACTOR(priv->launcher));*/
}

/* ------------------------------------------------------------------------- */
/* -------------------------------------------------------------    PUBLIC   */
/* ------------------------------------------------------------------------- */

void hd_render_manager_stop_transition()
{
  HdRenderManagerPrivate *priv = the_render_manager->priv;

  if (priv->timeline_playing)
    {
      guint frames;
      clutter_timeline_stop(priv->timeline_blur);
      frames = clutter_timeline_get_n_frames(priv->timeline_blur);
      on_timeline_blur_new_frame(priv->timeline_blur, frames, the_render_manager);
      on_timeline_blur_completed(priv->timeline_blur, the_render_manager);
    }

  hd_launcher_transition_stop();
}

void hd_render_manager_add_to_front_group (ClutterActor *item)
{
  HdRenderManagerPrivate *priv = the_render_manager->priv;

  clutter_actor_reparent(item, CLUTTER_ACTOR(priv->front));
}

static gboolean hd_render_manager_status_area_clicked(ClutterActor *actor,
                                                      ClutterEvent *event,
                                                      gpointer data)
{
  /* When the status area is clicked and we have grabbed its input viewport,
   * we want to do what clicking on the modal blocker would have done ->
   * delete the frontmost dialog/menu if there was one with a blocker */

  HdRenderManagerPrivate *priv = the_render_manager->priv;
  MBWindowManager *wm = MB_WM_COMP_MGR(priv->comp_mgr)->wm;
  MBWindowManagerClient *c;

  for (c=wm->stack_top;c;c=c->stacked_below)
  {
    int c_type = MB_WM_CLIENT_CLIENT_TYPE(c);

    if (c->xwin_modal_blocker)
      {
        /* Create fake button release event */
        int tint;
        struct timeval tv;
        XButtonEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = ButtonRelease;
        ev.send_event = True;
        ev.display = wm->xdpy;
        /* create fake timestamp */
        gettimeofday(&tv, 0);
        tint = (int) tv.tv_sec * 1000;
        tint = tint / 1000 * 1000;
        tint = tint + tv.tv_usec / 1000;
        ev.time = (Time) tint;
        ev.root = wm->root_win->xwindow;
        ev.window = c->xwin_modal_blocker;
        ev.subwindow = None;
        ev.x = event->button.x;
        ev.y = event->button.y;
        ev.x_root = 1;
        ev.y_root = 1;
        ev.state = 0;
        ev.button = Button1;
        ev.same_screen = True;
        /* send event - trap errors just in case the window has been
         * removed already (very unlikely) */
        mb_wm_util_trap_x_errors();
        XSendEvent(wm->xdpy, c->xwin_modal_blocker, False,
                   ButtonReleaseMask, (XEvent *)&ev);
        mb_wm_util_untrap_x_errors();
        /* Now ignore the click on any client below */
        break;
      }
    /* No point looking past an app or desktop */
    if (c_type == MBWMClientTypeApp ||
        c_type == MBWMClientTypeDesktop)
      break;
  }
  return TRUE;
}

void hd_render_manager_set_status_area (ClutterActor *item)
{
  HdRenderManagerPrivate *priv = the_render_manager->priv;

  if (priv->status_area)
    {
      g_object_unref(priv->status_area);
    }

  if (item)
    {
      MBWMCompMgrClient *cc;

      /*
       * Make the status area actor reactive.  Normally, this has no effect
       * (ie. when the status area region is not in our input viewport).
       * When it is in the viewport we put it there specifically to block
       * access to the status menu.  If we don't make it reactive the click
       * goes through to the background.  So prevent it.
       */
      cc = g_object_get_data(G_OBJECT(item), "HD-MBWMCompMgrClutterClient");
      priv->status_area_client = cc->wm_client;
      priv->status_area = g_object_ref(item);
      clutter_actor_reparent(priv->status_area,
          CLUTTER_ACTOR(priv->title_bar));
      clutter_actor_set_reactive(priv->status_area, TRUE);
      g_signal_connect(item, "notify::width",
                       G_CALLBACK(hd_render_manager_place_titlebar_elements),
                       NULL);
      g_signal_connect(item, "button-release-event",
                             G_CALLBACK(hd_render_manager_status_area_clicked),
                             NULL);
    }
  else
    {
      priv->status_area = NULL;
      priv->status_area_client = NULL;
    }

  hd_render_manager_place_titlebar_elements();
}

void hd_render_manager_set_status_menu (ClutterActor *item)
{
  HdRenderManagerPrivate *priv = the_render_manager->priv;

  if (priv->status_menu)
    {
      g_object_unref(priv->status_menu);
    }

  if (item)
    {
      priv->status_menu = g_object_ref(item);
      clutter_actor_reparent(priv->status_menu, CLUTTER_ACTOR(priv->front));
      clutter_actor_raise_top(CLUTTER_ACTOR(priv->status_menu));
    }
  else
    priv->status_menu = NULL;
}

void hd_render_manager_set_operator (ClutterActor *item)
{
  HdRenderManagerPrivate *priv = the_render_manager->priv;

  if (priv->operator)
    g_object_unref(priv->operator);
  priv->operator = CLUTTER_ACTOR(g_object_ref(item));
}

void hd_render_manager_set_loading  (ClutterActor *item)
{
  HdRenderManagerPrivate *priv = the_render_manager->priv;

  if (priv->loading_image == item)
    return;

  if (priv->loading_image)
    {
      /* Put the loading image back where it should be */
      if (priv->loading_image_parent)
        {
          clutter_actor_reparent (priv->loading_image,
                                  priv->loading_image_parent);
          g_object_unref (G_OBJECT (priv->loading_image_parent));
          priv->loading_image_parent = NULL;
        }
      else if (clutter_actor_get_parent(priv->loading_image))
        {
          /* Or just totally unparent it */
          clutter_container_remove_actor (
              CLUTTER_CONTAINER(clutter_actor_get_parent(priv->loading_image)),
              priv->loading_image);
          /* fade it out nicely, if it is a real image rather than a solid
           * colour */
          if (item==NULL && !CLUTTER_IS_RECTANGLE(priv->loading_image))
            hd_transition_fade_out_loading_screen(priv->loading_image);
        }
      /* Remove our reference */
      g_object_unref (G_OBJECT (priv->loading_image));
      priv->loading_image = NULL;
    }

  if (item)
    priv->loading_image = CLUTTER_ACTOR(g_object_ref(item));
}

ClutterActor *hd_render_manager_get_title_bar(void)
{
  return CLUTTER_ACTOR(the_render_manager->priv->title_bar);
}

ClutterActor *hd_render_manager_get_status_area(void)
{
  return CLUTTER_ACTOR(the_render_manager->priv->status_area);
}

MBWindowManagerClient *hd_render_manager_get_status_area_client(void)
{
  return the_render_manager->priv->status_area_client;
}

/* FIXME: this should not be exposed */
ClutterContainer *hd_render_manager_get_front_group(void)
{
  HdRenderManagerPrivate *priv = the_render_manager->priv;
  return CLUTTER_CONTAINER(priv->front);
}

/* #ClutterEffectCompleteFunc for hd_task_navigator_zoom_out(). */
static void zoom_out_completed(ClutterActor *actor,
                               MBWMCompMgrClutterClient *cmgrcc)
{
  mb_wm_object_unref(MB_WM_OBJECT(cmgrcc));
}


void hd_render_manager_set_state(HDRMStateEnum state)
{
  HdRenderManagerPrivate *priv;
  MBWMCompMgr          *cmgr;
  MBWindowManager      *wm;
  MBWindowManagerClient *c;


  priv = the_render_manager->priv;
  cmgr = MB_WM_COMP_MGR (priv->comp_mgr);

  g_debug("%s: STATE %s -> STATE %s", __FUNCTION__,
      hd_render_manager_state_str(priv->state),
      hd_render_manager_state_str(state));

  if (!priv->comp_mgr)
  {
    g_warning("%s: HdCompMgr not defined", __FUNCTION__);
    return;
  }
  wm = cmgr->wm;

  if (priv->in_set_state)
      {
        g_warning("%s: State change ignored as already in "
                  "hd_render_manager_set_state", __FUNCTION__);
        return;
      }
  priv->in_set_state = TRUE;

  if (state != priv->state)
    {
      HDRMStateEnum oldstate = priv->state;
      priv->previous_state = priv->state;
      priv->state = state;

      if ((oldstate == HDRM_STATE_NON_COMPOSITED &&
           state != HDRM_STATE_NON_COMP_PORT) ||
          (oldstate == HDRM_STATE_NON_COMP_PORT &&
           state != HDRM_STATE_NON_COMPOSITED))
        {
	  hd_comp_mgr_reset_overlay_shape (HD_COMP_MGR (priv->comp_mgr));

          /* redirect and track damage again */
          for (c = wm->stack_top; c; c = c->stacked_below)
            {
              if (c->cm_client &&
                  mb_wm_comp_mgr_clutter_client_is_unredirected (c->cm_client))
                {
                  mb_wm_comp_mgr_clutter_set_client_redirection (c->cm_client,
                                                                 TRUE);
                }

              if (c->cm_client)
                mb_wm_comp_mgr_clutter_client_track_damage (
                        MB_WM_COMP_MGR_CLUTTER_CLIENT (c->cm_client), True);
            }

          /* this is needed, otherwise task switcher background can remain
           * black (NB#140378) */
          hd_render_manager_queue_sync();
	}

      /* Return the actor if we used it for loading. */
      if (STATE_IS_LOADING(oldstate) &&
          priv->loading_image)
        {
          hd_render_manager_set_loading(NULL);
        }

      /* Goto HOME instead if tasw is not appropriate for some reason. */
      if (state == HDRM_STATE_TASK_NAV)
        {
          gboolean goto_tasw_now, goto_tasw_later;

          goto_tasw_now = goto_tasw_later = FALSE;
          if (!hd_task_navigator_is_empty() && !hd_wm_has_modal_blockers (wm))
            {
              if (STATE_IS_PORTRAIT (oldstate))
                goto_tasw_later = TRUE;
              else
                goto_tasw_now   = TRUE;
            }

          /* Switch to tasw when we've really exited portarit mode. */
          hd_transition_rotate_screen_and_change_state (goto_tasw_later
                          ? HDRM_STATE_TASK_NAV : HDRM_STATE_UNDEFINED);

          if (!goto_tasw_now)
            {
              /* It may not be very intuitive what we're doing if in APP state
               * you have a sysmodal and hit CTRL-Backspace (we go to HOME),
               * but whatever. */
              state = priv->state =
                goto_tasw_later && oldstate == HDRM_STATE_APP_PORTRAIT
                  ? HDRM_STATE_APP : HDRM_STATE_HOME;
              g_debug("you must have meant STATE %s -> STATE %s",
                      hd_render_manager_state_str(oldstate),
                      hd_render_manager_state_str(state));
              if (state == oldstate)
                goto out;
            }
        }
      else
        /* There may not be any rotation in progress in which case this
         * has no effect, otherwise cancel any possible previous indication
         * of state switching. */
        hd_transition_rotate_screen_and_change_state (HDRM_STATE_UNDEFINED);

      /* Discard notification previews. */
      if (STATE_DISCARD_PREVIEW_NOTE (state))
          for (c = wm->stack_top; c; c = c->stacked_below)
            if (HD_IS_INCOMING_EVENT_PREVIEW_NOTE (c))
              {
                mb_wm_client_hide (c);
                mb_wm_client_deliver_delete (c);
              }

      /* Enter or leave the task switcher. */
      if (STATE_NEED_TASK_NAV (state))
        {
          /* Zoom out if possible.  Otherwise if not coming from launcher
           * scroll it back to the top. */
          if (STATE_IS_APP(oldstate))
            {
              ClutterActor *actor;
              MBWindowManagerClient *mbwmc;
              MBWMCompMgrClutterClient *cmgrcc;

              /* This beautiful code seems to survive everything. */
              if ((mbwmc = mb_wm_get_visible_main_client(wm)) &&
                  (cmgrcc = MB_WM_COMP_MGR_CLUTTER_CLIENT(mbwmc->cm_client)) &&
                  (actor = mb_wm_comp_mgr_clutter_client_get_actor(cmgrcc)) &&
                  CLUTTER_ACTOR_IS_VISIBLE(actor) &&
                  hd_task_navigator_has_window(priv->task_nav, actor))
                {
                  /* Make the tasw fully opaque as it might have been made
                   * transparent while exiting it. */
                  clutter_actor_set_opacity(CLUTTER_ACTOR(priv->task_nav), 255);
                  range_set(&priv->task_nav_opacity, 1);

                  /* Make sure we stop any active transitions, as these don't
                   * work well with task_nav (esp. subview transitions where
                   * task nav takes only the frontmost - NB#120171) */
                  hd_transition_stop(priv->comp_mgr, mbwmc);

                  /* Make sure @cmgrcc stays around as long as needed. */
                  mb_wm_object_ref (MB_WM_OBJECT (cmgrcc));
                  hd_task_navigator_zoom_out(priv->task_nav, actor,
                          (ClutterEffectCompleteFunc)zoom_out_completed,
                          cmgrcc);
                }
            }
          else if (oldstate != HDRM_STATE_LAUNCHER)
            hd_task_navigator_scroll_back(priv->task_nav);
        }
      if (STATE_ONE_OF(state | oldstate, HDRM_STATE_TASK_NAV))
        /* Stop breathing the Tasks button when entering/leaving the switcher. */
        hd_title_bar_set_switcher_pulse(priv->title_bar, FALSE);

      /* Enter/leave the launcher. */
      if (state == HDRM_STATE_LAUNCHER)
        hd_launcher_show();
      if (oldstate == HDRM_STATE_LAUNCHER)
        hd_launcher_hide();

      /* Show/hide the loading image. */
      if (STATE_IS_LOADING(state) &&
          priv->loading_image)
        {
          ClutterActor *parent = clutter_actor_get_parent (priv->loading_image);
          /* Keep the loading screen parent to return to it later. */
          priv->loading_image_parent = parent ?
                              CLUTTER_ACTOR (g_object_ref (parent)) : NULL;
          clutter_actor_reparent (priv->loading_image,
                                  CLUTTER_ACTOR (priv->blur_front));
          clutter_actor_set_size(priv->loading_image,
                                 hd_comp_mgr_get_current_screen_width (),
                                 hd_comp_mgr_get_current_screen_height ()
                                 - HD_COMP_MGR_TOP_MARGIN);
          clutter_actor_set_position(priv->loading_image,
                                     0, HD_COMP_MGR_TOP_MARGIN);
          clutter_actor_show (priv->loading_image);
        }

      if (STATE_NEED_DESKTOP(state) != STATE_NEED_DESKTOP(oldstate))
        mb_wm_handle_show_desktop(wm, STATE_NEED_DESKTOP(state));

      if (STATE_SHOW_APPLETS(state) != STATE_SHOW_APPLETS(oldstate))
        hd_comp_mgr_update_applets_on_current_desktop_property (HD_COMP_MGR (priv->comp_mgr));

      /* if we have moved away from the home edit dialog mode, then
       * we must make sure there are no home edit dialogs left around */
      if (oldstate == HDRM_STATE_HOME_EDIT_DLG)
        hd_home_remove_dialogs(priv->home);

      /* Divert state change if going to some portrait-capable mode.
       * Allow for APP_PORTRAIT <=> HOME_PORTRAIT too. */
      if ((   (oldstate != HDRM_STATE_APP_PORTRAIT  && state == HDRM_STATE_APP)
           || (oldstate != HDRM_STATE_HOME_PORTRAIT && state == HDRM_STATE_HOME))
          && hd_comp_mgr_should_be_portrait (priv->comp_mgr))
        { g_debug("divert");
          priv->in_set_state = FALSE;
          hd_render_manager_set_state (state == HDRM_STATE_APP
                                       ? HDRM_STATE_APP_PORTRAIT
                                       : HDRM_STATE_HOME_PORTRAIT);
          return;
        }

      hd_render_manager_sync_clutter_before();

      /* Switch between portrait <=> landscape modes. */
      if (!!STATE_IS_PORTRAIT (oldstate) != !!STATE_IS_PORTRAIT (state))
        {
          if (STATE_IS_PORTRAIT (state))
            hd_transition_rotate_screen (wm, TRUE);
          else if (STATE_IS_PORTRAIT (oldstate))
            hd_transition_rotate_screen (wm, FALSE);
        }

      /* Reset CURRENT_APP_WIN when entering tasw. */
      /* Try not to change it unnecessary. */
      if (state == HDRM_STATE_TASK_NAV)
        hd_wm_current_app_is (wm, ~0);
      else if (oldstate == HDRM_STATE_TASK_NAV && !STATE_IS_APP (state))
        hd_wm_current_app_is (wm,  0);

      /* Signal the state has changed. */
      g_object_notify (G_OBJECT (the_render_manager), "state");

      if ((oldstate==HDRM_STATE_TASK_NAV &&
	   state==HDRM_STATE_APP) ||
	   (state==HDRM_STATE_HOME_EDIT_DLG))
	{

	  /* Do some tidying up that used to happen in
	   * hd_switcher_zoom_in_complete().  But if we do it
	   * there, it doesn't get called if we leave the switcher
	   * other than through a zoom-in (e.g. the shutter button
	   * being pressed).
	   *
	   * FIXME: Possibly the check for the client hibernating
	   * should also be moved here.
	   */

	  /* Added check for HDRM_STATE_HOME_EDIT_DLG here because we
	   * need a restack to ensure that blurring is set correctly.
	   * Usually we would blur, but if we HOME_EDIT, then lock and
	   * use the power key, we get to show a fullscreen dialog
	   * (which doesn't have blurring) right away.
	   */

	  /* make sure everything is in the correct order */
	  hd_comp_mgr_sync_stacking (HD_COMP_MGR (priv->comp_mgr));
	}

      /* When moving from an app to the task navigator, stop the transition
       * of brightness, saturation + blurring at the final values, so that
       * while the app zooms out, the background is already dimmed with the
       * vignette effect (and you tend not to notice the blur iterating to
       * the correct value). Solves 131502
       * Note: we can't do transition_stop here as Martin still wants the
       * zooming. */
      if (oldstate==HDRM_STATE_APP &&
          state==HDRM_STATE_TASK_NAV)
        {
          range_set(&priv->home_brightness, priv->home_brightness.b);
          range_set(&priv->home_saturation, priv->home_saturation.b);
          range_set(&priv->home_radius, priv->home_radius.b);
          on_timeline_blur_new_frame(priv->timeline_blur,
              clutter_timeline_get_current_frame(priv->timeline_blur), NULL);
        }

      if (state == HDRM_STATE_NON_COMPOSITED ||
          state == HDRM_STATE_NON_COMP_PORT)
        {
          MBWindowManagerClient *c;

          /* move SA offscreen */
          c = priv->status_area_client;
          if (c)
            {
              c->frame_geometry.x = c->window->geometry.x = -1000;
              mb_wm_client_geometry_mark_dirty(c);
              mb_wm_comp_mgr_client_configure (c->cm_client);
            }

	  hd_comp_mgr_reset_overlay_shape (HD_COMP_MGR (priv->comp_mgr));

          hd_comp_mgr_unredirect_topmost_client (wm);
	}
    }

out:
  priv->in_set_state = FALSE;
}

/* Upgrade the current state to portrait. */
void hd_render_manager_set_state_portrait (void)
{
  g_assert (STATE_IS_PORTRAIT_CAPABLE (the_render_manager->priv->state));
  if (the_render_manager->priv->state == HDRM_STATE_APP)
    hd_render_manager_set_state (HDRM_STATE_APP_PORTRAIT);
  else if (the_render_manager->priv->state == HDRM_STATE_NON_COMPOSITED)
    hd_render_manager_set_state (HDRM_STATE_NON_COMP_PORT);
  else
    hd_render_manager_set_state (HDRM_STATE_HOME_PORTRAIT);
}

/* ...and the opposite. */
void hd_render_manager_set_state_unportrait (void)
{
  g_assert (STATE_IS_PORTRAIT (the_render_manager->priv->state));
  if (the_render_manager->priv->state == HDRM_STATE_APP_PORTRAIT)
    hd_render_manager_set_state (HDRM_STATE_APP);
  else if (the_render_manager->priv->state == HDRM_STATE_NON_COMP_PORT)
    hd_render_manager_set_state (HDRM_STATE_NON_COMPOSITED);
  else
    hd_render_manager_set_state (HDRM_STATE_HOME);
}

/* Returns whether set_state() is in progress. */
gboolean hd_render_manager_is_changing_state(void)
{
  return the_render_manager->priv->in_set_state;
}

inline HDRMStateEnum hd_render_manager_get_state()
{
  if (!the_render_manager)
    return HDRM_STATE_UNDEFINED;
  return the_render_manager->priv->state;
}

inline HDRMStateEnum hd_render_manager_get_previous_state()
{
  if (!the_render_manager)
    return HDRM_STATE_UNDEFINED;
  return the_render_manager->priv->previous_state;
}

static const char *hd_render_manager_state_str(HDRMStateEnum state)
{
  GTypeClass *state_class = g_type_class_ref (HD_TYPE_RENDER_MANAGER_STATE);
  GEnumValue *state_value = g_enum_get_value (
                             G_ENUM_CLASS (state_class),
                             state);
  g_type_class_unref (state_class);

  if (!state_value)
    return "";

  return state_value->value_name;
}

const char *hd_render_manager_get_state_str()
{
  HdRenderManagerPrivate *priv = the_render_manager->priv;
  return hd_render_manager_state_str(priv->state);
}

static void
hd_render_manager_get_property (GObject    *object,
                                guint       property_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  switch (property_id)
    {
    case PROP_STATE:
      g_value_set_enum (value, hd_render_manager_get_state ());
      break;
    default:
      /* We don't have any other property... */
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
hd_render_manager_set_property (GObject      *gobject,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  switch (prop_id)
    {
    case PROP_STATE:
      hd_render_manager_set_state (g_value_get_enum (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
      break;
    }
}

gboolean hd_render_manager_in_transition(void)
{
  HdRenderManagerPrivate *priv = the_render_manager->priv;
  return clutter_timeline_is_playing(priv->timeline_blur);
}

/* Return @actor, an actor of a %HdApp to HDRM's care. */
void hd_render_manager_return_app(ClutterActor *actor)
{
  HdRenderManagerPrivate *priv = HD_RENDER_MANAGER_GET_PRIVATE (
		  the_render_manager);
  MBWMCompMgrClutterClient *cc;

  if (priv->disposed) {
    return;
  }

  /* If we can get the clutter client, and it says it is in an effect, leave
   * it alone as we don't want to be hiding/reparenting it.  */
  cc = g_object_get_data (G_OBJECT (actor), "HD-MBWMCompMgrClutterClient");
  if (cc && (mb_wm_comp_mgr_clutter_client_get_flags(cc) &
             MBWMCompMgrClutterClientEffectRunning))
    return;

  /* ONLY reparent to home_blur if we were currently someone else's.
   * Otherwise it's quite likely that the MBWMClutterClient has been destroyed,
   * but this actor won't be because it would be owned by home_blur.
   * See bug 121519.
   */
  if (clutter_actor_get_parent(actor))
    {
      clutter_actor_reparent(actor,
                             CLUTTER_ACTOR(the_render_manager->priv->home_blur));
      clutter_actor_lower_bottom(actor);
      clutter_actor_hide(actor);
    }
}

/* Same for dialogs. */
void hd_render_manager_return_dialog(ClutterActor *actor)
{
  clutter_actor_reparent(actor,
                         CLUTTER_ACTOR(the_render_manager->priv->app_top));
  clutter_actor_hide (actor);
}

/*
 * Clip the offscreen parts of a @geo, ensuring that it doesn't have negative
 * (x, y) coordinates.  Returns %FALSE if it's completely offscreen, meaning
 * you can ignore it.
 */
static gboolean
hd_render_manager_clip_geo(ClutterGeometry *geo)
{
  guint t;

  if (geo->x < 0)
    {
      if (-geo->x >= geo->width)
        return FALSE;
      geo->width += geo->x;
      geo->x = 0;
    }

  if (geo->y < 0)
    {
      if (-geo->y >= geo->height)
        return FALSE;
      geo->height += geo->y;
      geo->y = 0;
    }

  t = hd_comp_mgr_get_current_screen_width ();
  if (geo->x >= t)
    return FALSE;
  if (geo->x + geo->width > t)
    geo->width = t - geo->x;

  t = hd_comp_mgr_get_current_screen_height ();
  if (geo->y >= t)
    return FALSE;
  if (geo->y + geo->height > t)
    geo->height = t - geo->y;

  return TRUE;
}

/* Tries to guess whether @geo was meant for a @scrw x @scrh screen. */
static gboolean
hd_render_manager_geo_is_suitable_for_screen(const ClutterGeometry *geo,
                                             guint scrw, guint scrh)
{
  if (geo->x + (gint)geo->width  > (gint)scrw)
    return FALSE;
  if (geo->y + (gint)geo->height > (gint)scrh)
    return FALSE;
  return TRUE;
}

/*
 * Returns @actor's %ClutterGeometry.  If it finds that it is rotated
 * it maps @geo to the screens current orientation.  This allows to
 * compare geometries of all clients whether they're layed out for
 * portrait or landscape.  Of course all heuristics can break.
 */
static void
hd_render_manager_get_geo_for_current_screen(ClutterActor *actor,
                                             ClutterGeometry *geo)
{
  guint scrw, scrh;
  ClutterGeometry rgeo;

  scrw = hd_comp_mgr_get_current_screen_width();
  scrh = hd_comp_mgr_get_current_screen_height();
  clutter_actor_get_geometry(actor, geo);

  /* Is @geo ok for current orientation? */
  if (hd_render_manager_geo_is_suitable_for_screen(geo, scrw, scrh))
    return;

  /* If the rotated geometry is ok, return that, otherwise give it up. */
  rgeo = *geo;
  hd_util_rotate_geometry(&rgeo, scrw, scrh);
  if (hd_render_manager_geo_is_suitable_for_screen(&rgeo, scrw, scrh))
    *geo = rgeo;
}

/* Called to restack the windows in the way we use for rendering... */
static void hd_render_manager_restack()
{
  HdRenderManagerPrivate *priv = the_render_manager->priv;
  MBWindowManager *wm;
  MBWindowManagerClient *c;
  gboolean past_desktop = FALSE;
  gboolean blur_changed = FALSE;
  gint i;
  GList *previous_home_blur = 0;

  wm = MB_WM_COMP_MGR(priv->comp_mgr)->wm;
  /* Add all actors currently in the home_blur group */

  for (i=0;i<clutter_group_get_n_children(CLUTTER_GROUP(priv->home_blur));i++)
    previous_home_blur = g_list_prepend(previous_home_blur,
        clutter_group_get_nth_child(CLUTTER_GROUP(priv->home_blur), i));

  /* Order and choose which window actors will be visible */
  for (c = wm->stack_bottom; c; c = c->stacked_above)
    {
      past_desktop |= (wm->desktop == c);
      /* If we're past the desktop then add us to the stuff that will be
       * visible */

      if (c->cm_client && c->desktop >= 0) /* FIXME: should check against
					      current desktop? */
        {
	  /* If the client decides its own visibility, let it figure out
	   * its own stacking as well.
	   */
	  if (hd_render_manager_should_ignore_cm_client
	      (MB_WM_COMP_MGR_CLUTTER_CLIENT(c->cm_client)))
	      continue;

          ClutterActor *actor = 0;
          ClutterActor *desktop = mb_wm_comp_mgr_clutter_get_nth_desktop(
              MB_WM_COMP_MGR_CLUTTER(priv->comp_mgr), c->desktop);
          actor = mb_wm_comp_mgr_clutter_client_get_actor(
              MB_WM_COMP_MGR_CLUTTER_CLIENT(c->cm_client));
          if (actor)
            {
              ClutterActor *parent = clutter_actor_get_parent(actor);
              if (past_desktop)
                {
                  /* if we want to render this, add it. we need to be careful
                   * not to pull applets or other things out from where they
                   * were */
                  if (parent)
                    {
                      if (parent == CLUTTER_ACTOR(desktop) ||
                          parent == CLUTTER_ACTOR(priv->app_top))
                        {
                          clutter_actor_reparent(actor,
                                                 CLUTTER_ACTOR(priv->home_blur));
                        }
#if STACKING_DEBUG
                      else
                        g_debug("%s NOT MOVED - OWNED BY %s",
                            clutter_actor_get_name(actor)?clutter_actor_get_name(actor):"?",
                            clutter_actor_get_name(parent)?clutter_actor_get_name(parent):"?");
#endif /*STACKING_DEBUG*/
		      clutter_actor_raise_top(actor);
                    }
#if STACKING_DEBUG
                  else
                    g_debug("%s DOES NOT HAVE A PARENT",
                        clutter_actor_get_name(actor)?clutter_actor_get_name(actor):"?");
#endif /*STACKING_DEBUG*/
                }
              else
                {
                  /* else we put it back into the arena */
                  if (parent == CLUTTER_ACTOR(priv->home_blur) ||
                      parent == CLUTTER_ACTOR(priv->app_top))
                    clutter_actor_reparent(actor, desktop);
                }
            }
        }
    }

  /* Now start at the top and put actors in the non-blurred group
   * until we find one that fills the screen. If we didn't find
   * any that filled the screen then add the window that does. */
  {
    gint i, n_elements;

    n_elements = clutter_group_get_n_children(CLUTTER_GROUP(priv->home_blur));
    for (i=n_elements-1;i>=0;i--)
      {
        ClutterActor *child =
          clutter_group_get_nth_child(CLUTTER_GROUP(priv->home_blur), i);

	/* If the client decides its own visibility, skip it */
	if (hd_render_manager_should_ignore_actor(child))
	    continue;

        if (child != CLUTTER_ACTOR(priv->home) &&
            child != CLUTTER_ACTOR(priv->blur_front))
          {
            ClutterGeometry geo = {0};
            gboolean maximized;

            hd_render_manager_get_geo_for_current_screen(child, &geo);
            if (!hd_render_manager_clip_geo(&geo))
              /* It's neiteher maximized nor @app_top, it doesn't exist. */
              continue;
            maximized = hd_comp_mgr_client_is_maximized (
                                        *((MBGeometry*)((void*)&geo)));

            /* Maximized stuff should never be blurred (unless there
             * is nothing else) */
            /* If we are in HOME_EDIT_DLG state, the background is always
             * blurred, and if something is maximised it MUST be a dialog
             * (or we would have left the state) - so we want it brought
             * to app_top where it is NOT blurred. */
            if ((!maximized) || (priv->state == HDRM_STATE_HOME_EDIT_DLG))
              {
                clutter_actor_reparent(child, CLUTTER_ACTOR(priv->app_top));
                clutter_actor_lower_bottom(child);
		clutter_actor_show(child);
              }
            /* If this is maximized, or in dialog's position, don't
             * blur anything after */
            if (maximized || (
                geo.width == hd_comp_mgr_get_current_screen_width () &&
                geo.y + geo.height == hd_comp_mgr_get_current_screen_height ()
                ))
              break;

          }
      }
  }

  if (clutter_actor_get_parent(CLUTTER_ACTOR(priv->blur_front)) ==
                               CLUTTER_ACTOR(priv->home_blur))
    clutter_actor_raise_top(CLUTTER_ACTOR(priv->blur_front));

  /* We could have changed the order of the windows here, so update whether
   * we blur or not based on the order. */
  hd_render_manager_update_blur_state();

  /* And for speed of rendering, work out what is visible and what
   * isn't, and hide anything that would be rendered over by another app */
  hd_render_manager_set_visibilities();

  /* now compare the contents of home_blur to see if the blur group has
   * actually changed... */
  if (g_list_length(previous_home_blur) ==
      clutter_group_get_n_children(CLUTTER_GROUP(priv->home_blur)))
    {
      GList *it;
      for (i = 0, it = g_list_last(previous_home_blur);
           (i<clutter_group_get_n_children(CLUTTER_GROUP(priv->home_blur))) && it;
           i++, it=it->prev)
        {
          ClutterActor *child =
              clutter_group_get_nth_child(CLUTTER_GROUP(priv->home_blur), i);
          if (CLUTTER_ACTOR(it->data) != child)
            {
              //g_debug("*** RE-BLURRING *** because contents changed at pos %d", i);
              blur_changed = TRUE;
              break;
            }
        }
    }
  else
    {
      /*g_debug("*** RE-BLURRING *** because contents changed size %d -> %d",
          g_list_length(previous_home_blur),
          clutter_group_get_n_children(CLUTTER_GROUP(priv->home_blur)));*/
      blur_changed = TRUE;
    }
  g_list_free(previous_home_blur);

  /* ----------------------------- DEBUG PRINTING */
#if STACKING_DEBUG
  for (i = 0;i<clutter_group_get_n_children(CLUTTER_GROUP(priv->home_blur));i++)
    {
      ClutterActor *child =
                clutter_group_get_nth_child(CLUTTER_GROUP(priv->home_blur), i);
      const char *name = clutter_actor_get_name(child);
      g_debug("STACK[%d] %s %s", i, name?name:"?",
          CLUTTER_ACTOR_IS_VISIBLE(child)?"":"(invisible)");
    }
  for (i = 0;i<clutter_group_get_n_children(CLUTTER_GROUP(priv->app_top));i++)
      {
        ClutterActor *child =
                  clutter_group_get_nth_child(CLUTTER_GROUP(priv->app_top), i);
        const char *name = clutter_actor_get_name(child);
        g_debug("TOP[%d] %s %s", i, name?name:"?",
            CLUTTER_ACTOR_IS_VISIBLE(child)?"":"(invisible)");
      }

  for (c = wm->stack_bottom,i=0; c; c = c->stacked_above,i++)
    {
      ClutterActor *a = 0;

      if (c->cm_client)
        a = mb_wm_comp_mgr_clutter_client_get_actor(
                MB_WM_COMP_MGR_CLUTTER_CLIENT(c->cm_client));
      g_debug("WM[%d] : %s %s %s", i,
          c->name?c->name:"?",
          (a && clutter_actor_get_name(a)) ?  clutter_actor_get_name(a) : "?",
          (wm->desktop==c) ? "DESKTOP" : "");
    }
#endif /*STACKING_DEBUG*/
    /* ----------------------------- */

  /* because swapping parents doesn't appear to fire a redraw */
  if (blur_changed)
    hd_render_manager_blurred_changed();

  /* update our fixed title bar at the top of the screen */
  hd_title_bar_update(priv->title_bar, MB_WM_COMP_MGR(priv->comp_mgr));
}

static gboolean hd_render_manager_do_sync() {
  HdRenderManagerPrivate *priv = the_render_manager->priv;

  if (priv->in_idle_sync)
    {
      g_warning("%s: should not be called from itself", __FUNCTION__);
      return FALSE;
    }
  priv->in_idle_sync = TRUE;

  hd_render_manager_restack();

  priv->in_idle_sync = FALSE;

  /* Remove the callback... */
  if (priv->idle_sync_source)
    {
      g_source_remove(priv->idle_sync_source);
      priv->idle_sync_source = 0;
    }
  return FALSE;
}

void hd_render_manager_do_sync_if_queued() {
  HdRenderManagerPrivate *priv = the_render_manager->priv;

  if (!priv->in_idle_sync && priv->idle_sync_source)
    hd_render_manager_do_sync();
}

void hd_render_manager_queue_sync() {
  HdRenderManagerPrivate *priv = the_render_manager->priv;

  if (!priv->idle_sync_source)
    {
      /* Make this higher priority than HdTitleBar and
       * clutter_redraw */
      priv->idle_sync_source = g_idle_add_full(
          G_PRIORITY_DEFAULT+10,
          hd_render_manager_do_sync,
          NULL, NULL);
    }
}

static void hd_render_manager_update_blur_state()
{
  HdRenderManagerPrivate *priv = the_render_manager->priv;
  HDRMBlurEnum blur_flags;
  HdTitleBarVisEnum title_flags;
  MBWindowManager *wm = MB_WM_COMP_MGR(priv->comp_mgr)->wm;
  MBWindowManagerClient *c;
  gboolean blur = FALSE;
  gboolean blur_buttons = FALSE;
  gboolean has_video_overlay = FALSE;

  /* FIXME: check this for non-composited mode */

  /* Now look through the MBWM stack and see if we need to blur or not.
   * This happens when we have a dialog/menu in front of the main app */
  for (c=wm->stack_top;c;c=c->stacked_below)
    {
      int c_type = MB_WM_CLIENT_CLIENT_TYPE(c);

      /* If we are already blurred for something, now check and see if
       * any window we are blurring has a video overlay */
      if (blur) {
        if (hd_util_client_has_video_overlay(c))
          has_video_overlay = TRUE;
      }

      if (c_type == MBWMClientTypeApp)
        {
          /* If we have a fullscreen window then the top-left button and
           * status area will not be visible - so we don't want them
           * pulled out to the front. */
          if (c->window &&
              hd_comp_mgr_client_is_maximized (c->window->geometry))
            blur_buttons = TRUE;
          break;
        }
      if (c_type == MBWMClientTypeDesktop)
        break;
      if (c_type == MBWMClientTypeDialog ||
          c_type == MBWMClientTypeMenu ||
          c_type == HdWmClientTypeAppMenu ||
          c_type == HdWmClientTypeStatusMenu ||
          HD_IS_CONFIRMATION_NOTE (c) ||
          HD_IS_INFO_NOTE(c))
        {
          /* If this is a dialog that is maximised, it will be put in the
           * blur group - so do NOT blur the background for this alone.
           * Also this dialog is probably the VKB, which appears *over*
           * the top-left icon - so it acts like a system modal blocker
           * and we should not attempt to display unblurred top-left buttons */
          if (hd_comp_mgr_client_is_maximized (c->window->geometry))
            {
              blur_buttons = TRUE;
              break;
            }

          /*g_debug("%s: Blurring caused by window type %d, geo=%d,%d,%d,%d name '%s'",
              __FUNCTION__, c_type,
              c->window->geometry.x, c->window->geometry.y,
              c->window->geometry.width, c->window->geometry.height,
              c->name?c->name:"(null)");*/
          blur=TRUE;
          if (hd_util_client_has_modal_blocker(c))
            blur_buttons = TRUE;
        }

      /* If anything fills the entire screen, stop looking for things
       * to blur as you wouldn't see them anyway. */
      if (hd_comp_mgr_client_is_maximized(c->window->geometry))
        break;
    }

  blur_flags = priv->current_blur;
  title_flags = hd_title_bar_get_state(priv->title_bar);

  /* If we have a video overlay we can't blur properly - see
   * tidy_blur_group_set_chequer below */
  if (blur && !has_video_overlay)
    blur_flags = blur_flags | HDRM_BLUR_BACKGROUND;
  else
    blur_flags = blur_flags & ~HDRM_BLUR_BACKGROUND;

  /* Actually if we're in tasw the work was unnecessary but whatever. */
  if ((blur && !blur_buttons) || priv->state == HDRM_STATE_TASK_NAV)
    title_flags |= HDTB_VIS_FOREGROUND;
  else
    title_flags &= ~HDTB_VIS_FOREGROUND;

  if (blur_flags !=  priv->current_blur)
    hd_render_manager_set_blur(blur_flags);

  hd_title_bar_set_state(priv->title_bar, title_flags);

  /* If we have a video overlay, blurring sods it all up. Instead just
   * apply a chequer pattern to do our dimming */
  tidy_blur_group_set_chequer(
          CLUTTER_ACTOR(priv->home_blur),
          blur && has_video_overlay);
}

/* This is called when we are in the launcher subview so that we can blur and
 * darken the background even more */
void hd_render_manager_set_launcher_subview(gboolean subview)
{
  HdRenderManagerPrivate *priv = the_render_manager->priv;

  /*g_debug("%s: %s", __FUNCTION__, subview ? "SUBVIEW":"MAIN");*/
  if (subview)
    hd_render_manager_set_blur(priv->current_blur |
        HDRM_ZOOM_FOR_LAUNCHER_SUBMENU);
  else
    hd_render_manager_set_blur(priv->current_blur &
        ~HDRM_ZOOM_FOR_LAUNCHER_SUBMENU);
}

/* Work out if rect is visible after being clipped to avoid every
 * rect in blockers */
static gboolean
hd_render_manager_is_visible(GList *blockers,
                             ClutterGeometry rect)
{
  HdRenderManagerPrivate *priv = the_render_manager->priv;

  if (priv->state == HDRM_STATE_NON_COMPOSITED ||
      priv->state == HDRM_STATE_NON_COMP_PORT ||
      !hd_render_manager_clip_geo(&rect))
    return FALSE;

  /* clip for every block */
  for (; blockers; blockers = blockers->next)
    {
      ClutterGeometry blocker = *(ClutterGeometry*)blockers->data;
      gint rect_b, blocker_b;

      VISIBILITY ("RECT %dx%d%+d%+d BLOCKER %dx%d%+d%+d",
                  MBWM_GEOMETRY(&rect), MBWM_GEOMETRY(&blocker));

      /*
       * If rect does not fit inside blocker in the X axis...
       * Beware that ClutterGeometry.x and .y are signed, while .width
       * and .height are unsigned and the type propagation rules of C
       * makes sure we'll have trouble because the result is unsigned.
       * It's only significant, though when you compare signeds and
       * unsigneds.
       */
      if (!(blocker.x <= rect.x &&
            rect.x+(gint)rect.width <= blocker.x+(gint)blocker.width))
        continue;

      /* Because most windows will go edge->edge, just do a very simplistic
       * clipping in the Y direction */
      rect_b    = rect.y + rect.height;
      blocker_b = blocker.y + blocker.height;

      if (rect.y < blocker.y)
        { /* top of rect is above blocker */
          if (rect_b < blocker.y)
            /* rect is above blocker */
            continue;
          if (rect_b < blocker_b)
            /* rect is half above blocker, clip the rest */
            rect.height -= rect_b - blocker.y;
          else
            { /* rect is split into two pieces by blocker */
              rect.height = blocker.y - rect.y;
              if (hd_render_manager_is_visible(blockers, rect))
                /* upper half is visible */
                return TRUE;

              /* continue with the lower half */
              rect.y = blocker_b;
              rect.height = rect_b - blocker_b;
            }
        }
      else if (rect.y < blocker_b)
        { /* top of rect is inside blocker */
          if (rect_b < blocker_b)
            /* rect is confined in blocker */
            return FALSE;
          else
            { /* rect is half below blocker, clip the rest */
              rect.height -= blocker_b - rect.y;
              rect.y       = blocker_b;
            }
        }
      else
        /* rect is completely below blocker */;

      if (blocker.x <= rect.x &&
          rect.x+rect.width <= blocker.x+blocker.width)
        {
          if (blocker.y <= rect.y &&
              rect.y+rect.height <= blocker.y+blocker.height)
            {
              /* If rect fits inside blocker in the Y axis,
               * it is def. not visible */
              return FALSE;
            }
          else if (rect.y < blocker.y)
            {
              /* safety - if the blocker sits in the middle of us
               * it makes 2 rects, so don't use it */
              if (blocker.y+blocker.height >= rect.y+rect.height)
                /* rect out the bottom, clip to the blocker */
                rect.height = blocker.y - rect.y;
            }
          else
            { /* rect must be out the top, clip to the blocker */
              rect.height = (rect.y + rect.height) -
                            (blocker.y + blocker.height);
              rect.y = blocker.y + blocker.height;
            }
        }
    }

  return TRUE;
}

static
MBWindowManagerClient*
hd_render_manager_get_wm_client_from_actor(ClutterActor *actor)
{
  MBWindowManager *wm;
  MBWindowManagerClient *c;

  /* first off, try and get the client from the data set in the actor */
  MBWMCompMgrClient *cc = g_object_get_data (G_OBJECT (actor),
                                             "HD-MBWMCompMgrClutterClient");
  if (cc && cc->wm_client)
    return cc->wm_client;

  /*Or search... */
  wm = MB_WM_COMP_MGR(the_render_manager->priv->comp_mgr)->wm;
  /* Order and choose which window actors will be visible */
  for (c = wm->stack_bottom; c; c = c->stacked_above)
    if (c->cm_client) {
      ClutterActor *cactor = mb_wm_comp_mgr_clutter_client_get_actor(
                               MB_WM_COMP_MGR_CLUTTER_CLIENT(c->cm_client));
      if (actor == cactor)
        return c;
    }
  return 0;
}

static
gboolean hd_render_manager_should_ignore_cm_client(MBWMCompMgrClutterClient *cm_client)
{
  /* HdAnimationActor sets this flag to signal to whom it may concern that it
   * want to decide the visibility and stacking order of its window's clutter
   * client on its own.
   */
  return (mb_wm_comp_mgr_clutter_client_get_flags(cm_client) &
          MBWMCompMgrClutterClientDontShow);
}

static
gboolean hd_render_manager_should_ignore_actor(ClutterActor *actor)
{
  MBWindowManagerClient *wm_client;
  MBWMCompMgrClutterClient *cm_client;

  wm_client = hd_render_manager_get_wm_client_from_actor(actor);
  if (!(wm_client && wm_client->cm_client))
    return FALSE;
  cm_client = MB_WM_COMP_MGR_CLUTTER_CLIENT(wm_client->cm_client);

  return hd_render_manager_should_ignore_cm_client(cm_client);
}

static
gboolean hd_render_manager_actor_opaque(ClutterActor *actor)
{
  MBWindowManager *wm;
  MBWindowManagerClient *wm_client;

  /* this is ugly and slow, but is hopefully just a fallback... */
  if (!actor || !the_render_manager->priv->comp_mgr)
    /* this check is most probably unnecessary */
    return FALSE;
  wm = MB_WM_COMP_MGR(the_render_manager->priv->comp_mgr)->wm;
  wm_client = hd_render_manager_get_wm_client_from_actor(actor);
  return wm &&
         wm_client &&
         !wm_client->is_argb32 &&
         !mb_wm_theme_is_client_shaped(wm->theme, wm_client);
}

static
void hd_render_manager_append_geo_cb(ClutterActor *actor, gpointer data)
{
  GList **list = (GList**)data;
  if (hd_render_manager_actor_opaque(actor))
    {
      ClutterGeometry geo;

      hd_render_manager_get_geo_for_current_screen(actor, &geo);
      if (!hd_render_manager_clip_geo (&geo))
        return;
      *list = g_list_prepend(*list, g_memdup(&geo, sizeof(geo)));
      VISIBILITY ("BLOCKER %dx%d%+d%+d", MBWM_GEOMETRY(&geo));
    }
}

static
void hd_render_manager_set_visibilities()
{ VISIBILITY ("SET VISIBILITIES");
  HdRenderManagerPrivate *priv;
  GList *blockers = 0;
  GList *it;
  gint i, n_elements;
  ClutterGeometry fullscreen_geo = {0, 0,
          hd_comp_mgr_get_current_screen_width (),
          hd_comp_mgr_get_current_screen_height ()};
  MBWindowManager *wm;
  MBWindowManagerClient *c;
  gboolean has_fullscreen;

  priv = the_render_manager->priv;

  /* shortcut for non-composited mode */
  if (priv->state == HDRM_STATE_NON_COMPOSITED ||
      priv->state == HDRM_STATE_NON_COMP_PORT)
    {
      hd_render_manager_set_input_viewport();
      return;
    }

  /* first append all the top elements... */
  clutter_container_foreach(CLUTTER_CONTAINER(priv->app_top),
                            hd_render_manager_append_geo_cb,
                            (gpointer)&blockers);
  /* Now check to see if the whole screen is covered, and if so
   * don't bother rendering blurring */
  if (hd_render_manager_is_visible(blockers, fullscreen_geo))
    {
      clutter_actor_show(CLUTTER_ACTOR(priv->home_blur));
    }
  else
    {
      clutter_actor_hide(CLUTTER_ACTOR(priv->home_blur));
    }

  /* Then work BACKWARDS through the other items, working out if they are
   * visible or not */
  n_elements = clutter_group_get_n_children(CLUTTER_GROUP(priv->home_blur));
  for (i=n_elements-1;i>=0;i--)
    {
      ClutterActor *child =
        clutter_group_get_nth_child(CLUTTER_GROUP(priv->home_blur), i);
      if (child != CLUTTER_ACTOR(priv->blur_front))
        {
          ClutterGeometry geo;

	  /* If the client decides its own visibility, skip it */
	  if (hd_render_manager_should_ignore_actor(child))
	      continue;

          hd_render_manager_get_geo_for_current_screen(child, &geo);
          /*TEST clutter_actor_set_opacity(child, 63);*/
          VISIBILITY ("IS %p (%dx%d%+d%+d) VISIBLE?", child, MBWM_GEOMETRY(&geo));
          if (hd_render_manager_is_visible(blockers, geo))
            {
              VISIBILITY ("IS");
              clutter_actor_show(child);

              /* Add the geometry to our list of blockers and go to next... */
              if (hd_render_manager_actor_opaque(child))
                {
                  blockers = g_list_prepend(blockers, g_memdup(&geo, sizeof(geo)));
                  VISIBILITY ("MORE BLOCKER %dx%d%+d%+d", MBWM_GEOMETRY(&geo));
                }
            }
          else
            { /* Not visible, hide it unless... */
              VISIBILITY ("ISNT");
#ifdef __i386__
              /* On the device the flicker we can avoid with this check
               * upon subview->mainview transition is not visible. */
              if (!hd_transition_actor_will_go_away(child))
#endif
                clutter_actor_hide(child);
            }
        }
    }

  /* We did check STATE_NEED_DESKTOP(state) &&  CLUTTER_ACTOR_IS_VISIBLE(home)
   * and make an error here, but there are actually many cases where this is
   * valid. See NB#117092 */

  /* now free blockers */
  it = g_list_first(blockers);
  while (it)
    {
      g_free(it->data);
      it = it->next;
    }
  g_list_free(blockers);
  blockers = 0;

  /* Do we have a fullscreen client totally filling the screen? */
  /* Start from the top, looking down for visible, fullscreen windows
   * until we find one that fills the screen. */
  has_fullscreen = FALSE;
  wm = MB_WM_COMP_MGR(priv->comp_mgr)->wm;
  for (c = wm->stack_top; c && !has_fullscreen; c = c->stacked_below)
    {
      if (!c->cm_client || c->desktop < 0 || !c->window)
        /* It's probably an unnecessary check. */
        continue;
      if (!hd_render_manager_is_client_visible(c))
        continue;
      /* We must check for fullscreen-ness via the geometry, as notes
       * sometimes *are* fullscreen (so are positioned as such by hdrm_restack)
       * but do not have the flag (meaning they are overlapped by the title
       * bar). See bug 131081. */
      has_fullscreen |=
        hd_comp_mgr_client_is_maximized(c->window->geometry);
    }

  /* If we have a fullscreen something hide the blur_front
   * and move SA out of the way.  BTW blur_front is implcitly
   * shown by clutter when reparented. */
  c = priv->status_area_client;
  if (has_fullscreen)
    { VISIBILITY ("SA GO AWAY");
      clutter_actor_hide(CLUTTER_ACTOR(priv->blur_front));
      if (c && c->frame_geometry.y >= 0)
        { /* Move SA out of the way. */
          c->frame_geometry.y   = -c->frame_geometry.height;
          c->window->geometry.y = -c->window->geometry.height;
          mb_wm_client_geometry_mark_dirty(c);
          /* Unlike below, there should be no need to re-allocate the
           * title bar here, as if we have a fullscreen app, the title
           * will be invisible anyway. */
        }
    }
  else
    { VISIBILITY ("SA COME BACK");
      clutter_actor_show(CLUTTER_ACTOR(priv->blur_front));
      if (c && c->frame_geometry.y < 0)
        { /* Restore the position of SA. */
          c->frame_geometry.y = c->window->geometry.y = 0;
          mb_wm_client_geometry_mark_dirty(c);
          /* Now we have changed status area visibility, we must update
           * the position of the title. Ideally we wouldn't call this so
           * often, but for S3 this is least likely to cause regressions. */
          hd_render_manager_place_titlebar_elements();
        }
    }
  hd_render_manager_set_input_viewport();
}

/* Called by hd-task-navigator when its state changes, as when notifications
 * arrive the button in the top-left may need to change */
void hd_render_manager_update()
{
  hd_render_manager_sync_clutter_before();
}

/* Returns whether @c's actor is visible in clutter sense.  If so, then
 * it most probably is visible to the user as well.  It is assumed that
 * set_visibilities() have been sorted out for the current stacking. */
gboolean hd_render_manager_is_client_visible(MBWindowManagerClient *c)
{
  ClutterActor *a;
  MBWMCompMgrClutterClient *cc;
  HdRenderManagerPrivate *priv = the_render_manager->priv;

  if (priv->state == HDRM_STATE_NON_COMPOSITED ||
      priv->state == HDRM_STATE_NON_COMP_PORT)
    return FALSE;
  if (!(cc = MB_WM_COMP_MGR_CLUTTER_CLIENT(c->cm_client)))
    return FALSE;
  if (!(a  = mb_wm_comp_mgr_clutter_client_get_actor(cc)))
    return FALSE;

  /* If we were supposed to sync up visibilities, do it now
   * as we need to know we've got them. */
  hd_render_manager_do_sync_if_queued();

  /*
   * It is necessary to check the parents because sometimes
   * hd_render_manager_set_visibilities() hides the container
   * altogether.  Stage is never visible.  It is possible for
   * a client's actor to have a %NULL parent in case it was
   * never really mapped but deleted right away that time,
   * like in the case of unwanted notifications.
   */
  return hd_render_manager_actor_is_visible(a);
}

/* Place the status area, the operator logo and the title bar,
 * depending on the visible visual elements. */
void hd_render_manager_place_titlebar_elements (void)
{
  HdRenderManagerPrivate *priv = the_render_manager->priv;
  guint x;

  x = 0;

  /* Check whether buttons are visible based on state rather than anything
   * else. As actual actor visibilities may only be set on idle. */
  if (hd_title_bar_get_state(priv->title_bar) & HDTB_VIS_BTN_LEFT_MASK)
    x += hd_title_bar_get_button_width(priv->title_bar);

  if (priv->status_area && CLUTTER_ACTOR_IS_VISIBLE(priv->status_area))
    {
      g_assert(priv->status_area_client && priv->status_area_client->window);
      if (priv->status_area_client->frame_geometry.x != x)
        {
          /*
           * Reposition the status area.  Maybe we should just use
           * move_resize_client_xwin().  It is important that we
           * configure now because we may not get do a sync in the
           * near future, as mb is only waken up when there's an
           * X event.
           */
          MBWindowManagerClient *c = priv->status_area_client;
          c->frame_geometry.x = c->window->geometry.x = x;
          mb_wm_client_geometry_mark_dirty(c);
          mb_wm_comp_mgr_client_configure (c->cm_client);
          x += c->window->geometry.width;
        }
      else
        x += priv->status_area_client->frame_geometry.width;
    }

  if (priv->operator && CLUTTER_ACTOR_IS_VISIBLE(priv->operator))
    clutter_actor_set_x(priv->operator, HD_COMP_MGR_OPERATOR_PADDING + x);

  /* Force title bar to update the title/progress indicator position */
  hd_title_bar_update(priv->title_bar, MB_WM_COMP_MGR(priv->comp_mgr));
}

void hd_render_manager_blurred_changed()
{
  HdRenderManagerPrivate *priv;
  gboolean force = FALSE;
  gboolean force_not = FALSE;

  if (!the_render_manager) return;

  priv = the_render_manager->priv;

  /* from home_edit to home, applets swap from front to blurred and
   * it makes the transition look a bit strange */
  if (priv->previous_state == HDRM_STATE_HOME_EDIT &&
      priv->state          == HDRM_STATE_HOME)
    force = TRUE;

  /* If we're in the loading screen, we don't want to update blur
   * right away as the zooming loading image will fill the screen
   * soon anyway.
   */
  if (STATE_IS_LOADING(priv->state))
    force_not = TRUE;

  /* If we're in the middle of a transition to somewhere where we won't
   * blur, just 'hint' that we have damage (we'll recalculate next time
   * we blur). */
  if ((force || priv->home_radius.b != 0) && !force_not)
    tidy_blur_group_set_source_changed(
          CLUTTER_ACTOR(the_render_manager->priv->home_blur));
  else
    tidy_blur_group_hint_source_changed(
          CLUTTER_ACTOR(the_render_manager->priv->home_blur));
}

void
hd_render_manager_get_title_xy (int *x, int *y)
{
  if (!the_render_manager) return;

  hd_title_bar_get_xy (the_render_manager->priv->title_bar, x, y);
}

static gboolean
hd_render_manager_captured_event_cb (ClutterActor     *actor,
                                     ClutterEvent     *event,
                                     gpointer *data)
{
  /* We could, *maybe* get called before the_render_manager is set up - so do
   * a check anyway */
  if (the_render_manager &&
      the_render_manager->priv->has_input_blocker)
    {
      /* Just put a message here - this should only happen when the user
       * clicks really quickly */
      g_debug("%s: Input event blocked by "
              "hd_render_manager_add_input_blocker", __FUNCTION__);
      return TRUE; /* halt emission of this event */
    }

  return FALSE;
}

static gboolean
_hd_render_manager_remove_input_blocker_cb() {
/*  g_warning("%s: Input blocker removed because of timeout (window"
            " did not appear in time.", __FUNCTION__);*/
  hd_render_manager_remove_input_blocker();

  return FALSE;
}

/* Adds a input blocker which grabs the whole screen's input until either a
 * window appears or a timeout expires. We actually do input blocking by
 * grabbing the whole input viewport, and then ignoring any events captured
 * by clutter using hd_render_manager_captured_event_cb  */
void hd_render_manager_add_input_blocker() {
  HdRenderManagerPrivate *priv = the_render_manager->priv;
  if (!priv->has_input_blocker)
    {
      //g_debug("%s: Input Blocker ADDED", __FUNCTION__);
      priv->has_input_blocker = TRUE;
      hd_render_manager_set_input_viewport();
      /* After this timeout has expired we remove the blocker - this should
       * stop us getting into some broken state if the app does not start. */
      priv->has_input_blocker_timeout =
        g_timeout_add(1000,
                      (GSourceFunc)_hd_render_manager_remove_input_blocker_cb,
                      0);
    }
}

/* See hd_render_manager_add_input_blocker. This should be called when
 * we don't need the input blocked any more. */
void hd_render_manager_remove_input_blocker() {
  HdRenderManagerPrivate *priv = the_render_manager->priv;

  /* remove the timeout if there was one */
  if (priv->has_input_blocker_timeout)
    {
      g_source_remove(priv->has_input_blocker_timeout);
      priv->has_input_blocker_timeout = 0;
    }
   /* remove the modal blocker */
   if (priv->has_input_blocker)
     {
       //g_debug("%s: Input Blocker REMOVED", __FUNCTION__);
       priv->has_input_blocker = FALSE;
       hd_render_manager_set_input_viewport();
     }
 }

 gboolean hd_render_manager_allow_dbus_launch_transition() {
   HdRenderManagerPrivate *priv = the_render_manager->priv;
   /* We only allow a launch transition from dbus if a window hasn't been
    * mapped within a short time period - otherwise it is most likely
    * that the dbus signal arrived after the window was mapped. This has
    * had to be extended because under system load from things like flash,
    * dbus is so slow the message will often take >250ms. bug 128009 */
   return !hd_comp_mgr_is_portrait()
     && hd_comp_mgr_time_since_last_map(priv->comp_mgr) > 1000;
 }
