#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib.h>
#include <gst/base/gstbasesink.h>
#include <gst/gst.h>

#include "deepgramws.h"

GST_DEBUG_CATEGORY_STATIC (gst_deepgram_sink_debug);
#define GST_CAT_DEFAULT gst_deepgram_sink_debug

#define GST_TYPE_DEEPGRAM_SINK (gst_deepgram_sink_get_type ())
G_DECLARE_FINAL_TYPE (GstDeepgramSink, gst_deepgram_sink, GST, DEEPGRAM_SINK,
                      GstBaseSink)

struct _GstDeepgramSink
{
  GstBaseSink parent;
  gchar*      api_key;
  gchar*      model;
  gboolean    silent;
  DeepgramWS* ws;
};

G_DEFINE_TYPE (GstDeepgramSink, gst_deepgram_sink, GST_TYPE_BASE_SINK)

enum
{
  PROP_0,
  PROP_API_KEY,
  PROP_MODEL,
  PROP_SILENT
};

enum
{
  SIGNAL_TRANSCRIPT,
  SIGNAL_WORD,
  N_SIGNALS
};

static guint gst_deepgram_sink_signals[N_SIGNALS] = { 0 };

static GstStaticPadTemplate sink_template
    = GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
                               GST_STATIC_CAPS ("audio/x-raw, "
                                                "format = (string) S16LE, "
                                                "rate = (int) 16000, "
                                                "channels = (int) 1"));

static void     gst_deepgram_sink_set_property (GObject* object, guint prop_id,
                                                const GValue* value,
                                                GParamSpec*   pspec);
static void     gst_deepgram_sink_get_property (GObject* object, guint prop_id,
                                                GValue* value, GParamSpec* pspec);
static gboolean gst_deepgram_sink_start (GstBaseSink* basesink);
static gboolean gst_deepgram_sink_stop (GstBaseSink* basesink);
static GstFlowReturn gst_deepgram_sink_render (GstBaseSink* basesink,
                                               GstBuffer*   buffer);
static void
gst_deepgram_sink_on_deepgram_transcript (DeepgramWS* ws, const gchar* text,
                                          gboolean is_final, gdouble start_time,
                                          gdouble end_time, gpointer user_data);

static void gst_deepgram_sink_on_deepgram_word (DeepgramWS*  ws,
                                                const gchar* word,
                                                gdouble      start_time,
                                                gdouble      end_time,
                                                gpointer     user_data);

