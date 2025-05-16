#include <glib.h>
#include <gst/gst.h>
#include <stdio.h>
#include <string.h>

static void
decodebin_pad_added_cb (GstElement* dbin, GstPad* pad, gpointer userdata)
{
  GstElement* conv    = GST_ELEMENT (userdata);
  GstPad*     sinkpad = gst_element_get_static_pad (conv, "sink");
  gst_pad_link (pad, sinkpad);
  gst_object_unref (sinkpad);
}

static gboolean
bus_callback (GstBus* bus, GstMessage* msg, gpointer data)
{
  switch (GST_MESSAGE_TYPE (msg))
    {
    case GST_MESSAGE_ERROR:
      {
        GError* err   = NULL;
        gchar*  debug = NULL;
        gst_message_parse_error (msg, &err, &debug);
        g_printerr ("Error: %s\n", err->message);
        g_error_free (err);
        g_free (debug);
        break;
      }
    case GST_MESSAGE_EOS:
      g_print ("End of stream\n");
      break;
    default:
      break;
    }
  return TRUE;
}

static void
on_transcript (GstElement* sink, gchar* transcript, gboolean is_final,
               gdouble start_time, gdouble end_time, gpointer user_data)
{
  g_print ("[APP] %s transcript: [%.2f - %.2f] %s\n",
           is_final ? "Final" : "Partial", start_time, end_time, transcript);

  g_print ("----------------------------------------\n");
}

static void
on_word (GstElement* sink, gchar* word_text, gdouble start_time,
         gdouble end_time, gpointer user_data)
{
  g_print ("[APP] Word='%s'  start=%.2f  end=%.2f\n", word_text, start_time,
           end_time);
}

int
main (int argc, char* argv[])
{
  GstElement *pipeline, *filesrc, *decodebin, *audioconv, *audiores, *deepgram;
  GstBus*     bus;
  GMainLoop*  loop;

  gst_init (&argc, &argv);

  if (argc < 2)
    {
      g_printerr ("Usage: %s <input_audio_file>\n", argv[0]);
      return -1;
    }

  const gchar* filename = argv[1];
  const gchar* api_key  = g_getenv ("DEEPGRAM_API_KEY");
  if (!api_key)
    {
      g_printerr ("Environment variable DEEPGRAM_API_KEY not set.\n");
      return -1;
    }

  pipeline  = gst_pipeline_new ("deepgram-test");
  filesrc   = gst_element_factory_make ("filesrc", "filesrc");
  decodebin = gst_element_factory_make ("decodebin", "decoder");
  audioconv = gst_element_factory_make ("audioconvert", "convert");
  audiores  = gst_element_factory_make ("audioresample", "resample");
  deepgram  = gst_element_factory_make ("deepgramsink", "deepgram");

  if (!pipeline || !filesrc || !decodebin || !audioconv || !audiores
      || !deepgram)
    {
      g_printerr ("Error creating pipeline elements.\n");
      return -2;
    }

  g_object_set (filesrc, "location", filename, NULL);
  g_object_set (deepgram, "deepgram-api-key", api_key, "silent", TRUE, NULL);

  gst_bin_add_many (GST_BIN (pipeline), filesrc, decodebin, audioconv, audiores,
                    deepgram, NULL);

  g_signal_connect (decodebin, "pad-added", G_CALLBACK (decodebin_pad_added_cb),
                    audioconv);

  g_signal_connect (deepgram, "transcript", G_CALLBACK (on_transcript), NULL);
  g_signal_connect (deepgram, "word", G_CALLBACK (on_word), NULL);

  if (!gst_element_link (filesrc, decodebin))
    {
      g_printerr ("Failed to link filesrc -> decodebin\n");
      return -3;
    }
  if (!gst_element_link_many (audioconv, audiores, deepgram, NULL))
    {
      g_printerr ("Failed to link audioconv -> audiores -> deepgramsink\n");
      return -4;
    }

  bus = gst_element_get_bus (pipeline);
  gst_bus_add_watch (bus, bus_callback, NULL);
  gst_object_unref (bus);

  loop = g_main_loop_new (NULL, FALSE);
  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  g_print ("Running pipeline...\n");
  g_main_loop_run (loop);

  g_print ("Pipeline finished.\n");
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (pipeline);
  g_main_loop_unref (loop);

  return 0;
}
