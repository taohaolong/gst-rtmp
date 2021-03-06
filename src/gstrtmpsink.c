/*
 * GStreamer
 * Copyright (C) 2010 Jan Schmidt <thaytan@noraisin.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:element-rtmpsink
 *
 * This element delivers data to a streaming server via RTMP. It uses
 * librtmp, and supports any protocols/urls that librtmp supports.
 * The URL/location can contain extra connection or session parameters
 * for librtmp, such as 'flashver=version'. See the librtmp documentation
 * for more detail
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v videotestsrc ! ffenc_flv ! flvmux ! rtmpsink location='rtmp://localhost/path/to/stream live=1'
 * ]| Encode a test video stream to FLV video format and stream it via RTMP.
 * </refsect2>
 */


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>

#include "gstrtmpsink.h"

#ifdef G_OS_WIN32
#include <winsock2.h>
#endif

#include <stdlib.h>
#include <string.h>

GST_DEBUG_CATEGORY_STATIC (gst_rtmp_sink_debug);
#define GST_CAT_DEFAULT gst_rtmp_sink_debug
#define MAX_TCP_TIMEOUT 30
#define STR2AVAL(av, str)        av.av_val = str; av.av_len = strlen(av.av_val)

/* Filter signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

enum
{
  PROP_0,
  PROP_LOCATION,
  PROP_BACKUP_LOCATION,
  PROP_RECONNECTION_DELAY,
  PROP_TCP_TIMEOUT,
  ARG_LOG_LEVEL,
  PROP_FLASHVER,
};

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-flv")
    );

static void gst_rtmp_sink_finalize (GstRTMPSink * sink);
static void gst_rtmp_sink_uri_handler_init (gpointer g_iface,
    gpointer iface_data);
static void gst_rtmp_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_rtmp_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static gboolean gst_rtmp_sink_stop (GstBaseSink * sink);
static gboolean gst_rtmp_sink_start (GstBaseSink * sink);
static gboolean gst_rtmp_sink_event (GstBaseSink * sink, GstEvent * event);
static gboolean gst_rtmp_sink_setcaps (GstBaseSink * sink, GstCaps * caps);
static GstFlowReturn gst_rtmp_sink_render (GstBaseSink * sink, GstBuffer * buf);

static void
_do_init (GType gtype)
{
  static const GInterfaceInfo urihandler_info = {
    gst_rtmp_sink_uri_handler_init,
    NULL,
    NULL,
  };

  g_type_add_interface_static (gtype, GST_TYPE_URI_HANDLER, &urihandler_info);

  GST_DEBUG_CATEGORY_INIT (gst_rtmp_sink_debug, "rtmpsink", 0,
      "RTMP server element");
}

GST_BOILERPLATE_FULL (GstRTMPSink, gst_rtmp_sink, GstBaseSink,
    GST_TYPE_BASE_SINK, _do_init);


static void
gst_rtmp_sink_base_init (gpointer klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_set_details_simple (element_class,
      "RTMP output sink",
      "Sink/Network", "Sends FLV content to a server via RTMP",
      "Jan Schmidt <thaytan@noraisin.net>, Anthony Violo <anthony.violo@ubicast.eu>");

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&sink_template));

}

/* initialize the plugin's class */
static void
gst_rtmp_sink_class_init (GstRTMPSinkClass * klass)
{
  GObjectClass *gobject_class;
  GstBaseSinkClass *gstbasesink_class = (GstBaseSinkClass *) klass;

  gobject_class = (GObjectClass *) klass;

  gobject_class->set_property = gst_rtmp_sink_set_property;
  gobject_class->get_property = gst_rtmp_sink_get_property;

  gstbasesink_class->start = GST_DEBUG_FUNCPTR (gst_rtmp_sink_start);
  gstbasesink_class->stop = GST_DEBUG_FUNCPTR (gst_rtmp_sink_stop);
  gstbasesink_class->render = GST_DEBUG_FUNCPTR (gst_rtmp_sink_render);
  gobject_class->finalize = (GObjectFinalizeFunc)gst_rtmp_sink_finalize;
  gstbasesink_class->set_caps = GST_DEBUG_FUNCPTR (gst_rtmp_sink_setcaps);
  gstbasesink_class->event = GST_DEBUG_FUNCPTR (gst_rtmp_sink_event);

  gst_element_class_install_std_props (GST_ELEMENT_CLASS (klass),
      "location", PROP_LOCATION, G_PARAM_READWRITE, NULL);

  g_object_class_install_property (gobject_class, PROP_BACKUP_LOCATION,
    g_param_spec_string ("backup_location", "Backup_location", "Backup URI is used when main URI is not accessible anymore", 
      NULL, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, ARG_LOG_LEVEL,
    g_param_spec_int ("log-level", "Log level",
        "librtmp log level", RTMP_LOGCRIT, RTMP_LOGALL, RTMP_LOGERROR,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (G_OBJECT_CLASS (klass),
      PROP_RECONNECTION_DELAY, g_param_spec_uint64 ("reconnection-delay",
      "Delay between each reconnection in ns. 0 means that an error occurs when disconnected",
      "Delay between each reconnection in ns. 0 means that an error occurs when disconnected",
      0, G_MAXINT64, 10000000000, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (G_OBJECT_CLASS (klass),
      PROP_TCP_TIMEOUT,  g_param_spec_uint ("tcp-timeout",
      "Custom TCP timeout in sec. If 0, socket is in blocking mode (default librtmp behaviour)",
      "Custom TCP timeout in sec. If 0, socket is in blocking mode (default librtmp behaviour)",
      0, MAX_TCP_TIMEOUT, MAX_TCP_TIMEOUT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_FLASHVER,
    g_param_spec_string ("flashver", "Flashver", "Version of the Flash plugin used to run the SWF player. The default is gstreamer0.10-rtmp-ubicast", 
      NULL, G_PARAM_READWRITE));
}

static void
gst_rtmp_sink_finalize (GstRTMPSink * sink)
{
#ifdef G_OS_WIN32
  WSACleanup ();
#endif
  g_free (sink->backup_uri);
  g_free (sink->uri);
  GST_DEBUG_OBJECT (sink, "free all variables stored in memory");
  G_OBJECT_CLASS (parent_class)->finalize (G_OBJECT (sink));
}

static void
gst_rtmp_sink_init (GstRTMPSink * sink, GstRTMPSinkClass * klass)
{
#ifdef G_OS_WIN32
  WSADATA wsa_data;

  if (WSAStartup (MAKEWORD (2, 2), &wsa_data) != 0) {
    GST_ERROR_OBJECT (sink, "WSAStartup failed: 0x%08x", WSAGetLastError ());
  }
#endif
  sink->connection_status = 0;
  sink->reconnection_delay = 10000000000;
  sink->tcp_timeout = 3;
  sink->stream_meta_saved = FALSE;
  sink->video_meta_saved = FALSE;
  sink->audio_meta_saved = FALSE;
  sink->try_now_connection = TRUE;
  sink->send_error_count = 0;
  sink->disconnection_notified = 1;
  sink->is_backup = FALSE;
  sink->backup_uri = NULL;
  sink->flashver = "gstreamer0.10-rtmp-ubicast";
}

static gboolean
gst_rtmp_sink_start (GstBaseSink * basesink)
{
  GstRTMPSink *sink = GST_RTMP_SINK (basesink);

  if (!sink->is_backup) {
    if (!sink->uri) {
      GST_ELEMENT_ERROR (sink, RESOURCE, OPEN_WRITE,
          ("Please set URI for RTMP output"), ("No URI set before starting"));
      return FALSE;
    }
  }
  else {
  	if (!sink->backup_uri) {
  		GST_ELEMENT_WARNING (sink, RESOURCE, OPEN_WRITE,
          ("Backup uri is incorrect, can not switch to it"), NULL);
      return FALSE;
    }
  }
  if (!sink->is_backup)
  	sink->rtmp_uri = g_strdup (sink->uri);
  else
  	sink->rtmp_uri = g_strdup (sink->backup_uri);
  sink->rtmp = RTMP_Alloc ();

  if (!sink->rtmp) {
    GST_ERROR_OBJECT (sink, "Could not allocate librtmp's RTMP context");
    goto error;
  }

  RTMP_Init (sink->rtmp);
  if (!sink->is_backup) {
    if (!RTMP_SetupURL (sink->rtmp, sink->rtmp_uri)) {
      GST_ELEMENT_ERROR (sink, RESOURCE, OPEN_WRITE, (NULL),
          ("Failed to setup URL '%s'", sink->uri));
      goto error;
    }
  }
  else {
  	if (!RTMP_SetupURL (sink->rtmp, sink->rtmp_uri)) {
      GST_ELEMENT_ERROR (sink, RESOURCE, OPEN_WRITE, (NULL),
          ("Failed to setup URL '%s'", sink->backup_uri));
      goto error;
    }
  }


  GST_DEBUG_OBJECT (sink, "Created RTMP object");

  /* Mark this as an output connection */
  RTMP_EnableWrite (sink->rtmp);

  sink->first = TRUE;
  sink->have_write_error = FALSE;
  sink->first = TRUE;
  return TRUE;
error:
  if (sink->rtmp) {
    RTMP_Free (sink->rtmp);
    sink->rtmp = NULL;
  }
  g_free (sink->rtmp_uri);
  sink->rtmp_uri = NULL;
  return FALSE;
}

static gboolean
gst_rtmp_sink_stop (GstBaseSink * basesink)
{
  GstRTMPSink *sink = GST_RTMP_SINK (basesink);

  //gst_buffer_replace (&sink->header, NULL);
  if (sink->header) {
    gst_buffer_unref (sink->header);
    sink->header = NULL;
  }
  if (sink->rtmp) {
    RTMP_Close (sink->rtmp);
    RTMP_Free (sink->rtmp);
    sink->rtmp = NULL;
  }
  if (sink->rtmp_uri) {
    g_free (sink->rtmp_uri);
    sink->rtmp_uri = NULL;
  }
  return TRUE;
}

static
gboolean    copy_metadata(GstBuffer **meta_buf, GstBuffer *buf)
{
    *meta_buf = gst_buffer_new_and_alloc(GST_BUFFER_SIZE (buf));
    *meta_buf = gst_buffer_copy(buf);
    return TRUE;
}

static gboolean gst_rtmp_sink_option(GstRTMPSink *sink) {

  AVal flashver; 
  AVal timeout;
  AVal flashveropt;
  AVal timeoutopt;
  gchar *str;

  STR2AVAL(flashveropt, "flashver");
  STR2AVAL(timeoutopt, "timeout");
  STR2AVAL(flashver, sink->flashver);
  str = malloc(5 * sizeof(char*));
  snprintf(str, 5, "%d", sink->tcp_timeout);
  STR2AVAL(timeout, str);

  if (!RTMP_SetOpt(sink->rtmp, &flashveropt, &flashver)) {
    GST_ELEMENT_ERROR (sink, RESOURCE, OPEN_READ, (NULL),
           ("Failed to set flashver"));
    goto error;
  }

  if (!RTMP_SetOpt(sink->rtmp, &timeoutopt, &timeout)) {
    GST_ELEMENT_ERROR (sink, RESOURCE, OPEN_READ, (NULL),
           ("Failed to set flashver"));
    goto error;
  }

  return TRUE;
error:
  if (sink->rtmp) {
    RTMP_Free(sink->rtmp);
    sink->rtmp = NULL;
  }
  if (sink->rtmp_uri) {
    g_free(sink->rtmp_uri);
    sink->rtmp_uri = NULL;
  }
  return FALSE;
}

static GstFlowReturn
gst_rtmp_sink_render (GstBaseSink * bsink, GstBuffer * buf)
{
  GstRTMPSink *sink = GST_RTMP_SINK (bsink);
  gboolean need_unref = FALSE;
  gboolean result = TRUE;
  GstBuffer *reffed_buf = NULL;
  GstStructure *s;

  if (sink->connection_status) {
    if (!sink->stream_meta_saved && buf->data[0] == 18) {
        GST_LOG_OBJECT (sink, "save stream metadata, size : %d", GST_BUFFER_SIZE (buf));
        sink->stream_meta_saved = copy_metadata(&sink->stream_metadata, buf);
    }
    // Check if first packet is video type (video = 9) contain video type
    else if (!sink->video_meta_saved && buf->data[0] == 9) {
        GST_LOG_OBJECT (sink, "save video metada, size : %d", GST_BUFFER_SIZE (buf));
        sink->video_meta_saved = copy_metadata(&sink->video_metadata, buf);
    }
    // Check if first packet is audio type (audio = 8) contain video type
    else if (!sink->audio_meta_saved && buf->data[0] == 8) {
        GST_LOG_OBJECT (sink, "save audio metada, size : %d", GST_BUFFER_SIZE (buf));
        sink->audio_meta_saved = copy_metadata(&sink->audio_metadata, buf);
    }
  }
  if (sink->first) {
    if ((sink->sent_status == -1 || sink->connection_status == -1))
      sink->end_time_disc = GST_BUFFER_TIMESTAMP (buf);
    if ((sink->end_time_disc - sink->begin_time_disc > sink->reconnection_delay) || sink->try_now_connection) {
      GST_DEBUG_OBJECT (sink, "Maybe disconnected from RTMP server, reconnecting to be sure");
      if (sink->connection_status == -1 || sink->sent_status == -1) {
        GST_DEBUG_OBJECT (sink, "Reinitializing RTMP object");
        gst_rtmp_sink_stop (bsink);
        if (sink->backup_uri) {
        	sink->is_backup = !sink->is_backup;
        	if (!sink->is_backup) {
	          GST_LOG_OBJECT (sink, "Backup URI is not accessible, will switch on main URI : %s",
          		sink->backup_uri);
	         }
        	else {
	          GST_LOG_OBJECT (sink, "Main URI is not accessible, will switch on backup URI : %s",
          		sink->backup_uri);
	         }
        }
        else{
        	GST_LOG_OBJECT (sink, "No backup URI defined, try to reconnect on main URI");
        }
        gst_rtmp_sink_start (bsink);
        sink->begin_time_disc = sink->end_time_disc;
      }
      if (!RTMP_IsConnected (sink->rtmp)) {
        GST_DEBUG_OBJECT (sink, "Trying to connect");
        result = gst_rtmp_sink_option(sink);
        if (!result) {
          GST_ELEMENT_ERROR (sink, RESOURCE, OPEN_WRITE, (NULL),
            ("Could not set options, please check them"));
          goto init_failed;
        }
        if (!RTMP_Connect (sink->rtmp, NULL)
            || !RTMP_ConnectStream (sink->rtmp, 0)) {
          GST_DEBUG_OBJECT (sink, "Connection failed, freeing RTMP buffers");
          RTMP_Free (sink->rtmp);
          sink->rtmp = NULL;
          g_free (sink->rtmp_uri);
          sink->try_now_connection = FALSE;
          sink->rtmp_uri = NULL;
          sink->connection_status = -1;
          sink->send_error_count = 0;
          if (sink->reconnection_delay <= 0)
            goto init_failed;
          else {
            sink->begin_time_disc = GST_BUFFER_TIMESTAMP (buf);
            if (sink->disconnection_notified == 1) {
                GST_DEBUG_OBJECT (sink, "Emitting disconnected message");
                s = gst_structure_new ("disconnected",
                    "timestamp", G_TYPE_UINT64, sink->begin_time_disc, NULL);
                gst_element_post_message (GST_ELEMENT (sink),
                    gst_message_new_element (GST_OBJECT (sink), s));
                sink->connection_status = -1;
                sink->sent_status = 0;
                sink->disconnection_notified = 0;
            }
            return GST_FLOW_OK;
          }
        }
        GST_DEBUG_OBJECT (sink, "Opened connection to %s", sink->rtmp_uri);
      }

      GST_LOG_OBJECT (sink, "Caching first buffer of size %d for concatenation",
          GST_BUFFER_SIZE (buf));
      gst_buffer_replace (&sink->header, buf);   
      if (!sink->disconnection_notified) {
        GST_DEBUG_OBJECT (sink, "Success to reconnect to server, emitting reconnected message");
        s = gst_structure_new ("reconnected",
          "timestamp", G_TYPE_UINT64, sink->begin_time_disc, NULL);
          gst_element_post_message (GST_ELEMENT (sink),
        gst_message_new_element (GST_OBJECT (sink), s));
        sink->disconnection_notified = 1;
      }
      else if (sink->sent_status == -1 && sink->send_error_count >= 2) {
        GST_DEBUG_OBJECT (sink, "Insufficient bandwidth", sink->uri);
        s = gst_structure_new ("bandwidth",
            "timestamp", G_TYPE_UINT64, GST_BUFFER_TIMESTAMP (buf), NULL);
        gst_element_post_message (GST_ELEMENT (sink),
            gst_message_new_element (GST_OBJECT (sink), s));
        sink->send_error_count = 0;
       }
       sink->connection_status = 1;
       GST_DEBUG_OBJECT (sink, "Send back stream metadata to the server, dropping video/audio buffer");
       if (sink->stream_meta_saved)
         sink->connection_status = RTMP_Write (sink->rtmp,
           (char *) GST_BUFFER_DATA (sink->stream_metadata), GST_BUFFER_SIZE (sink->stream_metadata));
       if (sink->video_meta_saved)
         sink->connection_status = RTMP_Write (sink->rtmp,
           (char *) GST_BUFFER_DATA (sink->video_metadata), GST_BUFFER_SIZE (sink->video_metadata));
       if (sink->audio_meta_saved)
         sink->connection_status = RTMP_Write (sink->rtmp,
           (char *) GST_BUFFER_DATA (sink->audio_metadata), GST_BUFFER_SIZE (sink->audio_metadata));
    }
    else
      return GST_FLOW_OK;
    if (sink->header) {

      reffed_buf = buf = gst_buffer_join(gst_buffer_ref (sink->header),
          gst_buffer_ref (buf));
      need_unref = TRUE;
    }

    sink->first = FALSE;
    return GST_FLOW_OK;
  }

  if (sink->have_write_error)
    goto write_failed;

  if (sink->connection_status > 0) {
    GST_LOG_OBJECT (sink, "Sending %d bytes to RTMP server",
        GST_BUFFER_SIZE (buf)); 
      if (!(sink->sent_status = RTMP_Write (sink->rtmp,
                  (char *) GST_BUFFER_DATA (buf), GST_BUFFER_SIZE (buf)))) {
        GST_ELEMENT_ERROR (sink, RESOURCE, WRITE, (NULL),
            ("Allocation or flv packet too small error"));
        if (reffed_buf)
          gst_buffer_unref (reffed_buf);
        return GST_FLOW_ERROR;
      }
  }

  if (sink->sent_status == -1) {
    GST_DEBUG_OBJECT (sink, "RTMP send error");
    sink->send_error_count++;
    sink->first = TRUE;
    sink->begin_time_disc = GST_BUFFER_TIMESTAMP (buf);
    sink->try_now_connection = TRUE;
  }

  if (reffed_buf)
    gst_buffer_unref (reffed_buf);

  return GST_FLOW_OK;

init_failed:
  {
    if (sink->rtmp) {
      RTMP_Free (sink->rtmp);
      sink->rtmp = NULL;
    }
    if (sink->rtmp_uri) {
      g_free (sink->rtmp_uri);
      sink->rtmp_uri = NULL;
      sink->have_write_error = TRUE;
    }
    return GST_FLOW_ERROR;
  }

write_failed:
  {
    GST_ELEMENT_ERROR (sink, RESOURCE, WRITE, (NULL), ("Failed to write data"));
    //gst_buffer_unmap (buf, &map);
    if (need_unref)
      gst_buffer_unref (buf);
    sink->have_write_error = TRUE;
    return GST_FLOW_ERROR;
  }
}

/*
 * URI interface support.
 */
static GstURIType
gst_rtmp_sink_uri_get_type (void)
{
  return GST_URI_SINK;
}

static gchar **
gst_rtmp_sink_uri_get_protocols (void)
{
  static gchar *protocols[] =
      { (char *) "rtmp", (char *) "rtmpt", (char *) "rtmps", (char *) "rtmpe",
    (char *) "rtmfp", (char *) "rtmpte", (char *) "rtmpts", NULL
  };
  return protocols;
}

static const gchar *
gst_rtmp_sink_uri_get_uri (GstURIHandler * handler)
{
  GstRTMPSink *sink = GST_RTMP_SINK (handler);

  return sink->uri;
}

static gboolean
gst_rtmp_sink_uri_set_uri (GstURIHandler * handler, const gchar * uri)
{
  GstRTMPSink *sink = GST_RTMP_SINK (handler);
  gboolean ret = TRUE;
  gchar *real_uri;

  real_uri = !sink->is_backup ? sink->uri : sink->backup_uri;
  if (GST_STATE (sink) >= GST_STATE_PAUSED)
    return FALSE;

  g_free (real_uri);
  real_uri = NULL;

  if (uri != NULL) {
    int protocol;
    AVal host;
    unsigned int port;
    AVal playpath, app;

    if (!RTMP_ParseURL (uri, &protocol, &host, &port, &playpath, &app) ||
        !host.av_len || !playpath.av_len) {
      GST_ELEMENT_ERROR (sink, RESOURCE, OPEN_WRITE,
          ("Failed to parse URI %s", uri), (NULL));
      ret = FALSE;
    } else {
      real_uri = g_strdup (uri);
    }
    if (playpath.av_val)
      free (playpath.av_val);
  }
  if (ret)
    GST_DEBUG_OBJECT (sink, "Changed URI to %s", GST_STR_NULL (uri));
  if (!sink->is_backup)
  	sink->uri = real_uri;
  else
  	sink->backup_uri = real_uri;
  return TRUE;
}

static void
gst_rtmp_sink_uri_handler_init (gpointer g_iface, gpointer iface_data)
{
  GstURIHandlerInterface *iface = (GstURIHandlerInterface *) g_iface;

  iface->get_type = gst_rtmp_sink_uri_get_type;
  iface->get_protocols = gst_rtmp_sink_uri_get_protocols;
  iface->get_uri = gst_rtmp_sink_uri_get_uri;
  iface->set_uri = gst_rtmp_sink_uri_set_uri;
}

static void
gst_rtmp_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstRTMPSink *sink = GST_RTMP_SINK (object);

  switch (prop_id) {
    case PROP_LOCATION:
      gst_rtmp_sink_uri_set_uri (GST_URI_HANDLER (sink),
          g_value_get_string (value));
      break;
    case PROP_BACKUP_LOCATION: {
   	  sink->is_backup = TRUE;
      gst_rtmp_sink_uri_set_uri (GST_URI_HANDLER (sink),
        g_value_get_string (value));
      sink->is_backup = FALSE;
      break;
     }
    case PROP_RECONNECTION_DELAY:
      sink->reconnection_delay = g_value_get_uint64 (value);
      break;
    case PROP_TCP_TIMEOUT:
      sink->tcp_timeout = g_value_get_uint (value);
      break;
    case ARG_LOG_LEVEL:
	    RTMP_debuglevel = g_value_get_int(value);
	    break;
    case PROP_FLASHVER:
      sink->flashver = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_rtmp_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstRTMPSink *sink = GST_RTMP_SINK (object);

  switch (prop_id) {
    case PROP_LOCATION:
      g_value_set_string (value, sink->uri);
      break;
    case PROP_BACKUP_LOCATION:
      g_value_set_string (value, sink->backup_uri);
      break;
    case PROP_RECONNECTION_DELAY:
      g_value_set_uint64 (value, sink->reconnection_delay);
      break;
    case PROP_TCP_TIMEOUT:
      g_value_set_uint (value, sink->tcp_timeout);
      break;
    case ARG_LOG_LEVEL:
      g_value_set_int(value, RTMP_debuglevel);
      break;
    case PROP_FLASHVER:
       g_value_set_string (value, sink->flashver);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_rtmp_sink_setcaps (GstBaseSink * sink, GstCaps * caps)
{
  GstRTMPSink *rtmpsink = GST_RTMP_SINK (sink);
  GstStructure *s;
  const GValue *sh;
  GArray *buffers;
  gint i;

  GST_DEBUG_OBJECT (sink, "caps set to %" GST_PTR_FORMAT, caps);


  if (rtmpsink->header) {
    gst_buffer_unref (rtmpsink->header);
    rtmpsink->header = NULL;
  }

  rtmpsink->header = gst_buffer_new ();

  s = gst_caps_get_structure (caps, 0);

  sh = gst_structure_get_value (s, "streamheader");
  buffers = g_value_peek_pointer (sh);

  for (i = 0; i < buffers->len; ++i) {
    GValue *val;
    GstBuffer *buf;

    val = &g_array_index (buffers, GValue, i);
    buf = g_value_peek_pointer (val);

    gst_buffer_ref (buf);

    rtmpsink->header = gst_buffer_join (rtmpsink->header, buf);
  }

  GST_DEBUG_OBJECT (rtmpsink, "have %" G_GSIZE_FORMAT " bytes of header data",
      GST_BUFFER_SIZE (rtmpsink->header));

  return TRUE;
}

static gboolean
gst_rtmp_sink_event (GstBaseSink * sink, GstEvent * event)
{
  GstRTMPSink *rtmpsink = GST_RTMP_SINK (sink);

  switch (event->type) {
    case GST_EVENT_FLUSH_STOP:
      rtmpsink->have_write_error = FALSE;
      break;
    default:
      break;
  }

  return TRUE;
}