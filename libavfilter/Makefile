NAME = avfilter
DESC = FFmpeg audio/video filtering library

HEADERS = avfilter.h                                                    \
          buffersink.h                                                  \
          buffersrc.h                                                   \
          version.h                                                     \
          version_major.h                                               \

OBJS = allfilters.o                                                     \
       audio.o                                                          \
       avfilter.o                                                       \
       avfiltergraph.o                                                  \
       buffersink.o                                                     \
       buffersrc.o                                                      \
       colorspace.o                                                     \
       ccfifo.o                                                         \
       drawutils.o                                                      \
       formats.o                                                        \
       framepool.o                                                      \
       framequeue.o                                                     \
       graphdump.o                                                      \
       graphparser.o                                                    \
       version.o                                                        \
       video.o                                                          \


OBJS-$(HAVE_THREADS)                         += pthread.o

# subsystems
OBJS-$(CONFIG_SCENE_SAD)                     += scene_sad.o

# audio filters
OBJS-$(CONFIG_ACROSSFADE_FILTER)             += af_afade.o
OBJS-$(CONFIG_AFADE_FILTER)                  += af_afade.o
OBJS-$(CONFIG_AFORMAT_FILTER)                += af_aformat.o
OBJS-$(CONFIG_AMIX_FILTER)                   += af_amix.o
OBJS-$(CONFIG_ANULL_FILTER)                  += af_anull.o
OBJS-$(CONFIG_AREALTIME_FILTER)              += f_realtime.o
OBJS-$(CONFIG_ARESAMPLE_FILTER)              += af_aresample.o
OBJS-$(CONFIG_ASETPTS_FILTER)                += setpts.o
OBJS-$(CONFIG_ASPLIT_FILTER)                 += split.o
OBJS-$(CONFIG_ASTREAMSELECT_FILTER)          += f_streamselect.o framesync.o
OBJS-$(CONFIG_ATRIM_FILTER)                  += trim.o
OBJS-$(CONFIG_VOLUME_FILTER)                 += af_volume.o

OBJS-$(CONFIG_ANULLSRC_FILTER)               += asrc_anullsrc.o


OBJS-$(CONFIG_ANULLSINK_FILTER)              += asink_anullsink.o

# video filters
OBJS-$(CONFIG_ALPHAMERGE_FILTER)             += vf_alphamerge.o framesync.o
OBJS-$(CONFIG_BLEND_FILTER)                  += vf_blend.o framesync.o
OBJS-$(CONFIG_BM3D_FILTER)                   += vf_bm3d.o framesync.o

