NAME = avformat
DESC = FFmpeg container format library

HEADERS = avformat.h                                                    \
          avio.h                                                        \
          version.h                                                     \
          version_major.h                                               \

OBJS = allformats.o         \
       av1.o                \
       avformat.o           \
       avio.o               \
       aviobuf.o            \
       codecstring.o        \
       demux.o              \
       demux_utils.o        \
       dump.o               \
       dv.o                 \
       format.o             \
       id3v1.o              \
       id3v2.o              \
       isom_tags.o          \
       metadata.o           \
       mux.o                \
       mux_utils.o          \
       nal.o                \
       options.o            \
       os_support.o         \
       protocols.o          \
       riff.o               \
       sdp.o                \
       seek.o               \
       url.o                \
       urldecode.o          \
       utils.o              \
       version.o            \
       vpcc.o               \


# subsystems
OBJS-$(CONFIG_ISO_MEDIA)                 += isom.o
OBJS-$(CONFIG_ISO_WRITER)                += avc.o hevc.o vvc.o
OBJS-$(CONFIG_IAMFDEC)                   += iamf_reader.o iamf_parse.o iamf.o
OBJS-$(CONFIG_IAMFENC)                   += iamf_writer.o iamf.o
OBJS-$(CONFIG_NETWORK)                   += network.o
OBJS-$(CONFIG_RIFFDEC)                   += riffdec.o
OBJS-$(CONFIG_RIFFENC)                   += riffenc.o
OBJS-$(CONFIG_RTPDEC)                    += rdt.o                       \
                                            rtp.o                       \
                                            rtpdec.o                    \
                                            rtpdec_ac3.o                \
                                            rtpdec_amr.o                \
                                            rtpdec_asf.o                \
                                            rtpdec_av1.o                \
                                            rtpdec_dv.o                 \
                                            rtpdec_g726.o               \
                                            rtpdec_h261.o               \
                                            rtpdec_h263.o               \
                                            rtpdec_h263_rfc2190.o       \
                                            rtpdec_h264.o               \
                                            rtpdec_hevc.o               \
                                            rtpdec_ilbc.o               \
                                            rtpdec_jpeg.o               \
                                            rtpdec_latm.o               \
                                            rtpdec_mpa_robust.o         \
                                            rtpdec_mpeg12.o             \
                                            rtpdec_mpeg4.o              \
                                            rtpdec_mpegts.o             \
                                            rtpdec_opus.o               \
                                            rtpdec_qcelp.o              \
                                            rtpdec_qdm2.o               \
                                            rtpdec_qt.o                 \
                                            rtpdec_rfc4175.o            \
                                            rtpdec_svq3.o               \
                                            rtpdec_vc2hq.o              \
                                            rtpdec_vp8.o                \
                                            rtpdec_vp9.o                \
                                            rtpdec_xiph.o
OBJS-$(CONFIG_RTPENC_CHAIN)              += rtpenc_chain.o rtp.o
OBJS-$(CONFIG_SRTP)                      += srtp.o

# muxers/demuxers
OBJS-$(CONFIG_A64_MUXER)                 += a64.o rawenc.o
OBJS-$(CONFIG_AAC_DEMUXER)               += aacdec.o apetag.o img2.o rawdec.o
OBJS-$(CONFIG_AC3_DEMUXER)               += ac3dec.o rawdec.o
OBJS-$(CONFIG_AC3_MUXER)                 += rawenc.o
OBJS-$(CONFIG_ACM_DEMUXER)               += acm.o rawdec.o
OBJS-$(CONFIG_ADTS_MUXER)                += adtsenc.o apetag.o img2.o \
                                            id3v2enc.o
OBJS-$(CONFIG_ADX_DEMUXER)               += adxdec.o rawdec.o
OBJS-$(CONFIG_ADX_MUXER)                 += rawenc.o
OBJS-$(CONFIG_AEA_DEMUXER)               += aeadec.o pcm.o
OBJS-$(CONFIG_AEA_MUXER)                 += aeaenc.o rawenc.o
OBJS-$(CONFIG_AIFF_DEMUXER)              += aiffdec.o aiff.o pcm.o \
                                            mov_chan.o replaygain.o
