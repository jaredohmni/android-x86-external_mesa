# src/gallium/state_trackers/es/Makefile

# Build the ES 1/2 state tracker libraries
# This consists of core Mesa ES, plus GL/gallium state tracker.

TOP = ../../../..
include $(TOP)/configs/current

GLES_1_VERSION_MAJOR = 1
GLES_1_VERSION_MINOR = 1
GLES_1_VERSION_PATCH = 0

GLES_2_VERSION_MAJOR = 2
GLES_2_VERSION_MINOR = 0
GLES_2_VERSION_PATCH = 0


# Maybe move these into configs/default:
GLES_1_LIB = GLESv1_CM
GLES_1_LIB_NAME = lib$(GLES_1_LIB).so
GLES_2_LIB = GLESv2
GLES_2_LIB_NAME = lib$(GLES_2_LIB).so


INCLUDE_DIRS = \
	-I$(TOP)/src/mesa \
	-I$(TOP)/include

ES1_SOURCES = \
	st_es1.c \

ES1_OBJECTS = $(ES1_SOURCES:.c=.o)

ES2_SOURCES = \
	st_es2.c \

ES2_OBJECTS = $(ES2_SOURCES:.c=.o)


# we only need the gallium libs that the state trackers directly use:
GALLIUM_LIBS = \
	$(TOP)/src/gallium/auxiliary/cso_cache/libcso_cache.a \
	$(TOP)/src/gallium/auxiliary/rtasm/librtasm.a \
	$(TOP)/src/gallium/auxiliary/tgsi/libtgsi.a \
	$(TOP)/src/gallium/auxiliary/util/libutil.a

ES1_LIBS = \
	$(TOP)/src/mesa/es/libes1gallium.a \
	$(TOP)/src/mesa/es/libes1api.a

ES2_LIBS = \
	$(TOP)/src/mesa/es/libes2gallium.a \
	$(TOP)/src/mesa/es/libes2api.a

SYS_LIBS = -lm -pthread


.c.o:
	$(CC) -c $(INCLUDE_DIRS) $(CFLAGS) $< -o $@


# Default: make both GL ES 1.1 and GL ES 2.0 libraries
default: $(TOP)/$(LIB_DIR)/$(GLES_1_LIB_NAME) $(TOP)/$(LIB_DIR)/$(GLES_2_LIB_NAME)

# Make the shared libs
$(TOP)/$(LIB_DIR)/$(GLES_1_LIB_NAME): $(ES1_OBJECTS) $(ES1_LIBS)
	$(TOP)/bin/mklib -o $(GLES_1_LIB) \
		-major $(GLES_1_VERSION_MAJOR) \
		-minor $(GLES_1_VERSION_MINOR) \
		-patch $(GLES_1_VERSION_PATCH) \
		-install $(TOP)/$(LIB_DIR) \
		$(ES1_OBJECTS) \
		-Wl,--whole-archive $(ES1_LIBS) -Wl,--no-whole-archive \
		-Wl,--start-group $(GALLIUM_LIBS) -Wl,--end-group  \
		$(SYS_LIBS)

$(TOP)/$(LIB_DIR)/$(GLES_2_LIB_NAME): $(ES2_OBJECTS) $(ES1_LIBS)
	$(TOP)/bin/mklib -o $(GLES_2_LIB) \
		-major $(GLES_2_VERSION_MAJOR) \
		-minor $(GLES_2_VERSION_MINOR) \
		-patch $(GLES_2_VERSION_PATCH) \
		-install $(TOP)/$(LIB_DIR) \
		$(ES2_OBJECTS) \
		-Wl,--whole-archive $(ES2_LIBS) -Wl,--no-whole-archive \
		-Wl,--start-group $(GALLIUM_LIBS) -Wl,--end-group  \
		$(SYS_LIBS)

install: default
	$(INSTALL) -d $(INSTALL_DIR)/include/GLES
	$(INSTALL) -m 644 $(TOP)/include/GLES/*.h $(INSTALL_DIR)/include/GLES
	$(INSTALL) -d $(INSTALL_DIR)/include/GLES2
	$(INSTALL) -m 644 $(TOP)/include/GLES2/*.h $(INSTALL_DIR)/include/GLES2
	$(INSTALL) -d $(INSTALL_DIR)/$(LIB_DIR)
	$(INSTALL) $(TOP)/$(LIB_DIR)/libGLESv1* $(INSTALL_DIR)/$(LIB_DIR)
	$(INSTALL) $(TOP)/$(LIB_DIR)/libGLESv2* $(INSTALL_DIR)/$(LIB_DIR)

clean:
	-rm -f *.o *~
	-rm -f $(TOP)/$(LIB_DIR)/$(GLES_1_LIB_NAME)* $(TOP)/$(LIB_DIR)/$(GLES_2_LIB_NAME)*
	-rm -f *.pyc
