#include "deepgramws.h"

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <pthread.h>

struct _DeepgramWS
{
  GObject parent_instance;

  gchar*   api_key;
  gchar*   model;
  gboolean silent;

  SoupSession*             soup_session;
  SoupWebsocketConnection* ws_conn;

  GMutex    lock;
  GCond     cond;
  pthread_t ws_thread;
  gboolean  stop_thread;

  GQueue* audio_queue;
};

G_DEFINE_TYPE (DeepgramWS, deepgram_ws, G_TYPE_OBJECT)

static guint signals[N_WS_SIGNALS] = { 0 };

static void deepgram_ws_dispose (GObject* object);

static void* deepgram_ws_thread_func (void* user_data);
static void  deepgram_ws_on_message (SoupWebsocketConnection* conn, gint type,
                                     GBytes* message, gpointer user_data);

static void deepgram_ws_get_property (GObject* object, guint prop_id,
                                      GValue* value, GParamSpec* pspec);

static void deepgram_ws_set_property (GObject* object, guint prop_id,
                                      const GValue* value, GParamSpec* pspec);

typedef struct
{
  GMainLoop*               loop;
  SoupWebsocketConnection* conn;
  GError*                  error;
} DeepgramConnectData;

static void
deepgram_ws_connect_cb (GObject* source_object, GAsyncResult* res,
                        gpointer user_data)
{
  DeepgramConnectData* cd = (DeepgramConnectData*)user_data;
  cd->conn                = soup_session_websocket_connect_finish (
      SOUP_SESSION (source_object), res, &cd->error);
  g_main_loop_quit (cd->loop);
}