OBJS-$(CONFIG_AIFF_MUXER)                += aiffenc.o aiff.o id3v2enc.o
OBJS-$(CONFIG_ALP_MUXER)                 += alp.o rawenc.o
OBJS-$(CONFIG_AMR_DEMUXER)               += amr.o rawdec.o
OBJS-$(CONFIG_AMR_MUXER)                 += amr.o rawenc.o
OBJS-$(CONFIG_AMRNB_DEMUXER)             += amr.o rawdec.o
OBJS-$(CONFIG_AMRWB_DEMUXER)             += amr.o rawdec.o
OBJS-$(CONFIG_APAC_DEMUXER)              += apac.o rawdec.o
OBJS-$(CONFIG_APE_DEMUXER)               += ape.o apetag.o img2.o
OBJS-$(CONFIG_APM_MUXER)                 += apm.o rawenc.o
OBJS-$(CONFIG_APNG_DEMUXER)              += apngdec.o
OBJS-$(CONFIG_APNG_MUXER)                += apngenc.o
OBJS-$(CONFIG_APTX_MUXER)                += rawenc.o
OBJS-$(CONFIG_APTX_HD_MUXER)             += rawenc.o
OBJS-$(CONFIG_AQTITLE_DEMUXER)           += aqtitledec.o subtitles.o
OBJS-$(CONFIG_ASF_DEMUXER)               += asfdec_f.o asf.o asfcrypt.o \
                                            asf_tags.o avlanguage.o
OBJS-$(CONFIG_ASF_O_DEMUXER)             += asfdec_o.o asf.o asfcrypt.o \
                                            asf_tags.o avlanguage.o
OBJS-$(CONFIG_ASF_MUXER)                 += asfenc.o asf.o asf_tags.o \
                                            avlanguage.o
OBJS-$(CONFIG_ASS_DEMUXER)               += assdec.o subtitles.o
OBJS-$(CONFIG_ASS_MUXER)                 += assenc.o
OBJS-$(CONFIG_AU_DEMUXER)                += au.o pcm.o
OBJS-$(CONFIG_AU_MUXER)                  += au.o rawenc.o
OBJS-$(CONFIG_AVI_MUXER)                 += avienc.o mpegtsenc.o avlanguage.o rawutils.o
OBJS-$(CONFIG_AVR_DEMUXER)               += avr.o pcm.o
OBJS-$(CONFIG_AVS2_DEMUXER)              += avs2dec.o rawdec.o
OBJS-$(CONFIG_AVS2_MUXER)                += rawenc.o
OBJS-$(CONFIG_AVS3_DEMUXER)              += avs3dec.o rawdec.o
OBJS-$(CONFIG_AVS3_MUXER)                += rawenc.o
OBJS-$(CONFIG_BONK_DEMUXER)              += bonk.o rawdec.o
OBJS-$(CONFIG_CAF_DEMUXER)               += cafdec.o caf.o mov_chan.o mov_esds.o
OBJS-$(CONFIG_CAF_MUXER)                 += cafenc.o caf.o riff.o
OBJS-$(CONFIG_CAVSVIDEO_DEMUXER)         += cavsvideodec.o rawdec.o
OBJS-$(CONFIG_CAVSVIDEO_MUXER)           += rawenc.o
OBJS-$(CONFIG_CODEC2_DEMUXER)            += codec2.o pcm.o
OBJS-$(CONFIG_CODEC2_MUXER)              += codec2.o rawenc.o
OBJS-$(CONFIG_CODEC2RAW_DEMUXER)         += codec2.o pcm.o
OBJS-$(CONFIG_CODEC2RAW_MUXER)           += rawenc.o
OBJS-$(CONFIG_DATA_DEMUXER)              += rawdec.o
OBJS-$(CONFIG_DATA_MUXER)                += rawenc.o
OBJS-$(CONFIG_DASH_MUXER)                += dash.o dashenc.o hlsplaylist.o
OBJS-$(CONFIG_DASH_DEMUXER)              += dash.o dashdec.o
OBJS-$(CONFIG_DERF_DEMUXER)              += derf.o pcm.o
OBJS-$(CONFIG_DFPWM_DEMUXER)             += dfpwmdec.o pcm.o
OBJS-$(CONFIG_DFPWM_MUXER)               += rawenc.o
OBJS-$(CONFIG_DIRAC_DEMUXER)             += diracdec.o rawdec.o
OBJS-$(CONFIG_DIRAC_MUXER)               += rawenc.o
OBJS-$(CONFIG_DNXHD_DEMUXER)             += dnxhddec.o rawdec.o
OBJS-$(CONFIG_DNXHD_MUXER)               += rawenc.o
OBJS-$(CONFIG_DTS_DEMUXER)               += dtsdec.o rawdec.o
OBJS-$(CONFIG_DTS_MUXER)                 += rawenc.o
OBJS-$(CONFIG_DVBSUB_DEMUXER)            += dvbsub.o rawdec.o
OBJS-$(CONFIG_DVBTXT_DEMUXER)            += dvbtxt.o rawdec.o
OBJS-$(CONFIG_DVDVIDEO_DEMUXER)          += dvdvideodec.o dvdclut.o
OBJS-$(CONFIG_EAC3_DEMUXER)              += ac3dec.o rawdec.o
OBJS-$(CONFIG_EAC3_MUXER)                += rawenc.o
OBJS-$(CONFIG_EPAF_DEMUXER)              += epafdec.o pcm.o
OBJS-$(CONFIG_FILMSTRIP_MUXER)           += filmstripenc.o rawenc.o
OBJS-$(CONFIG_FLAC_DEMUXER)              += flacdec.o rawdec.o \
                                            flac_picture.o   \
                                            oggparsevorbis.o \
                                            replaygain.o     \
                                            vorbiscomment.o
