/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* 
 * Copyright (C) 2001, 2002 Havoc Pennington
 * Copyright (C) 2002, 2003 Red Hat Inc.
 * Some ICCCM manager selection code derived from fvwm2,
 * Copyright (C) 2001 Dominik Vogt, Matthias Clasen, and fvwm2 team
 * Copyright (C) 2003 Rob Adams
 * Copyright (C) 2004-2006 Elijah Newren
 * Copyright (C) 2013 Red Hat Inc.
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <clutter/clutter.h>

#include <X11/Xatom.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/dpms.h>

#include <meta/main.h>
#include <meta/errors.h>
#include "monitor-private.h"

#include "edid.h"

#define ALL_WL_TRANSFORMS ((1 << (WL_OUTPUT_TRANSFORM_FLIPPED_270 + 1)) - 1)

/* Look for DPI_FALLBACK in:
 * http://git.gnome.org/browse/gnome-settings-daemon/tree/plugins/xsettings/gsd-xsettings-manager.c
 * for the reasoning */
#define DPI_FALLBACK 96.0

struct _MetaMonitorManagerXrandr
{
  MetaMonitorManager parent_instance;

  Display *xdisplay;
  XRRScreenResources *resources;
  int time;
  int rr_event_base;
  int rr_error_base;
};

struct _MetaMonitorManagerXrandrClass
{
  MetaMonitorManagerClass parent_class;
};

G_DEFINE_TYPE (MetaMonitorManagerXrandr, meta_monitor_manager_xrandr, META_TYPE_MONITOR_MANAGER);

static enum wl_output_transform
wl_transform_from_xrandr (Rotation rotation)
{
  static const enum wl_output_transform y_reflected_map[4] = {
    WL_OUTPUT_TRANSFORM_FLIPPED_180,
    WL_OUTPUT_TRANSFORM_FLIPPED_90,
    WL_OUTPUT_TRANSFORM_FLIPPED,
    WL_OUTPUT_TRANSFORM_FLIPPED_270
  };
  enum wl_output_transform ret;

  switch (rotation & 0x7F)
    {
    default:
    case RR_Rotate_0:
      ret = WL_OUTPUT_TRANSFORM_NORMAL;
      break;
    case RR_Rotate_90:
      ret = WL_OUTPUT_TRANSFORM_90;
      break;
    case RR_Rotate_180:
      ret = WL_OUTPUT_TRANSFORM_180;
      break;
    case RR_Rotate_270:
      ret = WL_OUTPUT_TRANSFORM_270;
      break;
    }

  if (rotation & RR_Reflect_X)
    return ret + 4;
  else if (rotation & RR_Reflect_Y)
    return y_reflected_map[ret];
  else
    return ret;
}

#define ALL_ROTATIONS (RR_Rotate_0 | RR_Rotate_90 | RR_Rotate_180 | RR_Rotate_270)

static unsigned int
wl_transform_from_xrandr_all (Rotation rotation)
{
  unsigned ret;

  /* Handle the common cases first (none or all) */
  if (rotation == 0 || rotation == RR_Rotate_0)
    return (1 << WL_OUTPUT_TRANSFORM_NORMAL);

  /* All rotations and one reflection -> all of them by composition */
  if ((rotation & ALL_ROTATIONS) &&
      ((rotation & RR_Reflect_X) || (rotation & RR_Reflect_Y)))
    return ALL_WL_TRANSFORMS;

  ret = 1 << WL_OUTPUT_TRANSFORM_NORMAL;
  if (rotation & RR_Rotate_90)
    ret |= 1 << WL_OUTPUT_TRANSFORM_90;
  if (rotation & RR_Rotate_180)
    ret |= 1 << WL_OUTPUT_TRANSFORM_180;
  if (rotation & RR_Rotate_270)
    ret |= 1 << WL_OUTPUT_TRANSFORM_270;
  if (rotation & (RR_Rotate_0 | RR_Reflect_X))
    ret |= 1 << WL_OUTPUT_TRANSFORM_FLIPPED;
  if (rotation & (RR_Rotate_90 | RR_Reflect_X))
    ret |= 1 << WL_OUTPUT_TRANSFORM_FLIPPED_90;
  if (rotation & (RR_Rotate_180 | RR_Reflect_X))
    ret |= 1 << WL_OUTPUT_TRANSFORM_FLIPPED_180;
  if (rotation & (RR_Rotate_270 | RR_Reflect_X))
    ret |= 1 << WL_OUTPUT_TRANSFORM_FLIPPED_270;

  return ret;
}

