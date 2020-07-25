/*
 * copyright (c) 2003 fabrice bellard
 *
 * this file is part of ffmpeg.
 *
 * ffmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the gnu lesser general public
 * license as published by the free software foundation; either
 * version 2.1 of the license, or (at your option) any later version.
 *
 * ffmpeg is distributed in the hope that it will be useful,
 * but without any warranty; without even the implied warranty of
 * merchantability or fitness for a particular purpose.  see the gnu
 * lesser general public license for more details.
 *
 * you should have received a copy of the gnu lesser general public
 * license along with ffmpeg; if not, write to the free software
 * foundation, inc., 51 franklin street, fifth floor, boston, ma 02110-1301 usa
 */

/**
 * @file
 * simple media player based on the ffmpeg libraries
 */

#include "config.h"
#include <inttypes.h>
#include <math.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <unistd.h>

#include "libavutil/avstring.h"
#include "libavutil/eval.h"
#include "libavutil/mathematics.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/dict.h"
#include "libavutil/parseutils.h"
#include "libavutil/samplefmt.h"
#include "libavutil/avassert.h"
#include "libavutil/time.h"
#include "libavutil/bprint.h"
#include "libavformat/avformat.h"
#include "libavdevice/avdevice.h"
#include "libswscale/swscale.h"
#include "libavutil/opt.h"
#include "libavcodec/avfft.h"
#include "libswresample/swresample.h"
#include "libavformat/a.pb-c.h"

#include "cmdutils.h"

// #if config_avfilter
// # include "libavfilter/avfilter.h"
// # include "libavfilter/buffersink.h"
// # include "libavfilter/buffersrc.h"
// #endif

#include <SDL.h>
#include <SDL_thread.h>

#include <assert.h>

const char program_name[] = "ffrdp";
const int program_birth_year = 2020;

static unsigned sws_flags = SWS_BICUBIC;

static const struct TextureFormatEntry {
    enum AVPixelFormat format;
    int texture_fmt;
} sdl_texture_format_map[] = {
    { AV_PIX_FMT_RGB8,           SDL_PIXELFORMAT_RGB332 },
    { AV_PIX_FMT_RGB444,         SDL_PIXELFORMAT_RGB444 },
    { AV_PIX_FMT_RGB555,         SDL_PIXELFORMAT_RGB555 },
    { AV_PIX_FMT_BGR555,         SDL_PIXELFORMAT_BGR555 },
    { AV_PIX_FMT_RGB565,         SDL_PIXELFORMAT_RGB565 },
    { AV_PIX_FMT_BGR565,         SDL_PIXELFORMAT_BGR565 },
    { AV_PIX_FMT_RGB24,          SDL_PIXELFORMAT_RGB24 },
    { AV_PIX_FMT_BGR24,          SDL_PIXELFORMAT_BGR24 },
    { AV_PIX_FMT_0RGB32,         SDL_PIXELFORMAT_RGB888 },
    { AV_PIX_FMT_0BGR32,         SDL_PIXELFORMAT_BGR888 },
    { AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888 },
    { AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888 },
    { AV_PIX_FMT_RGB32,          SDL_PIXELFORMAT_ARGB8888 },
    { AV_PIX_FMT_RGB32_1,        SDL_PIXELFORMAT_RGBA8888 },
    { AV_PIX_FMT_BGR32,          SDL_PIXELFORMAT_ABGR8888 },
    { AV_PIX_FMT_BGR32_1,        SDL_PIXELFORMAT_BGRA8888 },
    { AV_PIX_FMT_YUV420P,        SDL_PIXELFORMAT_IYUV },
    { AV_PIX_FMT_YUYV422,        SDL_PIXELFORMAT_YUY2 },
    { AV_PIX_FMT_UYVY422,        SDL_PIXELFORMAT_UYVY },
    { AV_PIX_FMT_NONE,           SDL_PIXELFORMAT_UNKNOWN },
};

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_RendererInfo renderer_info = {0};

