ifndef inc_base
inc_base := 1

include $(imagineSrcDir)/base/Base.mk
include $(imagineSrcDir)/input/build.mk
include $(imagineSrcDir)/util/fdUtils.mk

LDLIBS += -lpthread -ldl

SRC += base/linux/Application.cc \
 base/linux/ApplicationContext.cc \
 base/linux/DRMFrameTimer.cc \
 base/linux/FBDevFrameTimer.cc \
 base/common/SimpleFrameTimer.cc \
 base/common/timer/TimerFD.cc \
 base/common/PosixPipe.cc \
 base/common/eventloop/FDCustomEvent.cc

include $(IMAGINE_PATH)/make/package/libdrm.mk

linuxWinSystem ?= x11

ifeq ($(linuxWinSystem), x11)
 include $(imagineSrcDir)/base/x11/build.mk
endif

SRC += base/common/eventloop/GlibEventLoop.cc
include $(IMAGINE_PATH)/make/package/glib.mk

ifneq ($(SUBENV), pandora)
 configDefs += CONFIG_BASE_DBUS
 SRC += base/linux/dbus.cc
 include $(IMAGINE_PATH)/make/package/gio.mk
endif

endif
