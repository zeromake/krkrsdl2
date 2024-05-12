#ifdef TVP_FFMPEG_WAVE_DECODER_IMPLEMENT
#include <string>

#include "tjsCommHead.h"
#include "WaveIntf.h"
#include "StorageIntf.h"
#include "DebugIntf.h"
#include "SysInitIntf.h"
#include "BinaryStream.h"
extern "C" {
    #ifndef __STDC_CONSTANT_MACROS
    #define __STDC_CONSTANT_MACROS
    #endif
    #ifndef __STDC_FORMAT_MACROS
    #define __STDC_FORMAT_MACROS
    #endif
    #ifndef UINT64_C
    #define UINT64_C(x)  (x ## ULL)
    #endif
    #include "libavutil/avutil.h"
    #include "libavutil/opt.h"
    #include "libavcodec/avcodec.h"
    #include "libavformat/avformat.h"
};

class FFMPEGWaveDecoder : public tTVPWaveDecoder
{
	bool  IsPlanar;

	int            StreamIdx;
	int            audio_buf_index;
	int            audio_buf_samples;
	int64_t        audio_frame_next_pts;
	uint64_t       stream_start_time;
	tTVPWaveFormat  TSSFormat;
	AVSampleFormat AVFmt;
	AVPacket       pkt_temp;
	AVStream *     AudioStream;

	AVPacket         Packet;
	tTJSBinaryStream* InputStream;
	AVFormatContext *FormatCtx;
	AVCodecContext  *CodecCtx;
	AVFrame *        frame;


public:
	bool    Open(const ttstr &url);
	int     audio_decode_frame();
	void    Clear();
	bool    ReadPacket();

	FFMPEGWaveDecoder()
		: InputStream(nullptr)
		, FormatCtx(nullptr)
		, CodecCtx(nullptr)
		, frame(nullptr)
	{
		av_log_set_level(AV_LOG_QUIET);
		memset(&Packet, 0, sizeof(Packet));
		memset(&TSSFormat, 0, sizeof(TSSFormat));
	}
	virtual ~FFMPEGWaveDecoder()
	{
		Clear();
	}

public:
	// ITSSWaveDecoder
	void GetFormat(tTVPWaveFormat &format);
	bool SetPosition(tjs_uint64 samplepos);
	bool Render(void *buf, tjs_uint bufsamplelen, tjs_uint &rendered);
};

void FFMPEGWaveDecoder::GetFormat(tTVPWaveFormat &format)
{
	format = TSSFormat;
}


static int AVReadFunc(void *opaque, uint8_t *buf, int buf_size)
{
	tjs_uint64 read = ((tTJSBinaryStream *)opaque)->Read((void *)buf, buf_size);
	if (read == 0)
	{
		return AVERROR_EOF;
	}
	return read;
}

static int64_t AVSeekFunc(void *opaque, int64_t offset, int whence)
{
	if (whence == AVSEEK_SIZE)
	{
		return ((tTJSBinaryStream *)opaque)->GetSize();
	}
    return ((tTJSBinaryStream *)opaque)->Seek(offset, whence & 0xFF);
}

template <typename T>
static unsigned char *_CopySamples(unsigned char *dst, AVFrame *frame, int samples, int buf_index)
{
	int buf_pos = buf_index * sizeof(T);
	T * pDst    = (T *)dst;
	for (int i = 0; i < samples; ++i, buf_pos += sizeof(T))
	{
		for (int j = 0; j < frame->ch_layout.nb_channels; ++j)
		{
			*pDst++ = *(T *)(frame->data[j] + buf_pos);
		}
	}
	return (unsigned char *)pDst;
}

static unsigned char *CopySamples(unsigned char *dst, AVFrame *frame, int samples, int buf_index)
{
	switch (frame->format)
	{
		case AV_SAMPLE_FMT_FLTP:
		case AV_SAMPLE_FMT_S32P:
			return _CopySamples<uint32_t>(dst, frame, samples, buf_index);
		case AV_SAMPLE_FMT_S16P:
			return _CopySamples<uint16_t>(dst, frame, samples, buf_index);
		default:
			return nullptr;
	}
}

bool FFMPEGWaveDecoder::Render(void *buf, tjs_uint bufsamplelen, tjs_uint &rendered)
{
	if (!InputStream)
	{
		return false;
	}
	int            remain      = bufsamplelen;
	int            sample_size = av_samples_get_buffer_size(NULL, TSSFormat.Channels, 1, AVFmt, 1);
	unsigned char *stream      = (unsigned char *)buf;
	while (remain)
	{
		if (audio_buf_index >= audio_buf_samples)
		{
			int decoded_samples = audio_decode_frame();
			if (decoded_samples < 0)
			{
				break;
			}
			audio_buf_samples = decoded_samples;
			audio_buf_index   = 0;
		}
		int samples = audio_buf_samples - audio_buf_index;
		if (samples > remain)
		{
			samples = remain;
		}

		if (!IsPlanar || TSSFormat.Channels == 1)
		{
			memcpy(stream, (frame->data[0] + audio_buf_index * sample_size), samples * sample_size);
			stream += samples * sample_size;
		}
		else
		{
			stream = CopySamples(stream, frame, samples, audio_buf_index);
		}
		remain -= samples;
		audio_buf_index += samples;
	}
    rendered = bufsamplelen - remain;
	return !remain;
}

