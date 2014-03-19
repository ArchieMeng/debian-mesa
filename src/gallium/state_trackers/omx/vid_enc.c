/**************************************************************************
 *
 * Copyright 2013 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

/*
 * Authors:
 *      Christian König <christian.koenig@amd.com>
 *
 */


#include <assert.h>

#include <OMX_Video.h>

/* bellagio defines a DEBUG macro that we don't want */
#ifndef DEBUG
#include <bellagio/omxcore.h>
#undef DEBUG
#else
#include <bellagio/omxcore.h>
#endif

#include <bellagio/omx_base_video_port.h>

#include "pipe/p_screen.h"
#include "pipe/p_video_codec.h"
#include "state_tracker/drm_driver.h"
#include "util/u_memory.h"

#include "entrypoint.h"
#include "vid_enc.h"

struct input_buf_private {
   struct pipe_video_buffer *buf;
   struct pipe_resource *bitstream;
   void *feedback;
};

struct output_buf_private {
   struct pipe_resource *bitstream;
   struct pipe_transfer *transfer;
};

static OMX_ERRORTYPE vid_enc_Constructor(OMX_COMPONENTTYPE *comp, OMX_STRING name);
static OMX_ERRORTYPE vid_enc_Destructor(OMX_COMPONENTTYPE *comp);
static OMX_ERRORTYPE vid_enc_SetParameter(OMX_HANDLETYPE handle, OMX_INDEXTYPE idx, OMX_PTR param);
static OMX_ERRORTYPE vid_enc_GetParameter(OMX_HANDLETYPE handle, OMX_INDEXTYPE idx, OMX_PTR param);
static OMX_ERRORTYPE vid_enc_SetConfig(OMX_HANDLETYPE handle, OMX_INDEXTYPE idx, OMX_PTR config);
static OMX_ERRORTYPE vid_enc_GetConfig(OMX_HANDLETYPE handle, OMX_INDEXTYPE idx, OMX_PTR config);
static OMX_ERRORTYPE vid_enc_MessageHandler(OMX_COMPONENTTYPE *comp, internalRequestMessageType *msg);
static OMX_ERRORTYPE vid_enc_AllocateInBuffer(omx_base_PortType *port, OMX_INOUT OMX_BUFFERHEADERTYPE **buf,
                                              OMX_IN OMX_U32 idx, OMX_IN OMX_PTR private, OMX_IN OMX_U32 size);
static OMX_ERRORTYPE vid_enc_UseInBuffer(omx_base_PortType *port, OMX_BUFFERHEADERTYPE **buf, OMX_U32 idx,
                                         OMX_PTR private, OMX_U32 size, OMX_U8 *mem);
static OMX_ERRORTYPE vid_enc_FreeInBuffer(omx_base_PortType *port, OMX_U32 idx, OMX_BUFFERHEADERTYPE *buf);
static OMX_ERRORTYPE vid_enc_EncodeFrame(omx_base_PortType *port, OMX_BUFFERHEADERTYPE *buf);
static OMX_ERRORTYPE vid_enc_AllocateOutBuffer(omx_base_PortType *comp, OMX_INOUT OMX_BUFFERHEADERTYPE **buf,
                                               OMX_IN OMX_U32 idx, OMX_IN OMX_PTR private, OMX_IN OMX_U32 size);
static OMX_ERRORTYPE vid_enc_FreeOutBuffer(omx_base_PortType *port, OMX_U32 idx, OMX_BUFFERHEADERTYPE *buf);
static void vid_enc_BufferEncoded(OMX_COMPONENTTYPE *comp, OMX_BUFFERHEADERTYPE* input, OMX_BUFFERHEADERTYPE* output);

static void vid_enc_name(char str[OMX_MAX_STRINGNAME_SIZE])
{
   snprintf(str, OMX_MAX_STRINGNAME_SIZE, OMX_VID_ENC_BASE_NAME, driver_descriptor.name);
}

static void vid_enc_name_avc(char str[OMX_MAX_STRINGNAME_SIZE])
{
   snprintf(str, OMX_MAX_STRINGNAME_SIZE, OMX_VID_ENC_AVC_NAME, driver_descriptor.name);
}