static gboolean
output_get_presentation_xrandr (MetaMonitorManagerXrandr *manager_xrandr,
                                MetaOutput               *output)
{
  MetaDisplay *display = meta_get_display ();
  gboolean value;
  Atom actual_type;
  int actual_format;
  unsigned long nitems, bytes_after;
  unsigned char *buffer;

  XRRGetOutputProperty (manager_xrandr->xdisplay,
                        (XID)output->output_id,
                        display->atom__MUTTER_PRESENTATION_OUTPUT,
                        0, G_MAXLONG, False, False, XA_CARDINAL,
                        &actual_type, &actual_format,
                        &nitems, &bytes_after, &buffer);

  if (actual_type != XA_CARDINAL || actual_format != 32 ||
      nitems < 1)
    return FALSE;

  value = ((int*)buffer)[0];

  XFree (buffer);
  return value;
}

static int
normalize_backlight (MetaOutput *output,
                     int         hw_value)
{
  return round ((double)(hw_value - output->backlight_min) /
                (output->backlight_max - output->backlight_min) * 100.0);
}

static int
output_get_backlight_xrandr (MetaMonitorManagerXrandr *manager_xrandr,
                             MetaOutput               *output)
{
  MetaDisplay *display = meta_get_display ();
  gboolean value;
  Atom actual_type;
  int actual_format;
  unsigned long nitems, bytes_after;
  unsigned char *buffer;

  XRRGetOutputProperty (manager_xrandr->xdisplay,
                        (XID)output->output_id,
                        display->atom_Backlight,
                        0, G_MAXLONG, False, False, XA_INTEGER,
                        &actual_type, &actual_format,
                        &nitems, &bytes_after, &buffer);

  if (actual_type != XA_INTEGER || actual_format != 32 ||
      nitems < 1)
    return -1;

  value = ((int*)buffer)[0];

  XFree (buffer);
  return normalize_backlight (output, value);
}

static void
output_get_backlight_limits_xrandr (MetaMonitorManagerXrandr *manager_xrandr,
                                    MetaOutput               *output)
{
  MetaDisplay *display = meta_get_display ();
  XRRPropertyInfo *info;

  meta_error_trap_push (display);
  info = XRRQueryOutputProperty (manager_xrandr->xdisplay,
                                 (XID)output->output_id,
                                 display->atom_Backlight);
  meta_error_trap_pop (display);

  if (info == NULL)
    {
      meta_verbose ("could not get output property for %s\n", output->name);
      return;
    }

  if (!info->range || info->num_values != 2)
    {
      meta_verbose ("backlight %s was not range\n", output->name);
      goto out;
    }

  output->backlight_min = info->values[0];
  output->backlight_max = info->values[1];

out:
  XFree (info);
}

static int
compare_outputs (const void *one,
                 const void *two)
{
  const MetaOutput *o_one = one, *o_two = two;

  return strcmp (o_one->name, o_two->name);
}

static guint8 *
get_edid_property (Display  *dpy,
                   RROutput  output,
                   Atom      atom,
                   gsize    *len)
{
  unsigned char *prop;
  int actual_format;
  unsigned long nitems, bytes_after;
  Atom actual_type;
  guint8 *result;

  XRRGetOutputProperty (dpy, output, atom,
                        0, 100, False, False,
                        AnyPropertyType,
                        &actual_type, &actual_format,
                        &nitems, &bytes_after, &prop);

  if (actual_type == XA_INTEGER && actual_format == 8)
    {
      result = g_memdup (prop, nitems);
      if (len)
        *len = nitems;
    }
  else
    {
      result = NULL;
    }

  XFree (prop);
    
  return result;
}

static GBytes *
read_output_edid (MetaMonitorManagerXrandr *manager_xrandr,
                  XID                       output_id)
{
  Atom edid_atom;
  guint8 *result;
  gsize len;

  edid_atom = XInternAtom (manager_xrandr->xdisplay, "EDID", FALSE);
  result = get_edid_property (manager_xrandr->xdisplay, output_id, edid_atom, &len);

  if (!result)
    {
      edid_atom = XInternAtom (manager_xrandr->xdisplay, "EDID_DATA", FALSE);
      result = get_edid_property (manager_xrandr->xdisplay, output_id, edid_atom, &len);
    }

  if (!result)
    {
      edid_atom = XInternAtom (manager_xrandr->xdisplay, "XFree86_DDC_EDID1_RAWDATA", FALSE);
      result = get_edid_property (manager_xrandr->xdisplay, output_id, edid_atom, &len);
    }

  if (result)
    {
      if (len > 0 && len % 128 == 0)
        return g_bytes_new_take (result, len);
      else
        g_free (result);
    }

  return NULL;
}

static gboolean
output_get_hotplug_mode_update (MetaMonitorManagerXrandr *manager_xrandr,
                                XID                       output_id)
{
  MetaDisplay *display = meta_get_display ();
  XRRPropertyInfo *info;
  gboolean result = FALSE;

  meta_error_trap_push (display);
  info = XRRQueryOutputProperty (manager_xrandr->xdisplay, output_id,
                                 display->atom_hotplug_mode_update);
  meta_error_trap_pop (display);

  if (info)
    {
      result = TRUE;
      XFree (info);
    }

  return result;
}