// static int default_width  = 640;
// static int default_height = 480;
// static int screen_height = 0;
// static int screen_width = 0;
static int alwaysontop = 0;

static int is_full_screen = 0;

static void refresh_loop_wait_event(SDL_Event *event) {
    // double remaining_time = 0.0;
    SDL_PumpEvents();
    while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
        // if (!cursor_hidden && av_gettime_relative() - cursor_last_shown > CURSOR_HIDE_DELAY) {
        //     SDL_ShowCursor(0);
        //     cursor_hidden = 1;
        // }
        // if (remaining_time > 0.0)
        //     av_usleep((int64_t)(remaining_time * 1000000.0));
        // remaining_time = REFRESH_RATE;
        // if (is->show_mode != SHOW_MODE_NONE && (!is->paused || is->force_refresh))
        //     video_refresh(is, &remaining_time);
        SDL_PumpEvents();
    }
}

static void toggle_full_screen()
{
    is_full_screen = !is_full_screen;
    SDL_SetWindowFullscreen(window, is_full_screen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

static void calculate_display_rect(SDL_Rect *rect,
                                   int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                                   int pic_width, int pic_height, AVRational pic_sar)
{
    AVRational aspect_ratio = pic_sar;
    int64_t width, height, x, y;

    if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0)
        aspect_ratio = av_make_q(1, 1);

    aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

    /* XXX: we suppose the screen has a 1.0 pixel ratio */
    height = scr_height;
    width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
    if (width > scr_width) {
        width = scr_width;
        height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
    }
    x = (scr_width - width) / 2;
    y = (scr_height - height) / 2;
    rect->x = scr_xleft + x;
    rect->y = scr_ytop  + y;
    rect->w = FFMAX((int)width,  1);
    rect->h = FFMAX((int)height, 1);
}

static int realloc_texture(SDL_Texture **texture, Uint32 new_format, int new_width, int new_height, SDL_BlendMode blendmode, int init_texture)
{
    Uint32 format;
    int access, w, h;
    if (!*texture || SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 || new_width != w || new_height != h || new_format != format) {
        void *pixels;
        int pitch;
        if (*texture)
            SDL_DestroyTexture(*texture);
        if (!(*texture = SDL_CreateTexture(renderer, new_format, SDL_TEXTUREACCESS_STREAMING, new_width, new_height)))
            return -1;
        if (SDL_SetTextureBlendMode(*texture, blendmode) < 0)
            return -1;
        if (init_texture) {
            if (SDL_LockTexture(*texture, NULL, &pixels, &pitch) < 0)
                return -1;
            memset(pixels, 0, pitch * new_height);
            SDL_UnlockTexture(*texture);
        }
        av_log(NULL, AV_LOG_VERBOSE, "Created %dx%d texture with %s.\n", new_width, new_height, SDL_GetPixelFormatName(new_format));
    }
    return 0;
}

static void get_sdl_pix_fmt_and_blendmode(int format, Uint32 *sdl_pix_fmt, SDL_BlendMode *sdl_blendmode)
{
    int i;
    *sdl_blendmode = SDL_BLENDMODE_NONE;
    *sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;
    if (format == AV_PIX_FMT_RGB32   ||
        format == AV_PIX_FMT_RGB32_1 ||
        format == AV_PIX_FMT_BGR32   ||
        format == AV_PIX_FMT_BGR32_1)
        *sdl_blendmode = SDL_BLENDMODE_BLEND;
    for (i = 0; i < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; i++) {
        if (format == sdl_texture_format_map[i].format) {
            *sdl_pix_fmt = sdl_texture_format_map[i].texture_fmt;
            return;
        }
    }
}
static struct SwsContext *img_convert_ctx;

static int upload_texture(SDL_Texture **tex, AVFrame *frame) {
    int ret = 0;
    Uint32 sdl_pix_fmt;
    SDL_BlendMode sdl_blendmode;
    get_sdl_pix_fmt_and_blendmode(frame->format, &sdl_pix_fmt, &sdl_blendmode);
    if (realloc_texture(tex, sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN ? SDL_PIXELFORMAT_ARGB8888 : sdl_pix_fmt, frame->width, frame->height, sdl_blendmode, 0) < 0)
        return -1;

    switch (sdl_pix_fmt) {
        case SDL_PIXELFORMAT_UNKNOWN:
            /* This should only happen if we are not using avfilter... */
            img_convert_ctx = sws_getCachedContext(img_convert_ctx,
                frame->width, frame->height, frame->format, frame->width, frame->height,
                AV_PIX_FMT_BGRA, sws_flags, NULL, NULL, NULL);
            if (img_convert_ctx != NULL) {
                uint8_t *pixels[4];
                int pitch[4];
                if (!SDL_LockTexture(*tex, NULL, (void **)pixels, pitch)) {
                    sws_scale(img_convert_ctx, (const uint8_t * const *)frame->data, frame->linesize,
                              0, frame->height, pixels, pitch);
                    SDL_UnlockTexture(*tex);
                }
            } else {
                av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
                ret = -1;
            }
            break;
        case SDL_PIXELFORMAT_IYUV:
            if (frame->linesize[0] > 0 && frame->linesize[1] > 0 && frame->linesize[2] > 0) {
                ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0], frame->linesize[0],
                                                       frame->data[1], frame->linesize[1],
                                                       frame->data[2], frame->linesize[2]);
            } else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 && frame->linesize[2] < 0) {
                ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height                    - 1), -frame->linesize[0],
                                                       frame->data[1] + frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[1],
                                                       frame->data[2] + frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[2]);
            } else {
                av_log(NULL, AV_LOG_ERROR, "Mixed negative and positive linesizes are not supported.\n");
                return -1;
            }
            break;
        default:
            if (frame->linesize[0] < 0) {
                ret = SDL_UpdateTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0]);
            } else {
                ret = SDL_UpdateTexture(*tex, NULL, frame->data[0], frame->linesize[0]);
            }
            break;
    }
    return ret;
}

