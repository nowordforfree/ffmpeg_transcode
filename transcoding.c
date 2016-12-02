#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavfilter/avfiltergraph.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
#include "libavutil/opt.h"
#include "libavutil/audio_fifo.h"
#include "libavutil/pixdesc.h"
#include "libswresample/swresample.h"
#include "x264/x264.h"

typedef struct Input {
    AVFormatContext *ifmt_ctx;
    AVCodecContext *dec_ctx_a;
    AVCodecContext *dec_ctx_v;
    AVCodec *codec_video;
    AVCodec *codec_audio;
    AVStream *stream_video;
    AVStream *stream_audio;
} Input;

typedef struct Output {
    AVFormatContext *ofmt_ctx;
    AVCodecContext *enc_ctx_a;
    AVCodecContext *enc_ctx_v;
    AVCodec *codec_video;
    AVCodec *codec_audio;
    AVStream *stream_video;
    AVStream *stream_audio;
    int stream_video_in_input;
    int stream_audio_in_input;
} Output;

typedef struct FilteringContext {
    AVFilterContext *buffersink_ctx;
    AVFilterContext *buffersrc_ctx;
    AVFilterGraph *filter_graph;
} FilteringContext;

// static FilteringContext *filter_ctx;
static Input *input;
static Output *output;
static int64_t pts = 0;
static int64_t vid_pts = 0;
static int64_t vid_dts = 0;

static int open_input_file(const char *filename)
{
    int ret;
    unsigned int i;

    ret = avformat_open_input(&input->ifmt_ctx, filename, NULL, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
        return ret;
    }

    ret = avformat_find_stream_info(input->ifmt_ctx, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
        return ret;
    }

    for (i = 0; i < input->ifmt_ctx->nb_streams; i++) {
        AVStream *stream;
        AVCodec *codec;
        AVCodecContext *codec_ctx;
        stream = input->ifmt_ctx->streams[i];
        /* Reencode video & audio */
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ||
            stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            if ((stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                 input->stream_video) ||
                (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
                 input->stream_audio))
                continue;

            codec = avcodec_find_decoder(stream->codecpar->codec_id);
            if (!codec) {
                av_log(NULL, AV_LOG_ERROR, "Could fild decoder for stream #%d\n", i);
                return AVERROR_INVALIDDATA;
            }
            codec_ctx = avcodec_alloc_context3(codec);
            if (!codec_ctx) {
                av_log(NULL, AV_LOG_ERROR, "Failed to allocate decoding context for stream #%d\n", i);
                return AVERROR_INVALIDDATA;
            }
            if (codec->capabilities & AV_CODEC_CAP_TRUNCATED)
                codec_ctx->flags |= AV_CODEC_FLAG_TRUNCATED;


            ret = avcodec_parameters_to_context(codec_ctx, stream->codecpar);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Could not initialize stream parameters\n");
                return ret;
            }
            /* Open decoder */
            ret = avcodec_open2(codec_ctx, codec, NULL);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Failed to open decoder for stream #%u\n", i);
                return ret;
            }
            if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                input->stream_video = stream;
                input->codec_video = codec;
                input->dec_ctx_v = codec_ctx;
            } else {
                input->stream_audio = stream;
                input->codec_audio = codec;
                input->dec_ctx_a = codec_ctx;
            }
        }
    }

    av_dump_format(input->ifmt_ctx, 0, filename, 0);
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