static void
meta_monitor_manager_xrandr_read_current (MetaMonitorManager *manager)
{
  MetaMonitorManagerXrandr *manager_xrandr = META_MONITOR_MANAGER_XRANDR (manager);
  XRRScreenResources *resources;
  RROutput primary_output;
  unsigned int i, j, k;
  unsigned int n_actual_outputs;
  int min_width, min_height;
  Screen *screen;
  BOOL dpms_capable, dpms_enabled;
  CARD16 dpms_state;

  if (manager_xrandr->resources)
    XRRFreeScreenResources (manager_xrandr->resources);
  manager_xrandr->resources = NULL;

  meta_error_trap_push (meta_get_display ());
  dpms_capable = DPMSCapable (manager_xrandr->xdisplay);
  meta_error_trap_pop (meta_get_display ());

  if (dpms_capable &&
      DPMSInfo (manager_xrandr->xdisplay, &dpms_state, &dpms_enabled) &&
      dpms_enabled)
    {
      switch (dpms_state)
        {
        case DPMSModeOn:
          manager->power_save_mode = META_POWER_SAVE_ON;
          break;
        case DPMSModeStandby:
          manager->power_save_mode = META_POWER_SAVE_STANDBY;
          break;
        case DPMSModeSuspend:
          manager->power_save_mode = META_POWER_SAVE_SUSPEND;
          break;
        case DPMSModeOff:
          manager->power_save_mode = META_POWER_SAVE_OFF;
          break;
        default:
          manager->power_save_mode = META_POWER_SAVE_UNSUPPORTED;
          break;
        }
    }
  else
    {
      manager->power_save_mode = META_POWER_SAVE_UNSUPPORTED;
    }

  XRRGetScreenSizeRange (manager_xrandr->xdisplay, DefaultRootWindow (manager_xrandr->xdisplay),
			 &min_width,
			 &min_height,
			 &manager->max_screen_width,
			 &manager->max_screen_height);

  screen = ScreenOfDisplay (manager_xrandr->xdisplay,
			    DefaultScreen (manager_xrandr->xdisplay));
  /* This is updated because we called RRUpdateConfiguration below */
  manager->screen_width = WidthOfScreen (screen);
  manager->screen_height = HeightOfScreen (screen);

  resources = XRRGetScreenResourcesCurrent (manager_xrandr->xdisplay,
					    DefaultRootWindow (manager_xrandr->xdisplay));
  if (!resources)
    return;

  manager_xrandr->resources = resources;
  manager_xrandr->time = resources->configTimestamp;
  manager->n_outputs = resources->noutput;
  manager->n_crtcs = resources->ncrtc;
  manager->n_modes = resources->nmode;
  manager->outputs = g_new0 (MetaOutput, manager->n_outputs);
  manager->modes = g_new0 (MetaMonitorMode, manager->n_modes);
  manager->crtcs = g_new0 (MetaCRTC, manager->n_crtcs);

  for (i = 0; i < (unsigned)resources->nmode; i++)
    {
      XRRModeInfo *xmode = &resources->modes[i];
      MetaMonitorMode *mode;

      mode = &manager->modes[i];

      mode->mode_id = xmode->id;
      mode->width = xmode->width;
      mode->height = xmode->height;
      mode->refresh_rate = (xmode->dotClock /
			    ((float)xmode->hTotal * xmode->vTotal));
    }

  for (i = 0; i < (unsigned)resources->ncrtc; i++)
    {
      XRRCrtcInfo *crtc;
      MetaCRTC *meta_crtc;

      crtc = XRRGetCrtcInfo (manager_xrandr->xdisplay, resources, resources->crtcs[i]);

      meta_crtc = &manager->crtcs[i];

      meta_crtc->crtc_id = resources->crtcs[i];
      meta_crtc->rect.x = crtc->x;
      meta_crtc->rect.y = crtc->y;
      meta_crtc->rect.width = crtc->width;
      meta_crtc->rect.height = crtc->height;
      meta_crtc->is_dirty = FALSE;
      meta_crtc->transform = wl_transform_from_xrandr (crtc->rotation);
      meta_crtc->all_transforms = wl_transform_from_xrandr_all (crtc->rotations);

      for (j = 0; j < (unsigned)resources->nmode; j++)
	{
	  if (resources->modes[j].id == crtc->mode)
	    {
	      meta_crtc->current_mode = &manager->modes[j];
	      break;
	    }
	}

      XRRFreeCrtcInfo (crtc);
    }

  meta_error_trap_push (meta_get_display ());
  primary_output = XRRGetOutputPrimary (manager_xrandr->xdisplay,
					DefaultRootWindow (manager_xrandr->xdisplay));
  meta_error_trap_pop (meta_get_display ());

  n_actual_outputs = 0;
  for (i = 0; i < (unsigned)resources->noutput; i++)
    {
      XRROutputInfo *output;
      MetaOutput *meta_output;

      output = XRRGetOutputInfo (manager_xrandr->xdisplay, resources, resources->outputs[i]);

      meta_output = &manager->outputs[n_actual_outputs];

      if (output->connection != RR_Disconnected)
	{
          GBytes *edid;
          MonitorInfo *parsed_edid;

	  meta_output->output_id = resources->outputs[i];
	  meta_output->name = g_strdup (output->name);

          edid = read_output_edid (manager_xrandr, meta_output->output_id);
          if (edid)
            {
              gsize len;

              parsed_edid = decode_edid (g_bytes_get_data (edid, &len));
              if (parsed_edid)
                {
                  meta_output->vendor = g_strndup (parsed_edid->manufacturer_code, 4);
                  if (parsed_edid->dsc_product_name[0])
                    meta_output->product = g_strndup (parsed_edid->dsc_product_name, 14);
                  else
                    meta_output->product = g_strdup_printf ("0x%04x", (unsigned)parsed_edid->product_code);
                  if (parsed_edid->dsc_serial_number[0])
                    meta_output->serial = g_strndup (parsed_edid->dsc_serial_number, 14);
                  else
                    meta_output->serial = g_strdup_printf ("0x%08x", parsed_edid->serial_number);

                  g_free (parsed_edid);
                }

              g_bytes_unref (edid);
            }

          if (!meta_output->vendor)
            {
              meta_output->vendor = g_strdup ("unknown");
              meta_output->product = g_strdup ("unknown");
              meta_output->serial = g_strdup ("unknown");
            }
	  meta_output->width_mm = output->mm_width;
	  meta_output->height_mm = output->mm_height;
	  meta_output->subpixel_order = COGL_SUBPIXEL_ORDER_UNKNOWN;
          meta_output->hotplug_mode_update =
              output_get_hotplug_mode_update (manager_xrandr, meta_output->output_id);

	  meta_output->n_modes = output->nmode;
	  meta_output->modes = g_new0 (MetaMonitorMode *, meta_output->n_modes);
	  for (j = 0; j < meta_output->n_modes; j++)
	    {
	      for (k = 0; k < manager->n_modes; k++)
		{
		  if (output->modes[j] == (XID)manager->modes[k].mode_id)
		    {
		      meta_output->modes[j] = &manager->modes[k];
		      break;
		    }
		}
	    }
	  meta_output->preferred_mode = meta_output->modes[0];

	  meta_output->n_possible_crtcs = output->ncrtc;
	  meta_output->possible_crtcs = g_new0 (MetaCRTC *, meta_output->n_possible_crtcs);
	  for (j = 0; j < (unsigned)output->ncrtc; j++)
	    {
	      for (k = 0; k < manager->n_crtcs; k++)
		{
		  if ((XID)manager->crtcs[k].crtc_id == output->crtcs[j])
		    {
		      meta_output->possible_crtcs[j] = &manager->crtcs[k];
		      break;
		    }
		}
	    }

	  meta_output->crtc = NULL;
	  for (j = 0; j < manager->n_crtcs; j++)
	    {
	      if ((XID)manager->crtcs[j].crtc_id == output->crtc)
		{
		  meta_output->crtc = &manager->crtcs[j];
		  break;
		}
	    }

	  meta_output->n_possible_clones = output->nclone;
	  meta_output->possible_clones = g_new0 (MetaOutput *, meta_output->n_possible_clones);
	  /* We can build the list of clones now, because we don't have the list of outputs
	     yet, so temporarily set the pointers to the bare XIDs, and then we'll fix them
	     in a second pass
	  */
	  for (j = 0; j < (unsigned)output->nclone; j++)
	    {
	      meta_output->possible_clones[j] = GINT_TO_POINTER (output->clones[j]);
	    }

	  meta_output->is_primary = ((XID)meta_output->output_id == primary_output);
	  meta_output->is_presentation = output_get_presentation_xrandr (manager_xrandr, meta_output);
	  output_get_backlight_limits_xrandr (manager_xrandr, meta_output);

	  if (!(meta_output->backlight_min == 0 && meta_output->backlight_max == 0))
	    meta_output->backlight = output_get_backlight_xrandr (manager_xrandr, meta_output);
	  else
	    meta_output->backlight = -1;

	  n_actual_outputs++;
	}

      XRRFreeOutputInfo (output);
    }

  manager->n_outputs = n_actual_outputs;

  /* Sort the outputs for easier handling in MetaMonitorConfig */
  qsort (manager->outputs, manager->n_outputs, sizeof (MetaOutput), compare_outputs);

  /* Now fix the clones */
  for (i = 0; i < manager->n_outputs; i++)
    {
      MetaOutput *meta_output;

      meta_output = &manager->outputs[i];

      for (j = 0; j < meta_output->n_possible_clones; j++)
	{
	  RROutput clone = GPOINTER_TO_INT (meta_output->possible_clones[j]);

	  for (k = 0; k < manager->n_outputs; k++)
	    {
	      if (clone == (XID)manager->outputs[k].output_id)
		{
		  meta_output->possible_clones[j] = &manager->outputs[k];
		  break;
		}
	    }
	}
    }
}

