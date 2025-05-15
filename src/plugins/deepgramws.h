#ifndef __DEEPGRAM_WS_H__
#define __DEEPGRAM_WS_H__

#include <glib-object.h>

G_BEGIN_DECLS

#define DEEPGRAM_TYPE_WS (deepgram_ws_get_type())
G_DECLARE_FINAL_TYPE (DeepgramWS, deepgram_ws, DEEPGRAM, WS, GObject)

DeepgramWS * deepgram_ws_new(void);

gboolean deepgram_ws_start(DeepgramWS *self);

void deepgram_ws_stop(DeepgramWS *self);

void deepgram_ws_push_audio(DeepgramWS *self, const guint8 *data, gsize size);

enum {
  PROP_WS_API_KEY = 1,
  PROP_WS_MODEL,
  PROP_WS_SILENT,
};

enum {
  SIGNAL_WS_TRANSCRIPT,
  N_WS_SIGNALS
};

G_END_DECLS

#endif /* __DEEPGRAM_WS_H__ */