bool FFMPEGWaveDecoder::SetPosition(tjs_uint64 samplepos)
{
	if (!InputStream)
	{
		return false;
	}
	if (samplepos && !TSSFormat.Seekable)
	{
		return false;
	}

	int64_t seek_target = samplepos / av_q2d(AudioStream->time_base) / TSSFormat.SamplesPerSec;
	if (AudioStream->start_time != AV_NOPTS_VALUE)
	{
		seek_target += AudioStream->start_time;
	}
	if (Packet.duration <= 0)
	{
		if (Packet.data)
		{
			av_packet_unref(&Packet);
		}
		if (!ReadPacket())
		{
			int ret = avformat_seek_file(FormatCtx, StreamIdx, 0, 0, 0, AVSEEK_FLAG_BACKWARD);
			if (ret < 0)
			{
				return false;
			}
			if (!ReadPacket())
			{
				return false;
			}
		}
	}
	int64_t seek_temp = seek_target - Packet.duration;
	for (;;)
	{
		if (seek_temp < 0)
		{
			seek_temp = 0;
		}
		int ret = avformat_seek_file(FormatCtx, StreamIdx, seek_temp, seek_temp, seek_temp, AVSEEK_FLAG_BACKWARD);
		if (ret < 0)
		{
			return false;
		}
		if (Packet.data)
		{
			av_packet_unref(&Packet);
		}
		if (!ReadPacket())
		{
			return false;
		}
		if (seek_target < Packet.dts)
		{
			seek_temp -= Packet.duration;
			continue;
		}
		pkt_temp = Packet;
		do
		{
			audio_buf_samples = audio_decode_frame();
			if (audio_buf_samples < 0)
			{
				return false;
			}
		} while ((int64_t)samplepos > audio_frame_next_pts);
		audio_buf_index = ((int64_t)samplepos - frame->pts);
		if (audio_buf_index < 0)
		{
			audio_buf_index = 0;
		}
		return true;
	}
	return false;
}

void FFMPEGWaveDecoder::Clear()
{
	if (Packet.data)
	{
		av_packet_unref(&Packet);
	}
	if (frame)
	{
		av_frame_free(&frame);
		frame = nullptr;
	}
	if (CodecCtx)
	{
		avcodec_free_context(&CodecCtx);
		CodecCtx = nullptr;
	}
	if (FormatCtx)
	{
		av_free(FormatCtx->pb->buffer);
		av_free(FormatCtx->pb);
		avformat_close_input(&FormatCtx);
		FormatCtx = nullptr;
	}
	if (InputStream)
	{
        delete InputStream;
		InputStream = nullptr;
	}
}

bool FFMPEGWaveDecoder::Open(const ttstr &url)
{
	Clear();
    InputStream = TVPCreateBinaryStreamForRead(url, TJS_W(""));
	if (InputStream == nullptr)
	{
		return false;
	}
	int          bufSize = 32 * 1024;
	AVIOContext *pIOCtx  = avio_alloc_context((unsigned char *)av_malloc(bufSize + AVPROBE_PADDING_SIZE), bufSize, 0, InputStream, AVReadFunc, 0, AVSeekFunc);

	const AVInputFormat *fmt = nullptr;
    tTJSNarrowStringHolder holder(url.c_str());
	av_probe_input_buffer2(pIOCtx, &fmt, holder, nullptr, 0, 0);
	AVFormatContext *ic = FormatCtx = avformat_alloc_context();
	ic->pb                          = pIOCtx;
	if (avformat_open_input(&ic, "", fmt, nullptr) < 0)
	{
		FormatCtx = nullptr;
		return false;
	}
	if (avformat_find_stream_info(ic, nullptr) < 0)
	{
		return false;
	}

	if (ic->pb)
	{
		ic->pb->eof_reached = 0;
	}

	const AVCodec* codec;
	StreamIdx = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);

	if (StreamIdx < 0 || StreamIdx == AVERROR_STREAM_NOT_FOUND)
	{
		return false;
	}

	AVCodecContext *avctx = CodecCtx = avcodec_alloc_context3(codec);
	if (avctx == nullptr)
	{
		return false;
	}

	AVStream *stream = FormatCtx->streams[StreamIdx];

	if (avcodec_parameters_to_context(avctx, stream->codecpar) < 0)
	{
		return false;
	}

	avctx->workaround_bugs   = 1;
	avctx->error_concealment = 3;

	if (avcodec_open2(avctx, avcodec_find_decoder(avctx->codec_id), nullptr) < 0)
	{
		return false;
	}

	memset(&TSSFormat, 0, sizeof(TSSFormat));

	TSSFormat.SamplesPerSec = avctx->sample_rate;
	TSSFormat.Channels      = avctx->ch_layout.nb_channels;
	TSSFormat.Seekable =
		(FormatCtx->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK)) != (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK);
	AVFmt = avctx->sample_fmt;
    switch (AVFmt)
	{
		case AV_SAMPLE_FMT_S16P:
		case AV_SAMPLE_FMT_S16:
			TSSFormat.BitsPerSample = 16;
			break;
		case AV_SAMPLE_FMT_FLTP:
		case AV_SAMPLE_FMT_FLT:
			TSSFormat.BitsPerSample = 32;
			// TSSFormat.BitsPerSample += 0x10000; // to identify as float
            TSSFormat.IsFloat = true;
			break;
		case AV_SAMPLE_FMT_S32P:
		case AV_SAMPLE_FMT_S32:
			TSSFormat.BitsPerSample = 32;
			break;
		default:
			return false;
	}
	IsPlanar = false;
	if (AVFmt == AV_SAMPLE_FMT_S16P ||
		AVFmt == AV_SAMPLE_FMT_FLTP ||
		AVFmt == AV_SAMPLE_FMT_S32P)
	{
		IsPlanar = true;
	}
	AudioStream                = stream;
	TSSFormat.TotalTime      = av_q2d(AudioStream->time_base) * AudioStream->duration * 1000;
	TSSFormat.TotalSamples = av_q2d(AudioStream->time_base) * AudioStream->duration * TSSFormat.SamplesPerSec;
    TSSFormat.BytesPerSample = TSSFormat.BitsPerSample / 8;

	audio_buf_index       = 0;
	audio_buf_samples     = 0;
	audio_frame_next_pts  = 0;
	pkt_temp.stream_index = -1;

	return true;
}