OBJS-$(CONFIG_COLORBALANCE_FILTER)           += vf_colorbalance.o
OBJS-$(CONFIG_COLORMAP_FILTER)               += vf_colormap.o framesync.o
OBJS-$(CONFIG_CONVOLVE_FILTER)               += vf_convolve.o framesync.o
OBJS-$(CONFIG_COPY_FILTER)                   += vf_copy.o
OBJS-$(CONFIG_CORR_FILTER)                   += vf_corr.o framesync.o
OBJS-$(CONFIG_CROP_FILTER)                   += vf_crop.o
OBJS-$(CONFIG_DECONVOLVE_FILTER)             += vf_convolve.o framesync.o
OBJS-$(CONFIG_DISPLACE_FILTER)               += vf_displace.o framesync.o
OBJS-$(CONFIG_DRAWBOX_FILTER)                += vf_drawbox.o
OBJS-$(CONFIG_DRAWGRID_FILTER)               += vf_drawbox.o
OBJS-$(CONFIG_FADE_FILTER)                   += vf_fade.o
OBJS-$(CONFIG_FORMAT_FILTER)                 += vf_format.o
OBJS-$(CONFIG_FPS_FILTER)                    += vf_fps.o
OBJS-$(CONFIG_FRAMERATE_FILTER)              += vf_framerate.o
OBJS-$(CONFIG_GUIDED_FILTER)                 += vf_guided.o framesync.o
OBJS-$(CONFIG_HALDCLUT_FILTER)               += vf_lut3d.o framesync.o
OBJS-$(CONFIG_HFLIP_FILTER)                  += vf_hflip.o
OBJS-$(CONFIG_HSTACK_FILTER)                 += vf_stack.o framesync.o
OBJS-$(CONFIG_HUE_FILTER)                    += vf_hue.o
OBJS-$(CONFIG_HYSTERESIS_FILTER)             += vf_hysteresis.o framesync.o
OBJS-$(CONFIG_IDENTITY_FILTER)               += vf_identity.o framesync.o
OBJS-$(CONFIG_LIBVMAF_FILTER)                += vf_libvmaf.o framesync.o
OBJS-$(CONFIG_LIMITDIFF_FILTER)              += vf_limitdiff.o framesync.o
OBJS-$(CONFIG_LUT2_FILTER)                   += vf_lut2.o framesync.o
OBJS-$(CONFIG_LUT3D_FILTER)                  += vf_lut3d.o framesync.o
OBJS-$(CONFIG_MASKEDCLAMP_FILTER)            += vf_maskedclamp.o framesync.o
OBJS-$(CONFIG_MASKEDMAX_FILTER)              += vf_maskedminmax.o framesync.o
OBJS-$(CONFIG_MASKEDMERGE_FILTER)            += vf_maskedmerge.o framesync.o
OBJS-$(CONFIG_MASKEDMIN_FILTER)              += vf_maskedminmax.o framesync.o
OBJS-$(CONFIG_MASKEDTHRESHOLD_FILTER)        += vf_maskedthreshold.o framesync.o
OBJS-$(CONFIG_MERGEPLANES_FILTER)            += vf_mergeplanes.o framesync.o
OBJS-$(CONFIG_MIDEQUALIZER_FILTER)           += vf_midequalizer.o framesync.o
OBJS-$(CONFIG_MIX_FILTER)                    += vf_mix.o framesync.o
OBJS-$(CONFIG_MORPHO_FILTER)                 += vf_morpho.o framesync.o
OBJS-$(CONFIG_MSAD_FILTER)                   += vf_identity.o framesync.o
OBJS-$(CONFIG_MULTIPLY_FILTER)               += vf_multiply.o framesync.o
OBJS-$(CONFIG_NOFORMAT_FILTER)               += vf_format.o
OBJS-$(CONFIG_NULL_FILTER)                   += vf_null.o
OBJS-$(CONFIG_OCIO_FILTER)                   += vf_opencolorio.o ocio_wrapper.o
OBJS-$(CONFIG_OVERLAY_FILTER)                += vf_overlay.o framesync.o
OBJS-$(CONFIG_OVERLAY_QSV_FILTER)            += vf_overlay_qsv.o framesync.o
OBJS-$(CONFIG_OVERLAY_VAAPI_FILTER)          += vf_overlay_vaapi.o framesync.o vaapi_vpp.o
OBJS-$(CONFIG_PAD_FILTER)                    += vf_pad.o
OBJS-$(CONFIG_PALETTEUSE_FILTER)             += vf_paletteuse.o framesync.o palette.o
OBJS-$(CONFIG_PREMULTIPLY_DYNAMIC_FILTER)    += vf_premultiply.o framesync.o
OBJS-$(CONFIG_PREMULTIPLY_FILTER)            += vf_premultiply.o framesync.o
OBJS-$(CONFIG_PSNR_FILTER)                   += vf_psnr.o framesync.o psnr.o
OBJS-$(CONFIG_REALTIME_FILTER)               += f_realtime.o
OBJS-$(CONFIG_REMAP_FILTER)                  += vf_remap.o framesync.o
OBJS-$(CONFIG_ROTATE_FILTER)                 += vf_rotate.o
OBJS-$(CONFIG_SCALE_FILTER)                  += vf_scale.o scale_eval.o framesync.o
OBJS-$(CONFIG_SCALE_D3D11_FILTER)            += vf_scale_d3d11.o scale_eval.o
OBJS-$(CONFIG_SCALE_D3D12_FILTER)            += vf_scale_d3d12.o scale_eval.o
OBJS-$(CONFIG_SCALE_NPP_FILTER)              += vf_scale_npp.o scale_eval.o
OBJS-$(CONFIG_SCALE_VAAPI_FILTER)            += vf_scale_vaapi.o scale_eval.o vaapi_vpp.o
OBJS-$(CONFIG_SCALE_VT_FILTER)               += vf_scale_vt.o scale_eval.o
OBJS-$(CONFIG_SCALE2REF_FILTER)              += vf_scale.o scale_eval.o framesync.o
OBJS-$(CONFIG_SCALE2REF_NPP_FILTER)          += vf_scale_npp.o scale_eval.o
OBJS-$(CONFIG_SETPTS_FILTER)                 += setpts.o
OBJS-$(CONFIG_SPLIT_FILTER)                  += split.o
OBJS-$(CONFIG_SR_AMF_FILTER)                 += vf_sr_amf.o scale_eval.o vf_amf_common.o
OBJS-$(CONFIG_SSIM_FILTER)                   += vf_ssim.o framesync.o
OBJS-$(CONFIG_SSIM360_FILTER)                += vf_ssim360.o framesync.o
OBJS-$(CONFIG_STREAMSELECT_FILTER)           += f_streamselect.o framesync.o
OBJS-$(CONFIG_TBLEND_FILTER)                 += vf_blend.o framesync.o
OBJS-$(CONFIG_THRESHOLD_FILTER)              += vf_threshold.o framesync.o
OBJS-$(CONFIG_TLUT2_FILTER)                  += vf_lut2.o framesync.o
OBJS-$(CONFIG_TMEDIAN_FILTER)                += vf_xmedian.o framesync.o
OBJS-$(CONFIG_TMIX_FILTER)                   += vf_mix.o framesync.o
OBJS-$(CONFIG_TRANSPOSE_FILTER)              += vf_transpose.o
OBJS-$(CONFIG_TRIM_FILTER)                   += trim.o
OBJS-$(CONFIG_UNPREMULTIPLY_FILTER)          += vf_premultiply.o framesync.o
OBJS-$(CONFIG_VARBLUR_FILTER)                += vf_varblur.o framesync.o
OBJS-$(CONFIG_VFLIP_FILTER)                  += vf_vflip.o
OBJS-$(CONFIG_VIF_FILTER)                    += vf_vif.o framesync.o
OBJS-$(CONFIG_VMAFMOTION_FILTER)             += vf_vmafmotion.o framesync.o
OBJS-$(CONFIG_VPP_AMF_FILTER)                += vf_vpp_amf.o scale_eval.o vf_amf_common.o
OBJS-$(CONFIG_VSTACK_FILTER)                 += vf_stack.o framesync.o
OBJS-$(CONFIG_XCORRELATE_FILTER)             += vf_convolve.o framesync.o
OBJS-$(CONFIG_XMEDIAN_FILTER)                += vf_xmedian.o framesync.o
OBJS-$(CONFIG_XPSNR_FILTER)                  += vf_xpsnr.o framesync.o psnr.o
OBJS-$(CONFIG_XSTACK_FILTER)                 += vf_stack.o framesync.o
OBJS-$(CONFIG_HSTACK_VAAPI_FILTER)           += vf_stack_vaapi.o framesync.o vaapi_vpp.o
OBJS-$(CONFIG_VSTACK_VAAPI_FILTER)           += vf_stack_vaapi.o framesync.o vaapi_vpp.o
OBJS-$(CONFIG_XSTACK_VAAPI_FILTER)           += vf_stack_vaapi.o framesync.o vaapi_vpp.o
OBJS-$(CONFIG_HSTACK_QSV_FILTER)             += vf_stack_qsv.o framesync.o
OBJS-$(CONFIG_VSTACK_QSV_FILTER)             += vf_stack_qsv.o framesync.o
OBJS-$(CONFIG_XSTACK_QSV_FILTER)             += vf_stack_qsv.o framesync.o