OBJS-$(CONFIG_FLAC_MUXER)                += flacenc.o flacenc_header.o \
                                            vorbiscomment.o
OBJS-$(CONFIG_FLV_DEMUXER)               += flvdec.o
OBJS-$(CONFIG_LIVE_FLV_DEMUXER)          += flvdec.o
OBJS-$(CONFIG_FLV_MUXER)                 += flvenc.o
OBJS-$(CONFIG_FWSE_DEMUXER)              += fwse.o pcm.o
OBJS-$(CONFIG_GSM_MUXER)                 += rawenc.o
OBJS-$(CONFIG_G722_DEMUXER)              += g722.o rawdec.o
OBJS-$(CONFIG_G722_MUXER)                += rawenc.o
OBJS-$(CONFIG_G723_1_MUXER)              += rawenc.o
OBJS-$(CONFIG_G726_MUXER)                += rawenc.o
OBJS-$(CONFIG_G726LE_MUXER)              += rawenc.o
OBJS-$(CONFIG_H261_DEMUXER)              += h261dec.o rawdec.o
OBJS-$(CONFIG_H261_MUXER)                += rawenc.o
OBJS-$(CONFIG_H263_DEMUXER)              += h263dec.o rawdec.o
OBJS-$(CONFIG_H263_MUXER)                += rawenc.o
OBJS-$(CONFIG_H264_DEMUXER)              += h264dec.o rawdec.o
OBJS-$(CONFIG_H264_MUXER)                += rawenc.o
OBJS-$(CONFIG_HCOM_DEMUXER)              += hcom.o pcm.o
OBJS-$(CONFIG_HEVC_DEMUXER)              += hevcdec.o rawdec.o
OBJS-$(CONFIG_HEVC_MUXER)                += rawenc.o
OBJS-$(CONFIG_EVC_DEMUXER)               += evcdec.o rawdec.o
OBJS-$(CONFIG_EVC_MUXER)                 += rawenc.o
OBJS-$(CONFIG_HLS_DEMUXER)               += hls.o hls_sample_encryption.o
OBJS-$(CONFIG_HLS_MUXER)                 += hlsenc.o hlsplaylist.o
OBJS-$(CONFIG_ILBC_MUXER)                += ilbc.o rawenc.o
OBJS-$(CONFIG_IMAGE2_DEMUXER)            += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE2_MUXER)              += img2enc.o img2.o
OBJS-$(CONFIG_IMAGE2PIPE_DEMUXER)        += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE2PIPE_MUXER)          += img2enc.o img2.o
OBJS-$(CONFIG_IMAGE_BMP_PIPE_DEMUXER)     += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_CRI_PIPE_DEMUXER)     += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_DDS_PIPE_DEMUXER)     += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_DPX_PIPE_DEMUXER)     += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_EXR_PIPE_DEMUXER)     += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_GEM_PIPE_DEMUXER)     += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_GIF_PIPE_DEMUXER)     += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_HDR_PIPE_DEMUXER)     += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_J2K_PIPE_DEMUXER)     += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_JPEG_PIPE_DEMUXER)    += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_JPEGLS_PIPE_DEMUXER)  += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_JPEGXL_PIPE_DEMUXER)  += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_JPEGXS_PIPE_DEMUXER)  += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_PAM_PIPE_DEMUXER)     += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_PBM_PIPE_DEMUXER)     += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_PCX_PIPE_DEMUXER)     += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_PFM_PIPE_DEMUXER)     += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_PGMYUV_PIPE_DEMUXER)  += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_PGM_PIPE_DEMUXER)     += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_PGX_PIPE_DEMUXER)     += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_PHM_PIPE_DEMUXER)     += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_PHOTOCD_PIPE_DEMUXER) += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_PICTOR_PIPE_DEMUXER)  += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_PNG_PIPE_DEMUXER)     += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_PPM_PIPE_DEMUXER)     += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_PSD_PIPE_DEMUXER)     += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_QDRAW_PIPE_DEMUXER)   += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_QOI_PIPE_DEMUXER)     += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_SGI_PIPE_DEMUXER)     += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_SVG_PIPE_DEMUXER)     += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_SUNRAST_PIPE_DEMUXER) += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_TIFF_PIPE_DEMUXER)    += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_VBN_PIPE_DEMUXER)     += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_WEBP_PIPE_DEMUXER)    += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_XBM_PIPE_DEMUXER)     += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_XPM_PIPE_DEMUXER)     += img2dec.o img2.o
OBJS-$(CONFIG_IMAGE_XWD_PIPE_DEMUXER)     += img2dec.o img2.o
OBJS-$(CONFIG_INGENIENT_DEMUXER)         += ingenientdec.o rawdec.o
OBJS-$(CONFIG_IPU_DEMUXER)               += ipudec.o rawdec.o
OBJS-$(CONFIG_IRCAM_DEMUXER)             += ircamdec.o ircam.o pcm.o
OBJS-$(CONFIG_IRCAM_MUXER)               += ircamenc.o ircam.o rawenc.o
OBJS-$(CONFIG_IVR_DEMUXER)               += rmdec.o rm.o rmsipr.o
OBJS-$(CONFIG_JACOSUB_DEMUXER)           += jacosubdec.o subtitles.o
OBJS-$(CONFIG_JACOSUB_MUXER)             += jacosubenc.o rawenc.o
OBJS-$(CONFIG_KUX_DEMUXER)               += flvdec.o
OBJS-$(CONFIG_KVAG_MUXER)                += kvag.o rawenc.o
OBJS-$(CONFIG_LATM_MUXER)                += latmenc.o rawenc.o
OBJS-$(CONFIG_LOAS_DEMUXER)              += loasdec.o rawdec.o
OBJS-$(CONFIG_LRC_DEMUXER)               += lrcdec.o lrc.o subtitles.o
OBJS-$(CONFIG_M4V_DEMUXER)               += m4vdec.o rawdec.o
OBJS-$(CONFIG_M4V_MUXER)                 += rawenc.o
OBJS-$(CONFIG_MATROSKA_DEMUXER)          += matroskadec.o matroska.o  \
                                            flac_picture.o rmsipr.o \
                                            oggparsevorbis.o vorbiscomment.o \
                                            qtpalette.o replaygain.o dovi_isom.o