static int write_metadata(AVFormatContext *from, AVFormatContext *to)
{
    AVDictionaryEntry *tag = NULL;
    while ((tag = av_dict_get(from->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
        av_dict_set(&to->metadata, tag->key, tag->value, 0);
}

static int open_output_file(const char *filename)
{
    AVFormatContext *ofmt_ctx = NULL;
    AVStream *out_stream;
    AVStream *in_stream;
    AVCodecContext *enc_ctx;
    AVCodecParameters *dec_par;
    AVCodec *encoder;
    int ret;
    unsigned int i;

    avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, filename);
    if (!ofmt_ctx) {
        av_log(NULL, AV_LOG_ERROR, "Could not create output context\n");
        return AVERROR_UNKNOWN;
    }

    if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ofmt_ctx->pb, filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Could not open output file '%s'", filename);
            return ret;
        }
    }

    output->ofmt_ctx = ofmt_ctx;

    for (i = 0; i < input->ifmt_ctx->nb_streams; i++) {
        in_stream = input->ifmt_ctx->streams[i];
        dec_par = in_stream->codecpar;

        if ((dec_par->codec_type == AVMEDIA_TYPE_VIDEO &&
             output->stream_video) ||
            (dec_par->codec_type == AVMEDIA_TYPE_AUDIO &&
             output->stream_audio))
            continue;

        out_stream = avformat_new_stream(ofmt_ctx, NULL);
        if (!out_stream) {
          av_log(NULL, AV_LOG_ERROR, "Failed allocating output stream\n");
          return AVERROR_UNKNOWN;
        }

        if (dec_par->codec_type == AVMEDIA_TYPE_VIDEO) {
            encoder = avcodec_find_encoder(ofmt_ctx->oformat->video_codec);
            if (!encoder) {
              av_log(NULL, AV_LOG_FATAL, "Necessary encoder not found\n");
              return AVERROR_INVALIDDATA;
            }

            enc_ctx = avcodec_alloc_context3(encoder);
            if (!enc_ctx) {
              av_log(NULL, AV_LOG_ERROR, "Failed to allocate encoding context for stream #%d\n", i);
              return AVERROR_INVALIDDATA;
            }

            output->enc_ctx_v           = enc_ctx;
            output->codec_video         = encoder;
            output->stream_video        = out_stream;
            output->stream_video_in_input = i;

            enc_ctx->height             = dec_par->height;
            enc_ctx->width              = dec_par->width;
            enc_ctx->sample_aspect_ratio = dec_par->sample_aspect_ratio;
            /* take first format from list of supported formats */
            if (encoder->pix_fmts)
                enc_ctx->pix_fmt        = encoder->pix_fmts[0];
            else
                enc_ctx->pix_fmt        = input->dec_ctx_v->pix_fmt;
            /* video time_base can be set to whatever is handy and supported by encoder */
            if (input->dec_ctx_v->time_base.num > 0)
                enc_ctx->time_base      = input->dec_ctx_v->time_base;
            else
                enc_ctx->time_base      = (AVRational){1, 25};
            enc_ctx->gop_size           = 25;
            enc_ctx->max_b_frames       = 1;
            if (ofmt_ctx->oformat->video_codec == AV_CODEC_ID_H264)
                av_opt_set(enc_ctx->priv_data, "preset", "medium", 0);
        } else if (dec_par->codec_type == AVMEDIA_TYPE_AUDIO) {
            encoder = avcodec_find_encoder(ofmt_ctx->oformat->audio_codec);
            if (!encoder) {
              av_log(NULL, AV_LOG_FATAL, "Necessary encoder not found\n");
              return AVERROR_INVALIDDATA;
            }

            enc_ctx = avcodec_alloc_context3(encoder);
            if (!enc_ctx) {
              av_log(NULL, AV_LOG_ERROR, "Failed to allocate encoding context for stream #%d\n", i);
              return AVERROR_INVALIDDATA;
            }

            output->enc_ctx_a         = enc_ctx;
            output->codec_audio       = encoder;
            output->stream_audio      = out_stream;
            output->stream_audio_in_input = i;

            if (input->dec_ctx_a->sample_rate)
                enc_ctx->sample_rate  = input->dec_ctx_a->sample_rate;
            else
                enc_ctx->sample_rate  = select_sample_rate(encoder);
            enc_ctx->channel_layout   = dec_par->channel_layout;
            enc_ctx->channels         = av_get_channel_layout_nb_channels(dec_par->channel_layout);
            /* take first format from list of supported formats */
            enc_ctx->sample_fmt       = encoder->sample_fmts[0];
            enc_ctx->frame_size       = 1024;
            enc_ctx->time_base        = (AVRational){1, enc_ctx->sample_rate};
            enc_ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
        } else {
            continue;
        }

        if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
            enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        ret = avcodec_parameters_from_context(out_stream->codecpar, enc_ctx);
        if (ret < 0) {
          av_log(NULL, AV_LOG_ERROR, "Could not initialize stream parameters\n");
          return ret;
        }

        /* Third parameter can be used to pass settings to encoder */
        ret = avcodec_open2(enc_ctx, encoder, NULL);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot open video encoder for stream #%u\n", i);
            return ret;
        }
    }

    av_dump_format(ofmt_ctx, 0, filename, 1);

    /* init muxer, write output file header */
    ret = avformat_write_header(ofmt_ctx, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error occurred when opening output file\n");
        return ret;
    }

    return 0;
}