OMX_ERRORTYPE vid_enc_LoaderComponent(stLoaderComponentType *comp)
{
   comp->componentVersion.s.nVersionMajor = 0;
   comp->componentVersion.s.nVersionMinor = 0;
   comp->componentVersion.s.nRevision = 0;
   comp->componentVersion.s.nStep = 1;
   comp->name_specific_length = 1;
   comp->constructor = vid_enc_Constructor;

   comp->name = CALLOC(1, OMX_MAX_STRINGNAME_SIZE);
   if (!comp->name)
      return OMX_ErrorInsufficientResources;

   vid_enc_name(comp->name);

   comp->name_specific = CALLOC(1, sizeof(char *));
   if (!comp->name_specific)
      goto error_arrays;

   comp->role_specific = CALLOC(1, sizeof(char *));
   if (!comp->role_specific)
      goto error_arrays;

   comp->name_specific[0] = CALLOC(1, OMX_MAX_STRINGNAME_SIZE);
   if (!comp->name_specific[0])
      goto error_specific;

   vid_enc_name_avc(comp->name_specific[0]);

   comp->role_specific[0] = CALLOC(1, OMX_MAX_STRINGNAME_SIZE);
   if (!comp->role_specific[0])
      goto error_specific;

   strcpy(comp->role_specific[0], OMX_VID_ENC_AVC_ROLE);

   return OMX_ErrorNone;

error_specific:
   FREE(comp->name_specific[0]);
   FREE(comp->role_specific[0]);

error_arrays:
   FREE(comp->role_specific);
   FREE(comp->name_specific);
   FREE(comp->name);

   return OMX_ErrorInsufficientResources;
}

static OMX_ERRORTYPE vid_enc_Constructor(OMX_COMPONENTTYPE *comp, OMX_STRING name)
{
   vid_enc_PrivateType *priv;
   omx_base_video_PortType *port;
   struct pipe_screen *screen;
   OMX_ERRORTYPE r;
   int i;

   assert(!comp->pComponentPrivate);

   priv = comp->pComponentPrivate = CALLOC(1, sizeof(vid_enc_PrivateType));
   if (!priv)
      return OMX_ErrorInsufficientResources;

   r = omx_base_filter_Constructor(comp, name);
   if (r)
	return r;

   priv->BufferMgmtCallback = vid_enc_BufferEncoded;
   priv->messageHandler = vid_enc_MessageHandler;
   priv->destructor = vid_enc_Destructor;

   comp->SetParameter = vid_enc_SetParameter;
   comp->GetParameter = vid_enc_GetParameter;
   comp->GetConfig = vid_enc_GetConfig;
   comp->SetConfig = vid_enc_SetConfig;

   priv->screen = omx_get_screen();
   if (!priv->screen)
      return OMX_ErrorInsufficientResources;

   screen = priv->screen->pscreen;
   if (!screen->get_video_param(screen, PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH,
                                PIPE_VIDEO_ENTRYPOINT_ENCODE, PIPE_VIDEO_CAP_SUPPORTED))
      return OMX_ErrorBadParameter;
 
   priv->s_pipe = screen->context_create(screen, priv->screen);
   if (!priv->s_pipe)
      return OMX_ErrorInsufficientResources;

   if (!vl_compositor_init(&priv->compositor, priv->s_pipe)) {
      priv->s_pipe->destroy(priv->s_pipe);
      priv->s_pipe = NULL;
      return OMX_ErrorInsufficientResources;
   }

   if (!vl_compositor_init_state(&priv->cstate, priv->s_pipe)) {
      vl_compositor_cleanup(&priv->compositor);
      priv->s_pipe->destroy(priv->s_pipe);
      priv->s_pipe = NULL;
      return OMX_ErrorInsufficientResources;
   }

   priv->t_pipe = screen->context_create(screen, priv->screen);
   if (!priv->t_pipe)
      return OMX_ErrorInsufficientResources;

   priv->sPortTypesParam[OMX_PortDomainVideo].nStartPortNumber = 0;
   priv->sPortTypesParam[OMX_PortDomainVideo].nPorts = 2;
   priv->ports = CALLOC(2, sizeof(omx_base_PortType *));
   if (!priv->ports)
      return OMX_ErrorInsufficientResources;

   for (i = 0; i < 2; ++i) {
      priv->ports[i] = CALLOC(1, sizeof(omx_base_video_PortType));
      if (!priv->ports[i])
         return OMX_ErrorInsufficientResources;

      base_video_port_Constructor(comp, &priv->ports[i], i, i == 0);
   }

   port = (omx_base_video_PortType *)priv->ports[OMX_BASE_FILTER_INPUTPORT_INDEX];
   port->sPortParam.format.video.nFrameWidth = 176;
   port->sPortParam.format.video.nFrameHeight = 144;
   port->sPortParam.format.video.eColorFormat = OMX_COLOR_FormatYUV420SemiPlanar;
   port->sVideoParam.eColorFormat = OMX_COLOR_FormatYUV420SemiPlanar;
   port->sPortParam.nBufferCountActual = 8;
   port->sPortParam.nBufferCountMin = 4;

   port->Port_SendBufferFunction = vid_enc_EncodeFrame;
   port->Port_AllocateBuffer = vid_enc_AllocateInBuffer;
   port->Port_UseBuffer = vid_enc_UseInBuffer;
   port->Port_FreeBuffer = vid_enc_FreeInBuffer;

   port = (omx_base_video_PortType *)priv->ports[OMX_BASE_FILTER_OUTPUTPORT_INDEX];
   strcpy(port->sPortParam.format.video.cMIMEType,"video/H264");
   port->sPortParam.format.video.nFrameWidth = 176;
   port->sPortParam.format.video.nFrameHeight = 144;
   port->sPortParam.format.video.eCompressionFormat = OMX_VIDEO_CodingAVC;
   port->sVideoParam.eCompressionFormat = OMX_VIDEO_CodingAVC;

   port->Port_AllocateBuffer = vid_enc_AllocateOutBuffer;
   port->Port_FreeBuffer = vid_enc_FreeOutBuffer;
 
   priv->bitrate.eControlRate = OMX_Video_ControlRateDisable;
   priv->bitrate.nTargetBitrate = 0;

   priv->quant.nQpI = OMX_VID_ENC_QUANT_I_FRAMES_DEFAULT;
   priv->quant.nQpP = OMX_VID_ENC_QUANT_P_FRAMES_DEFAULT;
   priv->quant.nQpB = OMX_VID_ENC_QUANT_B_FRAMES_DEFAULT;

   priv->force_pic_type.IntraRefreshVOP = OMX_FALSE; 
   priv->frame_num = 0;

   priv->scale.xWidth = OMX_VID_ENC_SCALING_WIDTH_DEFAULT;
   priv->scale.xHeight = OMX_VID_ENC_SCALING_WIDTH_DEFAULT;

   return OMX_ErrorNone;
}

