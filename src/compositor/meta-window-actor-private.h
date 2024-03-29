/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#ifndef META_WINDOW_ACTOR_PRIVATE_H
#define META_WINDOW_ACTOR_PRIVATE_H

#include <config.h>

#include <X11/extensions/Xdamage.h>
#include <meta/compositor-mutter.h>

MetaWindowActor *meta_window_actor_new (MetaWindow *window);

void meta_window_actor_destroy   (MetaWindowActor *self);

void meta_window_actor_show (MetaWindowActor *self,
                             MetaCompEffect   effect);
void meta_window_actor_hide (MetaWindowActor *self,
                             MetaCompEffect   effect);

void meta_window_actor_maximize   (MetaWindowActor *self,
                                   MetaRectangle   *old_rect,
                                   MetaRectangle   *new_rect);
void meta_window_actor_unmaximize (MetaWindowActor *self,
                                   MetaRectangle   *old_rect,
                                   MetaRectangle   *new_rect);

void meta_window_actor_process_damage (MetaWindowActor    *self,
                                       XDamageNotifyEvent *event);

void meta_window_actor_pre_paint      (MetaWindowActor    *self);
void meta_window_actor_post_paint     (MetaWindowActor    *self);
void meta_window_actor_frame_complete (MetaWindowActor    *self,
                                       CoglFrameInfo      *frame_info,
                                       gint64              presentation_time);

void meta_window_actor_invalidate_shadow (MetaWindowActor *self);

void meta_window_actor_get_shape_bounds (MetaWindowActor       *self,
                                          cairo_rectangle_int_t *bounds);

gboolean meta_window_actor_should_unredirect   (MetaWindowActor *self);
void     meta_window_actor_set_unredirected    (MetaWindowActor *self,
                                                gboolean         unredirected);

gboolean meta_window_actor_effect_in_progress  (MetaWindowActor *self);
void     meta_window_actor_sync_actor_geometry (MetaWindowActor *self,
                                                gboolean         did_placement);
void     meta_window_actor_sync_visibility     (MetaWindowActor *self);
void     meta_window_actor_update_shape        (MetaWindowActor *self);
void     meta_window_actor_update_opacity      (MetaWindowActor *self);
void     meta_window_actor_mapped              (MetaWindowActor *self);
void     meta_window_actor_unmapped            (MetaWindowActor *self);
void     meta_window_actor_set_updates_frozen  (MetaWindowActor *self,
                                                gboolean         updates_frozen);
void     meta_window_actor_queue_frame_drawn   (MetaWindowActor *self,
                                                gboolean         no_delay_frame);

void meta_window_actor_effect_completed (MetaWindowActor *actor,
                                         gulong           event);

#endif /* META_WINDOW_ACTOR_PRIVATE_H */