// static int init_filter(FilteringContext* fctx, AVCodecContext *dec_ctx,
//                        AVCodecContext *enc_ctx, const char *filter_spec)
// {
//     char args[512];
//     int ret = 0;
//     AVFilter *buffersrc = NULL;
//     AVFilter *buffersink = NULL;
//     AVFilterContext *buffersrc_ctx = NULL;
//     AVFilterContext *buffersink_ctx = NULL;
//     AVFilterInOut *outputs = avfilter_inout_alloc();
//     AVFilterInOut *inputs  = avfilter_inout_alloc();
//     AVFilterGraph *filter_graph = avfilter_graph_alloc();
//
//     if (!outputs || !inputs || !filter_graph) {
//         ret = AVERROR(ENOMEM);
//         goto end;
//     }
//
//     if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
//         buffersrc = avfilter_get_by_name("buffer");
//         buffersink = avfilter_get_by_name("buffersink");
//         if (!buffersrc || !buffersink) {
//             av_log(NULL, AV_LOG_ERROR, "filtering source or sink element not found\n");
//             ret = AVERROR_UNKNOWN;
//             goto end;
//         }
//
//         snprintf(args, sizeof(args),
//                 "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
//                 dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
//                 dec_ctx->time_base.num || 1, dec_ctx->time_base.den,
//                 dec_ctx->sample_aspect_ratio.num || 1,
//                 dec_ctx->sample_aspect_ratio.den);
//
//         ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
//                 args, NULL, filter_graph);
//         if (ret < 0) {
//             av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source\n");
//             goto end;
//         }
//
//         ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
//                 NULL, NULL, filter_graph);
//         if (ret < 0) {
//             av_log(NULL, AV_LOG_ERROR, "Cannot create buffer sink\n");
//             goto end;
//         }
//
//         ret = av_opt_set_bin(buffersink_ctx, "pix_fmts",
//                 (uint8_t*)&enc_ctx->pix_fmt, sizeof(enc_ctx->pix_fmt),
//                 AV_OPT_SEARCH_CHILDREN);
//         if (ret < 0) {
//             av_log(NULL, AV_LOG_ERROR, "Cannot set output pixel format\n");
//             goto end;
//         }
//     } else if (dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
//         buffersrc = avfilter_get_by_name("abuffer");
//         buffersink = avfilter_get_by_name("abuffersink");
//         if (!buffersrc || !buffersink) {
//             av_log(NULL, AV_LOG_ERROR, "filtering source or sink element not found\n");
//             ret = AVERROR_UNKNOWN;
//             goto end;
//         }
//
//         if (!dec_ctx->channel_layout)
//             dec_ctx->channel_layout =
//                 av_get_default_channel_layout(dec_ctx->channels);
//         snprintf(args, sizeof(args),
//                 "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%"PRIx64,
//                 dec_ctx->time_base.num, dec_ctx->time_base.den, dec_ctx->sample_rate,
//                 av_get_sample_fmt_name(dec_ctx->sample_fmt),
//                 dec_ctx->channel_layout);
//         ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
//                 args, NULL, filter_graph);
//         if (ret < 0) {
//             av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer source\n");
//             goto end;
//         }
//
//         ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
//                 NULL, NULL, filter_graph);
//         if (ret < 0) {
//             av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer sink\n");
//             goto end;
//         }
//
//         ret = av_opt_set_bin(buffersink_ctx, "sample_fmts",
//                 (uint8_t*)&enc_ctx->sample_fmt, sizeof(enc_ctx->sample_fmt),
//                 AV_OPT_SEARCH_CHILDREN);
//         if (ret < 0) {
//             av_log(NULL, AV_LOG_ERROR, "Cannot set output sample format\n");
//             goto end;
//         }
//
//         ret = av_opt_set_bin(buffersink_ctx, "channel_layouts",
//                 (uint8_t*)&enc_ctx->channel_layout,
//                 sizeof(enc_ctx->channel_layout), AV_OPT_SEARCH_CHILDREN);
//         if (ret < 0) {
//             av_log(NULL, AV_LOG_ERROR, "Cannot set output channel layout\n");
//             goto end;
//         }
//
//         ret = av_opt_set_bin(buffersink_ctx, "sample_rates",
//                 (uint8_t*)&enc_ctx->sample_rate, sizeof(enc_ctx->sample_rate),
//                 AV_OPT_SEARCH_CHILDREN);
//         if (ret < 0) {
//             av_log(NULL, AV_LOG_ERROR, "Cannot set output sample rate\n");
//             goto end;
//         }
//     } else {
//         ret = AVERROR_UNKNOWN;
//         goto end;
//     }
//
//     /* Endpoints for the filter graph. */
//     outputs->name       = av_strdup("in");
//     outputs->filter_ctx = buffersrc_ctx;
//     outputs->pad_idx    = 0;
//     outputs->next       = NULL;
//
//     inputs->name       = av_strdup("out");
//     inputs->filter_ctx = buffersink_ctx;
//     inputs->pad_idx    = 0;
//     inputs->next       = NULL;
//
//     if (!outputs->name || !inputs->name) {
//         ret = AVERROR(ENOMEM);
//         goto end;
//     }
//
//     if ((ret = avfilter_graph_parse_ptr(filter_graph, filter_spec,
//                     &inputs, &outputs, NULL)) < 0)
//         goto end;
//
//     if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
//         goto end;
//
//     /* Fill FilteringContext */
//     fctx->buffersrc_ctx = buffersrc_ctx;
//     fctx->buffersink_ctx = buffersink_ctx;
//     fctx->filter_graph = filter_graph;
//
// end:
//     avfilter_inout_free(&inputs);
//     avfilter_inout_free(&outputs);
//
//     return ret;
// }

