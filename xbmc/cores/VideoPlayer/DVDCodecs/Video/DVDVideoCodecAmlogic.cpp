/*
 *      Copyright (C) 2005-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include <math.h>

#include "DVDVideoCodecAmlogic.h"
#include "DVDClock.h"
#include "DVDStreamInfo.h"
#include "AMLCodec.h"
#include "utils/AMLUtils.h"
#include "utils/BitstreamConverter.h"
#include "utils/log.h"
#include "utils/SysfsUtils.h"
#include "threads/Atomics.h"
#include "settings/Settings.h"
#include "threads/Thread.h"

#define __MODULE_NAME__ "DVDVideoCodecAmlogic"

typedef struct frame_queue {
  double dts;
  double pts;
  double sort_time;
  struct frame_queue *nextframe;
} frame_queue;

CDVDVideoCodecAmlogic::CDVDVideoCodecAmlogic(CProcessInfo &processInfo) : CDVDVideoCodec(processInfo),
  m_Codec(NULL),
  m_pFormatName("amcodec"),
  m_opened(false),
  m_framerate(0.0),
  m_video_rate(0),
  m_mpeg2_sequence(NULL),
  m_h264_sequence(NULL),
  m_drop(false),
  m_has_keyframe(false),
  m_bitparser(NULL),
  m_bitstream(NULL)
{
  pthread_mutex_init(&m_queue_mutex, NULL);
}

CDVDVideoCodecAmlogic::~CDVDVideoCodecAmlogic()
{
  Dispose();
  pthread_mutex_destroy(&m_queue_mutex);
}

bool CDVDVideoCodecAmlogic::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options)
{
  if (!CSettings::GetInstance().GetBool(CSettings::SETTING_VIDEOPLAYER_USEAMCODEC))
    return false;
  if (hints.stills)
    return false;

  if (!aml_permissions())
  {
    CLog::Log(LOGERROR, "AML: no proper permission, please contact the device vendor. Skipping codec...");
    return false;
  }

  m_hints = hints;

  switch(m_hints.codec)
  {
    case AV_CODEC_ID_MJPEG:
      m_pFormatName = "am-mjpeg";
      break;
    case AV_CODEC_ID_MPEG1VIDEO:
    case AV_CODEC_ID_MPEG2VIDEO:
    case AV_CODEC_ID_MPEG2VIDEO_XVMC:
      if (m_hints.width <= CSettings::GetInstance().GetInt(CSettings::SETTING_VIDEOPLAYER_USEAMCODECMPEG2))
        return false;
      m_mpeg2_sequence_pts = 0;
      m_mpeg2_sequence = new mpeg2_sequence;
      m_mpeg2_sequence->width  = m_hints.width;
      m_mpeg2_sequence->height = m_hints.height;
      m_mpeg2_sequence->ratio  = m_hints.aspect;
      if (m_hints.fpsrate > 0 && m_hints.fpsscale != 0)
        m_mpeg2_sequence->rate = (float)m_hints.fpsrate / m_hints.fpsscale;
      else
        m_mpeg2_sequence->rate = 1.0;
      m_pFormatName = "am-mpeg2";
      break;
    case AV_CODEC_ID_H264:
      if (m_hints.width <= CSettings::GetInstance().GetInt(CSettings::SETTING_VIDEOPLAYER_USEAMCODECH264))
        return false;
      switch(hints.profile)
      {
        case FF_PROFILE_H264_HIGH_10:
        case FF_PROFILE_H264_HIGH_10_INTRA:
        case FF_PROFILE_H264_HIGH_422:
        case FF_PROFILE_H264_HIGH_422_INTRA:
        case FF_PROFILE_H264_HIGH_444_PREDICTIVE:
        case FF_PROFILE_H264_HIGH_444_INTRA:
        case FF_PROFILE_H264_CAVLC_444:
          return false;
      }
      if ((aml_support_h264_4k2k() == AML_NO_H264_4K2K) && ((m_hints.width > 1920) || (m_hints.height > 1088)))
      {
        // 4K is supported only on Amlogic S802/S812 chip
        return false;
      }

      if (m_hints.aspect == 0.0f)
      {
        m_h264_sequence_pts = 0;
        m_h264_sequence = new h264_sequence;
        m_h264_sequence->width  = m_hints.width;
        m_h264_sequence->height = m_hints.height;
        m_h264_sequence->ratio  = m_hints.aspect;
      }

      m_pFormatName = "am-h264";
      // convert h264-avcC to h264-annex-b as h264-avcC
      // under streamers can have issues when seeking.
      if (m_hints.extradata && *(uint8_t*)m_hints.extradata == 1)
      {
        m_bitstream = new CBitstreamConverter;
        m_bitstream->Open(m_hints.codec, (uint8_t*)m_hints.extradata, m_hints.extrasize, true);
        m_bitstream->ResetKeyframe();
        // make sure we do not leak the existing m_hints.extradata
        free(m_hints.extradata);
        m_hints.extrasize = m_bitstream->GetExtraSize();
        m_hints.extradata = malloc(m_hints.extrasize);
        memcpy(m_hints.extradata, m_bitstream->GetExtraData(), m_hints.extrasize);
      }
      else
      {
        m_bitparser = new CBitstreamParser();
        m_bitparser->Open();
      }

      // if we have SD PAL content assume it is widescreen
      // correct aspect ratio will be detected later anyway
      if (m_hints.width == 720 && m_hints.height == 576 && m_hints.aspect == 0.0f)
          m_hints.aspect = 1.8181818181818181;

      // assume widescreen for "HD Lite" channels
      // correct aspect ratio will be detected later anyway
      if ((m_hints.width == 1440 || m_hints.width ==1280) && m_hints.height == 1080 && m_hints.aspect == 0.0f)
          m_hints.aspect = 1.7777777777777778;

      break;
    case AV_CODEC_ID_MPEG4:
    case AV_CODEC_ID_MSMPEG4V2:
    case AV_CODEC_ID_MSMPEG4V3:
      if (m_hints.width <= CSettings::GetInstance().GetInt(CSettings::SETTING_VIDEOPLAYER_USEAMCODECMPEG4))
        return false;
      m_pFormatName = "am-mpeg4";
      break;
    case AV_CODEC_ID_H263:
    case AV_CODEC_ID_H263P:
    case AV_CODEC_ID_H263I:
      // amcodec can't handle h263
      return false;
      break;
//    case AV_CODEC_ID_FLV1:
//      m_pFormatName = "am-flv1";
//      break;
    case AV_CODEC_ID_RV10:
    case AV_CODEC_ID_RV20:
    case AV_CODEC_ID_RV30:
    case AV_CODEC_ID_RV40:
      // m_pFormatName = "am-rv";
      // rmvb is not handled well by amcodec
      return false;
      break;
    case AV_CODEC_ID_VC1:
      m_pFormatName = "am-vc1";
      break;
    case AV_CODEC_ID_WMV3:
      m_pFormatName = "am-wmv3";
      break;
    case AV_CODEC_ID_AVS:
    case AV_CODEC_ID_CAVS:
      m_pFormatName = "am-avs";
      break;
    case AV_CODEC_ID_VP9:
      if (!aml_support_vp9())
      {
        return false;
      }
      m_pFormatName = "am-vp9";
      break;
    case AV_CODEC_ID_HEVC:
      if (aml_support_hevc()) {
        if (!aml_support_hevc_4k2k() && ((m_hints.width > 1920) || (m_hints.height > 1088)))
        {
          // 4K HEVC is supported only on Amlogic S812 chip
          return false;
        }
      } else {
        // HEVC supported only on S805 and S812.
        return false;
      }
      if ((hints.profile == FF_PROFILE_HEVC_MAIN_10) && !aml_support_hevc_10bit())
      {
        return false;
      }
      m_pFormatName = "am-h265";
      m_bitstream = new CBitstreamConverter();
      m_bitstream->Open(m_hints.codec, (uint8_t*)m_hints.extradata, m_hints.extrasize, true);
      // make sure we do not leak the existing m_hints.extradata
      free(m_hints.extradata);
      m_hints.extrasize = m_bitstream->GetExtraSize();
      m_hints.extradata = malloc(m_hints.extrasize);
      memcpy(m_hints.extradata, m_bitstream->GetExtraData(), m_hints.extrasize);
      break;
    default:
      CLog::Log(LOGDEBUG, "%s: Unknown hints.codec(%d", __MODULE_NAME__, m_hints.codec);
      return false;
      break;
  }

  m_aspect_ratio = m_hints.aspect;
  m_Codec = new CAMLCodec();
  if (!m_Codec)
  {
    CLog::Log(LOGERROR, "%s: Failed to create Amlogic Codec", __MODULE_NAME__);
    return false;
  }
  m_opened = false;

  // allocate a dummy DVDVideoPicture buffer.
  // first make sure all properties are reset.
  memset(&m_videobuffer, 0, sizeof(DVDVideoPicture));

  m_videobuffer.dts = DVD_NOPTS_VALUE;
  m_videobuffer.pts = DVD_NOPTS_VALUE;
  m_videobuffer.format = RENDER_FMT_AML;
  m_videobuffer.color_range  = 0;
  m_videobuffer.color_matrix = 4;
  m_videobuffer.iFlags  = DVP_FLAG_ALLOCATED;
  m_videobuffer.iWidth  = m_hints.width;
  m_videobuffer.iHeight = m_hints.height;
  m_videobuffer.amlcodec = NULL;

  m_videobuffer.iDisplayWidth  = m_videobuffer.iWidth;
  m_videobuffer.iDisplayHeight = m_videobuffer.iHeight;
  if (m_hints.aspect > 0.0 && !m_hints.forced_aspect)
  {
    m_videobuffer.iDisplayWidth  = ((int)lrint(m_videobuffer.iHeight * m_hints.aspect)) & ~3;
    if (m_videobuffer.iDisplayWidth > m_videobuffer.iWidth)
    {
      m_videobuffer.iDisplayWidth  = m_videobuffer.iWidth;
      m_videobuffer.iDisplayHeight = ((int)lrint(m_videobuffer.iWidth / m_hints.aspect)) & ~3;
    }
  }

  m_processInfo.SetVideoDecoderName(m_pFormatName, true);
  m_processInfo.SetVideoDimensions(m_hints.width, m_hints.height);
  m_processInfo.SetVideoDeintMethod("hardware");
  m_processInfo.SetVideoDAR(m_hints.aspect);

  m_has_keyframe = false;

  CLog::Log(LOGINFO, "%s: Opened Amlogic Codec", __MODULE_NAME__);
  return true;
}

void CDVDVideoCodecAmlogic::Dispose(void)
{
  {
    CSingleLock lock(m_secure);
    for (std::set<CDVDAmlogicInfo*>::iterator it = m_inflight.begin(); it != m_inflight.end(); ++it)
      (*it)->invalidate();
  }

  if (m_Codec)
    m_Codec->CloseDecoder(), delete m_Codec, m_Codec = NULL;
  if (m_videobuffer.iFlags)
    m_videobuffer.iFlags = 0;
  if (m_mpeg2_sequence)
    delete m_mpeg2_sequence, m_mpeg2_sequence = NULL;
  if (m_h264_sequence)
    delete m_h264_sequence, m_h264_sequence = NULL;

  if (m_bitstream)
    delete m_bitstream, m_bitstream = NULL;

  if (m_bitparser)
    delete m_bitparser, m_bitparser = NULL;
}

int CDVDVideoCodecAmlogic::Decode(uint8_t *pData, int iSize, double dts, double pts)
{
  // Handle Input, add demuxer packet to input queue, we must accept it or
  // it will be discarded as VideoPlayerVideo has no concept of "try again".
  if (pData)
  {
    if (m_bitstream)
    {
      if (!m_bitstream->Convert(pData, iSize))
        return VC_ERROR;

      if (!m_bitstream->HasKeyframe())
      {
        CLog::Log(LOGDEBUG, "%s::Decode waiting for keyframe (bitstream)", __MODULE_NAME__);
        return VC_BUFFER;
      }
      pData = m_bitstream->GetConvertBuffer();
      iSize = m_bitstream->GetConvertSize();
    }
    else if (!m_has_keyframe && m_bitparser)
    {
      if (!m_bitparser->HasKeyframe(pData, iSize))
      {
        CLog::Log(LOGDEBUG, "%s::Decode waiting for keyframe (bitparser)", __MODULE_NAME__);
        return VC_BUFFER;
      }
      else
        m_has_keyframe = true;
    }
    FrameRateTracking( pData, iSize, dts, pts);

    if (!m_opened)
    {
      if (pts == DVD_NOPTS_VALUE)
        m_hints.ptsinvalid = true;

      if (m_Codec && !m_Codec->OpenDecoder(m_hints))
        CLog::Log(LOGERROR, "%s: Failed to open Amlogic Codec", __MODULE_NAME__);
      m_opened = true;
    }
  }

  if (m_hints.ptsinvalid)
    pts = DVD_NOPTS_VALUE;

  return m_Codec->Decode(pData, iSize, dts, pts);
}

void CDVDVideoCodecAmlogic::Reset(void)
{
  m_Codec->Reset();
  m_mpeg2_sequence_pts = 0;
  m_has_keyframe = false;
  if (m_bitstream && m_hints.codec == AV_CODEC_ID_H264)
    m_bitstream->ResetKeyframe();
}

bool CDVDVideoCodecAmlogic::GetPicture(DVDVideoPicture* pDvdVideoPicture)
{
  if (m_Codec)
    m_Codec->GetPicture(&m_videobuffer);
  *pDvdVideoPicture = m_videobuffer;

  CDVDAmlogicInfo* info = new CDVDAmlogicInfo(this, m_Codec, 
   m_Codec->GetOMXPts(), m_Codec->GetAmlDuration(), m_Codec->GetBufferIndex());

  {
    CSingleLock lock(m_secure);
    m_inflight.insert(info);
  }

  pDvdVideoPicture->amlcodec = info->Retain();

  // check for mpeg2 aspect ratio changes
  if (m_mpeg2_sequence && pDvdVideoPicture->pts >= m_mpeg2_sequence_pts)
    m_aspect_ratio = m_mpeg2_sequence->ratio;

  // check for h264 aspect ratio changes
  if (m_h264_sequence && pDvdVideoPicture->pts >= m_h264_sequence_pts)
    m_aspect_ratio = m_h264_sequence->ratio;

  pDvdVideoPicture->iDisplayWidth  = pDvdVideoPicture->iWidth;
  pDvdVideoPicture->iDisplayHeight = pDvdVideoPicture->iHeight;
  if (m_aspect_ratio > 1.0 && !m_hints.forced_aspect)
  {
    pDvdVideoPicture->iDisplayWidth  = ((int)lrint(pDvdVideoPicture->iHeight * m_aspect_ratio)) & ~3;
    if (pDvdVideoPicture->iDisplayWidth > pDvdVideoPicture->iWidth)
    {
      pDvdVideoPicture->iDisplayWidth  = pDvdVideoPicture->iWidth;
      pDvdVideoPicture->iDisplayHeight = ((int)lrint(pDvdVideoPicture->iWidth / m_aspect_ratio)) & ~3;
    }
  }

  return true;
}

bool CDVDVideoCodecAmlogic::ClearPicture(DVDVideoPicture *pDvdVideoPicture)
{
  SAFE_RELEASE(pDvdVideoPicture->amlcodec);
  return true;
}

void CDVDVideoCodecAmlogic::SetDropState(bool bDrop)
{
  if (bDrop == m_drop)
    return;

  m_drop = bDrop;
  if (bDrop)
    m_videobuffer.iFlags |=  DVP_FLAG_DROPPED;
  else
    m_videobuffer.iFlags &= ~DVP_FLAG_DROPPED;

  // Freerun mode causes amvideo driver to ignore timing and process frames
  // as quickly as they are coming from decoder. By enabling freerun mode we can
  // skip rendering of the frames that are requested to be dropped by VideoPlayer.
  //SysfsUtils::SetInt("/sys/class/video/freerun_mode", bDrop ? 1 : 0);
}

void CDVDVideoCodecAmlogic::SetCodecControl(int flags)
{
  if (m_Codec)
    m_Codec->SetDrain((flags & DVD_CODEC_CTRL_DRAIN) != 0);
}

void CDVDVideoCodecAmlogic::SetSpeed(int iSpeed)
{
  if (m_Codec)
    m_Codec->SetSpeed(iSpeed);
}

void CDVDVideoCodecAmlogic::FrameRateTracking(uint8_t *pData, int iSize, double dts, double pts)
{
  // mpeg2 handling
  if (m_mpeg2_sequence)
  {
    // probe demux for sequence_header_code NAL and
    // decode aspect ratio and frame rate.
    if (CBitstreamConverter::mpeg2_sequence_header(pData, iSize, m_mpeg2_sequence))
    {
      m_mpeg2_sequence_pts = pts;
      if (m_mpeg2_sequence_pts == DVD_NOPTS_VALUE)
        m_mpeg2_sequence_pts = dts;

      m_framerate = m_mpeg2_sequence->rate;
      m_video_rate = (int)(0.5 + (96000.0 / m_framerate));

      m_processInfo.SetVideoFps(m_framerate);

      // update m_hints for 1st frame fixup.
      switch(m_mpeg2_sequence->rate_info)
      {
        default:
        case 0x01:
          m_hints.fpsrate = 24000.0;
          m_hints.fpsscale = 1001.0;
          break;
        case 0x02:
          m_hints.fpsrate = 24000.0;
          m_hints.fpsscale = 1000.0;
          break;
        case 0x03:
          m_hints.fpsrate = 25000.0;
          m_hints.fpsscale = 1000.0;
          break;
        case 0x04:
          m_hints.fpsrate = 30000.0;
          m_hints.fpsscale = 1001.0;
          break;
        case 0x05:
          m_hints.fpsrate = 30000.0;
          m_hints.fpsscale = 1000.0;
          break;
        case 0x06:
          m_hints.fpsrate = 50000.0;
          m_hints.fpsscale = 1000.0;
          break;
        case 0x07:
          m_hints.fpsrate = 60000.0;
          m_hints.fpsscale = 1001.0;
          break;
        case 0x08:
          m_hints.fpsrate = 60000.0;
          m_hints.fpsscale = 1000.0;
          break;
      }
      m_hints.width    = m_mpeg2_sequence->width;
      m_hints.height   = m_mpeg2_sequence->height;
      m_hints.aspect   = m_mpeg2_sequence->ratio;
    }
    return;
  }

  // h264 aspect ratio handling
  if (m_h264_sequence)
  {
    // probe demux for SPS NAL and decode aspect ratio
    if (CBitstreamConverter::h264_sequence_header(pData, iSize, m_h264_sequence))
    {
      m_h264_sequence_pts = pts;
      if (m_h264_sequence_pts == DVD_NOPTS_VALUE)
          m_h264_sequence_pts = dts;

      CLog::Log(LOGDEBUG, "%s: detected h264 aspect ratio(%f)",
        __MODULE_NAME__, m_h264_sequence->ratio);
      m_hints.width    = m_h264_sequence->width;
      m_hints.height   = m_h264_sequence->height;
      m_hints.aspect   = m_h264_sequence->ratio;
    }
  }
}

void CDVDVideoCodecAmlogic::RemoveInfo(CDVDAmlogicInfo *info)
{
  CSingleLock lock(m_secure);
  m_inflight.erase(m_inflight.find(info));
}

CDVDAmlogicInfo::CDVDAmlogicInfo(CDVDVideoCodecAmlogic *codec, CAMLCodec *amlcodec, int omxPts, int amlDuration, uint32_t bufferIndex)
  : m_refs(0)
  , m_codec(codec)
  , m_amlCodec(amlcodec)
  , m_omxPts(omxPts)
  , m_amlDuration(amlDuration)
  , m_bufferIndex(bufferIndex)
  , m_rendered(false)
{
}

CDVDAmlogicInfo *CDVDAmlogicInfo::Retain()
{
  AtomicIncrement(&m_refs);
  return this;
}

long CDVDAmlogicInfo::Release()
{
  long count = AtomicDecrement(&m_refs);
  if (count == 0)
  {
    if (m_codec)
      m_codec->RemoveInfo(this);
    delete this;
  }

  return count;
}

CAMLCodec *CDVDAmlogicInfo::getAmlCodec() const
{
  CSingleLock lock(m_section);

  return m_amlCodec;
}

void CDVDAmlogicInfo::invalidate()
{
  CSingleLock lock(m_section);

  m_codec = NULL;
  m_amlCodec = NULL;
}