static SoupWebsocketConnection*
deepgram_ws_connect_sync (SoupSession* session, SoupMessage* msg,
                          const gchar* origin, GCancellable* cancellable,
                          GError** error_out)
{
  DeepgramConnectData cd = { 0 };
  cd.loop                = g_main_loop_new (NULL, FALSE);
  soup_session_websocket_connect_async (
      session, msg, origin, NULL, 0, cancellable, deepgram_ws_connect_cb, &cd);

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
deepgram_ws_class_init (DeepgramWSClass* klass)
{
  GObjectClass* object_class = G_OBJECT_CLASS (klass);

  object_class->dispose      = deepgram_ws_dispose;
  object_class->set_property = deepgram_ws_set_property;
  object_class->get_property = deepgram_ws_get_property;

  g_object_class_install_property (
      object_class, PROP_WS_API_KEY,
      g_param_spec_string ("api-key", "API Key", "Deepgram API Key", NULL,
                           G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (
      object_class, PROP_WS_MODEL,
      g_param_spec_string ("model", "Model", "Deepgram model name", "general",
                           G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (
      object_class, PROP_WS_SILENT,
      g_param_spec_boolean ("silent", "Silent", "Suppress console logging",
                            FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  signals[SIGNAL_WS_TRANSCRIPT] = g_signal_new (
      "transcript", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
      NULL, G_TYPE_NONE, 4, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_DOUBLE,
      G_TYPE_DOUBLE);

  signals[SIGNAL_WS_WORD]
      = g_signal_new ("word", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                      NULL, NULL, NULL, G_TYPE_NONE, 4, G_TYPE_STRING,
                      G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_DOUBLE);
}

static void
deepgram_ws_init (DeepgramWS* self)
{
  self->api_key      = NULL;
  self->model        = g_strdup ("general");
  self->silent       = FALSE;
  self->soup_session = NULL;
  self->ws_conn      = NULL;

  g_mutex_init (&self->lock);
  g_cond_init (&self->cond);

  self->stop_thread = FALSE;
  self->audio_queue = g_queue_new ();
}

static void
deepgram_ws_dispose (GObject* object)
{
  DeepgramWS* self = DEEPGRAM_WS (object);

  deepgram_ws_stop (self);

  if (self->api_key)
    {
      g_free (self->api_key);
      self->api_key = NULL;
    }
  if (self->model)
    {
      g_free (self->model);
      self->model = NULL;
    }

  if (self->audio_queue)
    {
      while (!g_queue_is_empty (self->audio_queue))
        {
          GBytes* chunk = g_queue_pop_head (self->audio_queue);
          if (chunk)
            g_bytes_unref (chunk);
        }
      g_queue_free (self->audio_queue);
      self->audio_queue = NULL;
    }

  G_OBJECT_CLASS (deepgram_ws_parent_class)->dispose (object);
}

static void
deepgram_ws_set_property (GObject* object, guint prop_id, const GValue* value,
                          GParamSpec* pspec)
{
  DeepgramWS* self = DEEPGRAM_WS (object);

  switch (prop_id)
    {
    case PROP_WS_API_KEY:
      if (self->api_key)
        g_free (self->api_key);
      self->api_key = g_value_dup_string (value);
      break;
    case PROP_WS_MODEL:
      if (self->model)
        g_free (self->model);
      self->model = g_value_dup_string (value);
      break;
    case PROP_WS_SILENT:
      self->silent = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
deepgram_ws_get_property (GObject* object, guint prop_id, GValue* value,
                          GParamSpec* pspec)
{
  DeepgramWS* self = DEEPGRAM_WS (object);

  switch (prop_id)
    {
    case PROP_WS_API_KEY:
      g_value_set_string (value, self->api_key);
      break;
    case PROP_WS_MODEL:
      g_value_set_string (value, self->model);
      break;
    case PROP_WS_SILENT:
      g_value_set_boolean (value, self->silent);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

DeepgramWS*
deepgram_ws_new (void)
{
  return g_object_new (DEEPGRAM_TYPE_WS, NULL);
}

gboolean
deepgram_ws_start (DeepgramWS* self)
{
  g_return_val_if_fail (DEEPGRAM_IS_WS (self), FALSE);

  if (!self->api_key || !*(self->api_key))
    {
      g_printerr ("[DeepgramWS] ERROR: no API key set.\n");
      return FALSE;
    }

  if (!self->soup_session)
    {
      self->soup_session = soup_session_new ();
    }

  self->stop_thread = FALSE;
  if (pthread_create (&self->ws_thread, NULL, deepgram_ws_thread_func, self)
      != 0)
    {
      g_printerr ("[DeepgramWS] Failed to create ws_thread.\n");
      return FALSE;
    }

  return TRUE;
}

void
deepgram_ws_stop (DeepgramWS* self)
{
  g_return_if_fail (DEEPGRAM_IS_WS (self));

  g_mutex_lock (&self->lock);
  self->stop_thread = TRUE;
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

  if (self->soup_session)
    {
      g_object_unref (self->soup_session);
      self->soup_session = NULL;
    }
}

void
deepgram_ws_push_audio (DeepgramWS* self, const guint8* data, gsize size)
{
  g_return_if_fail (DEEPGRAM_IS_WS (self));

  if (!data || size == 0)
    return;

  GBytes* chunk = g_bytes_new (data, size);

  g_mutex_lock (&self->lock);
  g_queue_push_tail (self->audio_queue, chunk);
  g_mutex_unlock (&self->lock);
}

static void*
deepgram_ws_thread_func (void* user_data)
{
  DeepgramWS* self = DEEPGRAM_WS (user_data);

  GError*                  error = NULL;
  SoupMessage*             msg   = NULL;
  gchar*                   url   = NULL;
  SoupWebsocketConnection* conn  = NULL;

  url = g_strdup_printf (
      "wss://api.deepgram.com/v1/listen"
      "?encoding=linear16&sample_rate=16000&channels=1&model=%s",
      self->model ? self->model : "general");

  msg = soup_message_new (SOUP_METHOD_GET, url);
  if (!msg)
    {
      g_printerr ("[DeepgramWS] Failed to create SoupMessage.\n");
      goto done;
    }

  {
    SoupMessageHeaders* headers  = soup_message_get_request_headers (msg);
    gchar*              auth_val = g_strdup_printf ("Token %s", self->api_key);
    soup_message_headers_append (headers, "Authorization", auth_val);
    g_free (auth_val);
  }

  g_print ("[DeepgramWS] Connecting to: %s\n", url);

  conn = deepgram_ws_connect_sync (self->soup_session, msg, NULL, NULL, &error);
  if (!conn)
    {
      g_printerr ("[DeepgramWS] WebSocket connect error: %s\n",
                  error ? error->message : "unknown");
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

  g_signal_connect (conn, "message", G_CALLBACK (deepgram_ws_on_message), self);

  g_print ("[DeepgramWS] WebSocket connected.\n");

  while (TRUE)
    {
      g_mutex_lock (&self->lock);
      if (self->stop_thread)
        {
          g_mutex_unlock (&self->lock);
          break;
        }

      GBytes* chunk = NULL;
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

  g_mutex_lock (&self->lock);
  self->ws_conn = NULL;
  g_mutex_unlock (&self->lock);

  g_print ("[DeepgramWS] ws_thread exiting.\n");
  return NULL;
}

static void
deepgram_ws_on_message (SoupWebsocketConnection* conn, gint type,
                        GBytes* message, gpointer user_data)
{
  if (type != SOUP_WEBSOCKET_DATA_TEXT)
    return;

  DeepgramWS* self = DEEPGRAM_WS (user_data);

  gsize         size = 0;
  gconstpointer data = g_bytes_get_data (message, &size);
  if (!data || size == 0)
    return;

  g_debug ("[DeepgramWS] Raw message:\n%.*s\n", (int)size, (const char*)data);

  JsonParser* parser = json_parser_new ();
  GError*     error  = NULL;

  if (!json_parser_load_from_data (parser, data, size, &error))
    {
      g_printerr ("[DeepgramWS] JSON parse error: %s\n",
                  error ? error->message : "unknown");
      if (error)
        {
          g_error_free (error);
          error = NULL;
        }
      g_object_unref (parser);
      return;
    }

  JsonNode* root = json_parser_get_root (parser);

  JsonObject* root_obj = json_node_get_object (root);

  if (!json_object_has_member (root_obj, "channel"))
    {
      g_object_unref (parser);
      return;
    }

  JsonObject* channel_obj = json_object_get_object_member (root_obj, "channel");
  if (!channel_obj)
    {
      g_object_unref (parser);
      return;
    }

  if (!json_object_has_member (channel_obj, "alternatives"))
    {
      g_object_unref (parser);
      return;
    }

  JsonArray* alt_arr
      = json_object_get_array_member (channel_obj, "alternatives");
  if (!alt_arr || json_array_get_length (alt_arr) == 0)
    {
      g_object_unref (parser);
      return;
    }

  JsonObject* first_alt = json_array_get_object_element (alt_arr, 0);
  if (!first_alt)
    {
      g_object_unref (parser);
      return;
    }

  gdouble    transcript_start_time = 0.0;
  gdouble    transcript_end_time   = 0.0;
  gboolean   has_start_time        = FALSE;
  JsonArray* words_arr = json_object_get_array_member (first_alt, "words");
  if (words_arr)
    {
      for (guint i = 0; i < json_array_get_length (words_arr); i++)
        {
          JsonObject* word_obj = json_array_get_object_element (words_arr, i);
          if (!word_obj)
            continue;

          const gchar* word_text  = NULL;
          gdouble      start_time = 0.0;
          gdouble      end_time   = 0.0;

          if (json_object_has_member (word_obj, "word"))
            {
              word_text = json_object_get_string_member (word_obj, "word");
            }
          if (json_object_has_member (word_obj, "start"))
            {
              start_time = json_object_get_double_member (word_obj, "start");
              if (!has_start_time)
                {
                  has_start_time        = TRUE;
                  transcript_start_time = start_time;
                }
            }
          if (json_object_has_member (word_obj, "end"))
            {
              end_time = json_object_get_double_member (word_obj, "end");
              if (end_time > transcript_end_time)
                {
                  transcript_end_time = end_time;
                }
            }

          if (word_text && *word_text)
            {
              g_signal_emit (self, signals[SIGNAL_WS_WORD], 0, word_text,
                             start_time, end_time);
            }
        }
    }

  const gchar* transcript = NULL;
  if (json_object_has_member (first_alt, "transcript"))
    {
      transcript = json_object_get_string_member (first_alt, "transcript");
    }

  gboolean is_final = FALSE;
  if (json_object_has_member (root_obj, "is_final"))
    {
      is_final = json_object_get_boolean_member (root_obj, "is_final");
    }

  if (transcript && *transcript)
    {
      if (!self->silent)
        {
          g_print ("[DeepgramWS] => %s: %s\n", is_final ? "Final" : "Partial",
                   transcript);
        }
      g_signal_emit (self, signals[SIGNAL_WS_TRANSCRIPT], 0, transcript,
                     is_final, transcript_start_time, transcript_end_time);
    }

  g_object_unref (parser);
}
