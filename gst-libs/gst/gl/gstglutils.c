/* 
 * GStreamer
 * Copyright (C) 2013 Matthew Waters <ystreet00@gmail.com>
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>

#include <gst/gst.h>

#include "gl.h"
#include "gstglutils.h"

#if GST_GL_HAVE_WINDOW_X11
#include <gst/gl/x11/gstgldisplay_x11.h>
#endif
#if GST_GL_HAVE_WINDOW_WAYLAND
#include <gst/gl/wayland/gstgldisplay_wayland.h>
#endif

#ifndef GL_FRAMEBUFFER_UNDEFINED
#define GL_FRAMEBUFFER_UNDEFINED          0x8219
#endif
#ifndef GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT
#define GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT 0x8CD6
#endif
#ifndef GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT
#define GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT 0x8CD7
#endif
#ifndef GL_FRAMEBUFFER_UNSUPPORTED
#define GL_FRAMEBUFFER_UNSUPPORTED        0x8CDD
#endif
#ifndef GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS
#define GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS 0x8CD9
#endif

#define USING_OPENGL(context) (gst_gl_context_check_gl_version (context, GST_GL_API_OPENGL, 1, 0))
#define USING_OPENGL3(context) (gst_gl_context_check_gl_version (context, GST_GL_API_OPENGL3, 3, 1))
#define USING_GLES(context) (gst_gl_context_check_gl_version (context, GST_GL_API_GLES, 1, 0))
#define USING_GLES2(context) (gst_gl_context_check_gl_version (context, GST_GL_API_GLES2, 2, 0))
#define USING_GLES3(context) (gst_gl_context_check_gl_version (context, GST_GL_API_GLES2, 3, 0))

static gchar *error_message;

/* called in the gl thread */
gboolean
gst_gl_context_check_framebuffer_status (GstGLContext * context)
{
  GLenum status = 0;
  status = context->gl_vtable->CheckFramebufferStatus (GL_FRAMEBUFFER);

  switch (status) {
    case GL_FRAMEBUFFER_COMPLETE:
      return TRUE;
      break;

    case GL_FRAMEBUFFER_UNSUPPORTED:
      GST_ERROR ("GL_FRAMEBUFFER_UNSUPPORTED");
      break;

    case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
      GST_ERROR ("GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT");
      break;

    case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
      GST_ERROR ("GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT");
      break;
    case GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS:
      GST_ERROR ("GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS");
      break;
#if GST_GL_HAVE_OPENGL
    case GL_FRAMEBUFFER_UNDEFINED:
      GST_ERROR ("GL_FRAMEBUFFER_UNDEFINED");
      break;
#endif
    default:
      GST_ERROR ("General FBO error");
  }

  return FALSE;
}

typedef struct _GenTexture
{
  guint width, height;
  GstVideoFormat format;
  guint result;
} GenTexture;