static GBytes *
meta_monitor_manager_xrandr_read_edid (MetaMonitorManager *manager,
                                       MetaOutput         *output)
{
  MetaMonitorManagerXrandr *manager_xrandr = META_MONITOR_MANAGER_XRANDR (manager);

  return read_output_edid (manager_xrandr, output->output_id);
}

static void
meta_monitor_manager_xrandr_set_power_save_mode (MetaMonitorManager *manager,
						 MetaPowerSave       mode)
{
  MetaMonitorManagerXrandr *manager_xrandr = META_MONITOR_MANAGER_XRANDR (manager);
  CARD16 state;

  switch (mode) {
  case META_POWER_SAVE_ON:
    state = DPMSModeOn;
    break;
  case META_POWER_SAVE_STANDBY:
    state = DPMSModeStandby;
    break;
  case META_POWER_SAVE_SUSPEND:
    state = DPMSModeSuspend;
    break;
  case META_POWER_SAVE_OFF:
    state = DPMSModeOff;
    break;
  default:
    return;
  }

  meta_error_trap_push (meta_get_display ());
  DPMSForceLevel (manager_xrandr->xdisplay, state);
  DPMSSetTimeouts (manager_xrandr->xdisplay, 0, 0, 0);
  meta_error_trap_pop (meta_get_display ());
}

