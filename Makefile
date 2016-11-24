CXX = gcc
PROGRAM = transcode
INCLUDEDIRS = -Iffmpeg/ -Ix264/

LIBDIRS = \
        -L./ffmpeg/libavcodec \
        -L./ffmpeg/libavformat \
        -L./ffmpeg/libavfilter \
        -L./ffmpeg/libpostproc \
        -L./ffmpeg/libswresample \
        -L./ffmpeg/libswscale \
        -L./ffmpeg/libavdevice \
        -L./ffmpeg/libavutil

LIBS = -lavfilter -lavformat -lavcodec -lswresample -lswscale -lpostproc \
			 -lavutil -lvpx -lx264 -lm -lz -lssl -lcrypto -pthread -ldl

CXXSOURCES = transcode.c
CXXOBJECTS = $(CXXSOURCES:.c=.o)
HEADERFLAGS = $(INCLUDEDIRS)
LDFLAGS = $(LIBDIRS) $(LIBS)

all: $(PROGRAM)
$(PROGRAM): $(CXXOBJECTS)
		$(CXX) -o $@ $(CXXOBJECTS) $(LDFLAGS)
clean:
		$(RM) -f $(CXXOBJECTS) $(PROGRAM)