static OMX_ERRORTYPE vid_enc_Destructor(OMX_COMPONENTTYPE *comp)
{
   vid_enc_PrivateType* priv = comp->pComponentPrivate;
   int i;

   if (priv->ports) {
      for (i = 0; i < priv->sPortTypesParam[OMX_PortDomainVideo].nPorts; ++i) {
         if(priv->ports[i])
            priv->ports[i]->PortDestructor(priv->ports[i]);
      }
      FREE(priv->ports);
      priv->ports=NULL;
   }

   for (i = 0; i < OMX_VID_ENC_NUM_SCALING_BUFFERS; ++i)
      if (priv->scale_buffer[i])
         priv->scale_buffer[i]->destroy(priv->scale_buffer[i]);

   if (priv->s_pipe) {
      vl_compositor_cleanup_state(&priv->cstate);
      vl_compositor_cleanup(&priv->compositor);
      priv->s_pipe->destroy(priv->s_pipe);
   }

   if (priv->t_pipe)
      priv->t_pipe->destroy(priv->t_pipe);

   if (priv->screen)
      omx_put_screen();

   return omx_workaround_Destructor(comp);
}

static OMX_ERRORTYPE vid_enc_SetParameter(OMX_HANDLETYPE handle, OMX_INDEXTYPE idx, OMX_PTR param)
{
   OMX_COMPONENTTYPE *comp = handle;
   vid_enc_PrivateType *priv = comp->pComponentPrivate;
   OMX_ERRORTYPE r;

   if (!param)
      return OMX_ErrorBadParameter;

   switch(idx) {
   case OMX_IndexParamPortDefinition: {
      OMX_PARAM_PORTDEFINITIONTYPE *def = param;

      r = omx_base_component_SetParameter(handle, idx, param);
      if (r)
         return r;

      if (def->nPortIndex == OMX_BASE_FILTER_INPUTPORT_INDEX) {
         omx_base_video_PortType *port;
         unsigned framesize;

         port = (omx_base_video_PortType *)priv->ports[OMX_BASE_FILTER_INPUTPORT_INDEX];
         framesize = port->sPortParam.format.video.nFrameWidth*
                     port->sPortParam.format.video.nFrameHeight;
         port->sPortParam.format.video.nSliceHeight = port->sPortParam.format.video.nFrameHeight;
         port->sPortParam.nBufferSize = framesize*3/2;

         port = (omx_base_video_PortType *)priv->ports[OMX_BASE_FILTER_OUTPUTPORT_INDEX];
         port->sPortParam.nBufferSize = framesize * 512 / (16*16);
      
         priv->frame_rate = def->format.video.xFramerate;

         priv->callbacks->EventHandler(comp, priv->callbackData, OMX_EventPortSettingsChanged,
                                       OMX_BASE_FILTER_OUTPUTPORT_INDEX, 0, NULL);
      }
      break;
   }
   case OMX_IndexParamStandardComponentRole: {
      OMX_PARAM_COMPONENTROLETYPE *role = param;

      r = checkHeader(param, sizeof(OMX_PARAM_COMPONENTROLETYPE));
      if (r)
         return r;

      if (strcmp((char *)role->cRole, OMX_VID_ENC_AVC_ROLE)) {
         return OMX_ErrorBadParameter;
      }

      break;
   }
   case OMX_IndexParamVideoBitrate: {
      OMX_VIDEO_PARAM_BITRATETYPE *bitrate = param;

      r = checkHeader(param, sizeof(OMX_VIDEO_PARAM_BITRATETYPE));
      if (r)
         return r;

      priv->bitrate = *bitrate;

      break;
   }
   case OMX_IndexParamVideoQuantization: {
      OMX_VIDEO_PARAM_QUANTIZATIONTYPE *quant = param;

      r = checkHeader(param, sizeof(OMX_VIDEO_PARAM_QUANTIZATIONTYPE));
      if (r)
         return r;

      priv->quant = *quant;

      break;
   }
   default:
      return omx_base_component_SetParameter(handle, idx, param);
   }
   return OMX_ErrorNone;
}

