#include <stdio.h>
#include <vector>
#include <iostream>
#include <fstream>
#include <time.h>
#define _XOPEN_SOURCE 600 /* for usleep */

extern "C" 
{
#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS
#endif

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfiltergraph.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/channel_layout.h>
#include <libavutil/common.h>
#include <libavutil/avstring.h>
#include <math.h>
}

using namespace std;

#define STREAM_DURATION   500.0
#define STREAM_FRAME_RATE 25 /* 25 images/s */
#define STREAM_PIX_FMT    AV_PIX_FMT_YUV420P /* default pix_fmt */

// Input from emulator
#define INPUT_WIDTH 160
#define INPUT_HEIGHT 144

class VideoRtspEncoder {
  static const int sws_flags = SWS_BICUBIC;

  public:
  VideoRtspEncoder() : m_fps(60), m_scale(8) {
  }

  void setFps(int fps) {
    m_fps = fps;
  }

  int fps() const {
    return m_fps;
  }

  void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt)
  {
    AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;
    (void)time_base;

    /*printf("pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s stream_index:%d\n",
      av_ts_make_string(pkt->pts), av_ts_make_string(pkt->pts, time_base),
      av_ts_make_string(pkt->dts), av_ts_make_string(pkt->dts, time_base),
      av_ts_make_string(pkt->duration), av_ts_make_string(pkt->duration, time_base),
      pkt->stream_index);*/
  }

  int write_frame(AVFormatContext *fmt_ctx, const AVRational *time_base, AVStream *st, AVPacket *pkt)
  {
    /* rescale output packet timestamp values from codec to stream timebase */
    //pkt->pts = av_rescale_q_rnd(pkt->pts, *time_base, st->time_base, AVRounding(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
    //pkt->dts = av_rescale_q_rnd(pkt->dts, *time_base, st->time_base, AVRounding(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
    pkt->duration = av_rescale_q(pkt->duration, *time_base, st->time_base);
    pkt->stream_index = st->index;

    /* Write the compressed frame to the media file. */
    log_packet(fmt_ctx, pkt);
    return av_interleaved_write_frame(fmt_ctx, pkt);
  }

  /* Add an output stream. */
  AVStream *add_stream(AVCodec **codec, enum AVCodecID codec_id)
  {
    AVCodecContext *c;
    AVStream *st;

    /* find the encoder */
    *codec = avcodec_find_encoder(codec_id);
    if (!(*codec)) {
      fprintf(stderr, "Could not find encoder for '%s'\n", "unknown");
      //avcodec_get_name(codec_id));
      exit(1);
    }

    st = avformat_new_stream(oc, *codec);
    if (!st) {
      fprintf(stderr, "Could not allocate stream\n");
      exit(1);
    }
    st->id = oc->nb_streams-1;
    c = st->codec;

    switch ((*codec)->type) {
      case AVMEDIA_TYPE_AUDIO:
        c->sample_fmt  = (*codec)->sample_fmts ?
          (*codec)->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
        c->bit_rate    = 64000;
        c->sample_rate = 44100;
        c->channels    = 2;
        break;

      case AVMEDIA_TYPE_VIDEO:
        c->codec_id = codec_id;
        //if(codec_id == CODEC_ID_H264) cout<<"Codec ID  "<<(AVCodecID)codec_id<<endl;
        c->bit_rate = 400000;
        /* Resolution must be a multiple of two. */
        c->width    = INPUT_WIDTH * m_scale;
        c->height   = INPUT_HEIGHT * m_scale;
        /* timebase: This is the fundamental unit of time (in seconds) in terms
         * of which frame timestamps are represented. For fixed-fps content,
         * timebase should be 1/framerate and timestamp increments should be
         * identical to 1. */
        c->time_base.den = STREAM_FRAME_RATE;
        c->time_base.num = 1;
        c->gop_size      = 12; /* emit one intra frame every twelve frames at most */
        c->pix_fmt       = STREAM_PIX_FMT;
        if (c->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
          /* just for testing, we also add B frames */
          c->max_b_frames = 2;
        }
        if (c->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
          /* Needed to avoid using macroblocks in which some coeffs overflow.
           * This does not happen with normal video, it just happens here as
           * the motion of the chroma plane does not match the luma plane. */
          c->mb_decision = 2;
        }
        break;

      default:
        break;
    }

    /* Some formats want stream headers to be separate. */
    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
      c->flags |= CODEC_FLAG_GLOBAL_HEADER;

    return st;
  }

  /**************************************************************/
  /* video output */

  void open_video(AVCodec *codec, AVStream *st)
  {
    int ret;
    AVCodecContext *c = st->codec;

    /* open the codec */
    ret = avcodec_open2(c, codec, NULL);
    if (ret < 0) {
      fprintf(stderr, "Could not open video codec: ");
      exit(1);
    }

    /* allocate and init a re-usable frame */
    frame = av_frame_alloc();
    if (!frame) {
      fprintf(stderr, "Could not allocate video frame\n");
      exit(1);
    }
    frame->format = c->pix_fmt;
    frame->width = c->width;
    frame->height = c->height;

    /* Allocate the encoded raw picture. */
    ret = avpicture_alloc(&dst_picture, c->pix_fmt, c->width, c->height);
    if (ret < 0) {
      fprintf(stderr, "Could not allocate picture: ");
      exit(1);
    }

    /* If the output format is not YUV420P, then a temporary YUV420P
     * picture is needed too. It is then converted to the required
     * output format. */
    /*if (c->pix_fmt != AV_PIX_FMT_YUV420P) {
      ret = avpicture_alloc(&src_picture, AV_PIX_FMT_YUV420P, c->width, c->height);
      if (ret < 0) {
        fprintf(stderr, "Could not allocate temporary picture:");
        exit(1);
      }
    }*/

    /* copy data and linesize picture pointers to frame */
    *((AVPicture *)frame) = dst_picture;
  }

  /* Prepare a dummy image. */
  void fill_yuv_image(AVPicture *pict, int frame_index,
      int width, int height)
  {
    int x, y, i;

    i = frame_index;

    /* Y */
    for (y = 0; y < height; y++)
      for (x = 0; x < width; x++)
        pict->data[0][y * pict->linesize[0] + x] = x + y + i * 3;

    /* Cb and Cr */
    for (y = 0; y < height / 2; y++) {
      for (x = 0; x < width / 2; x++) {
        pict->data[1][y * pict->linesize[1] + x] = 128 + y + i * 2;
        pict->data[2][y * pict->linesize[2] + x] = 64 + x + i * 5;
      }
    }
  }

  void write_video_frame(AVStream *st, uint8_t* data) {
    int ret;
    static struct SwsContext *sws_ctx;
    AVCodecContext *c = st->codec;

    if (!flush) {
      /* This context converts image to YUV420P from RGBA, and scales
       * up to desired size */
      if (!sws_ctx) {
        sws_ctx = sws_getContext(INPUT_WIDTH, INPUT_HEIGHT,
            AV_PIX_FMT_RGB32_1, c->width, c->height,
            AV_PIX_FMT_YUV420P, SWS_BICUBIC, 0, 0, 0);

        if (!sws_ctx) {
          fprintf(stderr, "Could not initialize the conversion context\n");
          exit(1);
        }
      }

      int inLinesize[1] = { 4 * INPUT_WIDTH }; // RGBA stride
      sws_scale(sws_ctx,
          (const uint8_t * const *)&data, inLinesize,
          0, c->height, dst_picture.data, dst_picture.linesize);
    }

    if (oc->oformat->flags & AVFMT_RAWPICTURE && !flush) {
      /* Raw video case - directly store the picture in the packet */
      AVPacket pkt;
      av_init_packet(&pkt);

      pkt.flags        |= AV_PKT_FLAG_KEY;
      pkt.stream_index  = st->index;
      pkt.data          = dst_picture.data[0];
      pkt.size          = sizeof(AVPicture);

      ret = av_interleaved_write_frame(oc, &pkt);
    } else {
      AVPacket pkt = { 0 };
      int got_packet;
      av_init_packet(&pkt);

      /* encode the image */
      frame->pts = frame_count;
      ret = avcodec_encode_video2(c, &pkt, flush ? NULL : frame, &got_packet);
      if (ret < 0) {
        fprintf(stderr, "Error encoding video frame:");
        exit(1);
      }
      /* If size is zero, it means the image was buffered. */

      if (got_packet) {
        //cout<<"got Packet"<<endl;
        ret = write_frame(oc, &c->time_base, st, &pkt);
      } else {
        if (flush) {
          video_is_eof = 1;
        }
        ret = 0;
      }
    }

    if (ret < 0) {
      fprintf(stderr, "Error while writing video frame: ");
      exit(1);
    }
    frame_count++;
  }

  void encodeFrame(uint8_t* data) {
    if (!video_st) {
      std::cout << "No video stream" << std::endl;
      return;
    }
    if (video_is_eof) {
      std::cout << "Video is EOF" << std::endl;
      return;
    }

    /* Compute current audio and video time. */
    video_time = (video_st && !video_is_eof) ? video_st->pts.val * av_q2d(video_st->time_base) : INFINITY;

    if (!flush && (!video_st || video_time >= STREAM_DURATION)) { // TODO don't use STREAM_DURATION
      flush = 1;
    }

    if (video_st && !video_is_eof) {
      write_video_frame(video_st, data);
    }
  }

  void initialize() {
    const char *filename = "rtsp://127.0.0.1:8554/live.sdp";

    /* Initialize libavcodec, and register all codecs and formats. */
    av_register_all();
    avformat_network_init();

    oc = avformat_alloc_context();
    if (!oc) {
      fprintf(stderr, "Error creating output context; aborting\n");
      exit(1);
    }

    oc->oformat = av_guess_format("rtsp", NULL, NULL);
    av_strlcpy(oc->filename, filename, sizeof(oc->filename));

    if (!oc) {
      exit(1);
    }

    fmt = oc->oformat;
    if(!fmt) {
      fprintf(stderr, "Error creating oformat\n");
      exit(1);
    }
    /* Add the audio and video streams using the default format codecs
     * and initialize the codecs. */

    /*if(av_opt_set(fmt,"rtsp_transport","tcp",0) < 0)
      cout<<"Opt not set\n";*/
    video_st = NULL;
    audio_st = NULL;
    //cout<<"Codec = "<<avcodec_get_name(fmt->video_codec)<<endl;
    if (fmt->video_codec != AV_CODEC_ID_NONE)
    {
      video_st = add_stream(&video_codec, fmt->video_codec);
    }
    /*if (fmt->audio_codec != AV_CODEC_ID_NONE)
      audio_st = add_stream(oc, &audio_codec, fmt->audio_codec);*/

    /* Now that all the parameters are set, we can open the audio and
     * video codecs and allocate the necessary encode buffers. */
    if (video_st) {
      open_video(video_codec, video_st);
    }
    /*if (audio_st)
      open_audio(oc, audio_codec, audio_st);*/

    av_dump_format(oc, 0, filename, 1);
    //char errorBuff[80];

    if (!(fmt->flags & AVFMT_NOFILE)) {
      ret = avio_open(&oc->pb, filename, AVIO_FLAG_WRITE);
      if (ret < 0) {
        //fprintf(stderr, "Could not open outfile '%s': %s", filename, av_make_error_string(errorBuff,80,ret));
        fprintf(stderr, "Could not open outfile '%s'\n", filename);
        exit(1);
      }

    }

    ret = avformat_write_header(oc, NULL);
    if (ret < 0) {
      fprintf(stderr, "Error occurred when writing header\n");
      exit(1);
    }

    flush = 0;
  }

  virtual ~VideoRtspEncoder() {
    /* Close each codec. */
    if (video_st) {
      close_video(oc, video_st);
    }

    if (!(fmt->flags & AVFMT_NOFILE))
      /* Close the output file. */
      avio_close(oc->pb);

    /* free the stream */
    avformat_free_context(oc);
  }

  void close_video(AVFormatContext *oc, AVStream *st) {
    /* Write the trailer, if any. The trailer must be written before you
     * close the CodecContexts open when you wrote the header; otherwise
     * av_write_trailer() may try to use memory that was freed on
     * av_codec_close(). */
    // TODO mpn not sure if this goes here
    av_write_trailer(oc);

    avcodec_close(st->codec);
    av_free(src_picture.data[0]);
    av_free(dst_picture.data[0]);
    av_frame_free(&frame);
  }

  private:
    int video_is_eof = 0, audio_is_eof = 0;
    AVFrame *frame;
    AVPicture src_picture, dst_picture;
    AVOutputFormat *fmt;
    AVFormatContext *oc;
    AVStream *audio_st, *video_st;
    AVCodec *audio_codec, *video_codec;
    double video_time;
    int flush, ret;
    int frame_count;

    int m_fps;
    int m_scale;

};