// static int init_filters(void)
// {
//     const char *filter_spec;
//     unsigned int i;
//     int ret;
//     filter_ctx = av_malloc_array(input->ifmt_ctx->nb_streams, sizeof(*filter_ctx));
//     if (!filter_ctx)
//         return AVERROR(ENOMEM);
//
//     for (i = 0; i < input->ifmt_ctx->nb_streams; i++) {
//         filter_ctx[i].buffersrc_ctx  = NULL;
//         filter_ctx[i].buffersink_ctx = NULL;
//         filter_ctx[i].filter_graph   = NULL;
//         if (input->ifmt_ctx->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
//             input->ifmt_ctx->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
//             continue;
//
//         AVCodecContext *dec_ctx, *enc_ctx;
//
//         if (input->ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
//             filter_spec = "null"; /* passthrough (dummy) filter for video */
//             dec_ctx = input->dec_ctx_v;
//             enc_ctx = output->enc_ctx_v;
//         } else {
//             filter_spec = "anull"; /* passthrough (dummy) filter for audio */
//             dec_ctx = input->dec_ctx_a;
//             enc_ctx = output->enc_ctx_a;
//         }
//
//         ret = init_filter(&filter_ctx[i], dec_ctx, enc_ctx, filter_spec);
//         if (ret)
//             return ret;
//     }
//     return 0;
// }

static int init_resampler(SwrContext **resample_context)
{
    *resample_context = swr_alloc_set_opts(NULL,
                          av_get_default_channel_layout(output->enc_ctx_a->channels),
                          output->enc_ctx_a->sample_fmt,
                          output->enc_ctx_a->sample_rate,
                          av_get_default_channel_layout(input->dec_ctx_a->channels),
                          input->dec_ctx_a->sample_fmt,
                          input->dec_ctx_a->sample_rate,
                          0, NULL);
    if (!*resample_context) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate resample context\n");
        return AVERROR(ENOMEM);
    }
    /** Open the resampler with the specified parameters. */
    int ret = swr_init(*resample_context);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not open resample context\n");
        swr_free(resample_context);
        return ret;
    }

    return 0;
}

static int init_fifo(AVAudioFifo **fifo)
{
    /** Create the FIFO buffer based on the specified output sample format. */
    if (!(*fifo = av_audio_fifo_alloc(output->enc_ctx_a->sample_fmt,
                                      output->enc_ctx_a->channels, 1))) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate FIFO\n");
        return AVERROR(ENOMEM);
    }
    return 0;
}