OBJS-$(CONFIG_MATROSKA_MUXER)            += matroskaenc.o matroska.o \
                                            flacenc_header.o avlanguage.o \
                                            vorbiscomment.o wv.o dovi_isom.o
OBJS-$(CONFIG_MCC_DEMUXER)               += mccdec.o subtitles.o
OBJS-$(CONFIG_MICRODVD_DEMUXER)          += microdvddec.o subtitles.o
OBJS-$(CONFIG_MJPEG_2000_DEMUXER)        += rawdec.o mj2kdec.o
OBJS-$(CONFIG_MJPEG_DEMUXER)             += rawdec.o
OBJS-$(CONFIG_MJPEG_MUXER)               += rawenc.o
OBJS-$(CONFIG_MLP_DEMUXER)               += rawdec.o mlpdec.o
OBJS-$(CONFIG_MLP_MUXER)                 += rawenc.o
OBJS-$(CONFIG_MLV_DEMUXER)               += mlvdec.o riffdec.o
OBJS-$(CONFIG_MMF_MUXER)                 += mmf.o rawenc.o
OBJS-$(CONFIG_MOV_DEMUXER)               += mov.o mov_chan.o mov_esds.o \
                                            qtpalette.o replaygain.o dovi_isom.o \
                                            dvdclut.o
OBJS-$(CONFIG_MOV_MUXER)                 += movenc.o \
                                            movenchint.o mov_chan.o rtp.o \
                                            movenccenc.o movenc_ttml.o rawutils.o \
                                            apv.o dovi_isom.o evc.o cbs.o cbs_av1.o cbs_apv.o
