TOP = ../../../..
include $(TOP)/configs/current

LIBNAME = xorgtracker

LIBRARY_INCLUDES = \
	-DHAVE_CONFIG_H \
	$(shell pkg-config --cflags-only-I pixman-1 xorg-server libdrm xproto) \
	-I$(TOP)/src/gallium/include \
	-I$(TOP)/src/gallium/auxiliary \
	-I$(TOP)/include \
	-I$(TOP)/src/mesa \
	-I$(TOP)/src/mesa/drivers/dri/common \
	-I$(TOP)/src/mesa/main

C_SOURCES = $(wildcard ./*.c)

include ../../Makefile.template