static void set_sdl_yuv_conversion_mode(AVFrame *frame)
{
#if SDL_VERSION_ATLEAST(2,0,8)
    SDL_YUV_CONVERSION_MODE mode = SDL_YUV_CONVERSION_AUTOMATIC;
    if (frame && (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUYV422 || frame->format == AV_PIX_FMT_UYVY422)) {
        if (frame->color_range == AVCOL_RANGE_JPEG)
            mode = SDL_YUV_CONVERSION_JPEG;
        else if (frame->colorspace == AVCOL_SPC_BT709)
            mode = SDL_YUV_CONVERSION_BT709;
        else if (frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M || frame->colorspace == AVCOL_SPC_SMPTE240M)
            mode = SDL_YUV_CONVERSION_BT601;
    }
    SDL_SetYUVConversionMode(mode);
#endif
}

static SDL_Texture *vid_texture;

/* display the current picture, if any */
static void frame_display(AVFrame *frame)
{
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    SDL_Rect rect;

    int scr_w, scr_h;
    SDL_GetWindowSize(window, &scr_w, &scr_h);

    calculate_display_rect(&rect, 0, 0, scr_w, scr_h, frame->width, frame->height, frame->sample_aspect_ratio);
    if (upload_texture(&vid_texture, frame) < 0)
        return;

    set_sdl_yuv_conversion_mode(frame);
    SDL_RenderCopyEx(renderer, vid_texture, NULL, &rect, 0, NULL, /*SDL_FLIP_VERTICAL */ 0);
    set_sdl_yuv_conversion_mode(NULL);

    SDL_RenderPresent(renderer);
}

static int read_req(AVIOContext *pb, Req **preq) {
    int reqsize = avio_rb32(pb);
    if (pb->eof_reached) {
        return AVERROR_EOF;
    }
    if (reqsize > 1024*1024*64) {
        return AVERROR_INVALIDDATA;
    }
    void *reqdata = malloc(reqsize);
    if (avio_read(pb, reqdata, reqsize) < 0) {
        return AVERROR_EOF;
    }

    Req *req = req__unpack(NULL, reqsize, reqdata);
    if (req == NULL) {
        return AVERROR_EOF;
    }

    *preq = req;
    return 0;
}