static int init_converted_samples(uint8_t ***converted_input_samples,
                                  AVCodecContext *output_codec_context,
                                  int frame_size)
{
    int ret;
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

static int convert_samples(SwrContext *resample_context,
                           uint8_t **converted_data,
                           const uint8_t **input_data,
                           const int frame_size)
{
    int ret;
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

static int add_samples_to_fifo(AVAudioFifo *fifo,
                               uint8_t **converted_input_samples,
                               const int frame_size)
{
    int ret;
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

static int decode(AVCodecContext *avctx, AVFrame *frame, int *got_frame, AVPacket *pkt)
{
    int ret;
    *got_frame = 0;

    if (pkt) {
        ret = avcodec_send_packet(avctx, pkt);
        if (ret < 0)
            return ret == AVERROR_EOF ? 0 : ret;
    }

    ret = avcodec_receive_frame(avctx, frame);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
        return ret;
    if (ret >= 0)
        *got_frame = 1;

    return 0;
}

static int encode(AVCodecContext *avctx, AVPacket *pkt, AVFrame *frame,
                  AVRational tb_src, AVRational tb_dst, enum AVMediaType type)
{
    int ret;

    ret = avcodec_send_frame(avctx, frame);
    if (ret < 0)
        return ret;

    while (1) {
        ret = avcodec_receive_packet(avctx, pkt);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN))
                ret = 0;
            break;
        }

        av_packet_rescale_ts(pkt, tb_src, tb_dst);

        if (type == AVMEDIA_TYPE_VIDEO) {
            pkt->stream_index = output->stream_video->index;
        } else if (type == AVMEDIA_TYPE_AUDIO) {
            pkt->stream_index = output->stream_audio->index;
        } else {
            av_log(NULL, AV_LOG_WARNING, "Unknown media type provided\n");
        }
        // av_log(NULL, AV_LOG_INFO, "encoded packet\n");
        // av_log(NULL, AV_LOG_INFO, "pts: %"PRId64"\ndts: %"PRId64"\npos: %"PRId64"\n",
        //                           pkt->pts, pkt->dts, pkt->pos);
        // av_packet_rescale_ts(pkt, tb_src, tb_dst);
        ret = av_interleaved_write_frame(output->ofmt_ctx, pkt);
        av_packet_unref(pkt);
        if (ret < 0)
            break;
    }

    return ret;
}

static int encode_write_frame(AVFrame *filt_frame, enum AVMediaType type) {
    int ret;
    int got_frame_local;
    AVCodecContext *enc_ctx;
    AVRational tb_src, tb_dst;
    AVPacket enc_pkt;
    int stream_index;

    if (type == AVMEDIA_TYPE_VIDEO) {
        enc_ctx = output->enc_ctx_v;
        tb_src = enc_ctx->time_base;
        tb_dst = output->stream_video->time_base;
        if (filt_frame) {
            if (filt_frame->pts != AV_NOPTS_VALUE) {
                if (filt_frame->pts < vid_pts) {
                    vid_pts += 2;
                    filt_frame->pts = vid_pts;
                } else {
                    vid_pts = filt_frame->pts;
                }
            }
            if (filt_frame->pkt_dts != AV_NOPTS_VALUE) {
                if (filt_frame->pkt_dts < vid_dts) {
                    vid_dts += 2;
                    filt_frame->pkt_dts = vid_dts;
                } else {
                    vid_dts = filt_frame->pkt_dts;
                }
            }
            if (filt_frame->pts != AV_NOPTS_VALUE &&
                filt_frame->pkt_dts != AV_NOPTS_VALUE) {
                if (filt_frame->pts < filt_frame->pkt_dts) {
                    vid_pts += vid_dts - vid_pts;
                    filt_frame->pts = vid_pts;
                }
            }
            // if (filt_frame->pkt_dts < filt_frame->pts) {
            //     filt_frame->pts = av_rescale_q(filt_frame->pts,
            //                                    enc_ctx->time_base,
            //                                    output->ofmt_ctx->streams[stream_index]->time_base);
            //     filt_frame->pkt_dts = av_rescale_q(filt_frame->pkt_dts,
            //                                        enc_ctx->time_base,
            //                                        output->ofmt_ctx->streams[stream_index]->time_base);
            // }
        }
    } else if (type == AVMEDIA_TYPE_AUDIO) {
        if (filt_frame) {
            filt_frame->pts = pts;
            pts += filt_frame->nb_samples;
        }
        enc_ctx = output->enc_ctx_a;
        tb_src = enc_ctx->time_base;
        tb_dst = output->stream_audio->time_base;
    }

    /* encode filtered frame */
    av_init_packet(&enc_pkt);
    enc_pkt.data = NULL;
    enc_pkt.size = 0;

    ret = encode(enc_ctx, &enc_pkt, filt_frame, tb_src, tb_dst, type);

    if (filt_frame)
        av_frame_free(&filt_frame);

    return ret;
}

