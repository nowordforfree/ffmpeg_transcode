/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * simple audio converter
 *
 * @example transcode_aac.c
 * Convert an input audio file to AAC in an MP4 container using FFmpeg.
 * @author Andreas Unterweger (dustsigns@gmail.com)
 */

#include "libavformat/avformat.h"
#include "libavformat/avio.h"
#include "libavcodec/avcodec.h"
#include "libavutil/audio_fifo.h"
#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/frame.h"
#include "libavutil/opt.h"
#include "libswresample/swresample.h"

/** The output bit rate in kbit/s */
#define OUTPUT_BIT_RATE 192000
/* global error code container */
int ret;

/** Open an input file and the required decoder. */
static int open_input_file(const char *filename,
                           AVFormatContext **input_format_context,
                           AVCodecContext **input_codec_context)
{
    AVCodecContext *avctx;
    AVCodec *input_codec;
    AVStream *audio_stream;

    /** Open the input file to read from it. */
    ret = avformat_open_input(input_format_context, filename, NULL, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not open input file '%s'\n", filename);
        *input_format_context = NULL;
        return ret;
    }

    /** Get information on the input file (number of streams etc.). */
    ret = avformat_find_stream_info(*input_format_context, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not open find stream info\n");
        avformat_close_input(input_format_context);
        return ret;
    }
    av_dump_format(*input_format_context, 0, filename, 0);

    if ((*input_format_context)->nb_streams == 0) {
        av_log(NULL, AV_LOG_ERROR, "No streams in input file\n");
        avformat_close_input(input_format_context);
        return AVERROR_EXIT;
    } else if ((*input_format_context)->nb_streams == 1) {
        audio_stream = (*input_format_context)->streams[0];
    } else {
        int stream_index = av_find_best_stream(*input_format_context,
                                                AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
        if (stream_index < 0) {
            av_log(NULL, AV_LOG_ERROR, "Failed to find audio stream\n");
            avformat_close_input(input_format_context);
            return AVERROR_EXIT;
        }
        audio_stream = (*input_format_context)->streams[stream_index];
    }

    /** Find a decoder for the audio stream. */
    input_codec = avcodec_find_decoder(audio_stream->codecpar->codec_id);
    if (!input_codec) {
        av_log(NULL, AV_LOG_ERROR, "Could not find input codec\n");
        avformat_close_input(input_format_context);
        return AVERROR_EXIT;
    }

    /** allocate a new decoding context */
    avctx = avcodec_alloc_context3(input_codec);
    if (!avctx) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate a decoding context\n");
        avformat_close_input(input_format_context);
        return AVERROR(ENOMEM);
    }

    /** initialize the stream parameters with demuxer information */
    ret = avcodec_parameters_to_context(avctx, audio_stream->codecpar);
    if (ret < 0) {
        avformat_close_input(input_format_context);
        avcodec_free_context(&avctx);
        return ret;
    }

    /** Open the decoder for the audio stream to use it later. */
    ret = avcodec_open2(avctx, input_codec, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not open input codec\n");
        avcodec_free_context(&avctx);
        avformat_close_input(input_format_context);
        return ret;
    }

    /** Save the decoder context for easier access later. */
    *input_codec_context = avctx;

    return 0;
}

/* just pick the highest supported samplerate */
static int select_sample_rate(AVCodec *codec)
{
    const int *p;
    int best_samplerate = 0;

    if (!codec->supported_samplerates)
        return 44100;

    p = codec->supported_samplerates;
    while (*p) {
        best_samplerate = FFMAX(*p, best_samplerate);
        p++;
    }
    return best_samplerate;
}

/**
 * Open an output file and the required encoder.
 * Also set some basic encoder parameters.
 * Some of these parameters are based on the input file's parameters.
 */