static SDL_mutex *frame_render_lock;
static SDL_cond *frame_render_cond;
static int frame_render_ok;
static AVFrame *decode_frame;

static int read_input_thread(void *_) {
    const AVCodec *codec;
    AVCodecContext *c = NULL;

    codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if (!codec) {
        av_log(NULL, AV_LOG_FATAL, "hevc codec not found\n");
        exit(1);
    }

    c = avcodec_alloc_context3(codec);
    if (!c) {
        av_log(NULL, AV_LOG_FATAL, "Could open codec\n");
        exit(1);
    }

    if (avcodec_open2(c, codec, NULL) < 0) {
        av_log(NULL, AV_LOG_FATAL, "Could open codec\n");
        exit(1);
    }

    decode_frame = av_frame_alloc();
    if (!decode_frame) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate video frame\n");
        exit(1);
    }

    AVIOContext *pb = NULL;
    if (avio_open(&pb, "pipe:0", AVIO_FLAG_READ) < 0) {
        av_log(NULL, AV_LOG_FATAL, "open pipe:0 failed\n");
        exit(1);
    }

    void *extradata = NULL;
    int extradata_size = 0;

    for (;;) {
        Req *req = NULL;
        if (read_req(pb, &req) < 0) {
            av_log(NULL, AV_LOG_FATAL, "read eof\n");
            exit(0);
        }

        if (req->message_case == REQ__MESSAGE_HEADER) {
            extradata_size = req->header->extradata.len;
            extradata = malloc(extradata_size);
            memcpy(extradata, req->header->extradata.data, extradata_size);
        } else if (req->message_case == REQ__MESSAGE_PACKET) {
            av_log(NULL, AV_LOG_INFO, "got req\n");

            AVPacket pkt;
            av_init_packet(&pkt);
            pkt.data = req->packet->data.data;
            pkt.size = req->packet->data.len;
            if (extradata != NULL) {
                av_packet_add_side_data(&pkt, AV_PKT_DATA_NEW_EXTRADATA, extradata, extradata_size);
                extradata = NULL;
            }

            int r = avcodec_send_packet(c, &pkt);
            if (r < 0) {
                av_log(NULL, AV_LOG_FATAL, "send packet error\n");
                exit(1);
            }

            for (;;) {
                int r = avcodec_receive_frame(c, decode_frame);
                if (r == AVERROR_EOF || r == AVERROR(EAGAIN)) {
                    break;
                } else if (r < 0) {
                    av_log(NULL, AV_LOG_FATAL, "receive frame error\n");
                    exit(1);
                }

                if (!req->packet->decode_only) {
                    SDL_Event e = {};
                    e.type = SDL_USEREVENT;
                    SDL_PushEvent(&e);

                    SDL_LockMutex(frame_render_lock);
                    while (!frame_render_ok)
                    {
                        SDL_CondWait(frame_render_cond, frame_render_lock);
                    }
                    frame_render_ok = 0;
                    SDL_UnlockMutex(frame_render_lock);
                }
            }
        }

        req__free_unpacked(req, NULL);
    }

    return 0;
}

