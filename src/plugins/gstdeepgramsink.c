#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib.h>
#include <gst/base/gstbasesink.h>
#include <gst/gst.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

GST_DEBUG_CATEGORY_STATIC (gst_deepgram_sink_debug);
#define GST_CAT_DEFAULT gst_deepgram_sink_debug

#define GST_TYPE_DEEPGRAM_SINK (gst_deepgram_sink_get_type ())
G_DECLARE_FINAL_TYPE (GstDeepgramSink, gst_deepgram_sink, GST, DEEPGRAM_SINK,
                      GstBaseSink)

struct _GstDeepgramSink
{
  GstBaseSink parent;

  gchar*   api_key;
  gchar*   model;
  gboolean started;
  gboolean silent;

  SoupSession*             soup_sess;
  SoupWebsocketConnection* ws_conn;

  GMutex lock;
  GCond  cond;

  pthread_t ws_thread;
  gboolean  stop_thread;

  GQueue* audio_queue;
};

G_DEFINE_TYPE (GstDeepgramSink, gst_deepgram_sink, GST_TYPE_BASE_SINK)

enum
{
  PROP_0,
  PROP_API_KEY,
  PROP_MODEL,
  PROP_SILENT
};

static GstStaticPadTemplate sink_template
    = GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
                               GST_STATIC_CAPS ("audio/x-raw, "
                                                "format = (string) S16LE, "
                                                "rate = (int) 16000, "
                                                "channels = (int) 1"));

static void gst_deepgram_sink_set_property (GObject* object, guint prop_id,
                                            const GValue* value,
                                            GParamSpec*   pspec);
static void gst_deepgram_sink_get_property (GObject* object, guint prop_id,
                                            GValue* value, GParamSpec* pspec);

static gboolean      gst_deepgram_sink_start (GstBaseSink* basesink);
static gboolean      gst_deepgram_sink_stop (GstBaseSink* basesink);
static GstFlowReturn gst_deepgram_sink_render (GstBaseSink* basesink,
                                               GstBuffer*   buffer);

static void* gst_deepgram_sink_ws_thread_func (void* user_data);

static void gst_deepgram_sink_on_message (SoupWebsocketConnection* conn,
                                          gint type, GBytes* message,
                                          gpointer user_data);

typedef struct
{
  GMainLoop*               loop;
  SoupWebsocketConnection* conn;
  GError*                  error;
} DeepgramConnectData;

static void
deepgram_connect_cb (GObject* source_object, GAsyncResult* res,
                     gpointer user_data)
{
  DeepgramConnectData* cd = (DeepgramConnectData*)user_data;
  cd->conn                = soup_session_websocket_connect_finish (
      SOUP_SESSION (source_object), res, &cd->error);
  g_main_loop_quit (cd->loop);
}

static SoupWebsocketConnection*
deepgram_websocket_connect_sync (SoupSession* session, SoupMessage* msg,
                                 const gchar* origin, GCancellable* cancellable,
                                 GError** error_out)
{
  DeepgramConnectData cd;
  memset (&cd, 0, sizeof (cd));

  cd.loop = g_main_loop_new (NULL, FALSE);

  soup_session_websocket_connect_async (session, msg, origin, NULL, 0,
                                        cancellable, deepgram_connect_cb, &cd);

  g_main_loop_run (cd.loop);
  g_main_loop_unref (cd.loop);

  if (!cd.conn)
    {

      if (error_out)
        {
          *error_out = cd.error;
        }
      else
        {
          g_clear_error (&cd.error);
        }
      return NULL;
    }

  return cd.conn;
}

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

  gst_element_class_set_static_metadata (
      element_class, "DeepgramSink", "Sink/Audio",
      "Sends raw PCM to Deepgram via WebSockets, prints transcripts.",
      "Your Name <you@example.com>");

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
  self->api_key   = NULL;
  self->model     = g_strdup ("general");
  self->started   = FALSE;
  self->soup_sess = NULL;
  self->ws_conn   = NULL;
  self->silent    = TRUE;
  g_mutex_init (&self->lock);
  g_cond_init (&self->cond);
  self->stop_thread = FALSE;
  self->audio_queue = g_queue_new ();

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

  self->started     = TRUE;
  self->stop_thread = FALSE;

  self->soup_sess = soup_session_new ();

  if (pthread_create (&self->ws_thread, NULL, gst_deepgram_sink_ws_thread_func,
                      self)
      != 0)
    {
      g_printerr ("[deepgramsink] Failed to create ws_thread.\n");
      return FALSE;
    }

  return TRUE;
}