static void
gst_deepgram_sink_class_init (GstDeepgramSinkClass* klass)
{
  GObjectClass*     gobject_class  = G_OBJECT_CLASS (klass);
  GstElementClass*  element_class  = GST_ELEMENT_CLASS (klass);
  GstBaseSinkClass* basesink_class = GST_BASE_SINK_CLASS (klass);

  gobject_class->set_property = gst_deepgram_sink_set_property;
  gobject_class->get_property = gst_deepgram_sink_get_property;

  g_object_class_install_property (
      gobject_class, PROP_API_KEY,
      g_param_spec_string ("deepgram-api-key", "Deepgram API Key",
                           "API key for Deepgram real-time transcription", NULL,
                           G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (
      gobject_class, PROP_MODEL,
      g_param_spec_string ("model", "Deepgram Model",
                           "Deepgram model (e.g., 'nova')", "general",
                           G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (
      gobject_class, PROP_SILENT,
      g_param_spec_boolean ("silent", "Silent",
                            "Suppress console logging of transcripts", FALSE,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_deepgram_sink_signals[SIGNAL_TRANSCRIPT] = g_signal_new (
      "transcript", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
      NULL, G_TYPE_NONE, 4, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_DOUBLE,
      G_TYPE_DOUBLE);

  gst_deepgram_sink_signals[SIGNAL_WORD] = g_signal_new (
      "word", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
      G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_DOUBLE, G_TYPE_DOUBLE);

  gst_element_class_set_static_metadata (
      element_class, "DeepgramSink", "Sink/Audio",
      "Sends raw PCM to Deepgram via WebSockets, prints transcripts.",
      "Max Golovanchuk <mexxik@gmail.com>");

  gst_element_class_add_pad_template (
      element_class, gst_static_pad_template_get (&sink_template));

  basesink_class->start  = GST_DEBUG_FUNCPTR (gst_deepgram_sink_start);
  basesink_class->stop   = GST_DEBUG_FUNCPTR (gst_deepgram_sink_stop);
  basesink_class->render = GST_DEBUG_FUNCPTR (gst_deepgram_sink_render);

  GST_DEBUG_CATEGORY_INIT (gst_deepgram_sink_debug, "deepgramsink", 0,
                           "Deepgram sink plugin");
}

static void
gst_deepgram_sink_init (GstDeepgramSink* self)
{
  self->api_key = NULL;
  self->model   = g_strdup ("general");
  self->silent  = FALSE;
  self->ws      = NULL;
  gst_base_sink_set_sync (GST_BASE_SINK (self), TRUE);
}

static void
gst_deepgram_sink_set_property (GObject* object, guint prop_id,
                                const GValue* value, GParamSpec* pspec)
{
  GstDeepgramSink* self = GST_DEEPGRAM_SINK (object);

  switch (prop_id)
    {
    case PROP_API_KEY:
      g_free (self->api_key);
      self->api_key = g_value_dup_string (value);
      break;
    case PROP_MODEL:
      g_free (self->model);
      self->model = g_value_dup_string (value);
      break;
    case PROP_SILENT:
      self->silent = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
gst_deepgram_sink_get_property (GObject* object, guint prop_id, GValue* value,
                                GParamSpec* pspec)
{
  GstDeepgramSink* self = GST_DEEPGRAM_SINK (object);

  switch (prop_id)
    {
    case PROP_API_KEY:
      g_value_set_string (value, self->api_key);
      break;
    case PROP_MODEL:
      g_value_set_string (value, self->model);
      break;
    case PROP_SILENT:
      g_value_set_boolean (value, self->silent);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static gboolean
gst_deepgram_sink_start (GstBaseSink* basesink)
{
  GstDeepgramSink* self = GST_DEEPGRAM_SINK (basesink);

  g_print ("[deepgramsink] Starting\n");

  if (!self->api_key || strlen (self->api_key) == 0)
    {
      g_printerr ("[deepgramsink] ERROR: no Deepgram API key set.\n");
      return FALSE;
    }

  self->ws = deepgram_ws_new ();

  g_object_set (self->ws, "api-key", self->api_key, NULL);
  g_object_set (self->ws, "model", self->model, NULL);
  g_object_set (self->ws, "silent", self->silent, NULL);

  g_signal_connect (self->ws, "transcript",
                    G_CALLBACK (gst_deepgram_sink_on_deepgram_transcript),
                    self);

  g_signal_connect (self->ws, "word",
                    G_CALLBACK (gst_deepgram_sink_on_deepgram_word), self);

  if (!deepgram_ws_start (self->ws))
    {
      g_printerr ("[deepgramsink] Failed to start DeepgramWS.\n");
      g_clear_object (&self->ws);
      return FALSE;
    }

  return TRUE;
}

static gboolean
gst_deepgram_sink_stop (GstBaseSink* basesink)
{
  GstDeepgramSink* self = GST_DEEPGRAM_SINK (basesink);

  g_print ("[deepgramsink] Stopping\n");

  if (self->ws)
    {
      deepgram_ws_stop (self->ws);
      g_clear_object (&self->ws);
    }

  return TRUE;
}

static GstFlowReturn
gst_deepgram_sink_render (GstBaseSink* basesink, GstBuffer* buffer)
{
  GstDeepgramSink* self = GST_DEEPGRAM_SINK (basesink);

  if (!self->ws)
    return GST_FLOW_OK;

  GstMapInfo map;
  if (!gst_buffer_map (buffer, &map, GST_MAP_READ))
    {
      return GST_FLOW_ERROR;
    }

  deepgram_ws_push_audio (self->ws, map.data, map.size);

  gst_buffer_unmap (buffer, &map);
  return GST_FLOW_OK;
}

static void
gst_deepgram_sink_on_deepgram_transcript (DeepgramWS* ws, const gchar* text,
                                          gboolean is_final, gdouble start_time,
                                          gdouble end_time, gpointer user_data)
{
  GstDeepgramSink* self = GST_DEEPGRAM_SINK (user_data);

  if (!self->silent)
    {
      g_print ("[deepgramsink] => %s: %s\n", is_final ? "Final" : "Partial",
               text);
    }

  g_signal_emit (self, gst_deepgram_sink_signals[SIGNAL_TRANSCRIPT], 0, text,
                 is_final, start_time, end_time);
}

static void
gst_deepgram_sink_on_deepgram_word (DeepgramWS* ws, const gchar* word,
                                    gdouble start_time, gdouble end_time,
                                    gpointer user_data)
{
  GstDeepgramSink* self = GST_DEEPGRAM_SINK (user_data);

  g_signal_emit (self, gst_deepgram_sink_signals[SIGNAL_WORD], 0, word,
                 start_time, end_time);
}

static gboolean
gst_deepgram_sink_plugin_init (GstPlugin* plugin)
{
  return gst_element_register (plugin, "deepgramsink", GST_RANK_NONE,
                               GST_TYPE_DEEPGRAM_SINK);
}

#define PACKAGE "gst-deepgram"

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, deepgramsink,
                   "Send audio to Deepgram over WebSockets, print transcripts",
                   gst_deepgram_sink_plugin_init, "1.0", "LGPL", PACKAGE,
                   "http://gstreamer.net/")