static int open_output_file(const char *filename,
                            AVCodecContext *input_codec_context,
                            AVFormatContext **output_format_context,
                            AVCodecContext **output_codec_context)
{
    AVCodecContext *avctx          = NULL;
    AVIOContext *output_io_context = NULL;
    AVStream *stream               = NULL;
    AVCodec *output_codec          = NULL;

    /** Open the output file to write to it. */
    ret = avio_open(&output_io_context, filename, AVIO_FLAG_WRITE);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not open output file '%s'\n", filename);
        return ret;
    }

    /** Create a new format context for the output container format. */
    if (!(*output_format_context = avformat_alloc_context())) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate output format context\n");
        return AVERROR(ENOMEM);
    }

    /** Associate the output file (pointer) with the container format context. */
    (*output_format_context)->pb = output_io_context;

    /** Guess the desired container format based on the file extension. */
    if (!((*output_format_context)->oformat = av_guess_format(NULL, filename,
                                                              NULL))) {
        av_log(NULL, AV_LOG_ERROR, "Could not find output file format\n");
        goto cleanup;
    }

    /** Find the encoder to be used by its name. */
    if (!(output_codec = avcodec_find_encoder((*output_format_context)->oformat->audio_codec))) {
        av_log(NULL, AV_LOG_ERROR, "Could not find an audio encoder.\n");
        goto cleanup;
    }

    /** Create a new audio stream in the output file container. */
    if (!(stream = avformat_new_stream(*output_format_context, NULL))) {
        av_log(NULL, AV_LOG_ERROR, "Could not create new stream\n");
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }

    avctx = avcodec_alloc_context3(output_codec);
    if (!avctx) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate an encoding context\n");
        ret = AVERROR(ENOMEM);
        goto cleanup;
    }

    /**
     * Set the basic encoder parameters.
     * The input file's sample rate is used to avoid a sample rate conversion.
     */
    avctx->channels       = input_codec_context->channels;
    avctx->channel_layout = av_get_default_channel_layout(input_codec_context->channels);
    avctx->sample_rate    = select_sample_rate(output_codec);
    avctx->sample_fmt     = output_codec->sample_fmts[0];
    avctx->bit_rate       = OUTPUT_BIT_RATE;

    /** Allow the use of the experimental AAC encoder */
    avctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    /** Set the sample rate for the container. */
    stream->time_base.den = input_codec_context->sample_rate;
    stream->time_base.num = 1;

    /**
     * Some container formats (like MP4) require global headers to be present
     * Mark the encoder so that it behaves accordingly.
     */
    if ((*output_format_context)->oformat->flags & AVFMT_GLOBALHEADER)
        avctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    /** Open the encoder for the audio stream to use it later. */
    ret = avcodec_open2(avctx, output_codec, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not open output codec\n");
        goto cleanup;
    }

    ret = avcodec_parameters_from_context(stream->codecpar, avctx);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not initialize stream parameters\n");
        goto cleanup;
    }

    av_dump_format(*output_format_context, 0, filename, 1);

    /** Save the encoder context for easier access later. */
    *output_codec_context = avctx;

    return 0;

cleanup:
    avcodec_free_context(&avctx);
    avio_closep(&(*output_format_context)->pb);
    avformat_free_context(*output_format_context);
    *output_format_context = NULL;
    return ret < 0 ? ret : AVERROR_EXIT;
}

/** Initialize one data packet for reading or writing. */
static void init_packet(AVPacket *packet)
{
    av_init_packet(packet);
    /** Set the packet data and size so that it is recognized as being empty. */
    packet->data = NULL;
    packet->size = 0;
}

/** Initialize one audio frame for reading from the input file */
static int init_input_frame(AVFrame **frame)
{
    if (!(*frame = av_frame_alloc())) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate input frame\n");
        return AVERROR(ENOMEM);
    }
    return 0;
}

/**
 * Initialize the audio resampler based on the input and output codec settings.
 * If the input and output sample formats differ, a conversion is required
 * libswresample takes care of this, but requires initialization.
 */
