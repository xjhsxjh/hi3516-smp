ifeq ($(PARAM_FILE), )
     PARAM_FILE:=../Makefile.param
     include $(PARAM_FILE)
endif

SRC  := $(wildcard *.c)
OBJ  := $(SRC:%.c=%.o)

INC_FLAGS := -I$(REL_INC)
CFLAGS += -Wall -g -lpthread $(INC_FLAGS)
CFLAGS += $(LIBS_CFLAGS)
CFLAGS += -ldl

LIBS += $(REL_LIB)/libisp.a
LIBS += $(REL_LIB)/libmpi.a
LIBS += $(REL_LIB)/lib_hidehaze.a
LIBS += $(REL_LIB)/lib_hidrc.a
LIBS += $(REL_LIB)/lib_hildci.a
LIBS += $(REL_LIB)/libhifisheyecalibrate.a
LIBS += $(REL_LIB)/libsecurec.a
LIBS += $(AUDIO_LIBA)
CFLAGS += $(LIBS_CFLAGS)

CFLAGS += -lm

include $(PWD)/Make.$(OSTYPE)