OBJS-$(CONFIG_MP2_MUXER)                 += rawenc.o
OBJS-$(CONFIG_MP3_DEMUXER)               += mp3dec.o replaygain.o
OBJS-$(CONFIG_MP3_MUXER)                 += mp3enc.o rawenc.o id3v2enc.o
OBJS-$(CONFIG_MPC_DEMUXER)               += mpc.o apetag.o img2.o
OBJS-$(CONFIG_MPC8_DEMUXER)              += mpc8.o apetag.o img2.o
OBJS-$(CONFIG_MPEG1VIDEO_MUXER)          += rawenc.o
OBJS-$(CONFIG_MPEG2VIDEO_MUXER)          += rawenc.o
OBJS-$(CONFIG_MPEGTS_DEMUXER)            += mpegts.o
OBJS-$(CONFIG_MPEGTS_MUXER)              += mpegtsenc.o
OBJS-$(CONFIG_MPEGVIDEO_DEMUXER)         += mpegvideodec.o rawdec.o
OBJS-$(CONFIG_MPL2_DEMUXER)              += mpl2dec.o subtitles.o
OBJS-$(CONFIG_MPSUB_DEMUXER)             += mpsubdec.o subtitles.o
OBJS-$(CONFIG_MXF_DEMUXER)               += mxfdec.o mxf.o avlanguage.o
OBJS-$(CONFIG_NISTSPHERE_DEMUXER)        += nistspheredec.o pcm.o
OBJS-$(CONFIG_NSP_DEMUXER)               += nspdec.o pcm.o
OBJS-$(CONFIG_NULL_MUXER)                += nullenc.o
OBJS-$(CONFIG_OBU_MUXER)                 += rawenc.o
OBJS-$(CONFIG_OGG_DEMUXER)               += oggdec.o         \
                                            oggparsecelt.o   \
                                            oggparsedirac.o  \
                                            oggparseflac.o   \
                                            oggparseogm.o    \
                                            oggparseopus.o   \
                                            oggparseskeleton.o \
                                            oggparsespeex.o  \
                                            oggparsetheora.o \
                                            oggparsevorbis.o \
                                            oggparsevp8.o    \
                                            replaygain.o     \
                                            vorbiscomment.o  \
                                            flac_picture.o
OBJS-$(CONFIG_OGA_MUXER)                 += oggenc.o \
                                            vorbiscomment.o
OBJS-$(CONFIG_OGG_MUXER)                 += oggenc.o \
                                            vorbiscomment.o
OBJS-$(CONFIG_OGV_MUXER)                 += oggenc.o \
                                            vorbiscomment.o
OBJS-$(CONFIG_OMA_DEMUXER)               += omadec.o pcm.o oma.o
OBJS-$(CONFIG_OMA_MUXER)                 += omaenc.o rawenc.o oma.o id3v2enc.o
OBJS-$(CONFIG_OPUS_MUXER)                += oggenc.o \
                                            vorbiscomment.o