static int init_resampler(AVCodecContext *input_codec_context,
                          AVCodecContext *output_codec_context,
                          SwrContext **resample_context)
{
        /**
         * Create a resampler context for the conversion.
         * Set the conversion parameters.
         * Default channel layouts based on the number of channels
         * are assumed for simplicity (they are sometimes not detected
         * properly by the demuxer and/or decoder).
         */
        *resample_context = swr_alloc_set_opts(NULL,
                                              av_get_default_channel_layout(output_codec_context->channels),
                                              output_codec_context->sample_fmt,
                                              output_codec_context->sample_rate,
                                              av_get_default_channel_layout(input_codec_context->channels),
                                              input_codec_context->sample_fmt,
                                              input_codec_context->sample_rate,
                                              0, NULL);
        if (!*resample_context) {
            av_log(NULL, AV_LOG_ERROR, "Could not allocate resample context\n");
            return AVERROR(ENOMEM);
        }
        /** Open the resampler with the specified parameters. */
        ret = swr_init(*resample_context);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Could not open resample context\n");
            swr_free(resample_context);
            return ret;
        }
    return 0;
}

/** Initialize a FIFO buffer for the audio samples to be encoded. */
static int init_fifo(AVAudioFifo **fifo, AVCodecContext *output_codec_context)
{
    /** Create the FIFO buffer based on the specified output sample format. */
    if (!(*fifo = av_audio_fifo_alloc(output_codec_context->sample_fmt,
                                      output_codec_context->channels, 1))) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate FIFO\n");
        return AVERROR(ENOMEM);
    }
    return 0;
}

/** Write the header of the output file container. */
static int write_output_file_header(AVFormatContext *output_format_context)
{
    ret = avformat_write_header(output_format_context, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not write output file header\n");
        return ret;
    }
    return 0;
}

/** Decode one audio frame from the input file. */
static int decode_audio_frame(AVFrame *frame,
                              AVFormatContext *input_format_context,
                              AVCodecContext *input_codec_context,
                              int *data_present, int *finished)
{
    /** Packet used for temporary storage. */
    AVPacket input_packet;
    init_packet(&input_packet);

    /** Read one audio frame from the input file into a temporary packet. */
    ret = av_read_frame(input_format_context, &input_packet);
    if (ret < 0) {
        /** If we are at the end of the file, flush the decoder below. */
        if (ret == AVERROR_EOF)
            *finished = 1;
        else {
            av_log(NULL, AV_LOG_ERROR, "Could not read frame\n");
            return ret;
        }
    }
    /**
     * Decode the audio frame stored in the temporary packet.
     * The input audio stream decoder is used to do this.
     * If we are at the end of the file, pass an empty packet to the decoder
     * to flush it.
     */
    ret = avcodec_decode_audio4(input_codec_context, frame,
                                data_present, &input_packet);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not decode frame\n");
        av_packet_unref(&input_packet);
        return ret;
    }

    /**
     * If the decoder has not been flushed completely, we are not finished,
     * so that this function has to be called again.
     */
    if (*finished && *data_present)
        *finished = 0;
    av_packet_unref(&input_packet);
    return 0;
}

/**
 * Initialize a temporary storage for the specified number of audio samples.
 * The conversion requires temporary storage due to the different format.
 * The number of audio samples to be allocated is specified in frame_size.
 */
static int init_converted_samples(uint8_t ***converted_input_samples,
                                  AVCodecContext *output_codec_context,
                                  int frame_size)
{
    /**
     * Allocate as many pointers as there are audio channels.
     * Each pointer will later point to the audio samples of the corresponding
     * channels (although it may be NULL for interleaved formats).
     */
    if (!(*converted_input_samples = calloc(output_codec_context->channels,
                                            sizeof(**converted_input_samples)))) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate converted input sample pointers\n");
        return AVERROR(ENOMEM);
    }

    /**
     * Allocate memory for the samples of all channels in one consecutive
     * block for convenience.
     */
    ret = av_samples_alloc(*converted_input_samples, NULL,
                            output_codec_context->channels,
                            frame_size,
                            output_codec_context->sample_fmt, 0);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate converted input samples\n");
        av_freep(&(*converted_input_samples)[0]);
        free(*converted_input_samples);
        return ret;
    }
    return 0;
}