static Rotation
wl_transform_to_xrandr (enum wl_output_transform transform)
{
  switch (transform)
    {
    case WL_OUTPUT_TRANSFORM_NORMAL:
      return RR_Rotate_0;
    case WL_OUTPUT_TRANSFORM_90:
      return RR_Rotate_90;
    case WL_OUTPUT_TRANSFORM_180:
      return RR_Rotate_180;
    case WL_OUTPUT_TRANSFORM_270:
      return RR_Rotate_270;
    case WL_OUTPUT_TRANSFORM_FLIPPED:
      return RR_Reflect_X | RR_Rotate_0;
    case WL_OUTPUT_TRANSFORM_FLIPPED_90:
      return RR_Reflect_X | RR_Rotate_90;
    case WL_OUTPUT_TRANSFORM_FLIPPED_180:
      return RR_Reflect_X | RR_Rotate_180;
    case WL_OUTPUT_TRANSFORM_FLIPPED_270:
      return RR_Reflect_X | RR_Rotate_270;
    }

  g_assert_not_reached ();
}

static void
output_set_presentation_xrandr (MetaMonitorManagerXrandr *manager_xrandr,
                                MetaOutput               *output,
                                gboolean                  presentation)
{
  MetaDisplay *display = meta_get_display ();
  int value = presentation;

  meta_error_trap_push (display);
  XRRChangeOutputProperty (manager_xrandr->xdisplay,
                           (XID)output->output_id,
                           display->atom__MUTTER_PRESENTATION_OUTPUT,
                           XA_CARDINAL, 32, PropModeReplace,
                           (unsigned char*) &value, 1);
  meta_error_trap_pop (display);
}