static OMX_ERRORTYPE vid_enc_GetParameter(OMX_HANDLETYPE handle, OMX_INDEXTYPE idx, OMX_PTR param)
{
   OMX_COMPONENTTYPE *comp = handle;
   vid_enc_PrivateType *priv = comp->pComponentPrivate;
   OMX_ERRORTYPE r;

   if (!param)
      return OMX_ErrorBadParameter;

   switch(idx) {
   case OMX_IndexParamStandardComponentRole: {
      OMX_PARAM_COMPONENTROLETYPE *role = param;

      r = checkHeader(param, sizeof(OMX_PARAM_COMPONENTROLETYPE));
      if (r)
         return r;

      strcpy((char *)role->cRole, OMX_VID_ENC_AVC_ROLE);
      break;
   }
   case OMX_IndexParamVideoInit:
      r = checkHeader(param, sizeof(OMX_PORT_PARAM_TYPE));
      if (r)
         return r;

      memcpy(param, &priv->sPortTypesParam[OMX_PortDomainVideo], sizeof(OMX_PORT_PARAM_TYPE));
      break;

   case OMX_IndexParamVideoPortFormat: {
      OMX_VIDEO_PARAM_PORTFORMATTYPE *format = param;
      omx_base_video_PortType *port;

      r = checkHeader(param, sizeof(OMX_VIDEO_PARAM_PORTFORMATTYPE));
      if (r)
         return r;

      if (format->nPortIndex > 1)
         return OMX_ErrorBadPortIndex;

      port = (omx_base_video_PortType *)priv->ports[format->nPortIndex];
      memcpy(format, &port->sVideoParam, sizeof(OMX_VIDEO_PARAM_PORTFORMATTYPE));
      break;
   }
   case OMX_IndexParamVideoBitrate: {
      OMX_VIDEO_PARAM_BITRATETYPE *bitrate = param;

      r = checkHeader(param, sizeof(OMX_VIDEO_PARAM_BITRATETYPE));
      if (r)
         return r;

      bitrate->eControlRate = priv->bitrate.eControlRate;
      bitrate->nTargetBitrate = priv->bitrate.nTargetBitrate;

      break;
   }
   case OMX_IndexParamVideoQuantization: {
      OMX_VIDEO_PARAM_QUANTIZATIONTYPE *quant = param;

      r = checkHeader(param, sizeof(OMX_VIDEO_PARAM_QUANTIZATIONTYPE));
      if (r)
         return r;

      quant->nQpI = priv->quant.nQpI;
      quant->nQpP = priv->quant.nQpP;
      quant->nQpB = priv->quant.nQpB;

      break;
   }

   default:
      return omx_base_component_GetParameter(handle, idx, param);

   }
   return OMX_ErrorNone;
}