/**
 * Convert the input audio samples into the output sample format.
 * The conversion happens on a per-frame basis, the size of which is specified
 * by frame_size.
 */
static int convert_samples(const uint8_t **input_data,
                           uint8_t **converted_data, const int frame_size,
                           SwrContext *resample_context)
{
    /** Convert the samples using the resampler. */
    ret = swr_convert(resample_context,
                      converted_data, frame_size,
                      input_data    , frame_size);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not convert input samples\n");
        return ret;
    }

    return 0;
}

/** Add converted input audio samples to the FIFO buffer for later processing. */
static int add_samples_to_fifo(AVAudioFifo *fifo,
                               uint8_t **converted_input_samples,
                               const int frame_size)
{
    /**
     * Make the FIFO as large as it needs to be to hold both,
     * the old and the new samples.
     */
    ret = av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) + frame_size);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not reallocate FIFO\n");
        return ret;
    }

    /** Store the new samples in the FIFO buffer. */
    if (av_audio_fifo_write(fifo, (void **)converted_input_samples,
                            frame_size) < frame_size) {
        av_log(NULL, AV_LOG_ERROR, "Could not write data to FIFO\n");
        return AVERROR_EXIT;
    }
    return 0;
}

/**
 * Read one audio frame from the input file, decodes, converts and stores
 * it in the FIFO buffer.
 */
static int read_decode_convert_and_store(AVAudioFifo *fifo,
                                         AVFormatContext *input_format_context,
                                         AVCodecContext *input_codec_context,
                                         AVCodecContext *output_codec_context,
                                         SwrContext *resampler_context,
                                         int *finished)
{
    /** Temporary storage of the input samples of the frame read from the file. */
    AVFrame *input_frame = NULL;
    /** Temporary storage for the converted input samples. */
    uint8_t **converted_input_samples = NULL;
    int data_present;

    /** Initialize temporary storage for one input frame. */
    if (init_input_frame(&input_frame))
        goto cleanup;
    /** Decode one frame worth of audio samples. */
    if (decode_audio_frame(input_frame, input_format_context,
                           input_codec_context, &data_present, finished))
        goto cleanup;
    /**
     * If we are at the end of the file and there are no more samples
     * in the decoder which are delayed, we are actually finished.
     * This must not be treated as an error.
     */
    if (*finished && !data_present) {
        ret = 0;
        goto cleanup;
    }
    /** If there is decoded data, convert and store it */
    if (data_present) {
        /** Initialize the temporary storage for the converted input samples. */
        if (init_converted_samples(&converted_input_samples, output_codec_context,
                                   input_frame->nb_samples))
            goto cleanup;

        /**
         * Convert the input samples to the desired output sample format.
         * This requires a temporary storage provided by converted_input_samples.
         */
        if (convert_samples((const uint8_t**)input_frame->extended_data, converted_input_samples,
                            input_frame->nb_samples, resampler_context))
            goto cleanup;

        /** Add the converted input samples to the FIFO buffer for later processing. */
        if (add_samples_to_fifo(fifo, converted_input_samples,
                                input_frame->nb_samples))
            goto cleanup;
        ret = 0;
    }
    ret = 0;

cleanup:
    if (converted_input_samples) {
        av_freep(&converted_input_samples[0]);
        free(converted_input_samples);
    }
    av_frame_free(&input_frame);

    return ret;
}

/**
 * Initialize one input frame for writing to the output file.
 * The frame will be exactly frame_size samples large.
 */