static gboolean
gst_deepgram_sink_stop (GstBaseSink* basesink)
{
  GstDeepgramSink* self = GST_DEEPGRAM_SINK (basesink);

  g_print ("[deepgramsink] Stopping\n");
  self->started     = FALSE;
  self->stop_thread = TRUE;

  g_mutex_lock (&self->lock);
  if (self->ws_conn)
    {
      soup_websocket_connection_close (self->ws_conn, 1000, "Normal closure");
    }
  g_mutex_unlock (&self->lock);

  if (self->ws_thread)
    {
      pthread_join (self->ws_thread, NULL);
      self->ws_thread = 0;
    }

  if (self->soup_sess)
    {
      g_object_unref (self->soup_sess);
      self->soup_sess = NULL;
    }

  g_mutex_lock (&self->lock);
  while (!g_queue_is_empty (self->audio_queue))
    {
      GBytes* chunk = g_queue_pop_head (self->audio_queue);
      if (chunk)
        g_bytes_unref (chunk);
    }
  g_mutex_unlock (&self->lock);
  g_queue_free (self->audio_queue);
  self->audio_queue = g_queue_new ();

  return TRUE;
}

static GstFlowReturn
gst_deepgram_sink_render (GstBaseSink* basesink, GstBuffer* buffer)
{
  GstDeepgramSink* self = GST_DEEPGRAM_SINK (basesink);

  if (!self->started)
    return GST_FLOW_OK;

  GstMapInfo map;
  if (!gst_buffer_map (buffer, &map, GST_MAP_READ))
    {
      return GST_FLOW_ERROR;
    }

  GBytes* chunk = g_bytes_new (map.data, map.size);
  gst_buffer_unmap (buffer, &map);

  g_mutex_lock (&self->lock);
  g_queue_push_tail (self->audio_queue, chunk);
  g_mutex_unlock (&self->lock);

  return GST_FLOW_OK;
}

static void*
gst_deepgram_sink_ws_thread_func (void* user_data)
{
  GstDeepgramSink* self = GST_DEEPGRAM_SINK (user_data);

  GError*                  error = NULL;
  SoupMessage*             msg   = NULL;
  gchar*                   url   = NULL;
  SoupWebsocketConnection* conn  = NULL;

  url = g_strdup_printf (
      "wss://api.deepgram.com/v1/"
      "listen?encoding=linear16&sample_rate=16000&channels=1&model=%s",
      self->model ? self->model : "general");

  msg = soup_message_new (SOUP_METHOD_GET, url);
  if (!msg)
    {
      g_printerr ("[deepgramsink] Failed to create SoupMessage.\n");
      goto done;
    }

  {
    SoupMessageHeaders* headers  = soup_message_get_request_headers (msg);
    gchar*              auth_val = g_strdup_printf ("Token %s", self->api_key);
    soup_message_headers_append (headers, "Authorization", auth_val);
    g_free (auth_val);
  }

  g_print ("[deepgramsink] Connecting to: %s\n", url);

  conn = deepgram_websocket_connect_sync (self->soup_sess, msg, NULL, NULL,
                                          &error);
  if (!conn)
    {
      g_printerr ("[deepgramsink] WebSocket connect error: %s\n",
                  (error ? error->message : "unknown"));
      if (error)
        {
          g_error_free (error);
          error = NULL;
        }
      goto done;
    }

  g_mutex_lock (&self->lock);
  self->ws_conn = conn;
  g_mutex_unlock (&self->lock);

  g_signal_connect (conn, "message", G_CALLBACK (gst_deepgram_sink_on_message),
                    self);

  g_print ("[deepgramsink] WebSocket connected.\n");

  while (!self->stop_thread)
    {
      GBytes* chunk = NULL;

      g_mutex_lock (&self->lock);
      if (!g_queue_is_empty (self->audio_queue))
        {
          chunk = g_queue_pop_head (self->audio_queue);
        }
      g_mutex_unlock (&self->lock);

      if (chunk)
        {
          gsize         size = 0;
          gconstpointer data = g_bytes_get_data (chunk, &size);
          if (size > 0 && conn)
            {
              soup_websocket_connection_send_binary (conn, data, size);
            }
          g_bytes_unref (chunk);
          chunk = NULL;
        }
      else
        {
          g_usleep (100000);
        }
    }

  if (conn
      && soup_websocket_connection_get_state (conn)
             == SOUP_WEBSOCKET_STATE_OPEN)
    {
      soup_websocket_connection_close (conn, 1000, "Normal closure");
    }

done:
  g_clear_object (&msg);
  g_free (url);

  g_print ("[deepgramsink] ws_thread exiting.\n");
  return NULL;
}

