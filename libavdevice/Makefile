NAME    = avdevice
DESC    = FFmpeg device handling library

HEADERS = avdevice.h                                                    \
          version.h                                                     \
          version_major.h                                               \

OBJS    = alldevices.o                                                  \
          avdevice.o                                                    \
          utils.o                                                       \
          version.o                                                     \

OBJS-$(HAVE_LIBC_MSVCRT)                 += file_open.o

# input/output devices
OBJS-$(CONFIG_ANDROID_CAMERA_INDEV)      += android_camera.o
OBJS-$(CONFIG_AUDIOTOOLBOX_OUTDEV)       += audiotoolbox.o
OBJS-$(CONFIG_AVFOUNDATION_INDEV)        += avfoundation.o
OBJS-$(CONFIG_LAVFI_INDEV)               += lavfi.o

# Windows resource file