static OMX_ERRORTYPE vid_enc_SetConfig(OMX_HANDLETYPE handle, OMX_INDEXTYPE idx, OMX_PTR config)
{
   OMX_COMPONENTTYPE *comp = handle;
   vid_enc_PrivateType *priv = comp->pComponentPrivate;
   OMX_ERRORTYPE r;
   int i;
 
   if (!config)
      return OMX_ErrorBadParameter;
                         
   switch(idx) {
   case OMX_IndexConfigVideoIntraVOPRefresh: {
      OMX_CONFIG_INTRAREFRESHVOPTYPE *type = config;

      r = checkHeader(config, sizeof(OMX_CONFIG_INTRAREFRESHVOPTYPE));
      if (r)
         return r;
      
      priv->force_pic_type = *type;
      
      break;
   }
   case OMX_IndexConfigCommonScale: {
      OMX_CONFIG_SCALEFACTORTYPE *scale = config;

      r = checkHeader(config, sizeof(OMX_CONFIG_SCALEFACTORTYPE));
      if (r)
         return r;

      if (scale->xWidth < 176 || scale->xHeight < 144)
         return OMX_ErrorBadParameter;

      for (i = 0; i < OMX_VID_ENC_NUM_SCALING_BUFFERS; ++i) {
         if (priv->scale_buffer[i]) {
            priv->scale_buffer[i]->destroy(priv->scale_buffer[i]);
            priv->scale_buffer[i] = NULL;
         }
      }

      priv->scale = *scale;
      if (priv->scale.xWidth != 0xffffffff && priv->scale.xHeight != 0xffffffff) {
         struct pipe_video_buffer templat = {};
 
         templat.buffer_format = PIPE_FORMAT_NV12;
         templat.chroma_format = PIPE_VIDEO_CHROMA_FORMAT_420;
         templat.width = priv->scale.xWidth; 
         templat.height = priv->scale.xHeight; 
         templat.interlaced = false;
         for (i = 0; i < OMX_VID_ENC_NUM_SCALING_BUFFERS; ++i) {
            priv->scale_buffer[i] = priv->s_pipe->create_video_buffer(priv->s_pipe, &templat);
            if (!priv->scale_buffer[i])
               return OMX_ErrorInsufficientResources;
         }
      }

      break;
   }
   default:
      return omx_base_component_SetConfig(handle, idx, config);
   }

   return OMX_ErrorNone;
}

static OMX_ERRORTYPE vid_enc_GetConfig(OMX_HANDLETYPE handle, OMX_INDEXTYPE idx, OMX_PTR config)
{
   OMX_COMPONENTTYPE *comp = handle;
   vid_enc_PrivateType *priv = comp->pComponentPrivate;
   OMX_ERRORTYPE r;

   if (!config)
      return OMX_ErrorBadParameter;

   switch(idx) {
   case OMX_IndexConfigCommonScale: {
      OMX_CONFIG_SCALEFACTORTYPE *scale = config;

      r = checkHeader(config, sizeof(OMX_CONFIG_SCALEFACTORTYPE));
      if (r)
         return r;

      scale->xWidth = priv->scale.xWidth;
      scale->xHeight = priv->scale.xHeight;

      break;
   }
   default:
      return omx_base_component_GetConfig(handle, idx, config);
   }
   
   return OMX_ErrorNone;
}

static OMX_ERRORTYPE vid_enc_MessageHandler(OMX_COMPONENTTYPE* comp, internalRequestMessageType *msg)
{
   vid_enc_PrivateType* priv = comp->pComponentPrivate;

   if (msg->messageType == OMX_CommandStateSet) {
      if ((msg->messageParam == OMX_StateIdle ) && (priv->state == OMX_StateLoaded)) {

         struct pipe_video_codec templat = {};
         omx_base_video_PortType *port;

         port = (omx_base_video_PortType *)priv->ports[OMX_BASE_FILTER_INPUTPORT_INDEX];

         templat.profile = PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE;
         templat.entrypoint = PIPE_VIDEO_ENTRYPOINT_ENCODE;
         templat.chroma_format = PIPE_VIDEO_CHROMA_FORMAT_420;
         templat.width = priv->scale_buffer[priv->current_scale_buffer] ?
                            priv->scale.xWidth : port->sPortParam.format.video.nFrameWidth;
         templat.height = priv->scale_buffer[priv->current_scale_buffer] ?
                            priv->scale.xHeight : port->sPortParam.format.video.nFrameHeight;
         templat.max_references = 1;

         priv->codec = priv->s_pipe->create_video_codec(priv->s_pipe, &templat);

      } else if ((msg->messageParam == OMX_StateLoaded) && (priv->state == OMX_StateIdle)) {
         if (priv->codec) {
            priv->codec->destroy(priv->codec);
            priv->codec = NULL;
         }
      }
   }

   return omx_base_component_MessageHandler(comp, msg);
}