OBJS-$(CONFIG_OSQ_DEMUXER)               += osq.o rawdec.o
OBJS-$(CONFIG_PCM_ALAW_DEMUXER)          += pcmdec.o pcm.o
OBJS-$(CONFIG_PCM_ALAW_MUXER)            += pcmenc.o rawenc.o
OBJS-$(CONFIG_PCM_F32BE_DEMUXER)         += pcmdec.o pcm.o
OBJS-$(CONFIG_PCM_F32BE_MUXER)           += pcmenc.o rawenc.o
OBJS-$(CONFIG_PCM_F32LE_DEMUXER)         += pcmdec.o pcm.o
OBJS-$(CONFIG_PCM_F32LE_MUXER)           += pcmenc.o rawenc.o
OBJS-$(CONFIG_PCM_F64BE_DEMUXER)         += pcmdec.o pcm.o
OBJS-$(CONFIG_PCM_F64BE_MUXER)           += pcmenc.o rawenc.o
OBJS-$(CONFIG_PCM_F64LE_DEMUXER)         += pcmdec.o pcm.o
OBJS-$(CONFIG_PCM_F64LE_MUXER)           += pcmenc.o rawenc.o
OBJS-$(CONFIG_PCM_MULAW_DEMUXER)         += pcmdec.o pcm.o
OBJS-$(CONFIG_PCM_MULAW_MUXER)           += pcmenc.o rawenc.o
OBJS-$(CONFIG_PCM_S16BE_DEMUXER)         += pcmdec.o pcm.o
OBJS-$(CONFIG_PCM_S16BE_MUXER)           += pcmenc.o rawenc.o
OBJS-$(CONFIG_PCM_S16LE_DEMUXER)         += pcmdec.o pcm.o
OBJS-$(CONFIG_PCM_S16LE_MUXER)           += pcmenc.o rawenc.o
OBJS-$(CONFIG_PCM_S24BE_DEMUXER)         += pcmdec.o pcm.o
OBJS-$(CONFIG_PCM_S24BE_MUXER)           += pcmenc.o rawenc.o
OBJS-$(CONFIG_PCM_S24LE_DEMUXER)         += pcmdec.o pcm.o
OBJS-$(CONFIG_PCM_S24LE_MUXER)           += pcmenc.o rawenc.o
OBJS-$(CONFIG_PCM_S32BE_DEMUXER)         += pcmdec.o pcm.o
OBJS-$(CONFIG_PCM_S32BE_MUXER)           += pcmenc.o rawenc.o
OBJS-$(CONFIG_PCM_S32LE_DEMUXER)         += pcmdec.o pcm.o
OBJS-$(CONFIG_PCM_S32LE_MUXER)           += pcmenc.o rawenc.o
OBJS-$(CONFIG_PCM_S8_DEMUXER)            += pcmdec.o pcm.o
OBJS-$(CONFIG_PCM_S8_MUXER)              += pcmenc.o rawenc.o
OBJS-$(CONFIG_PCM_U16BE_DEMUXER)         += pcmdec.o pcm.o
OBJS-$(CONFIG_PCM_U16BE_MUXER)           += pcmenc.o rawenc.o
OBJS-$(CONFIG_PCM_U16LE_DEMUXER)         += pcmdec.o pcm.o
OBJS-$(CONFIG_PCM_U16LE_MUXER)           += pcmenc.o rawenc.o
OBJS-$(CONFIG_PCM_U24BE_DEMUXER)         += pcmdec.o pcm.o
OBJS-$(CONFIG_PCM_U24BE_MUXER)           += pcmenc.o rawenc.o
OBJS-$(CONFIG_PCM_U24LE_DEMUXER)         += pcmdec.o pcm.o
OBJS-$(CONFIG_PCM_U24LE_MUXER)           += pcmenc.o rawenc.o
OBJS-$(CONFIG_PCM_U32BE_DEMUXER)         += pcmdec.o pcm.o
OBJS-$(CONFIG_PCM_U32BE_MUXER)           += pcmenc.o rawenc.o
OBJS-$(CONFIG_PCM_U32LE_DEMUXER)         += pcmdec.o pcm.o
OBJS-$(CONFIG_PCM_U32LE_MUXER)           += pcmenc.o rawenc.o
OBJS-$(CONFIG_PCM_U8_DEMUXER)            += pcmdec.o pcm.o
OBJS-$(CONFIG_PCM_U8_MUXER)              += pcmenc.o rawenc.o
OBJS-$(CONFIG_PCM_VIDC_DEMUXER)          += pcmdec.o pcm.o
OBJS-$(CONFIG_PCM_VIDC_MUXER)            += pcmenc.o rawenc.o
OBJS-$(CONFIG_PJS_DEMUXER)               += pjsdec.o subtitles.o
OBJS-$(CONFIG_PVF_DEMUXER)               += pvfdec.o pcm.o
OBJS-$(CONFIG_RAWVIDEO_MUXER)            += rawenc.o
OBJS-$(CONFIG_RCWT_DEMUXER)              += rcwtdec.o subtitles.o
OBJS-$(CONFIG_RCWT_MUXER)                += rcwtenc.o subtitles.o
OBJS-$(CONFIG_REALTEXT_DEMUXER)          += realtextdec.o subtitles.o
OBJS-$(CONFIG_RKA_DEMUXER)               += rka.o apetag.o img2.o
OBJS-$(CONFIG_RM_DEMUXER)                += rmdec.o rm.o rmsipr.o
OBJS-$(CONFIG_RM_MUXER)                  += rmenc.o rm.o
OBJS-$(CONFIG_ROQ_MUXER)                 += idroqenc.o rawenc.o
OBJS-$(CONFIG_RSO_DEMUXER)               += rsodec.o rso.o pcm.o
OBJS-$(CONFIG_RSO_MUXER)                 += rsoenc.o rso.o rawenc.o
OBJS-$(CONFIG_RTP_MPEGTS_MUXER)          += rtpenc_mpegts.o
OBJS-$(CONFIG_RTP_MUXER)                 += rtp.o         \
                                            rtpenc_aac.o     \
                                            rtpenc_latm.o    \
                                            rtpenc_amr.o     \
                                            rtpenc_av1.o     \
                                            rtpenc_h261.o    \
                                            rtpenc_h263.o    \
                                            rtpenc_h263_rfc2190.o \
                                            rtpenc_h264_hevc.o    \
                                            rtpenc_jpeg.o \
                                            rtpenc_mpv.o     \
                                            rtpenc.o      \
                                            rtpenc_rfc4175.o    \
                                            rtpenc_vc2hq.o              \
                                            rtpenc_vp8.o  \
                                            rtpenc_vp9.o                \
                                            rtpenc_xiph.o