static void
meta_monitor_manager_xrandr_apply_configuration (MetaMonitorManager *manager,
						 MetaCRTCInfo       **crtcs,
						 unsigned int         n_crtcs,
						 MetaOutputInfo     **outputs,
						 unsigned int         n_outputs)
{
  MetaMonitorManagerXrandr *manager_xrandr = META_MONITOR_MANAGER_XRANDR (manager);
  MetaDisplay *display = meta_get_display ();
  unsigned i;
  int width, height, width_mm, height_mm;

  meta_display_grab (display);

  /* First compute the new size of the screen (framebuffer) */
  width = 0; height = 0;
  for (i = 0; i < n_crtcs; i++)
    {
      MetaCRTCInfo *crtc_info = crtcs[i];
      MetaCRTC *crtc = crtc_info->crtc;
      crtc->is_dirty = TRUE;

      if (crtc_info->mode == NULL)
        continue;

      if (meta_monitor_transform_is_rotated (crtc_info->transform))
        {
          width = MAX (width, crtc_info->x + crtc_info->mode->height);
          height = MAX (height, crtc_info->y + crtc_info->mode->width);
        }
      else
        {
          width = MAX (width, crtc_info->x + crtc_info->mode->width);
          height = MAX (height, crtc_info->y + crtc_info->mode->height);
        }
    }

  /* Second disable all newly disabled CRTCs, or CRTCs that in the previous
     configuration would be outside the new framebuffer (otherwise X complains
     loudly when resizing)
     CRTC will be enabled again after resizing the FB
  */
  for (i = 0; i < n_crtcs; i++)
    {
      MetaCRTCInfo *crtc_info = crtcs[i];
      MetaCRTC *crtc = crtc_info->crtc;

      if (crtc_info->mode == NULL ||
          crtc->rect.x + crtc->rect.width > width ||
          crtc->rect.y + crtc->rect.height > height)
        {
          XRRSetCrtcConfig (manager_xrandr->xdisplay,
                            manager_xrandr->resources,
                            (XID)crtc->crtc_id,
                            manager_xrandr->time,
                            0, 0,
                            None,
                            RR_Rotate_0,
                            NULL, 0);

          crtc->rect.x = 0;
          crtc->rect.y = 0;
          crtc->rect.width = 0;
          crtc->rect.height = 0;
          crtc->current_mode = NULL;
        }
    }

  /* Disable CRTCs not mentioned in the list */
  for (i = 0; i < manager->n_crtcs; i++)
    {
      MetaCRTC *crtc = &manager->crtcs[i];

      if (crtc->is_dirty)
        {
          crtc->is_dirty = FALSE;
          continue;
        }
      if (crtc->current_mode == NULL)
        continue;

      XRRSetCrtcConfig (manager_xrandr->xdisplay,
                        manager_xrandr->resources,
                        (XID)crtc->crtc_id,
                        manager_xrandr->time,
                        0, 0,
                        None,
                        RR_Rotate_0,
                        NULL, 0);

      crtc->rect.x = 0;
      crtc->rect.y = 0;
      crtc->rect.width = 0;
      crtc->rect.height = 0;
      crtc->current_mode = NULL;
    }

  g_assert (width > 0 && height > 0);
  /* The 'physical size' of an X screen is meaningless if that screen
   * can consist of many monitors. So just pick a size that make the
   * dpi 96.
   *
   * Firefox and Evince apparently believe what X tells them.
   */
  width_mm = (width / DPI_FALLBACK) * 25.4 + 0.5;
  height_mm = (height / DPI_FALLBACK) * 25.4 + 0.5;
  meta_error_trap_push (display);
  XRRSetScreenSize (manager_xrandr->xdisplay, DefaultRootWindow (manager_xrandr->xdisplay),
                    width, height, width_mm, height_mm);
  meta_error_trap_pop (display);

  for (i = 0; i < n_crtcs; i++)
    {
      MetaCRTCInfo *crtc_info = crtcs[i];
      MetaCRTC *crtc = crtc_info->crtc;

      if (crtc_info->mode != NULL)
        {
          MetaMonitorMode *mode;
          XID *outputs;
          unsigned int j, n_outputs;
          int width, height;
          Status ok;
          unsigned long old_controlled_mask;
          unsigned long new_controlled_mask;

          mode = crtc_info->mode;

          n_outputs = crtc_info->outputs->len;
          outputs = g_new (XID, n_outputs);

          old_controlled_mask = 0;
          for (j = 0; j < manager->n_outputs; j++)
            {
              MetaOutput *output;

              output = &manager->outputs[j];

              if (output->crtc == crtc)
                old_controlled_mask |= 1UL << j;
            }

          new_controlled_mask = 0;
          for (j = 0; j < n_outputs; j++)
            {
              MetaOutput *output;

              output = ((MetaOutput**)crtc_info->outputs->pdata)[j];

              output->is_dirty = TRUE;
              output->crtc = crtc;
              new_controlled_mask |= 1UL << j;

              outputs[j] = output->output_id;
            }

          if (crtc->current_mode == mode &&
              crtc->rect.x == crtc_info->x &&
              crtc->rect.y == crtc_info->y &&
              crtc->transform == crtc_info->transform &&
              old_controlled_mask == new_controlled_mask)
            {
              /* No change */
              goto next;
            }

          meta_error_trap_push (display);
          ok = XRRSetCrtcConfig (manager_xrandr->xdisplay,
                                 manager_xrandr->resources,
                                 (XID)crtc->crtc_id,
                                 manager_xrandr->time,
                                 crtc_info->x, crtc_info->y,
                                 (XID)mode->mode_id,
                                 wl_transform_to_xrandr (crtc_info->transform),
                                 outputs, n_outputs);
          meta_error_trap_pop (display);

          if (ok != Success)
            {
              meta_warning ("Configuring CRTC %d with mode %d (%d x %d @ %f) at position %d, %d and transfrom %u failed\n",
                            (unsigned)(crtc->crtc_id), (unsigned)(mode->mode_id),
                            mode->width, mode->height, (float)mode->refresh_rate,
                            crtc_info->x, crtc_info->y, crtc_info->transform);
              goto next;
            }

          if (meta_monitor_transform_is_rotated (crtc_info->transform))
            {
              width = mode->height;
              height = mode->width;
            }
          else
            {
              width = mode->width;
              height = mode->height;
            }

          crtc->rect.x = crtc_info->x;
          crtc->rect.y = crtc_info->y;
          crtc->rect.width = width;
          crtc->rect.height = height;
          crtc->current_mode = mode;
          crtc->transform = crtc_info->transform;

        next:
          g_free (outputs);
        }
    }

  for (i = 0; i < n_outputs; i++)
    {
      MetaOutputInfo *output_info = outputs[i];
      MetaOutput *output = output_info->output;

      if (output_info->is_primary)
        {
          meta_error_trap_push (display);
          XRRSetOutputPrimary (manager_xrandr->xdisplay,
                               DefaultRootWindow (manager_xrandr->xdisplay),
                               (XID)output_info->output->output_id);
          meta_error_trap_pop (display);
        }

      output_set_presentation_xrandr (manager_xrandr,
                                      output_info->output,
                                      output_info->is_presentation);

      output->is_primary = output_info->is_primary;
      output->is_presentation = output_info->is_presentation;
    }

  /* Disable outputs not mentioned in the list */
  for (i = 0; i < manager->n_outputs; i++)
    {
      MetaOutput *output = &manager->outputs[i];

      if (output->is_dirty)
        {
          output->is_dirty = FALSE;
          continue;
        }

      output->crtc = NULL;
      output->is_primary = FALSE;
    }

  meta_display_ungrab (display);
}