static int init_output_frame(AVFrame **frame,
                             AVCodecContext *output_codec_context,
                             int frame_size)
{
    /** Create a new frame to store the audio samples. */
    if (!(*frame = av_frame_alloc())) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate output frame\n");
        return AVERROR_EXIT;
    }

    /**
     * Set the frame's parameters, especially its size and format.
     * av_frame_get_buffer needs this to allocate memory for the
     * audio samples of the frame.
     * Default channel layouts based on the number of channels
     * are assumed for simplicity.
     */
    (*frame)->nb_samples     = frame_size;
    (*frame)->channel_layout = output_codec_context->channel_layout;
    (*frame)->format         = output_codec_context->sample_fmt;
    (*frame)->sample_rate    = output_codec_context->sample_rate;

    /**
     * Allocate the samples of the created frame. This call will make
     * sure that the audio frame can hold as many samples as specified.
     */
    ret = av_frame_get_buffer(*frame, 0);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could allocate output frame samples\n");
        av_frame_free(frame);
        return ret;
    }

    return 0;
}

/** Global timestamp for the audio frames */
static int64_t pts = 0;

/** Encode one frame worth of audio to the output file. */
static int encode_audio_frame(AVFrame *frame,
                              AVFormatContext *output_format_context,
                              AVCodecContext *output_codec_context,
                              int *data_present)
{
    /** Packet used for temporary storage. */
    AVPacket output_packet;
    init_packet(&output_packet);

    /** Set a timestamp based on the sample rate for the container. */
    if (frame) {
        frame->pts = pts;
        pts += frame->nb_samples;
    }

    /**
     * Encode the audio frame and store it in the temporary packet.
     * The output audio stream encoder is used to do this.
     */
     ret = avcodec_encode_audio2(output_codec_context, &output_packet,
                                 frame, data_present);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not encode frame\n");
        av_packet_unref(&output_packet);
        return ret;
    }

    /** Write one audio frame from the temporary packet to the output file. */
    if (*data_present) {
        ret = av_write_frame(output_format_context, &output_packet);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Could not write frame\n");
            av_packet_unref(&output_packet);
            return ret;
        }

        av_packet_unref(&output_packet);
    }

    return 0;
}

/**
 * Load one audio frame from the FIFO buffer, encode and write it to the
 * output file.
 */
static int load_encode_and_write(AVAudioFifo *fifo,
                                 AVFormatContext *output_format_context,
                                 AVCodecContext *output_codec_context)
{
    /** Temporary storage of the output samples of the frame written to the file. */
    AVFrame *output_frame;
    /**
     * Use the maximum number of possible samples per frame.
     * If there is less than the maximum possible frame size in the FIFO
     * buffer use this number. Otherwise, use the maximum possible frame size
     */
    const int frame_size = FFMIN(av_audio_fifo_size(fifo),
                                 output_codec_context->frame_size);

    int data_written;

    /** Initialize temporary storage for one output frame. */
    if (init_output_frame(&output_frame, output_codec_context, frame_size))
        return AVERROR_EXIT;

    /**
     * Read as many samples from the FIFO buffer as required to fill the frame.
     * The samples are stored in the frame temporarily.
     */
    if (av_audio_fifo_read(fifo, (void **)output_frame->data, frame_size) < frame_size) {
        av_log(NULL, AV_LOG_ERROR, "Could not read data from FIFO\n");
        av_frame_free(&output_frame);
        return AVERROR_EXIT;
    }

    /** Encode one frame worth of audio samples. */
    if (encode_audio_frame(output_frame, output_format_context,
                           output_codec_context, &data_written)) {
        av_frame_free(&output_frame);
        return AVERROR_EXIT;
    }
    av_frame_free(&output_frame);
    return 0;
}

/** Write the trailer of the output file container. */
static int write_output_file_trailer(AVFormatContext *output_format_context)
{
    ret = av_write_trailer(output_format_context);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not write output file trailer\n");
        return ret;
    }
    return 0;
}