static OMX_ERRORTYPE vid_enc_AllocateInBuffer(omx_base_PortType *port, OMX_INOUT OMX_BUFFERHEADERTYPE **buf,
                                              OMX_IN OMX_U32 idx, OMX_IN OMX_PTR private, OMX_IN OMX_U32 size)
{
   OMX_VIDEO_PORTDEFINITIONTYPE *def = &port->sPortParam.format.video;
   OMX_COMPONENTTYPE* comp = port->standCompContainer;
   vid_enc_PrivateType *priv = comp->pComponentPrivate;
   struct pipe_video_buffer templat = {};
   struct input_buf_private *inp;
   OMX_ERRORTYPE r;

   r = base_port_AllocateBuffer(port, buf, idx, private, size);
   if (r)
      return r;

   inp = (*buf)->pInputPortPrivate = CALLOC(1, sizeof(struct input_buf_private));
   if (!inp) {
      base_port_FreeBuffer(port, idx, *buf);
      return OMX_ErrorInsufficientResources;
   }

   templat.buffer_format = PIPE_FORMAT_NV12;
   templat.chroma_format = PIPE_VIDEO_CHROMA_FORMAT_420;
   templat.width = def->nFrameWidth;
   templat.height = def->nFrameHeight;
   templat.interlaced = false;

   inp->buf = priv->s_pipe->create_video_buffer(priv->s_pipe, &templat);
   if (!inp->buf) {
      FREE(inp);
      base_port_FreeBuffer(port, idx, *buf);
      return OMX_ErrorInsufficientResources;
   }

   return OMX_ErrorNone;
}

static OMX_ERRORTYPE vid_enc_UseInBuffer(omx_base_PortType *port, OMX_BUFFERHEADERTYPE **buf, OMX_U32 idx,
                                         OMX_PTR private, OMX_U32 size, OMX_U8 *mem)
{
   OMX_VIDEO_PORTDEFINITIONTYPE *def = &port->sPortParam.format.video;
   OMX_COMPONENTTYPE* comp = port->standCompContainer;
   vid_enc_PrivateType *priv = comp->pComponentPrivate;
   struct pipe_video_buffer templat = {};
   struct input_buf_private *inp;
   OMX_ERRORTYPE r;

   r = base_port_UseBuffer(port, buf, idx, private, size, mem);
   if (r)
      return r;

   inp = (*buf)->pInputPortPrivate = CALLOC(1, sizeof(struct input_buf_private));
   if (!inp) {
      base_port_FreeBuffer(port, idx, *buf);
      return OMX_ErrorInsufficientResources;
   }

   templat.buffer_format = PIPE_FORMAT_NV12;
   templat.chroma_format = PIPE_VIDEO_CHROMA_FORMAT_420;
   templat.width = def->nFrameWidth;
   templat.height = def->nFrameHeight;
   templat.interlaced = false;

   inp->buf = priv->s_pipe->create_video_buffer(priv->s_pipe, &templat);
   if (!inp->buf) {
      FREE(inp);
      base_port_FreeBuffer(port, idx, *buf);
      return OMX_ErrorInsufficientResources;
   }

   return OMX_ErrorNone;
}

static OMX_ERRORTYPE vid_enc_FreeInBuffer(omx_base_PortType *port, OMX_U32 idx, OMX_BUFFERHEADERTYPE *buf)
{
   struct input_buf_private *inp = buf->pInputPortPrivate;
   pipe_resource_reference(&inp->bitstream, NULL);
   inp->buf->destroy(inp->buf);
   FREE(inp);
   return base_port_FreeBuffer(port, idx, buf);
}

static OMX_ERRORTYPE vid_enc_AllocateOutBuffer(omx_base_PortType *port, OMX_INOUT OMX_BUFFERHEADERTYPE **buf,
                                               OMX_IN OMX_U32 idx, OMX_IN OMX_PTR private, OMX_IN OMX_U32 size)
{
   OMX_ERRORTYPE r;

   r = base_port_AllocateBuffer(port, buf, idx, private, size);
   if (r)
      return r;

   FREE((*buf)->pBuffer);
   (*buf)->pBuffer = NULL;
   (*buf)->pOutputPortPrivate = CALLOC(1, sizeof(struct output_buf_private));
   if (!(*buf)->pOutputPortPrivate) {
      base_port_FreeBuffer(port, idx, *buf);
      return OMX_ErrorInsufficientResources;
   }

   return OMX_ErrorNone;
}

static OMX_ERRORTYPE vid_enc_FreeOutBuffer(omx_base_PortType *port, OMX_U32 idx, OMX_BUFFERHEADERTYPE *buf)
{
   OMX_COMPONENTTYPE* comp = port->standCompContainer;
   vid_enc_PrivateType *priv = comp->pComponentPrivate;

   if (buf->pOutputPortPrivate) {
      struct output_buf_private *outp = buf->pOutputPortPrivate;
      if (outp->transfer)
         pipe_transfer_unmap(priv->t_pipe, outp->transfer);
      pipe_resource_reference(&outp->bitstream, NULL);
      FREE(outp);
      buf->pOutputPortPrivate = NULL;
   }
   buf->pBuffer = NULL;

   return base_port_FreeBuffer(port, idx, buf);
}