int FFMPEGWaveDecoder::audio_decode_frame()
{
	AVCodecContext *dec      = CodecCtx;
	for (;;)
	{
		while (pkt_temp.stream_index != -1)
		{
			if (!frame)
			{
				frame = av_frame_alloc();
			}
			else
			{
				av_frame_unref(frame);
			}

			int got_frame;
			//  SUGGESTION
			//  Now that avcodec_decode_audio4 is deprecated and replaced
			//  by 2 calls (receive frame and send packet), this could be optimized
			//  into separate routines or separate threads.
			//  Also now that it always consumes a whole buffer some code
			//  in the caller may be able to be optimized.
			int len1 = 0;
			if (len1 == 0)
			{
				len1 = avcodec_send_packet(dec, &pkt_temp);
			}
			if (len1 == AVERROR(EAGAIN))
			{
			    len1 = 0;
			}
			len1 = avcodec_receive_frame(dec, frame);
			got_frame = (len1 == 0) ? 1 : 0;
			if (len1 == AVERROR(EAGAIN))
			{
				len1 = 0;
			}
			if (len1 == 0)
			{
				len1 = pkt_temp.size;
			}
			if (len1 < 0)
			{
				pkt_temp.size = 0;
				break;
			}
			pkt_temp.dts = pkt_temp.pts = AV_NOPTS_VALUE;
			pkt_temp.data += len1;
			pkt_temp.size -= len1;
			if ((pkt_temp.data && pkt_temp.size <= 0) || (!pkt_temp.data && !got_frame))
			{
				pkt_temp.stream_index = -1;
			}

			if (!got_frame)
			{
				continue;
			}

			AVRational tb = {1, frame->sample_rate};

			if (frame->pts != AV_NOPTS_VALUE)
			{
				frame->pts = av_rescale_q(frame->pts, dec->time_base, tb);
			}
			else if (audio_frame_next_pts != AV_NOPTS_VALUE)
			{
				AVRational a = {1, (int)TSSFormat.SamplesPerSec};
				frame->pts   = av_rescale_q(audio_frame_next_pts, a, tb);
			}

			if (frame->pts != AV_NOPTS_VALUE)
				audio_frame_next_pts = frame->pts + frame->nb_samples;

			return frame->nb_samples;
		}

		if (Packet.data)
		{
			av_packet_unref(&Packet);
		}

		pkt_temp.stream_index = -1;

		if (!ReadPacket())
		{
			return -1;
		}

		pkt_temp = Packet;
	}
	return -1;
}

bool FFMPEGWaveDecoder::ReadPacket()
{
	for (;;)
	{
		int ret = av_read_frame(FormatCtx, &Packet);
		if (ret < 0)
		{
			return false;
		}
		if (Packet.stream_index == StreamIdx)
		{
			stream_start_time = AudioStream->start_time;
			return true;
		}
		av_packet_unref(&Packet);
	}
	return false;
}

class FFMPEGDecoderCreator : public tTVPWaveDecoderCreator
{
public:
    tTVPWaveDecoder* Create(const ttstr& storagename, const ttstr& extension) {
        FFMPEGWaveDecoder* decoder = new FFMPEGWaveDecoder();
        if (!decoder->Open(storagename)) {
            delete decoder;
            decoder = nullptr;
        }
        return decoder;
    }
};

FFMPEGDecoderCreator ffWaveDecoderCreator;
//---------------------------------------------------------------------------
void TVPRegisterFFMPEGWaveDecoderCreator()
{
	TVPRegisterWaveDecoderCreator(&ffWaveDecoderCreator);
}

#endif