static void
gst_deepgram_sink_on_message (SoupWebsocketConnection* conn, gint type,
                              GBytes* message, gpointer user_data)
{
  if (type != SOUP_WEBSOCKET_DATA_TEXT)
    {
      return;
    }

  GstDeepgramSink* self = GST_DEEPGRAM_SINK (user_data);

  gsize         size = 0;
  gconstpointer data = g_bytes_get_data (message, &size);
  if (!data || size == 0)
    return;

  /* Parse JSON with json-glib. */
  JsonParser* parser = json_parser_new ();
  GError*     error  = NULL;

  if (!json_parser_load_from_data (parser, data, size, &error))
    {
      g_printerr ("[deepgramsink] JSON parse error: %s\n",
                  (error ? error->message : "unknown"));
      if (error)
        {
          g_error_free (error);
          error = NULL;
        }
      g_object_unref (parser);
      return;
    }

  JsonNode* root = json_parser_get_root (parser);
  if (!root || json_node_get_node_type (root) != JSON_NODE_OBJECT)
    {
      g_object_unref (parser);
      return;
    }

  JsonObject* root_obj = json_node_get_object (root);
  if (json_object_has_member (root_obj, "channel"))
    {
      JsonNode* channel_node = json_object_get_member (root_obj, "channel");
      if (JSON_NODE_HOLDS_OBJECT (channel_node))
        {
          JsonObject* channel_obj = json_node_get_object (channel_node);
          if (json_object_has_member (channel_obj, "alternatives"))
            {
              JsonNode* alt_node
                  = json_object_get_member (channel_obj, "alternatives");
              if (JSON_NODE_HOLDS_ARRAY (alt_node))
                {
                  JsonArray* alt_arr = json_node_get_array (alt_node);
                  if (json_array_get_length (alt_arr) > 0)
                    {
                      JsonNode* first_alt = json_array_get_element (alt_arr, 0);
                      if (first_alt && JSON_NODE_HOLDS_OBJECT (first_alt))
                        {
                          JsonObject* alt_obj
                              = json_node_get_object (first_alt);
                          if (json_object_has_member (alt_obj, "transcript"))
                            {
                              const gchar* transcript
                                  = json_object_get_string_member (
                                      alt_obj, "transcript");
                              if (transcript && strlen (transcript) > 0)
                                {
                                  gboolean is_final = FALSE;
                                  if (json_object_has_member (root_obj,
                                                              "is_final"))
                                    {
                                      is_final
                                          = json_object_get_boolean_member (
                                              root_obj, "is_final");
                                    }
                                  if (!self->silent && transcript
                                      && *transcript)
                                    {
                                      g_print ("[deepgramsink] %s: %s\n",
                                               is_final ? "Final" : "Partial",
                                               transcript);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

  g_object_unref (parser);
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