static OMX_ERRORTYPE vid_enc_EncodeFrame(omx_base_PortType *port, OMX_BUFFERHEADERTYPE *buf)
{
   OMX_COMPONENTTYPE* comp = port->standCompContainer;
   vid_enc_PrivateType *priv = comp->pComponentPrivate;
   struct input_buf_private *inp = buf->pInputPortPrivate;
   unsigned size = priv->ports[OMX_BASE_FILTER_OUTPUTPORT_INDEX]->sPortParam.nBufferSize;
   OMX_VIDEO_PORTDEFINITIONTYPE *def = &port->sPortParam.format.video;
   struct pipe_h264_enc_picture_desc picture;
   struct pipe_h264_enc_rate_control *rate_ctrl = &picture.rate_ctrl;
   struct pipe_video_buffer *vbuf = inp->buf;

   pipe_resource_reference(&inp->bitstream, NULL);

   if (buf->nFilledLen == 0) {
      if (buf->nFlags & OMX_BUFFERFLAG_EOS)
         buf->nFilledLen = buf->nAllocLen;
      return base_port_SendBufferFunction(port, buf);
   }

   if (buf->pOutputPortPrivate) {
      vbuf = buf->pOutputPortPrivate;
   } else {
      /* ------- load input image into video buffer ---- */
      struct pipe_sampler_view **views;
      struct pipe_box box = {};
      void *ptr;

      views = inp->buf->get_sampler_view_planes(vbuf);
      if (!views)
         return OMX_ErrorInsufficientResources;

      ptr = buf->pBuffer;

      box.width = def->nFrameWidth;
      box.height = def->nFrameHeight;
      box.depth = 1;

      priv->s_pipe->transfer_inline_write(priv->s_pipe, views[0]->texture, 0,
                                          PIPE_TRANSFER_WRITE, &box,
                                          ptr, def->nStride, 0);


      ptr = ((uint8_t*)buf->pBuffer) + (def->nStride * box.height);

      box.width = def->nFrameWidth / 2;
      box.height = def->nFrameHeight / 2;
      box.depth = 1;

      priv->s_pipe->transfer_inline_write(priv->s_pipe, views[1]->texture, 0,
                                          PIPE_TRANSFER_WRITE, &box,
                                          ptr, def->nStride, 0);
   }

   /* -------------- scale input image --------- */

   if (priv->scale_buffer[priv->current_scale_buffer]) {
      struct vl_compositor *compositor = &priv->compositor;
      struct vl_compositor_state *s = &priv->cstate;
      struct pipe_sampler_view **views;
      struct pipe_surface **dst_surface;
      unsigned i;

      views = vbuf->get_sampler_view_planes(vbuf);
      dst_surface = priv->scale_buffer[priv->current_scale_buffer]->get_surfaces
                       (priv->scale_buffer[priv->current_scale_buffer]);
      vl_compositor_clear_layers(s);

      for (i = 0; i < VL_MAX_SURFACES; ++i) {
         struct u_rect src_rect;

         if (!views[i] || !dst_surface[i])
            continue;

         src_rect.x0 = 0;
         src_rect.y0 = 0;
         src_rect.x1 = port->sPortParam.format.video.nFrameWidth;
         src_rect.y1 = port->sPortParam.format.video.nFrameHeight;

         if (i > 0) {
            src_rect.x1 /= 2;
            src_rect.y1 /= 2;
         }

         vl_compositor_set_rgba_layer(s, compositor, 0, views[i], &src_rect, NULL, NULL);
         vl_compositor_render(s, compositor, dst_surface[i], NULL, false);
      }
      
      size  = priv->scale.xWidth * priv->scale.xHeight * 2; 
      vbuf = priv->scale_buffer[priv->current_scale_buffer++];
      priv->current_scale_buffer %= OMX_VID_ENC_NUM_SCALING_BUFFERS;
   }

   priv->s_pipe->flush(priv->s_pipe, NULL, 0);

   /* -------------- allocate output buffer --------- */

   inp->bitstream = pipe_buffer_create(priv->s_pipe->screen, PIPE_BIND_VERTEX_BUFFER,
                                       PIPE_USAGE_STREAM, size);

   /* -------------- decode frame --------- */

   switch (priv->bitrate.eControlRate) {
   case OMX_Video_ControlRateVariable:
      rate_ctrl->rate_ctrl_method = PIPE_H264_ENC_RATE_CONTROL_METHOD_VARIABLE;
      break; 
   case OMX_Video_ControlRateConstant:
      rate_ctrl->rate_ctrl_method = PIPE_H264_ENC_RATE_CONTROL_METHOD_CONSTANT;
      break; 
   case OMX_Video_ControlRateVariableSkipFrames:
      rate_ctrl->rate_ctrl_method = PIPE_H264_ENC_RATE_CONTROL_METHOD_VARIABLE_SKIP;
      break;
   case OMX_Video_ControlRateConstantSkipFrames:
      rate_ctrl->rate_ctrl_method = PIPE_H264_ENC_RATE_CONTROL_METHOD_CONSTANT_SKIP;
      break;
   default:
      rate_ctrl->rate_ctrl_method = PIPE_H264_ENC_RATE_CONTROL_METHOD_DISABLE;
      break;
   } 
      
   if (rate_ctrl->rate_ctrl_method != PIPE_H264_ENC_RATE_CONTROL_METHOD_DISABLE) {
      if (priv->bitrate.nTargetBitrate < OMX_VID_ENC_BITRATE_MIN)
         rate_ctrl->target_bitrate = OMX_VID_ENC_BITRATE_MIN;
      else if (priv->bitrate.nTargetBitrate < OMX_VID_ENC_BITRATE_MAX)
         rate_ctrl->target_bitrate = priv->bitrate.nTargetBitrate;
      else
         rate_ctrl->target_bitrate = OMX_VID_ENC_BITRATE_MAX;
      rate_ctrl->peak_bitrate = rate_ctrl->target_bitrate;    
      rate_ctrl->frame_rate_den = OMX_VID_ENC_CONTROL_FRAME_RATE_DEN_DEFAULT;
      rate_ctrl->frame_rate_num = ((priv->frame_rate) >> 16) * rate_ctrl->frame_rate_den;
      if (rate_ctrl->target_bitrate < OMX_VID_ENC_BITRATE_MEDIAN)
         rate_ctrl->vbv_buffer_size = MIN2((rate_ctrl->target_bitrate * 2.75), OMX_VID_ENC_BITRATE_MEDIAN);
      else
         rate_ctrl->vbv_buffer_size = rate_ctrl->target_bitrate;

      if (rate_ctrl->frame_rate_num) {
         unsigned long long t = rate_ctrl->target_bitrate;
         t *= rate_ctrl->frame_rate_den;
         rate_ctrl->target_bits_picture = t / rate_ctrl->frame_rate_num;
      } else {
         rate_ctrl->target_bits_picture = rate_ctrl->target_bitrate;
      }
      rate_ctrl->peak_bits_picture_integer = rate_ctrl->target_bits_picture;
      rate_ctrl->peak_bits_picture_fraction = 0;
   } else
      memset(rate_ctrl, 0, sizeof(struct pipe_h264_enc_rate_control));
   
   picture.quant_i_frames = priv->quant.nQpI;
   picture.quant_p_frames = priv->quant.nQpP;
   picture.quant_b_frames = priv->quant.nQpB;

   if (!(priv->frame_num % OMX_VID_ENC_IDR_PERIOD_DEFAULT) || priv->force_pic_type.IntraRefreshVOP) {
      picture.picture_type = PIPE_H264_ENC_PICTURE_TYPE_IDR;
      priv->frame_num = 0;
   } else
      picture.picture_type = PIPE_H264_ENC_PICTURE_TYPE_P;	
   
   picture.frame_num = priv->frame_num++;
   priv->force_pic_type.IntraRefreshVOP = OMX_FALSE; 

   priv->codec->begin_frame(priv->codec, vbuf, &picture.base);
   priv->codec->encode_bitstream(priv->codec, vbuf, inp->bitstream, &inp->feedback);
   priv->codec->end_frame(priv->codec, vbuf, &picture.base);
 
   return base_port_SendBufferFunction(port, buf);
}