/** Convert an audio file to an AAC file in an MP4 container. */
int main(int argc, char **argv)
{
    AVFormatContext *input_format_context = NULL, *output_format_context = NULL;
    AVCodecContext *input_codec_context = NULL, *output_codec_context = NULL;
    SwrContext *resample_context = NULL;
    AVAudioFifo *fifo = NULL;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s <input file> <output file>\n", argv[0]);
        exit(1);
    }

    /** Register all codecs and formats so that they can be used. */
    av_register_all();
    avformat_network_init();
    /** Open the input file for reading. */
    if (open_input_file(argv[1], &input_format_context,
                        &input_codec_context))
        goto cleanup;
    /** Open the output file for writing. */
    if (open_output_file(argv[2], input_codec_context,
                         &output_format_context, &output_codec_context))
        goto cleanup;
    /** Initialize the resampler to be able to convert audio sample formats. */
    if (init_resampler(input_codec_context, output_codec_context,
                       &resample_context))
        goto cleanup;
    /** Initialize the FIFO buffer to store audio samples to be encoded. */
    if (init_fifo(&fifo, output_codec_context))
        goto cleanup;
    /** Write the header of the output file container. */
    if (write_output_file_header(output_format_context))
        goto cleanup;

    /**
     * Loop as long as we have input samples to read or output samples
     * to write; abort as soon as we have neither.
     */
    while (1) {
        /** Use the encoder's desired frame size for processing. */
        const int output_frame_size = output_codec_context->frame_size;
        int finished                = 0;

        /**
         * Make sure that there is one frame worth of samples in the FIFO
         * buffer so that the encoder can do its work.
         * Since the decoder's and the encoder's frame size may differ, we
         * need to FIFO buffer to store as many frames worth of input samples
         * that they make up at least one frame worth of output samples.
         */
        while (av_audio_fifo_size(fifo) < output_frame_size) {
            /**
             * Decode one frame worth of audio samples, convert it to the
             * output sample format and put it into the FIFO buffer.
             */
            if (read_decode_convert_and_store(fifo, input_format_context,
                                              input_codec_context,
                                              output_codec_context,
                                              resample_context, &finished))
                goto cleanup;

            /**
             * If we are at the end of the input file, we continue
             * encoding the remaining audio samples to the output file.
             */
            if (finished)
                break;
        }

        /**
         * If we have enough samples for the encoder, we encode them.
         * At the end of the file, we pass the remaining samples to
         * the encoder.
         */
        while (av_audio_fifo_size(fifo) >= output_frame_size ||
               (finished && av_audio_fifo_size(fifo) > 0))
            /**
             * Take one frame worth of audio samples from the FIFO buffer,
             * encode it and write it to the output file.
             */
            if (load_encode_and_write(fifo, output_format_context,
                                      output_codec_context))
                goto cleanup;

        /**
         * If we are at the end of the input file and have encoded
         * all remaining samples, we can exit this loop and finish.
         */
        if (finished) {
            int data_written;
            /** Flush the encoder as it may have delayed frames. */
            do {
                if (encode_audio_frame(NULL, output_format_context,
                                       output_codec_context, &data_written))
                    goto cleanup;
            } while (data_written);
            break;
        }
    }

    /** Write the trailer of the output file container. */
    if (write_output_file_trailer(output_format_context))
        goto cleanup;
    ret = 0;

cleanup:
    if (fifo)
        av_audio_fifo_free(fifo);
    swr_free(&resample_context);
    if (output_codec_context)
        avcodec_free_context(&output_codec_context);
    if (output_format_context) {
        avio_closep(&output_format_context->pb);
        avformat_free_context(output_format_context);
    }
    if (input_codec_context)
        avcodec_free_context(&input_codec_context);
    if (input_format_context)
        avformat_close_input(&input_format_context);

    if (ret)
        av_log(NULL, AV_LOG_ERROR, "%s\n", av_err2str(ret));

    return 0;
}