OBJS-$(CONFIG_ALLRGB_FILTER)                 += vsrc_testsrc.o
OBJS-$(CONFIG_ALLYUV_FILTER)                 += vsrc_testsrc.o
OBJS-$(CONFIG_COLOR_FILTER)                  += vsrc_testsrc.o
OBJS-$(CONFIG_COLORCHART_FILTER)             += vsrc_testsrc.o
OBJS-$(CONFIG_COLORSPECTRUM_FILTER)          += vsrc_testsrc.o
OBJS-$(CONFIG_GFXCAPTURE_FILTER)             += vsrc_gfxcapture.o vsrc_gfxcapture_winrt.o
OBJS-$(CONFIG_HALDCLUTSRC_FILTER)            += vsrc_testsrc.o
OBJS-$(CONFIG_NULLSRC_FILTER)                += vsrc_testsrc.o
OBJS-$(CONFIG_PAL75BARS_FILTER)              += vsrc_testsrc.o
OBJS-$(CONFIG_PAL100BARS_FILTER)             += vsrc_testsrc.o
OBJS-$(CONFIG_RGBTESTSRC_FILTER)             += vsrc_testsrc.o
OBJS-$(CONFIG_SMPTEBARS_FILTER)              += vsrc_testsrc.o
OBJS-$(CONFIG_SMPTEHDBARS_FILTER)            += vsrc_testsrc.o
OBJS-$(CONFIG_TESTSRC_FILTER)                += vsrc_testsrc.o
OBJS-$(CONFIG_TESTSRC2_FILTER)               += vsrc_testsrc.o
OBJS-$(CONFIG_YUVTESTSRC_FILTER)             += vsrc_testsrc.o
OBJS-$(CONFIG_ZONEPLATE_FILTER)              += vsrc_testsrc.o

OBJS-$(CONFIG_NULLSINK_FILTER)               += vsink_nullsink.o

# multimedia filters
OBJS-$(CONFIG_CONCAT_FILTER)                 += avf_concat.o

# multimedia sources
OBJS-$(CONFIG_AMOVIE_FILTER)                 += src_movie.o
OBJS-$(CONFIG_MOVIE_FILTER)                  += src_movie.o

# vulkan libs

# Objects duplicated from other libraries for shared builds

# Windows resource file

SKIPHEADERS-$(CONFIG_LCMS2)                  += fflcms2.h
SKIPHEADERS-$(CONFIG_LIBVIDSTAB)             += vidstabutils.h


TOOLS     = graph2dot
TESTPROGS = drawutils filtfmts formats integral

TESTPROGS-$(CONFIG_DRAWVG_FILTER) += drawvg

TOOLS-$(CONFIG_LIBZMQ) += zmqsend