static void vid_enc_BufferEncoded(OMX_COMPONENTTYPE *comp, OMX_BUFFERHEADERTYPE* input, OMX_BUFFERHEADERTYPE* output)
{
   struct input_buf_private *inp = input->pInputPortPrivate;
   vid_enc_PrivateType *priv = comp->pComponentPrivate;
   struct output_buf_private *outp = output->pOutputPortPrivate;
   struct pipe_box box = {};
   unsigned size;

   input->nFilledLen = 0; /* mark buffer as empty */

   if (!inp->bitstream)
      return;

   /* ------------- map result buffer ----------------- */

   if (outp->transfer)
      pipe_transfer_unmap(priv->t_pipe, outp->transfer);

   pipe_resource_reference(&outp->bitstream, inp->bitstream);

   box.width = inp->bitstream->width0;
   box.height = inp->bitstream->height0;
   box.depth = inp->bitstream->depth0;

   output->pBuffer = priv->t_pipe->transfer_map(priv->t_pipe, outp->bitstream, 0,
                                                PIPE_TRANSFER_READ_WRITE,
                                                &box, &outp->transfer);
 
   /* ------------- get size of result ----------------- */

   priv->codec->get_feedback(priv->codec, inp->feedback, &size);

   output->nOffset = 0;
   output->nFilledLen = size; /* mark buffer as full */
}