OBJS-$(CONFIG_RTSP_DEMUXER)              += rtsp.o rtspdec.o httpauth.o
OBJS-$(CONFIG_RTSP_MUXER)                += rtsp.o rtspenc.o httpauth.o
OBJS-$(CONFIG_SAMI_DEMUXER)              += samidec.o subtitles.o
OBJS-$(CONFIG_SBC_DEMUXER)               += sbcdec.o rawdec.o
OBJS-$(CONFIG_SBC_MUXER)                 += rawenc.o
OBJS-$(CONFIG_SCC_DEMUXER)               += sccdec.o subtitles.o
OBJS-$(CONFIG_SDP_DEMUXER)               += rtsp.o
OBJS-$(CONFIG_SDX_DEMUXER)               += sdxdec.o pcm.o
OBJS-$(CONFIG_SHORTEN_DEMUXER)           += shortendec.o rawdec.o
OBJS-$(CONFIG_SLN_DEMUXER)               += pcmdec.o pcm.o
OBJS-$(CONFIG_SOL_DEMUXER)               += sol.o pcm.o
OBJS-$(CONFIG_SOX_DEMUXER)               += soxdec.o pcm.o
OBJS-$(CONFIG_SOX_MUXER)                 += soxenc.o rawenc.o
OBJS-$(CONFIG_SPEEX_MUXER)               += oggenc.o \
                                            vorbiscomment.o
OBJS-$(CONFIG_SRT_DEMUXER)               += srtdec.o subtitles.o
OBJS-$(CONFIG_SRT_MUXER)                 += srtenc.o
OBJS-$(CONFIG_STL_DEMUXER)               += stldec.o subtitles.o
OBJS-$(CONFIG_SUBVIEWER1_DEMUXER)        += subviewer1dec.o subtitles.o
OBJS-$(CONFIG_SUBVIEWER_DEMUXER)         += subviewerdec.o subtitles.o
OBJS-$(CONFIG_TAK_DEMUXER)               += takdec.o apetag.o img2.o rawdec.o
OBJS-$(CONFIG_TEDCAPTIONS_DEMUXER)       += tedcaptionsdec.o subtitles.o
OBJS-$(CONFIG_TRUEHD_DEMUXER)            += rawdec.o mlpdec.o
OBJS-$(CONFIG_TRUEHD_MUXER)              += rawenc.o
OBJS-$(CONFIG_TTA_DEMUXER)               += tta.o apetag.o img2.o
OBJS-$(CONFIG_TTA_MUXER)                 += ttaenc.o apetag.o img2.o
OBJS-$(CONFIG_VC1_DEMUXER)               += rawdec.o vc1dec.o
OBJS-$(CONFIG_VC1_MUXER)                 += rawenc.o
OBJS-$(CONFIG_VOBSUB_DEMUXER)            += subtitles.o # mpeg demuxer is in the dependencies
OBJS-$(CONFIG_VPLAYER_DEMUXER)           += vplayerdec.o subtitles.o
OBJS-$(CONFIG_VVC_DEMUXER)               += vvcdec.o rawdec.o
OBJS-$(CONFIG_VVC_MUXER)                 += rawenc.o
OBJS-$(CONFIG_W64_DEMUXER)               += wavdec.o w64.o pcm.o
OBJS-$(CONFIG_W64_MUXER)                 += wavenc.o w64.o
OBJS-$(CONFIG_WADY_DEMUXER)              += wady.o pcm.o
OBJS-$(CONFIG_WAV_DEMUXER)               += wavdec.o pcm.o
OBJS-$(CONFIG_WAV_MUXER)                 += wavenc.o
OBJS-$(CONFIG_WEBM_MUXER)                += matroskaenc.o matroska.o \
                                            avlanguage.o
OBJS-$(CONFIG_WEBP_MUXER)                += webpenc.o
OBJS-$(CONFIG_WEBVTT_DEMUXER)            += webvttdec.o subtitles.o
OBJS-$(CONFIG_WEBVTT_MUXER)              += webvttenc.o
OBJS-$(CONFIG_WHIP_MUXER)                += whip.o avc.o http.o srtp.o
OBJS-$(CONFIG_WSD_DEMUXER)               += wsddec.o rawdec.o
OBJS-$(CONFIG_WV_DEMUXER)                += wvdec.o wv.o apetag.o img2.o
OBJS-$(CONFIG_WVE_DEMUXER)               += wvedec.o pcm.o
OBJS-$(CONFIG_WV_MUXER)                  += wvenc.o wv.o apetag.o img2.o
OBJS-$(CONFIG_XMD_DEMUXER)               += xmd.o pcm.o

# external library muxers/demuxers