static void
meta_monitor_manager_xrandr_change_backlight (MetaMonitorManager *manager,
					      MetaOutput         *output,
					      gint                value)
{
  MetaMonitorManagerXrandr *manager_xrandr = META_MONITOR_MANAGER_XRANDR (manager);
  MetaDisplay *display = meta_get_display ();
  int hw_value;

  hw_value = round ((double)value / 100.0 * output->backlight_max + output->backlight_min);

  meta_error_trap_push (display);
  XRRChangeOutputProperty (manager_xrandr->xdisplay,
                           (XID)output->output_id,
                           display->atom_Backlight,
                           XA_INTEGER, 32, PropModeReplace,
                           (unsigned char *) &hw_value, 1);
  meta_error_trap_pop (display);

  /* We're not selecting for property notifies, so update the value immediately */
  output->backlight = normalize_backlight (output, hw_value);
}

static void
meta_monitor_manager_xrandr_get_crtc_gamma (MetaMonitorManager  *manager,
					    MetaCRTC            *crtc,
					    gsize               *size,
					    unsigned short     **red,
					    unsigned short     **green,
					    unsigned short     **blue)
{
  MetaMonitorManagerXrandr *manager_xrandr = META_MONITOR_MANAGER_XRANDR (manager);
  XRRCrtcGamma *gamma;

  gamma = XRRGetCrtcGamma (manager_xrandr->xdisplay, (XID)crtc->crtc_id);

  *size = gamma->size;
  *red = g_memdup (gamma->red, sizeof (unsigned short) * gamma->size);
  *green = g_memdup (gamma->green, sizeof (unsigned short) * gamma->size);
  *blue = g_memdup (gamma->blue, sizeof (unsigned short) * gamma->size);

  XRRFreeGamma (gamma);
}

static void
meta_monitor_manager_xrandr_set_crtc_gamma (MetaMonitorManager *manager,
					    MetaCRTC           *crtc,
					    gsize               size,
					    unsigned short     *red,
					    unsigned short     *green,
					    unsigned short     *blue)
{
  MetaMonitorManagerXrandr *manager_xrandr = META_MONITOR_MANAGER_XRANDR (manager);
  XRRCrtcGamma *gamma;

  gamma = XRRAllocGamma (size);
  memcpy (gamma->red, red, sizeof (unsigned short) * size);
  memcpy (gamma->green, green, sizeof (unsigned short) * size);
  memcpy (gamma->blue, blue, sizeof (unsigned short) * size);

  XRRSetCrtcGamma (manager_xrandr->xdisplay, (XID)crtc->crtc_id, gamma);

  XRRFreeGamma (gamma);
}

static void
meta_monitor_manager_xrandr_rebuild_derived (MetaMonitorManager *manager)
{
  /* This will be a no-op if the change was from our side, as
     we already called it in the DBus method handler */
  meta_monitor_config_update_current (manager->config, manager);

  meta_monitor_manager_rebuild_derived (manager);
}

