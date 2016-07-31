
extern "C" {
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/common.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
}

class VideoEncoder {

  public:
    VideoEncoder(int fps) : m_fps(fps), m_pts(0) {
      // Setup the codec and the context
      avcodec_register_all();
       
      const char* filename = "/tmp/out.mpg";
      f = fopen(filename, "wb");
      if (!f) {
        fprintf(stderr, "Could not open %s\n", filename);
        exit(1);
      }

      /* find the video encoder */
      AVCodecID codec_id = AV_CODEC_ID_MPEG2VIDEO;
      m_codec = avcodec_find_encoder(codec_id);

      if (!m_codec) {
        fprintf(stderr, "Codec not found\n");
        exit(1);
      }

      m_context = avcodec_alloc_context3(m_codec);
      if (!m_context) {
        fprintf(stderr, "Could not allocate video codec context\n");
        exit(1);
      }

      if (codec_id == AV_CODEC_ID_H264) {
        av_opt_set(m_context->priv_data, "preset", "slow", 0);
      }

      /* put sample parameters */
      m_context->bit_rate = 400000;
      /* resolution must be a multiple of two */
      m_context->width = 160;
      m_context->height = 144;
      /* frames per second */
      m_context->time_base = (AVRational){1, m_fps};
      /* emit one intra frame every ten frames
       * check frame pict_type before passing frame
       * to encoder, if frame->pict_type is AV_PICTURE_TYPE_I
       * then gop_size is ignored and the output of encoder
       * will always be I frame irrespective to gop_size
       */
      m_context->gop_size = 10;
      m_context->max_b_frames = 1;
      m_context->pix_fmt = AV_PIX_FMT_YUV420P;

      // To convert from emulator RGBA32 to YUV420P
      m_swscaleContext = sws_getContext(m_context->width, m_context->height,
          AV_PIX_FMT_RGB32_1, m_context->width, m_context->height,
          AV_PIX_FMT_YUV420P, 0, 0, 0, 0);
      if (!m_swscaleContext) {
        fprintf(stderr, "Failed to allocate swscale context\n");
        exit(1);
      }

      /* open it */
      if (avcodec_open2(m_context, m_codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        exit(1);
      }
    }

    int fps() const {
      return m_fps;
    }

    virtual ~VideoEncoder() {
      closeFile();
      avcodec_close(m_context);
      av_free(m_context);
    }

    void encodeFrame(uint8_t* data) {
      //int i, ret, x, y, got_output;
      int ret, got_output;

      printf("Encode video frame\n");

      AVFrame* frame = av_frame_alloc();
      if (!frame) {
        fprintf(stderr, "Could not allocate video frame\n");
        exit(1);
      }
      frame->format = m_context->pix_fmt;
      frame->width  = m_context->width;
      frame->height = m_context->height;

      /* the image can be allocated by any means and av_image_alloc() is
       * just the most convenient way if av_malloc() is to be used */
      ret = av_image_alloc(frame->data, frame->linesize, m_context->width,
          m_context->height, m_context->pix_fmt, 32);
      if (ret < 0) {
        fprintf(stderr, "Could not allocate raw picture buffer\n");
        exit(1);
      }

      AVPacket pkt;
      av_init_packet(&pkt);
      pkt.data = NULL;    // packet data will be allocated by the encoder
      pkt.size = 0;

      fflush(stdout);
      /* prepare a dummy image */
      /* Y */
      /*for (y = 0; y < m_context->height; y++) {
        for (x = 0; x < m_context->width; x++) {
          frame->data[0][y * frame->linesize[0] + x] = x + y + (m_pts % 25) * 3;
        }
      }

      // Cb and Cr
      for (y = 0; y < m_context->height/2; y++) {
        for (x = 0; x < m_context->width/2; x++) {
          frame->data[1][y * frame->linesize[1] + x] = 128 + y + (m_pts % 25) * 2;
          frame->data[2][y * frame->linesize[2] + x] = 64 + x + (m_pts % 25) * 5;
        }
      }*/

      // Convert RGBA32 -> YUV420P
      //uint8_t * inData[1] = { frame->data }; // RGB32 have one plane (?)
      int inLinesize[1] = { 4*m_context->width }; // RGBA stride
      sws_scale(m_swscaleContext, (const uint8_t* const*)&data, inLinesize, 0, m_context->height, frame->data, frame->linesize);

      frame->pts = m_pts;
      m_pts++;

      /* encode the image */
      ret = avcodec_encode_video2(m_context, &pkt, frame, &got_output);
      if (ret < 0) {
        fprintf(stderr, "Error encoding frame\n");
        exit(1);
      }

      if (got_output) {
        printf("Write frame %3d (size=%5d)\n", m_pts, pkt.size);
        fwrite(pkt.data, 1, pkt.size, f);
        av_packet_unref(&pkt);
      }

      /* get the delayed frames */
      /*for (got_output = 1; got_output; i++) {
        fflush(stdout);

        ret = avcodec_encode_video2(c, &pkt, NULL, &got_output);
        if (ret < 0) {
        fprintf(stderr, "Error encoding frame\n");
        exit(1);
        }

        if (got_output) {
        printf("Write frame %3d (size=%5d)\n", i, pkt.size);
        fwrite(pkt.data, 1, pkt.size, f);
        av_packet_unref(&pkt);
        }
        }*/

      av_freep(&frame->data[0]);
      av_frame_free(&frame);
      //printf("\n");
    }

    void closeFile() {
      /* add sequence end code to have a real MPEG file */
      uint8_t endcode[] = { 0, 0, 1, 0xb7 };
      fwrite(endcode, 1, sizeof(endcode), f);
      fclose(f);

    }

  private:
    AVCodec *m_codec;
    AVCodecContext *m_context = NULL;
    FILE *f;
    int m_fps;
    int m_pts;
    struct SwsContext* m_swscaleContext;
    int m_available;

};