static void
_gen_texture (GstGLContext * context, GenTexture * data)
{
  const GstGLFuncs *gl = context->gl_vtable;
  GLenum internal_format;

  GST_TRACE ("Generating texture format:%u dimensions:%ux%u", data->format,
      data->width, data->height);

  gl->GenTextures (1, &data->result);
  gl->BindTexture (GL_TEXTURE_2D, data->result);

  internal_format =
      gst_gl_sized_gl_format_from_gl_format_type (context, GL_RGBA,
      GL_UNSIGNED_BYTE);
  if (data->width > 0 && data->height > 0)
    gl->TexImage2D (GL_TEXTURE_2D, 0, internal_format,
        data->width, data->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

  gl->TexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl->TexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl->TexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl->TexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  GST_LOG ("generated texture id:%d", data->result);
}

/* deprecated. replaced by GstGLMemory */
void
gst_gl_context_gen_texture (GstGLContext * context, GLuint * pTexture,
    GstVideoFormat v_format, GLint width, GLint height)
{
  GenTexture data = { width, height, v_format, 0 };

  gst_gl_context_thread_add (context, (GstGLContextThreadFunc) _gen_texture,
      &data);

  *pTexture = data.result;
}

static void
_del_texture (GstGLContext * context, guint * texture)
{
  context->gl_vtable->DeleteTextures (1, texture);
}

/* deprecated. replaced by GstGLMemory */
void
gst_gl_context_del_texture (GstGLContext * context, GLuint * pTexture)
{
  gst_gl_context_thread_add (context, (GstGLContextThreadFunc) _del_texture,
      pTexture);
}

typedef struct _GenTextureFull
{
  const GstVideoInfo *info;
  const gint comp;
  guint result;
} GenTextureFull;

static void
_gen_texture_full (GstGLContext * context, GenTextureFull * data)
{
  const GstGLFuncs *gl = context->gl_vtable;
  GstVideoGLTextureType tex_type;
  GstVideoFormat v_format;
  GLint glinternalformat = 0;
  GLenum glformat = 0;
  GLenum gltype = 0;

  gl->GenTextures (1, &data->result);
  gl->BindTexture (GL_TEXTURE_2D, data->result);

  v_format = GST_VIDEO_INFO_FORMAT (data->info);
  tex_type = gst_gl_texture_type_from_format (context, v_format, data->comp);
  glformat = gst_gl_format_from_gl_texture_type (tex_type);
  gltype = GL_UNSIGNED_BYTE;
  if (v_format == GST_VIDEO_FORMAT_RGB16)
    gltype = GL_UNSIGNED_SHORT_5_6_5;
  glinternalformat = gst_gl_sized_gl_format_from_gl_format_type (context,
      glformat, gltype);

  gl->TexImage2D (GL_TEXTURE_2D, 0, glinternalformat,
      GST_VIDEO_INFO_COMP_WIDTH (data->info, data->comp),
      GST_VIDEO_INFO_COMP_HEIGHT (data->info, data->comp), 0, glformat, gltype,
      NULL);

  gl->TexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl->TexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl->TexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl->TexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* deprecated. replaced by GstGLMemory */
void
gst_gl_generate_texture_full (GstGLContext * context, const GstVideoInfo * info,
    const guint comp, gint stride[], gsize offset[], gsize size[],
    GLuint * pTexture)
{
  GenTextureFull data = { info, comp, 0 };

  switch (GST_VIDEO_INFO_FORMAT (info)) {
    case GST_VIDEO_FORMAT_RGB:
    case GST_VIDEO_FORMAT_BGR:
    {
      stride[0] = GST_ROUND_UP_4 (GST_VIDEO_INFO_WIDTH (info) * 3);
      offset[0] = 0;
      size[0] = stride[0] * GST_VIDEO_INFO_HEIGHT (info);
      break;
    }
    case GST_VIDEO_FORMAT_RGB16:
    {
      stride[0] = GST_ROUND_UP_4 (GST_VIDEO_INFO_WIDTH (info) * 2);
      offset[0] = 0;
      size[0] = stride[0] * GST_VIDEO_INFO_HEIGHT (info);
      break;
    }
    case GST_VIDEO_FORMAT_RGBA:
    case GST_VIDEO_FORMAT_BGRA:
    case GST_VIDEO_FORMAT_ARGB:
    case GST_VIDEO_FORMAT_ABGR:
    case GST_VIDEO_FORMAT_RGBx:
    case GST_VIDEO_FORMAT_BGRx:
    case GST_VIDEO_FORMAT_xRGB:
    case GST_VIDEO_FORMAT_xBGR:
    case GST_VIDEO_FORMAT_AYUV:
    {
      stride[0] = GST_ROUND_UP_4 (GST_VIDEO_INFO_WIDTH (info) * 4);
      offset[0] = 0;
      size[0] = stride[0] * GST_VIDEO_INFO_HEIGHT (info);
      break;
    }
    case GST_VIDEO_FORMAT_NV12:
    case GST_VIDEO_FORMAT_NV21:
    {
      size[comp] = stride[comp] * GST_VIDEO_INFO_COMP_HEIGHT (info, comp);
      if (comp == 0) {
        stride[0] = GST_ROUND_UP_4 (GST_VIDEO_INFO_COMP_WIDTH (info, 1));
        offset[0] = 0;
      } else {
        stride[1] = GST_ROUND_UP_4 (GST_VIDEO_INFO_COMP_WIDTH (info, 1) * 2);
        offset[1] = size[0];
      }
      break;
    }
    case GST_VIDEO_FORMAT_I420:
    case GST_VIDEO_FORMAT_YV12:
    case GST_VIDEO_FORMAT_Y444:
    case GST_VIDEO_FORMAT_Y42B:
    case GST_VIDEO_FORMAT_Y41B:
    {
      stride[comp] = GST_ROUND_UP_4 (GST_VIDEO_INFO_COMP_WIDTH (info, comp));
      size[comp] = stride[comp] * GST_VIDEO_INFO_COMP_HEIGHT (info, comp);
      if (comp == 0)
        offset[0] = 0;
      else if (comp == 1)
        offset[1] = size[0];
      else
        offset[2] = offset[1] + size[1];
      break;
    }
    default:
      GST_WARNING ("unsupported %s",
          gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (info)));
      break;
  }

  gst_gl_context_thread_add (context,
      (GstGLContextThreadFunc) _gen_texture_full, &data);

  *pTexture = data.result;
}

typedef struct _GenFBO
{
  GstGLFramebuffer *frame;
  gint width, height;
  GLuint *fbo, *depth;
} GenFBO;

static void
_gen_fbo (GstGLContext * context, GenFBO * data)
{
  gst_gl_framebuffer_generate (data->frame, data->width, data->height,
      data->fbo, data->depth);
}

gboolean
gst_gl_context_gen_fbo (GstGLContext * context, gint width, gint height,
    GLuint * fbo, GLuint * depthbuffer)
{
  GstGLFramebuffer *frame = gst_gl_framebuffer_new (context);

  GenFBO data = { frame, width, height, fbo, depthbuffer };

  gst_gl_context_thread_add (context, (GstGLContextThreadFunc) _gen_fbo, &data);

  gst_object_unref (frame);

  return TRUE;
}

typedef struct _UseFBO2
{
  GstGLFramebuffer *frame;
  gint texture_fbo_width;
  gint texture_fbo_height;
  GLuint fbo;
  GLuint depth_buffer;
  GLuint texture_fbo;
  GLCB_V2 cb;
  gpointer stuff;
} UseFBO2;

static void
_use_fbo_v2 (GstGLContext * context, UseFBO2 * data)
{
  gst_gl_framebuffer_use_v2 (data->frame, data->texture_fbo_width,
      data->texture_fbo_height, data->fbo, data->depth_buffer,
      data->texture_fbo, data->cb, data->stuff);
}

gboolean
gst_gl_context_use_fbo_v2 (GstGLContext * context, gint texture_fbo_width,
    gint texture_fbo_height, GLuint fbo, GLuint depth_buffer,
    GLuint texture_fbo, GLCB_V2 cb, gpointer stuff)
{
  GstGLFramebuffer *frame = gst_gl_framebuffer_new (context);

  UseFBO2 data =
      { frame, texture_fbo_width, texture_fbo_height, fbo, depth_buffer,
    texture_fbo, cb, stuff
  };

  gst_gl_context_thread_add (context, (GstGLContextThreadFunc) _use_fbo_v2,
      &data);

  gst_object_unref (frame);

  return TRUE;
}

typedef struct _DelFBO
{
  GstGLFramebuffer *frame;
  GLuint fbo;
  GLuint depth;
} DelFBO;

/* Called in the gl thread */
static void
_del_fbo (GstGLContext * context, DelFBO * data)
{
  gst_gl_framebuffer_delete (data->frame, data->fbo, data->depth);
}

/* Called by gltestsrc and glfilter */
void
gst_gl_context_del_fbo (GstGLContext * context, GLuint fbo, GLuint depth_buffer)
{
  GstGLFramebuffer *frame = gst_gl_framebuffer_new (context);

  DelFBO data = { frame, fbo, depth_buffer };

  gst_gl_context_thread_add (context, (GstGLContextThreadFunc) _del_fbo, &data);

  gst_object_unref (frame);
}

static void
_compile_shader (GstGLContext * context, GstGLShader ** shader)
{
  GError *error = NULL;

  gst_gl_shader_compile (*shader, &error);
  if (error) {
    gst_gl_context_set_error (context, "%s", error->message);
    g_error_free (error);
    error = NULL;
    gst_gl_context_clear_shader (context);
    gst_object_unref (*shader);
    *shader = NULL;
  }
}

/* Called by glfilter */
gboolean
gst_gl_context_gen_shader (GstGLContext * context, const gchar * vert_src,
    const gchar * frag_src, GstGLShader ** shader)
{
  g_return_val_if_fail (frag_src != NULL || vert_src != NULL, FALSE);
  g_return_val_if_fail (shader != NULL, FALSE);

  *shader = gst_gl_shader_new (context);

  if (frag_src)
    gst_gl_shader_set_fragment_source (*shader, frag_src);
  if (vert_src)
    gst_gl_shader_set_vertex_source (*shader, vert_src);

  gst_gl_context_thread_add (context, (GstGLContextThreadFunc) _compile_shader,
      shader);

  return *shader != NULL;
}

void
gst_gl_context_set_error (GstGLContext * context, const char *format, ...)
{
  va_list args;

  if (error_message)
    g_free (error_message);

  va_start (args, format);
  error_message = g_strdup_vprintf (format, args);
  va_end (args);

  GST_WARNING ("%s", error_message);
}

gchar *
gst_gl_context_get_error (void)
{
  return error_message;
}

/* Called by glfilter */
void
gst_gl_context_del_shader (GstGLContext * context, GstGLShader * shader)
{
  gst_object_unref (shader);
}

static gboolean
gst_gl_display_found (GstElement * element, GstGLDisplay * display)
{
  if (display) {
    GST_LOG_OBJECT (element, "already have a display (%p)", display);
    return TRUE;
  }

  return FALSE;
}

GST_DEBUG_CATEGORY_STATIC (GST_CAT_CONTEXT);

static gboolean
pad_query (const GValue * item, GValue * value, gpointer user_data)
{
  GstPad *pad = g_value_get_object (item);
  GstQuery *query = user_data;
  gboolean res;

  res = gst_pad_peer_query (pad, query);

  if (res) {
    g_value_set_boolean (value, TRUE);
    return FALSE;
  }

  GST_CAT_INFO_OBJECT (GST_CAT_CONTEXT, pad, "pad peer query failed");
  return TRUE;
}

gboolean
gst_gl_run_query (GstElement * element, GstQuery * query,
    GstPadDirection direction)
{
  GstIterator *it;
  GstIteratorFoldFunction func = pad_query;
  GValue res = { 0 };

  g_value_init (&res, G_TYPE_BOOLEAN);
  g_value_set_boolean (&res, FALSE);

  /* Ask neighbor */
  if (direction == GST_PAD_SRC)
    it = gst_element_iterate_src_pads (element);
  else
    it = gst_element_iterate_sink_pads (element);

  while (gst_iterator_fold (it, func, &res, query) == GST_ITERATOR_RESYNC)
    gst_iterator_resync (it);

  gst_iterator_free (it);

  return g_value_get_boolean (&res);
}

static GstQuery *
_gst_context_query (GstElement * element,
    gpointer ptr, const gchar * display_type)
{
  GstQuery *query;
  GstContext *ctxt;

  /*  2a) Query downstream with GST_QUERY_CONTEXT for the context and
   *      check if downstream already has a context of the specific type
   *  2b) Query upstream as above.
   */
  query = gst_query_new_context (display_type);
  if (gst_gl_run_query (element, query, GST_PAD_SRC)) {
    gst_query_parse_context (query, &ctxt);
    GST_CAT_INFO_OBJECT (GST_CAT_CONTEXT, element,
        "found context (%p) in downstream query", ctxt);
  } else if (gst_gl_run_query (element, query, GST_PAD_SINK)) {
    gst_query_parse_context (query, &ctxt);
    GST_CAT_INFO_OBJECT (GST_CAT_CONTEXT, element,
        "found context (%p) in upstream query", ctxt);
  } else {
    /* 3) Post a GST_MESSAGE_NEED_CONTEXT message on the bus with
     *    the required context type and afterwards check if a
     *    usable context was set now as in 1). The message could
     *    be handled by the parent bins of the element and the
     *    application.
     */
    GstMessage *msg;

    GST_CAT_INFO_OBJECT (GST_CAT_CONTEXT, element,
        "posting need context message");
    msg = gst_message_new_need_context (GST_OBJECT_CAST (element),
        display_type);
    gst_element_post_message (element, msg);
  }

  /*
   * Whomever responds to the need-context message performs a
   * GstElement::set_context() with the required context in which the element
   * is required to update the display_ptr or call gst_gl_handle_set_context().
   */

  return query;
}

static void
gst_gl_display_context_query (GstElement * element, GstGLDisplay ** display_ptr)
{
  GstContext *ctxt = NULL;
  GstQuery *query = NULL;

#ifndef GST_DISABLE_GST_DEBUG
  if (!GST_CAT_CONTEXT)
    GST_DEBUG_CATEGORY_GET (GST_CAT_CONTEXT, "GST_CONTEXT");
#endif

  query =
      _gst_context_query (element, display_ptr, GST_GL_DISPLAY_CONTEXT_TYPE);
  gst_query_parse_context (query, &ctxt);

  if (ctxt && gst_context_has_context_type (ctxt, GST_GL_DISPLAY_CONTEXT_TYPE)) {
    GstGLDisplay *tmp_disp = NULL;
    if (gst_context_get_gl_display (ctxt, &tmp_disp) && tmp_disp)
      *display_ptr = tmp_disp;
  }

  if (*display_ptr)
    goto out;

#if GST_GL_HAVE_WINDOW_X11
  gst_query_unref (query);
  query = _gst_context_query (element, display_ptr, "gst.x11.display.handle");
  gst_query_parse_context (query, &ctxt);
  if (ctxt && gst_context_has_context_type (ctxt, "gst.x11.display.handle")) {
    const GstStructure *s;
    Display *display;

    s = gst_context_get_structure (ctxt);
    if (gst_structure_get (s, "display", G_TYPE_POINTER, &display, NULL)
        && display) {
      *display_ptr =
          (GstGLDisplay *) gst_gl_display_x11_new_with_display (display);
    }
  }

  if (*display_ptr)
    goto out;
#endif

#if GST_GL_HAVE_WINDOW_WAYLAND
  gst_query_unref (query);
  query =
      _gst_context_query (element, display_ptr,
      "GstWaylandDisplayHandleContextType");
  gst_query_parse_context (query, &ctxt);
  if (ctxt
      && gst_context_has_context_type (ctxt,
          "GstWaylandDisplayHandleContextType")) {
    const GstStructure *s;
    struct wl_display *display;

    s = gst_context_get_structure (ctxt);
    if (gst_structure_get (s, "display", G_TYPE_POINTER, &display, NULL)
        && display) {
      *display_ptr =
          (GstGLDisplay *) gst_gl_display_wayland_new_with_display (display);
    }
  }

  if (*display_ptr)
    goto out;
#endif

out:
  gst_query_unref (query);
}

static void
gst_gl_context_query (GstElement * element, GstGLContext ** context_ptr)
{
  GstContext *ctxt;
  GstQuery *query;

#ifndef GST_DISABLE_GST_DEBUG
  if (!GST_CAT_CONTEXT)
    GST_DEBUG_CATEGORY_GET (GST_CAT_CONTEXT, "GST_CONTEXT");
#endif

  query = _gst_context_query (element, context_ptr, "gst.gl.app_context");
  gst_query_parse_context (query, &ctxt);
  if (ctxt && gst_context_has_context_type (ctxt, "gst.gl.app_context")) {
    const GstStructure *s = gst_context_get_structure (ctxt);
    GstGLContext *tmp_ctx = NULL;
    if (gst_structure_get (s, "context", GST_GL_TYPE_CONTEXT, &tmp_ctx, NULL)
        && tmp_ctx)
      *context_ptr = tmp_ctx;
  }

  gst_query_unref (query);
}

/*  4) Create a context by itself and post a GST_MESSAGE_HAVE_CONTEXT
 *     message.
 */
static void
gst_gl_display_context_propagate (GstElement * element, GstGLDisplay * display)
{
  GstContext *context;
  GstMessage *msg;

  if (!display) {
    GST_ERROR_OBJECT (element, "Could not get GL display connection");
    return;
  }

  context = gst_context_new (GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
  gst_context_set_gl_display (context, display);

  GST_CAT_INFO_OBJECT (GST_CAT_CONTEXT, element,
      "posting have context (%p) message with display (%p)", context, display);
  msg = gst_message_new_have_context (GST_OBJECT_CAST (element), context);
  gst_element_post_message (GST_ELEMENT_CAST (element), msg);
}

gboolean
gst_gl_ensure_element_data (gpointer element, GstGLDisplay ** display_ptr,
    GstGLContext ** context_ptr)
{
  GstGLDisplay *display;

  g_return_val_if_fail (element != NULL, FALSE);
  g_return_val_if_fail (display_ptr != NULL, FALSE);
  g_return_val_if_fail (context_ptr != NULL, FALSE);

  /*  1) Check if the element already has a context of the specific
   *     type.
   */
  display = *display_ptr;
  if (gst_gl_display_found (element, display))
    goto done;

  gst_gl_display_context_query (element, display_ptr);

  /* Neighbour found and it updated the display */
  if (gst_gl_display_found (element, *display_ptr))
    goto get_gl_context;

  /* If no neighboor, or application not interested, use system default */
  display = gst_gl_display_new ();

  *display_ptr = display;

  gst_gl_display_context_propagate (element, display);

get_gl_context:
  if (*context_ptr)
    goto done;

  gst_gl_context_query (element, context_ptr);

done:
  return *display_ptr != NULL;
}

gboolean
gst_gl_handle_set_context (GstElement * element, GstContext * context,
    GstGLDisplay ** display, GstGLContext ** other_context)
{
  GstGLDisplay *display_replacement = NULL;
  GstGLContext *context_replacement = NULL;
  const gchar *context_type;

  g_return_val_if_fail (display != NULL, FALSE);
  g_return_val_if_fail (other_context != NULL, FALSE);

  if (!context)
    return FALSE;

  context_type = gst_context_get_context_type (context);

  if (g_strcmp0 (context_type, GST_GL_DISPLAY_CONTEXT_TYPE) == 0) {
    if (!gst_context_get_gl_display (context, &display_replacement)) {
      GST_WARNING_OBJECT (element, "Failed to get display from context");
      return FALSE;
    }
  }
#if GST_GL_HAVE_WINDOW_X11
  else if (g_strcmp0 (context_type, "gst.x11.display.handle") == 0) {
    const GstStructure *s;
    Display *display;

    s = gst_context_get_structure (context);
    if (gst_structure_get (s, "display", G_TYPE_POINTER, &display, NULL))
      display_replacement =
          (GstGLDisplay *) gst_gl_display_x11_new_with_display (display);
  }
#endif
#if GST_GL_HAVE_WINDOW_WAYLAND
  else if (g_strcmp0 (context_type, "GstWaylandDisplayHandleContextType") == 0) {
    const GstStructure *s;
    struct wl_display *display;

    s = gst_context_get_structure (context);
    if (gst_structure_get (s, "display", G_TYPE_POINTER, &display, NULL))
      display_replacement =
          (GstGLDisplay *) gst_gl_display_wayland_new_with_display (display);
  }
#endif
  else if (g_strcmp0 (context_type, "gst.gl.app_context") == 0) {
    const GstStructure *s = gst_context_get_structure (context);
    GstGLDisplay *context_display;
    GstGLDisplay *element_display;

    if (gst_structure_get (s, "context", GST_GL_TYPE_CONTEXT,
            &context_replacement, NULL)) {
      context_display = gst_gl_context_get_display (context_replacement);
      element_display = display_replacement ? display_replacement : *display;
      if (element_display
          && (gst_gl_display_get_handle_type (element_display) &
              gst_gl_display_get_handle_type (context_display)) == 0) {
        GST_ELEMENT_WARNING (element, LIBRARY, SETTINGS, ("%s",
                "Cannot set a GL context with a different display type"), ("%s",
                "Cannot set a GL context with a different display type"));
        gst_object_unref (context_replacement);
        context_replacement = NULL;
      }
      gst_object_unref (context_display);
    }
  }

  if (display_replacement) {
    GstGLDisplay *old = *display;
    *display = display_replacement;

    if (old)
      gst_object_unref (old);
  }

  if (context_replacement) {
    GstGLContext *old = *other_context;
    *other_context = context_replacement;

    if (old)
      gst_object_unref (old);
  }

  return TRUE;
}

gboolean
gst_gl_handle_context_query (GstElement * element, GstQuery * query,
    GstGLDisplay ** display, GstGLContext ** other_context)
{
  gboolean res = FALSE;
  const gchar *context_type;
  GstContext *context, *old_context;

  g_return_val_if_fail (element != NULL, FALSE);
  g_return_val_if_fail (query != NULL, FALSE);
  g_return_val_if_fail (display != NULL, FALSE);
  g_return_val_if_fail (other_context != NULL, FALSE);

  gst_query_parse_context_type (query, &context_type);

  if (g_strcmp0 (context_type, GST_GL_DISPLAY_CONTEXT_TYPE) == 0) {

    gst_query_parse_context (query, &old_context);

    if (old_context)
      context = gst_context_copy (old_context);
    else
      context = gst_context_new (GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);

    gst_context_set_gl_display (context, *display);
    gst_query_set_context (query, context);
    gst_context_unref (context);

    res = *display != NULL;
  }
#if GST_GL_HAVE_WINDOW_X11
  else if (g_strcmp0 (context_type, "gst.x11.display.handle") == 0) {
    GstStructure *s;
    Display *x11_display = NULL;

    gst_query_parse_context (query, &old_context);

    if (old_context)
      context = gst_context_copy (old_context);
    else
      context = gst_context_new ("gst.x11.display.handle", TRUE);

    if (*display
        && ((*display)->type & GST_GL_DISPLAY_TYPE_X11) ==
        GST_GL_DISPLAY_TYPE_X11)
      x11_display = (Display *) gst_gl_display_get_handle (*display);

    s = gst_context_writable_structure (context);
    gst_structure_set (s, "display", G_TYPE_POINTER, x11_display, NULL);

    gst_query_set_context (query, context);
    gst_context_unref (context);

    res = x11_display != NULL;
  }
#endif
#if GST_GL_HAVE_WINDOW_WAYLAND
  else if (g_strcmp0 (context_type, "GstWaylandDisplayHandleContextType") == 0) {
    GstStructure *s;
    struct wl_display *wayland_display = NULL;

    gst_query_parse_context (query, &old_context);

    if (old_context)
      context = gst_context_copy (old_context);
    else
      context = gst_context_new ("GstWaylandDisplayHandleContextType", TRUE);

    if (*display
        && ((*display)->type & GST_GL_DISPLAY_TYPE_WAYLAND) ==
        GST_GL_DISPLAY_TYPE_WAYLAND)
      wayland_display =
          (struct wl_display *) gst_gl_display_get_handle (*display);

    s = gst_context_writable_structure (context);
    gst_structure_set (s, "display", G_TYPE_POINTER, wayland_display, NULL);

    gst_query_set_context (query, context);
    gst_context_unref (context);

    res = wayland_display != NULL;
  }
#endif
  else if (g_strcmp0 (context_type, "gst.gl.app_context") == 0) {
    GstStructure *s;

    gst_query_parse_context (query, &old_context);

    if (old_context)
      context = gst_context_copy (old_context);
    else
      context = gst_context_new ("gst.gl.app_context", TRUE);

    s = gst_context_writable_structure (context);
    gst_structure_set (s, "context", GST_GL_TYPE_CONTEXT, *other_context, NULL);
    gst_query_set_context (query, context);
    gst_context_unref (context);

    res = *other_context != NULL;
  }

  return res;
}

gsize
gst_gl_get_plane_data_size (GstVideoInfo * info, GstVideoAlignment * align,
    guint plane)
{
  gint padded_height;
  gsize plane_size;

  padded_height = info->height;

  if (align)
    padded_height += align->padding_top + align->padding_bottom;

  padded_height =
      GST_VIDEO_FORMAT_INFO_SCALE_HEIGHT (info->finfo, plane, padded_height);

  plane_size = GST_VIDEO_INFO_PLANE_STRIDE (info, plane) * padded_height;

  return plane_size;
}

GstCaps *
gst_gl_caps_replace_all_caps_features (const GstCaps * caps,
    const gchar * feature_name)
{
  GstCaps *tmp = gst_caps_copy (caps);
  guint n = gst_caps_get_size (tmp);
  guint i = 0;

  for (i = 0; i < n; i++) {
    GstCapsFeatures *features = gst_caps_get_features (tmp, i);
    if (features) {
      guint n_f = gst_caps_features_get_size (features);
      guint j = 0;
      for (j = 0; j < n_f; j++) {
        gst_caps_features_remove_id (features,
            gst_caps_features_get_nth_id (features, j));
      }
    }

    gst_caps_features_add (features, feature_name);
  }

  return tmp;
}
