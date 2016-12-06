CXX = gcc
PROGRAM = transcode
INCLUDEDIRS = -I./ffmpeg -I./x264

LIBDIRS = \
        -L./ffmpeg/libavcodec \
        -L./ffmpeg/libavformat \
        -L./ffmpeg/libavfilter \
        -L./ffmpeg/libpostproc \
        -L./ffmpeg/libswresample \
        -L./ffmpeg/libswscale \
        -L./ffmpeg/libavdevice \
        -L./ffmpeg/libavutil \
        -L./x264

LIBS = -lavfilter -lavformat -lavcodec -lswresample -lswscale -lpostproc \
			 -lavutil -lx264 -lm -lz -lssl -lcrypto -pthread -ldl

MODULES = transcoding
CXXSOURCES = $(MODULES).c
CXXOBJECTS = $(MODULES).o
HEADERFLAGS = $(INCLUDEDIRS)
LDFLAGS = $(LIBDIRS) $(LIBS)

.PHONY: all

all: executable

build:
		$(CXX) $(HEADERFLAGS) -c $(CXXSOURCES)

executable: build
		$(CXX) -o $(PROGRAM) $(CXXOBJECTS) $(LDFLAGS)

clean:
		$(RM) -f $(CXXOBJECTS) $(PROGRAM)
