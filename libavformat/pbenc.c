#include "a.pb-c.h"
#include "avformat.h"

static void write_req(AVIOContext *pb, Req *req) {
    int reqsize = req__get_packed_size(req);
    void *reqdata = malloc(reqsize);
    req__pack(req, reqdata);
    avio_wb32(pb, reqsize);
    avio_write(pb, reqdata, reqsize);
    free(reqdata);
}

static int pb_write_header(AVFormatContext *s) {
    if (s->nb_streams != 1) {
        av_log(s, AV_LOG_ERROR, "more than 1 stream\n");
        return AVERROR(EINVAL);
    }

    AVStream *stream = s->streams[0];
    if (stream->codecpar->codec_id != AV_CODEC_ID_HEVC) {
        av_log(s, AV_LOG_ERROR, "%s is not hevc\n", avcodec_get_name(stream->codecpar->codec_id));
        return AVERROR(EINVAL);
    }

    AVCodecParameters *codecpar = stream->codecpar;
    AVIOContext *pb = s->pb;

    Req req = REQ__INIT;
    Header header = HEADER__INIT;
    header.extradata.data = codecpar->extradata;
    header.extradata.len = codecpar->extradata_size;
    req.message_case = REQ__MESSAGE_HEADER;
    req.header = &header;

    write_req(pb, &req);

    return 0;
}

static int pb_write_packet(AVFormatContext *s, AVPacket *pkt) {
    AVIOContext *pb = s->pb;

    Req req = REQ__INIT;
    Packet reqpacket = PACKET__INIT;
    reqpacket.data.data = pkt->data;
    reqpacket.data.len = pkt->size;
    reqpacket.key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
    req.message_case = REQ__MESSAGE_PACKET;
    req.packet = &reqpacket;

    write_req(pb, &req);

    return 0;
}

AVOutputFormat ff_pb_muxer = {
    .name           = "pb",
    .long_name      = NULL_IF_CONFIG_SMALL("pb - custom protobuf format"),
    .extensions     = "pb",
    .video_codec    = AV_CODEC_ID_HEVC,
    .write_header   = pb_write_header,
    .write_packet   = pb_write_packet,
    .flags             = AVFMT_GLOBALHEADER | AVFMT_TS_NONSTRICT | AVFMT_VARIABLE_FPS |
                         AVFMT_ALLOW_FLUSH,
};