/* handle an event sent by the GUI */
static void event_loop()
{
    SDL_Event event;

    for (;;) {
        refresh_loop_wait_event(&event);
        switch (event.type) {
        case SDL_USEREVENT:
            SDL_LockMutex(frame_render_lock);
            frame_display(decode_frame);
            frame_render_ok = 1;
            SDL_CondSignal(frame_render_cond);
            SDL_UnlockMutex(frame_render_lock);
            break;

        case SDL_KEYDOWN:
            if (event.key.keysym.sym == SDLK_q) {
                exit(0);
                break;
            }
            switch (event.key.keysym.sym) {
            case SDLK_f:
                toggle_full_screen();
                // cur_stream->force_refresh = 1;
                break;
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEMOTION:
            break;
        case SDL_WINDOWEVENT:
            switch (event.window.event) {
                case SDL_WINDOWEVENT_SIZE_CHANGED:
                    // screen_width  = event.window.data1;
                    // screen_height = event.window.data2;
                    // if (cur_stream->vis_texture) {
                    //     SDL_DestroyTexture(cur_stream->vis_texture);
                    //     cur_stream->vis_texture = NULL;
                    // }
                    break;
                case SDL_WINDOWEVENT_EXPOSED:
                    // cur_stream->force_refresh = 1;
                    break;
            }
            break;
        case SDL_QUIT:
            exit(1);
            break;
        default:
            break;
        }
    }
}

// static void set_default_window_size(int width, int height, AVRational sar)
// {
//     SDL_Rect rect;
//     int max_width  = screen_width  ? screen_width  : INT_MAX;
//     int max_height = screen_height ? screen_height : INT_MAX;
//     if (max_width == INT_MAX && max_height == INT_MAX)
//         max_height = height;
//     calculate_display_rect(&rect, 0, 0, max_width, max_height, width, height, sar);
//     default_width  = rect.w;
//     default_height = rect.h;
// }

// static int video_open_set_window_size()
// {
//     int w = screen_width ? screen_width : default_width;
//     int h = screen_height ? screen_height : default_height;

//     // SDL_SetWindowTitle(window, window_title);

//     SDL_SetWindowSize(window, w, h);
//     // SDL_SetWindowPosition(window, screen_left, screen_top);
//     if (is_full_screen)
//         SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
//     SDL_ShowWindow(window);

//     return 0;
// }

void show_help_default(const char *opt, const char *arg)
{
}

/* Called from the main */
int main(int argc, char **argv)
{
    av_log_set_flags(AV_LOG_SKIP_REPEATED);

    frame_render_lock = SDL_CreateMutex();
    frame_render_cond = SDL_CreateCond();

    // signal(SIGINT , sigterm_handler); /* Interrupt (ANSI).    */
    // signal(SIGTERM, sigterm_handler); /* Termination (ANSI).  */

    if (SDL_Init (SDL_INIT_VIDEO | SDL_INIT_TIMER)) {
        av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
        av_log(NULL, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
        exit(1);
    }

    SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
    SDL_EventState(SDL_USEREVENT, SDL_IGNORE);

    // av_init_packet(&flush_pkt);
    // flush_pkt.data = (uint8_t *)&flush_pkt;

    {
        int flags = SDL_WINDOW_HIDDEN;
        if (alwaysontop)
#if SDL_VERSION_ATLEAST(2,0,5)
            flags |= SDL_WINDOW_ALWAYS_ON_TOP;
#else
            av_log(NULL, AV_LOG_WARNING, "Your SDL version doesn't support SDL_WINDOW_ALWAYS_ON_TOP. Feature will be inactive.\n");
#endif
        if (/*borderless*/0)
            flags |= SDL_WINDOW_BORDERLESS;
        else
            flags |= SDL_WINDOW_RESIZABLE;
        window = SDL_CreateWindow("ffrdp", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 800, 600, flags);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        if (window) {
            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
            if (!renderer) {
                av_log(NULL, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n", SDL_GetError());
                renderer = SDL_CreateRenderer(window, -1, 0);
            }
            if (renderer) {
                if (!SDL_GetRendererInfo(renderer, &renderer_info))
                    av_log(NULL, AV_LOG_VERBOSE, "Initialized %s renderer.\n", renderer_info.name);
            }
        }
        if (!window || !renderer || !renderer_info.num_texture_formats) {
            av_log(NULL, AV_LOG_FATAL, "Failed to create window or renderer: %s", SDL_GetError());
            exit(1);
        }
    }

    SDL_CreateThread(read_input_thread, "", (void *)NULL);

    // SDL_SetWindowSize(window, 800, 600);
    // SDL_SetWindowPosition(window, screen_left, screen_top);
    if (is_full_screen)
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_ShowWindow(window);

    event_loop();

    /* never returns */

    return 0;
}