static int load_encode_and_write(AVAudioFifo *fifo,
                                 AVFormatContext *output_format_context,
                                 AVCodecContext *output_codec_context)
{
    int ret;
    AVFrame *output_frame;

    const int frame_size = FFMIN(av_audio_fifo_size(fifo),
                                 output_codec_context->frame_size);

    if (!(output_frame = av_frame_alloc())) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate output frame\n");
        return AVERROR_EXIT;
    }

    output_frame->nb_samples     = frame_size;
    output_frame->channel_layout = output_codec_context->channel_layout;
    output_frame->format         = output_codec_context->sample_fmt;
    output_frame->sample_rate    = output_codec_context->sample_rate;

    ret = av_frame_get_buffer(output_frame, 0);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could allocate output frame samples\n");
        av_frame_free(&output_frame);
        return ret;
    }

    if (av_audio_fifo_read(fifo, (void **)output_frame->data, frame_size) < frame_size) {
        av_log(NULL, AV_LOG_ERROR, "Could not read data from FIFO\n");
        av_frame_free(&output_frame);
        return AVERROR_EXIT;
    }

    ret = encode_write_frame(output_frame, AVMEDIA_TYPE_AUDIO);

    return ret;
}

// static int filter_encode_write_frame(AVFrame *frame, int stream_index)
// {
//     int ret;
//     AVFrame *filt_frame;
//
//     /* push the decoded frame into the filtergraph */
//     ret = av_buffersrc_add_frame_flags(filter_ctx[stream_index].buffersrc_ctx,
//             frame, 0);
//     if (ret < 0) {
//         av_log(NULL, AV_LOG_ERROR, "Error while feeding the filtergraph\n");
//         return ret;
//     }
//
//     /* pull filtered frames from the filtergraph */
//     while (1) {
//         filt_frame = av_frame_alloc();
//         if (!filt_frame) {
//             ret = AVERROR(ENOMEM);
//             break;
//         }
//         ret = av_buffersink_get_frame(filter_ctx[stream_index].buffersink_ctx,
//                 filt_frame);
//         if (ret < 0) {
//             /* if no more frames for output - returns AVERROR(EAGAIN)
//              * if flushed and no more frames for output - returns AVERROR_EOF
//              * rewrite retcode to 0 to show it as normal procedure completion
//              */
//             if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
//                 ret = 0;
//             av_frame_free(&filt_frame);
//             break;
//         }
//
//         ret = encode_write_frame(filt_frame, stream_index);
//         if (ret < 0)
//             break;
//     }
//
//     return ret;
// }