# protocols I/O
OBJS-$(CONFIG_ANDROID_CONTENT_PROTOCOL)  += file.o
OBJS-$(CONFIG_CRYPTO_PROTOCOL)           += crypto.o
OBJS-$(CONFIG_DATA_PROTOCOL)             += data_uri.o
OBJS-$(CONFIG_FFRTMPCRYPT_PROTOCOL)      += rtmpcrypt.o rtmpdigest.o rtmpdh.o
OBJS-$(CONFIG_FFRTMPHTTP_PROTOCOL)       += rtmphttp.o
OBJS-$(CONFIG_FILE_PROTOCOL)             += file.o
OBJS-$(CONFIG_FD_PROTOCOL)               += file.o
OBJS-$(CONFIG_HTTP_PROTOCOL)             += http.o httpauth.o
OBJS-$(CONFIG_HTTPPROXY_PROTOCOL)        += http.o httpauth.o
OBJS-$(CONFIG_HTTPS_PROTOCOL)            += http.o httpauth.o
OBJS-$(CONFIG_MMSH_PROTOCOL)             += mmsh.o mms.o asf_tags.o
OBJS-$(CONFIG_MMST_PROTOCOL)             += mmst.o mms.o asf_tags.o
OBJS-$(CONFIG_PIPE_PROTOCOL)             += file.o
OBJS-$(CONFIG_RTMP_PROTOCOL)             += rtmpproto.o rtmpdigest.o rtmppkt.o
OBJS-$(CONFIG_RTMPE_PROTOCOL)            += rtmpproto.o rtmpdigest.o rtmppkt.o
OBJS-$(CONFIG_RTMPS_PROTOCOL)            += rtmpproto.o rtmpdigest.o rtmppkt.o
OBJS-$(CONFIG_RTMPT_PROTOCOL)            += rtmpproto.o rtmpdigest.o rtmppkt.o
OBJS-$(CONFIG_RTMPTE_PROTOCOL)           += rtmpproto.o rtmpdigest.o rtmppkt.o
OBJS-$(CONFIG_RTMPTS_PROTOCOL)           += rtmpproto.o rtmpdigest.o rtmppkt.o
OBJS-$(CONFIG_RTP_PROTOCOL)              += rtpproto.o ip.o
OBJS-$(CONFIG_SMP_DEMUXER)               += smpdec.o
OBJS-$(CONFIG_SMP_MUXER)                 += smpenc.o
OBJS-$(CONFIG_SMP_PROTOCOL)              += smp.o
OBJS-$(CONFIG_SRTP_PROTOCOL)             += srtpproto.o srtp.o
OBJS-$(CONFIG_TCP_PROTOCOL)              += tcp.o
TLS-OBJS-$(CONFIG_GNUTLS)                += tls_gnutls.o
TLS-OBJS-$(CONFIG_LIBTLS)                += tls_libtls.o
TLS-OBJS-$(CONFIG_MBEDTLS)               += tls_mbedtls.o
TLS-OBJS-$(CONFIG_OPENSSL)               += tls_openssl.o
TLS-OBJS-$(CONFIG_SECURETRANSPORT)       += tls_securetransport.o
TLS-OBJS-$(CONFIG_SCHANNEL)              += tls_schannel.o
OBJS-$(CONFIG_TLS_PROTOCOL)              += tls.o $(TLS-OBJS-yes)
OBJS-$(CONFIG_DTLS_PROTOCOL)             += tls.o $(TLS-OBJS-yes)
OBJS-$(CONFIG_UDP_PROTOCOL)              += udp.o ip.o
OBJS-$(CONFIG_UDPLITE_PROTOCOL)          += udp.o ip.o
OBJS-$(CONFIG_UNIX_PROTOCOL)             += unix.o

# external library protocols
OBJS-$(CONFIG_SRT_PROTOCOL)             += srt.o

# Objects duplicated from other libraries for shared builds

# libavdevice dependencies

# Windows resource file

SKIPHEADERS-$(CONFIG_IMF_DEMUXER)        += imf.h
SKIPHEADERS-$(CONFIG_FFRTMPCRYPT_PROTOCOL) += rtmpdh.h
SKIPHEADERS-$(CONFIG_NETWORK)            += network.h rtsp.h

TESTPROGS = seek                                                        \
            url                                                         \
            seek_utils
#           async                                                       \

FIFO-MUXER-TESTPROGS-$(CONFIG_NETWORK)   += fifo_muxer
TESTPROGS-$(CONFIG_FIFO_MUXER)           += $(FIFO-MUXER-TESTPROGS-yes)
TESTPROGS-$(CONFIG_FFRTMPCRYPT_PROTOCOL) += rtmpdh
TESTPROGS-$(CONFIG_MOV_MUXER)            += movenc
TESTPROGS-$(CONFIG_NETWORK)              += noproxy
TESTPROGS-$(CONFIG_SRTP)                 += srtp
TESTPROGS-$(CONFIG_IMF_DEMUXER)          += imf

TOOLS     = aviocat                                                     \
            ismindex                                                    \
            pktdumper                                                   \
            probetest                                                   \
            seek_print                                                  \
            sidxindex                                                   \
            venc_data_dump