static gboolean
meta_monitor_manager_xrandr_handle_xevent (MetaMonitorManager *manager,
					   XEvent             *event)
{
  MetaMonitorManagerXrandr *manager_xrandr = META_MONITOR_MANAGER_XRANDR (manager);
  MetaOutput *old_outputs;
  MetaCRTC *old_crtcs;
  MetaMonitorMode *old_modes;
  int n_old_outputs;
  gboolean new_config;

  if ((event->type - manager_xrandr->rr_event_base) != RRScreenChangeNotify)
    return FALSE;

  XRRUpdateConfiguration (event);

  /* Save the old structures, so they stay valid during the update */
  old_outputs = manager->outputs;
  n_old_outputs = manager->n_outputs;
  old_modes = manager->modes;
  old_crtcs = manager->crtcs;

  manager->serial++;
  meta_monitor_manager_xrandr_read_current (manager);

  new_config = manager_xrandr->resources->timestamp >=
    manager_xrandr->resources->configTimestamp;
  if (meta_monitor_manager_has_hotplug_mode_update (manager))

    {
      /* Check if the current intended configuration is a result of an
         XRandR call.  Otherwise, hotplug_mode_update tells us to get
         a new preferred mode on hotplug events to handle dynamic
         guest resizing. */
      if (new_config)
        meta_monitor_manager_xrandr_rebuild_derived (manager);
      else
        meta_monitor_config_make_default (manager->config, manager);
    }
  else
    {
      /* Check if the current intended configuration has the same outputs
         as the new real one, or if the event is a result of an XRandR call.
         If so, we can go straight to rebuild the logical config and tell
         the outside world.
         Otherwise, this event was caused by hotplug, so give a chance to
         MetaMonitorConfig.

         Note that we need to check both the timestamps and the list of
         outputs, because the X server might emit spurious events with new
         configTimestamps (bug 702804), and the driver may have changed
         the EDID for some other reason (old qxl and vbox drivers). */
      if (new_config || meta_monitor_config_match_current (manager->config, manager))
        meta_monitor_manager_xrandr_rebuild_derived (manager);
      else if (!meta_monitor_config_apply_stored (manager->config, manager))
        meta_monitor_config_make_default (manager->config, manager);
    }

  meta_monitor_manager_free_output_array (old_outputs, n_old_outputs);
  g_free (old_modes);
  g_free (old_crtcs);

  return TRUE;
}

static void
meta_monitor_manager_xrandr_init (MetaMonitorManagerXrandr *manager_xrandr)
{
  MetaDisplay *display = meta_get_display ();

  manager_xrandr->xdisplay = display->xdisplay;

  if (!XRRQueryExtension (manager_xrandr->xdisplay,
			  &manager_xrandr->rr_event_base,
			  &manager_xrandr->rr_error_base))
    {
      return;
    }
  else
    {
      /* We only use ScreenChangeNotify, but GDK uses the others,
	 and we don't want to step on its toes */
      XRRSelectInput (manager_xrandr->xdisplay,
		      DefaultRootWindow (manager_xrandr->xdisplay),
		      RRScreenChangeNotifyMask
		      | RRCrtcChangeNotifyMask
		      | RROutputPropertyNotifyMask);
    }
}

static void
meta_monitor_manager_xrandr_finalize (GObject *object)
{
  MetaMonitorManagerXrandr *manager_xrandr = META_MONITOR_MANAGER_XRANDR (object);

  if (manager_xrandr->resources)
    XRRFreeScreenResources (manager_xrandr->resources);
  manager_xrandr->resources = NULL;

  G_OBJECT_CLASS (meta_monitor_manager_xrandr_parent_class)->finalize (object);
}

static void
meta_monitor_manager_xrandr_class_init (MetaMonitorManagerXrandrClass *klass)
{
  MetaMonitorManagerClass *manager_class = META_MONITOR_MANAGER_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = meta_monitor_manager_xrandr_finalize;

  manager_class->read_current = meta_monitor_manager_xrandr_read_current;
  manager_class->read_edid = meta_monitor_manager_xrandr_read_edid;
  manager_class->apply_configuration = meta_monitor_manager_xrandr_apply_configuration;
  manager_class->set_power_save_mode = meta_monitor_manager_xrandr_set_power_save_mode;
  manager_class->change_backlight = meta_monitor_manager_xrandr_change_backlight;
  manager_class->get_crtc_gamma = meta_monitor_manager_xrandr_get_crtc_gamma;
  manager_class->set_crtc_gamma = meta_monitor_manager_xrandr_set_crtc_gamma;
  manager_class->handle_xevent = meta_monitor_manager_xrandr_handle_xevent;
}