static int flush_encoder(int stream_index)
{
    int ret;
    AVCodec *codec;
    enum AVMediaType type;
    type = output->ofmt_ctx->streams[stream_index]->codecpar->codec_type;
    if (type == AVMEDIA_TYPE_VIDEO) {
        codec = output->codec_video;
    } else if (type == AVMEDIA_TYPE_AUDIO) {
        codec = output->codec_audio;
    }
    if (!(codec->capabilities & AV_CODEC_CAP_DELAY))
        return 0;

    av_log(NULL, AV_LOG_INFO, "Flushing stream #%u encoder\n", stream_index);
    ret = encode_write_frame(NULL, type);

    return ret;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
      av_log(NULL, AV_LOG_ERROR, "Usage: %s <input file> <output file>\n", argv[0]);
      return 1;
    }

    SwrContext        *resample_context = NULL;
    AVAudioFifo       *fifo = NULL;
    enum AVCodecID    in_codec;
    enum AVCodecID    need_codec;
    unsigned int      i;
    int               ret;
    int               got_frame;
    // int               audio_pts = 0;
    // int               video_pts = 0;
    // int               audio_dts = 0;
    // int               video_dts = 0;
    // int               last_video_pts = 0;

    av_register_all();
    avfilter_register_all();
    avformat_network_init();

    input = av_malloc(sizeof(*input));
    if (!input)
        return AVERROR(ENOMEM);

    output = av_malloc(sizeof(*output));
    if (!output)
        return AVERROR(ENOMEM);

    ret = open_input_file(argv[1]);
    if (ret < 0)
        goto end;
    ret = open_output_file(argv[2]);
    if (ret < 0)
        goto end;
    // ret = init_filters();
    // if (ret < 0)
    //     goto end;
    ret = init_resampler(&resample_context);
    if (ret < 0)
        goto end;
    ret = init_fifo(&fifo);
    if (ret < 0)
        goto end;

    /* read all packets */
    while (1) {
        int finished              = 0;
        AVFrame *frame            = NULL;
        int ret                   = AVERROR_EXIT;
        int data_present;
        int stream_index;
        enum AVMediaType type;
        AVPacket packet;

        av_init_packet(&packet);
        /** Set the packet data and size so that it is recognized as being empty. */
        packet.data = NULL;
        packet.size = 0;

        ret = av_read_frame(input->ifmt_ctx, &packet);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                finished = 1;
                while (finished && av_audio_fifo_size(fifo) > 0) {
                    if (load_encode_and_write(fifo, output->ofmt_ctx, output->enc_ctx_a))
                        goto end;
                }
                break;
            } else {
                av_log(NULL, AV_LOG_ERROR, "Could not read frame\n");
                goto end;
            }
        }
        stream_index = packet.stream_index;
        type = input->ifmt_ctx->streams[stream_index]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO) {
            if (stream_index != output->stream_video_in_input) {
                av_packet_unref(&packet);
                continue;
            }
            in_codec    = input->stream_video->codecpar->codec_id;
            need_codec  = output->stream_video->codecpar->codec_id;
        }
        if (type == AVMEDIA_TYPE_AUDIO) {
            if (stream_index != output->stream_audio_in_input) {
                av_packet_unref(&packet);
                continue;
            }
            in_codec    = input->stream_audio->codecpar->codec_id;
            need_codec  = output->stream_audio->codecpar->codec_id;
        }

        if (in_codec != need_codec)
        {
            AVCodecContext *codec_ctx;

            if (!(frame = av_frame_alloc())) {
                av_log(NULL, AV_LOG_ERROR, "Could not allocate input frame\n");
                ret = AVERROR(ENOMEM);
                goto end;
            }

            if (type == AVMEDIA_TYPE_VIDEO) {
                codec_ctx = input->dec_ctx_v;
                // av_packet_rescale_ts(&packet,
                //                       codec_ctx->time_base,
                //                       input->stream_video->time_base);
            } else {
                codec_ctx = input->dec_ctx_a;
                // av_packet_rescale_ts(&packet,
                //                       codec_ctx->time_base,
                //                       input->stream_audio->time_base);
            }

            ret = decode(codec_ctx, frame, &got_frame, &packet);
            if (ret < 0) {
                av_frame_free(&frame);
                av_log(NULL, AV_LOG_ERROR, "Decoding failed\n");
                break;
            }

            if (got_frame) {
                if (type == AVMEDIA_TYPE_AUDIO)
                {
                    /** Use the encoder's desired frame size for processing. */
                    const int output_frame_size = output->enc_ctx_a->frame_size;
                    while (av_audio_fifo_size(fifo) < output_frame_size && !finished) {
                        uint8_t **converted_input_samples = NULL;

                        if (init_converted_samples(&converted_input_samples, output->enc_ctx_a,
                                                    frame->nb_samples))
                            goto end;
                        /**
                         * Convert the input samples to the desired output sample format.
                         * This requires a temporary storage provided by converted_input_samples.
                         */
                        if (convert_samples(resample_context, converted_input_samples,
                                            (const uint8_t**)frame->extended_data,
                                            frame->nb_samples))
                            goto end;
                        /** Add the converted input samples to the FIFO buffer for later processing. */
                        if (add_samples_to_fifo(fifo, converted_input_samples, frame->nb_samples))
                            goto end;
                    }

                    while (av_audio_fifo_size(fifo) >= output_frame_size ||
                           (finished && av_audio_fifo_size(fifo) > 0)) {
                        if (load_encode_and_write(fifo, output->ofmt_ctx, output->enc_ctx_a))
                            goto end;
                    }
                }
                if (type == AVMEDIA_TYPE_VIDEO)
                {
                    frame->pts = av_frame_get_best_effort_timestamp(frame);
                    ret = encode_write_frame(frame, stream_index);
                    if (ret < 0)
                        goto end;
                }
            }
        }
        else
        {
            AVRational tb_src, tb_dst;
            AVPacket pkt;
            av_init_packet(&pkt);
            pkt.data = packet.data;
            pkt.size = packet.size;
            if (type == AVMEDIA_TYPE_VIDEO) {
                tb_src = input->stream_video->time_base;
                tb_dst = output->stream_video->time_base;
                pkt.stream_index = output->stream_video->index;
                /* remux this frame without reencoding */
                av_packet_rescale_ts(&pkt, tb_src, tb_dst);
            }
            if (type == AVMEDIA_TYPE_AUDIO) {
                tb_src = input->stream_audio->time_base;
                tb_dst = output->stream_audio->time_base;
                pkt.stream_index = output->stream_audio->index;
                pkt.pts = pkt.dts = AV_NOPTS_VALUE;
                // if (packet.pts != AV_NOPTS_VALUE) {
                //     if (pts < packet.pts)
                //         pts += packet.pts - pts;
                //     else {
                //         pts += 2;
                //         packet.pts = pts;
                //     }
                // }
                // if (packet.dts != AV_NOPTS_VALUE) {
                //     if (packet.dts < packet.pts)
                //         packet.dts = packet.pts;
                // }
            }

            // av_log(NULL, AV_LOG_INFO, "global pts: %"PRId64"\tstream_index: %u\n", pts, stream_index);
            // av_log(NULL, AV_LOG_INFO, "pts: %"PRId64"\tdts: %"PRId64"\n-----\n", pkt.pts, pkt.dts);
            ret = av_interleaved_write_frame(output->ofmt_ctx, &pkt);
            av_packet_unref(&pkt);
            if (ret < 0)
                goto end;
        }
        av_packet_unref(&packet);
    }

    /* flush video encoder */
    if (output->stream_video) {
        ret = flush_encoder(output->stream_video->index);
        if (ret < 0) {
            if (ret == AVERROR_EOF)
                ret = 0;
            else {
                av_log(NULL, AV_LOG_ERROR, "Flushing encoder failed\n");
                goto end;
            }
        }
    }
    /* flush audio encoder */
    if (output->stream_audio) {
        ret = flush_encoder(output->stream_audio->index);
        if (ret < 0) {
            if (ret == AVERROR_EOF)
                ret = 0;
            else {
                av_log(NULL, AV_LOG_ERROR, "Flushing encoder failed\n");
                goto end;
            }
        }
    }

    av_log(NULL, AV_LOG_INFO, "Going to write metadata\n");
    write_metadata(input->ifmt_ctx, output->ofmt_ctx);

    av_log(NULL, AV_LOG_INFO, "Going to write trailer\n");
    av_write_trailer(output->ofmt_ctx);
end:
    if (fifo)
        av_audio_fifo_free(fifo);
    swr_free(&resample_context);
    // for (i = 0; i < input->ifmt_ctx->nb_streams; i++) {
    //     if (filter_ctx && filter_ctx[i].filter_graph)
    //         avfilter_graph_free(&filter_ctx[i].filter_graph);
    // }
    if (input->dec_ctx_v)
        avcodec_close(input->dec_ctx_v);
    if (input->dec_ctx_a)
        avcodec_close(input->dec_ctx_a);
    if (output->enc_ctx_v)
        avcodec_close(output->enc_ctx_v);
    if (output->enc_ctx_a)
        avcodec_close(output->enc_ctx_a);
    // av_free(filter_ctx);
    avformat_close_input(&input->ifmt_ctx);
    if (output->ofmt_ctx && !(output->ofmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&output->ofmt_ctx->pb);
    avformat_free_context(output->ofmt_ctx);

    if (ret < 0)
        av_log(NULL, AV_LOG_ERROR, "Error occurred: %s\n", av_err2str(ret));

    return 0;
}
