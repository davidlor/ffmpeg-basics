#include "stdafx.h"

#include <iostream>
#include <thread>
#include <mutex>

#include <queue>
#include <objbase.h>
#include <condition_variable>


#define SDL_MAIN_HANDLED // must define for avoid header conflict

extern "C" {
	#include <libavformat/avformat.h>
	#include <libswscale/swscale.h>
	#include <libavcodec/avcodec.h>
	#include <libavcodec/codec.h>
	#include <libavutil/avutil.h>
	#include <libavutil/time.h>
	#include <libavutil/frame.h>
	#include <libavutil/opt.h>
	#include <libavutil/samplefmt.h>
	#include <libswresample/swresample.h>
	#include <SDL.h>
	#include <sdl_thread.h>
	#include <SDL_ttf.h>
}

#pragma comment(lib,"avcodec.lib")
#pragma comment(lib,"avdevice.lib")
#pragma comment(lib,"avfilter.lib")
#pragma comment(lib,"avformat.lib")
#pragma comment(lib,"avutil.lib")
#pragma comment(lib,"swresample.lib")
#pragma comment(lib,"swscale.lib")
#pragma comment(lib, "SDL2.lib")
#pragma comment(lib, "SDL2_ttf.lib")

inline BYTE* GetBitColor(BYTE* buffer, int x, int y, int pitch, int nbyte)
{
	return (BYTE*)(LPBYTE(buffer) + ((y*pitch) + (x*nbyte)));
}

int64_t calculateTargetBytePosition(const AVStream* stream, int64_t targetTimestamp) {
	// Calculate the target byte position based on the timestamp and bitrate
	int64_t targetBytePosition = av_rescale_q(targetTimestamp, stream->time_base, { 1, AV_TIME_BASE }) * stream->codecpar->bit_rate / 8;

	return targetBytePosition;
}

int64_t calculateTargetTimestamp(const AVRational& timeBase, double targetTimeInSeconds) {
	return av_rescale_q((int64_t)(targetTimeInSeconds * AV_TIME_BASE), av_get_time_base_q(), timeBase);

}

#define MAX_VIDEO_PIC_NUM  1  //maximum number of decoded buffer 

// structure for storing packets
typedef struct PacketQueue {
	AVPacketList* first_pkt, *last_pkt;
	int nb_packets;
	SDL_mutex* mutex;
} PacketQueue;

//synchronize both video and audio
enum {
	AV_SYNC_AUDIO_MASTER, // default choice 
	AV_SYNC_VIDEO_MASTER,
	AV_SYNC_EXTERNAL_CLOCK, // synchronize to an external clock 
};

typedef struct video_pic
{
	AVFrame frame;
	float clock;    
	float duration; 
	int frame_NUM;  
}video_pic;

void convertRGBtoYUV(int xsize, int ysize, AVFrame *rgbFrame, AVFrame *yuvFrame, SwsContext* swsContext) {
	// Perform YUV to RGB conversion
	sws_scale(swsContext, rgbFrame->data, rgbFrame->linesize, 0, rgbFrame->height, yuvFrame->data, yuvFrame->linesize);
}

void convertYUVtoRGB(int xsize, int ysize, AVFrame *yuvFrame, AVFrame *rgbFrame, SwsContext* swsContext) {
	// Perform YUV to RGB conversion
	sws_scale(swsContext, yuvFrame->data, yuvFrame->linesize, 0, yuvFrame->height, rgbFrame->data, rgbFrame->linesize);

	return;

	// the following code will slow down the frame rate
	int size = xsize*ysize;
	int size_quat = size + xsize / 2 * ysize / 2;
	int wrap = yuvFrame->linesize[0];
	int halfWidth = wrap / 2;

	for (int j = 0; j < ysize; j++)
		for (int i = 0; i < xsize; i++)
		{
			int uvIndex = halfWidth * (j / 2) + (i / 2);
			float Y = yuvFrame->data[0][j*wrap + i];
			float U = yuvFrame->data[1][uvIndex] - 128;
			float V = yuvFrame->data[2][uvIndex] - 128;

			float R = Y + 1.402 * V;
			float G = Y - 0.344 * U - 0.714 * V;
			float B = Y + 1.772 * U;


			if (R < 0) { R = 0; } if (G < 0) { G = 0; } if (B < 0) { B = 0; }
			if (R > 255) { R = 255; } if (G > 255) { G = 255; } if (B > 255) { B = 255; }

			
			BYTE* pColor = GetBitColor(rgbFrame->data[0], i, j, rgbFrame->linesize[0], 3);

			/// image processing here
			pColor[0] = BYTE(R);
			pColor[1] = BYTE(G);
			pColor[2] = BYTE(B);
		}
}

class SDLffMpeg
{
public:
	SDL_Surface* textSurface;
	bool pause;
	bool loop;
	bool quit;
	bool oneframe;
	int speed;
	int skipspeed;
	int keyframeCount;
	int64_t startTime;
	int64_t currTime;
	int frameReadCount;


	int64_t target_time_ms;
	AVStream* videoStream;
	AVStream* audioStream;

	bool bFirst;
	bool bFirstFrame;
	int64_t firstPacketTime;

	int Output(char* sFileName)
	{
		// Initialize SwsContext for conversion
		struct SwsContext *swsContextRGB = sws_getContext(frame_width, frame_height, AV_PIX_FMT_YUV420P,
			frame_width, frame_height, AV_PIX_FMT_RGB24, SWS_BICUBIC, NULL, NULL, NULL);

		struct SwsContext *swsContextYUV = sws_getContext(frame_width, frame_height, AV_PIX_FMT_RGB24,
			frame_width, frame_height, AV_PIX_FMT_YUV420P,	SWS_BILINEAR, NULL, NULL, NULL);


		// Open output video file
		//const char *outputFileName = "output.mp4";
		AVFormatContext *outputFormatContext = avformat_alloc_context();

		
		if (avformat_alloc_output_context2(&outputFormatContext, NULL, NULL, sFileName) < 0) {
			fprintf(stderr, "Failed to create output format context\n");
			avformat_free_context(outputFormatContext);
			return -1;
		}


		if (avio_open(&outputFormatContext->pb, sFileName, AVIO_FLAG_WRITE) < 0) {
			fprintf(stderr, "Failed to open output file\n");
			avformat_free_context(outputFormatContext);
			avformat_close_input(&ifmt_ctx);
			sws_freeContext(swsContextRGB);
			return -1;
		}

		// Add a video stream to the output format
		AVStream *videoStreamOut = avformat_new_stream(outputFormatContext, NULL);
		if (!videoStreamOut) {
			fprintf(stderr, "Failed to create video stream\n");
			avio_closep(&outputFormatContext->pb);
			avformat_free_context(outputFormatContext);
			avformat_close_input(&ifmt_ctx);
			sws_freeContext(swsContextRGB);
			return -1;
		}

		//const AVCodec *outputVideoCodec = avcodec_find_encoder_by_name("libx264rgb");  // Use the appropriate codec
		const AVCodec *outputVideoCodec = avcodec_find_encoder(AV_CODEC_ID_H264);
		if (!outputVideoCodec) {
			fprintf(stderr, "Failed to find video encoder\n");
			avio_closep(&outputFormatContext->pb);
			avformat_free_context(outputFormatContext);
			avformat_close_input(&ifmt_ctx);
			sws_freeContext(swsContextRGB);
			return -1;
		}

		AVCodecContext *outputVideoCodecContext = avcodec_alloc_context3(outputVideoCodec);
		if (!outputVideoCodecContext) {
			fprintf(stderr, "Failed to allocate video codec context\n");
			avio_closep(&outputFormatContext->pb);
			avformat_free_context(outputFormatContext);
			avformat_close_input(&ifmt_ctx);
			sws_freeContext(swsContextRGB);
			return -1;
		}
		
		AVCodecParameters *inputCodecParameters = ifmt_ctx->streams[0]->codecpar;
		outputVideoCodecContext->width = inputCodecParameters->width;
		outputVideoCodecContext->height = inputCodecParameters->height;
		outputVideoCodecContext->codec_type = AVMEDIA_TYPE_VIDEO;
		outputVideoCodecContext->codec_id = AV_CODEC_ID_H264;
		outputVideoCodecContext->bit_rate = inputCodecParameters->bit_rate;
//		outputVideoCodecContext->gop_size = 12;
//		outputVideoCodecContext->max_b_frames = 1;
		outputVideoCodecContext->pix_fmt = AV_PIX_FMT_YUV420P;
		outputVideoCodecContext->framerate = { 30, 1 };//video_codecContent->framerate;
		outputVideoCodecContext->time_base = { 1, 30 };//ifmt_ctx->streams[0]->time_base;
		outputVideoCodecContext->bit_rate = video_codecContent->bit_rate;
		outputVideoCodecContext->skip_frame = video_codecContent->skip_frame;

		av_channel_layout_default(&outputVideoCodecContext->ch_layout, 2);
		outputVideoCodecContext->sample_rate = video_codecContent->sample_rate;
		outputVideoCodecContext->sample_fmt = video_codecContent->sample_fmt;

		AVRational outputtimeBase = outputVideoCodecContext->time_base;// { 1, AV_TIME_BASE };

		if (avcodec_open2(outputVideoCodecContext, outputVideoCodec, NULL) < 0) {
			fprintf(stderr, "Failed to open video encoder\n");
			avcodec_free_context(&outputVideoCodecContext);
			avio_closep(&outputFormatContext->pb);
			avformat_free_context(outputFormatContext);
			avformat_close_input(&ifmt_ctx);
			sws_freeContext(swsContextRGB);
			return -1;
		}

		// Initialize the output stream
		if (avcodec_parameters_from_context(videoStreamOut->codecpar, outputVideoCodecContext) < 0) {
			fprintf(stderr, "Failed to copy codec parameters to output stream\n");
			avcodec_free_context(&outputVideoCodecContext);
			avio_closep(&outputFormatContext->pb);
			avformat_free_context(outputFormatContext);
			avformat_close_input(&ifmt_ctx);
			sws_freeContext(swsContextRGB);
			return -1;
		}
		
		// Allocate an AVFrame for YUV data
		AVFrame *yuvFrame = av_frame_alloc();
		if (!yuvFrame) {
			fprintf(stderr, "Failed to allocate YUV frame\n");
			avcodec_free_context(&outputVideoCodecContext);
			avio_closep(&outputFormatContext->pb);
			avformat_free_context(outputFormatContext);
			avformat_close_input(&ifmt_ctx);
			sws_freeContext(swsContextRGB);
			return -1;
		}
		
		yuvFrame->width = frame_width;
		yuvFrame->height = frame_height;
		yuvFrame->format = AV_PIX_FMT_YUV420P;

		// Allocate buffer for the YUV frame data
		int ret = av_frame_get_buffer(yuvFrame, 32); // Assuming 32-byte alignment, adjust as needed
		if (ret < 0) {
			fprintf(stderr, "Failed to allocate buffer for YUV frame\n");
			av_frame_free(&yuvFrame);
			avcodec_free_context(&outputVideoCodecContext);
			avio_closep(&outputFormatContext->pb);
			avformat_free_context(outputFormatContext);
			avformat_close_input(&ifmt_ctx);
			sws_freeContext(swsContextRGB);
			return -1;
		}

		// Allocate buffer for the RGB frame data
		AVFrame *rgbFrame = av_frame_alloc();
		if (!rgbFrame) {
			fprintf(stderr, "Failed to allocate RGB frame\n");
			av_frame_free(&yuvFrame);
			avcodec_free_context(&outputVideoCodecContext);
			avio_closep(&outputFormatContext->pb);
			avformat_free_context(outputFormatContext);
			avformat_close_input(&ifmt_ctx);
			sws_freeContext(swsContextRGB);
			return -1;
		}

		// Set the parameters for the RGB frame
		rgbFrame->width = frame_width;
		rgbFrame->height = frame_height;
		rgbFrame->format = AV_PIX_FMT_RGB24;

		// Allocate buffer for the RGB frame data
		ret = av_frame_get_buffer(rgbFrame, 32); // Assuming 32-byte alignment, adjust as needed
		if (ret < 0) {
			fprintf(stderr, "Failed to allocate buffer for RGB frame\n");
			av_frame_free(&yuvFrame);
			av_frame_free(&rgbFrame);
			avcodec_free_context(&outputVideoCodecContext);
			avio_closep(&outputFormatContext->pb);
			avformat_free_context(outputFormatContext);
			avformat_close_input(&ifmt_ctx);
			sws_freeContext(swsContextRGB);
			return -1;
		}

		///////////////////////////////////////////////////// audio stream
		// Add an audio stream to the output MP4 file		
			// Resample audio frame

		// Find the audio encoder for MP4
		const AVCodec *audioEncoder = avcodec_find_encoder_by_name("aac");// avcodec_find_encoder(AV_CODEC_ID_AAC);
		if (!audioEncoder) {
			fprintf(stderr, "Failed to find audio encoder for MP4\n");
			avformat_free_context(outputFormatContext);
			return -1;
		}

		AVStream *audioStreamOut = avformat_new_stream(outputFormatContext, audioEncoder);
		if (!audioStreamOut) {
			fprintf(stderr, "Failed to create output audio stream for MP4\n");
			avformat_free_context(outputFormatContext);
			return -1;
		}

		// Set audio codec parameters for the output MP4 file
		avcodec_parameters_copy(audioStreamOut->codecpar, audioStream->codecpar);
		audioStreamOut->codecpar->codec_id = audioEncoder->id;

		// Open the audio encoderaudioStream
		AVCodecContext *audioEncoderContext = avcodec_alloc_context3(audioEncoder);
		if (!audioEncoderContext) {
			fprintf(stderr, "Failed to allocate audio encoder context for MP4\n");
			avformat_free_context(outputFormatContext);
			return -1;
		}

		audioEncoderContext->codec_type = AVMEDIA_TYPE_AUDIO;
		audioEncoderContext->bit_rate = audio_codecContent->bit_rate;
		audioEncoderContext->sample_rate = audio_codecContent->sample_rate;
		audioEncoderContext->ch_layout.nb_channels = 2;
		audioEncoderContext->frame_size = audio_codecContent->frame_size;
		audioEncoderContext->sample_fmt = audio_codecContent->sample_fmt;
		audioEncoderContext->ch_layout = audio_codecContent->ch_layout; //AV_CH_LAYOUT_MONO;
		audioEncoderContext->time_base = audio_codecContent->time_base;

//		audioEncoderContext->profile = audio_codecContent->profile;
//		audioEncoderContext->strict_std_compliance = audio_codecContent->strict_std_compliance;

// Set options for the audio encoder
		AVDictionary *audioOptions = NULL;
		av_dict_set(&audioOptions, "strict", "-2", 0);  // Use experimental AAC encoding

														// Open codec

		if (avcodec_open2(audioEncoderContext, audioEncoder, &audioOptions) < 0) {
			fprintf(stderr, "Failed to open audio encoder for MP4\n");
			avcodec_free_context(&audioEncoderContext);
			avformat_free_context(outputFormatContext);
			return -1;
		}

		// Initialize the audio encoder stream
		avcodec_parameters_from_context(audioStreamOut->codecpar, audioEncoderContext);

		// Resample audio if needed (change sample rate, channels, etc.)
		SwrContext *swrContextAudio = swr_alloc();
		av_opt_set_int(swrContextAudio, "in_channel_layout", audioStream->codecpar->ch_layout.nb_channels, 0);
		av_opt_set_int(swrContextAudio, "in_sample_rate", audioStream->codecpar->sample_rate, 0);
		av_opt_set_sample_fmt(swrContextAudio, "in_sample_fmt", audio_codecContent->sample_fmt, 0);

		av_opt_set_int(swrContextAudio, "out_channel_layout", audioStreamOut->codecpar->ch_layout.nb_channels, 0);
		av_opt_set_int(swrContextAudio, "out_sample_rate", audioStreamOut->codecpar->sample_rate, 0);
		av_opt_set_sample_fmt(swrContextAudio, "out_sample_fmt", audioEncoderContext->sample_fmt, 0);



		if (swr_init(swrContextAudio) < 0) {
			fprintf(stderr, "Failed to initialize audio resampler\n");
			swr_free(&swrContextAudio);
			avcodec_free_context(&audioEncoderContext);
			avformat_free_context(outputFormatContext);
			return -1;
		}

		//          final call for header
		//////////////////////////////////////////////
		if (avformat_write_header(outputFormatContext, NULL) < 0) {
			fprintf(stderr, "Failed to write header to output file\n");
			avcodec_free_context(&outputVideoCodecContext);
			avio_closep(&outputFormatContext->pb);
			avformat_free_context(outputFormatContext);
			avformat_close_input(&ifmt_ctx);
			sws_freeContext(swsContextRGB);
			return -1;
		}

		//////////////////////////////////////////////////////// getting ready for the beginning of audio

		// Seek to the beginning of the input file
		av_seek_frame(ifmt_ctx, -1, 0, AVSEEK_FLAG_BACKWARD);

		// Main loop
		AVPacket* packet = av_packet_alloc();
		int frameCount = 0;
			
		AVFrame *decodedFrame = av_frame_alloc();
		if (!decodedFrame) {
			fprintf(stderr, "Failed to allocate decoded frame\n");
			av_packet_unref(packet);
			return -1;
		}

		AVFrame *resampledAudioFrame = av_frame_alloc();
		int outputFrameNumber = 0;
		int outputPacketNumber = 0;
		while (av_read_frame(ifmt_ctx, packet) >= 0) {
			if (packet->stream_index == videoStreamOut->index) {
				// Decode the frame

				int ret = avcodec_send_packet(video_codecContent, packet);
				// h264 mmco: unref short failure is normal warning and we can ignore it
				if (ret < 0) {
					fprintf(stderr, "Error sending a packet for decoding\n");
					av_packet_unref(packet);
					break;
				}

				while (ret >= 0)
				{
					///////////////////////////// receive //////////////////////////////////
					// allocating each time

					ret = avcodec_receive_frame(video_codecContent, decodedFrame);

					if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
					{
						fprintf(stderr, "not enough data to decode\n");
						av_frame_unref(decodedFrame);
						break;
					}
					else if (ret < 0)
					{
						fprintf(stderr, "Error during decoding\n");
						av_frame_unref(decodedFrame);
						break;
					}


					// Convert YUV to RGB
					convertYUVtoRGB(frame_width, frame_height, decodedFrame, rgbFrame, swsContextRGB);

					// Modify the image content (e.g., apply some filter)
					// For simplicity, let's invert the colors
					for (int y = 0; y < decodedFrame->height; ++y) {
						for (int x = 0; x < decodedFrame->width; ++x) {
							uint8_t *pixel = rgbFrame->data[0] + y * rgbFrame->linesize[0] + x * 3;
							pixel[0] = 255 - pixel[0];  // Red
						//	pixel[1] = 255 - pixel[1];  // Green
						//	pixel[2] = 255 - pixel[2];  // Blue
						}
					}

					convertRGBtoYUV(frame_width, frame_height, rgbFrame, yuvFrame, swsContextYUV);

					// int64_t ptsTime = av_rescale_q(video_packt.dts, player->videoStream->time_base, timeBase);
					AVRational frameRate = outputVideoCodecContext->framerate;// the frame rate of your video stream;

					// Calculate PTS based on frame number and frame rate
					yuvFrame->pts = av_rescale_q(outputFrameNumber, frameRate, outputVideoCodecContext->time_base);

					outputFrameNumber++;

					// Encode the modified frame
					if (avcodec_send_frame(outputVideoCodecContext, yuvFrame) < 0) {
						fprintf(stderr, "Failed to send frame for encoding\n");
						av_frame_free(&decodedFrame);
						av_packet_unref(packet);
						break;
					}
				}

				av_packet_unref(packet);

				while (avcodec_receive_packet(outputVideoCodecContext, packet) >= 0) {
					AVRational frameRate = outputVideoCodecContext->framerate;// the frame rate of your video stream;

																			  // Calculate PTS based on frame number and frame rate
					int64_t mypts = av_rescale_q(outputFrameNumber, frameRate, outputVideoCodecContext->time_base);

					int64_t frameTime;
					int64_t frameDuration;

					frameDuration = videoStreamOut->time_base.den / 30.0f; // i.e. 25
					frameTime = outputPacketNumber * frameDuration;
					packet->pts = frameTime / videoStreamOut->time_base.num;
					packet->duration = frameDuration;

					packet->dts = packet->pts;
					packet->stream_index = videoStreamOut->index;

					if (av_interleaved_write_frame(outputFormatContext, packet) < 0) {
						fprintf(stderr, "Failed to write packet to output file\n");
						av_packet_unref(packet);
						break;
					}
					outputPacketNumber++;
					av_packet_unref(packet);
				}				
			}
			else if (packet->stream_index == audioStreamOut->index)
			{
				ret = avcodec_send_packet(audio_codecContent, packet);
				if (ret < 0) {
					fprintf(stderr, "Error submitting the packet to the decoder\n");
					exit(1);
				}

				while (ret >= 0)
				{
					ret = avcodec_receive_frame(audio_codecContent, audio_frame);
					
					if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
						break;
					else if (ret < 0) {
						fprintf(stderr, "Error during decoding\n");
						break;
					}

					if (!resampledAudioFrame) {
						fprintf(stderr, "Failed to allocate resampled audio frame\n");
						break;
					}
					
					int nb_samples = audio_frame->nb_samples;
					resampledAudioFrame->pts = audio_frame->pts;
					resampledAudioFrame->format = audio_frame->format;
					resampledAudioFrame->sample_rate = audio_frame->sample_rate;
					resampledAudioFrame->ch_layout.nb_channels = audio_frame->ch_layout.nb_channels;
					resampledAudioFrame->nb_samples = audio_frame->nb_samples;
					resampledAudioFrame->duration = audio_frame->duration;
					resampledAudioFrame->pkt_dts = audio_frame->pkt_dts;
					resampledAudioFrame->pkt_size = audio_frame->pkt_size;
					resampledAudioFrame->pkt_pos = audio_frame->pkt_pos;
					resampledAudioFrame->linesize[0] = audio_frame->linesize[0];
		//			resampledAudioFrame->best_effort_timestamp = audio_frame->best_effort_timestamp;

					if (resampledAudioFrame->pts != AV_NOPTS_VALUE)
					{
						//下面是得到解码后的裸流数据进行处理，根据裸流数据的特征做相应的处理，如AAC解码后是PCM，h264解码后是YUV等。
						int data_size = av_get_bytes_per_sample(audio_codecContent->sample_fmt);
						if (data_size < 0) {
							// This should not occur, checking just for paranoia 
							fprintf(stderr, "Failed to calculate data size\n");
							break;
						}

						int buf_size = audioEncoderContext->frame_size *av_get_bytes_per_sample(audioEncoderContext->sample_fmt) *	audio_frame->ch_layout.nb_channels;
				
						resampledAudioFrame->nb_samples = av_rescale_rnd(
							swr_get_delay(swrContextAudio, audio_frame->sample_rate) + audio_frame->nb_samples,
							resampledAudioFrame->sample_rate,
							audio_frame->sample_rate,
							AV_ROUND_UP
						);

						if ((ret = av_frame_get_buffer(resampledAudioFrame, 0)) < 0) {

							char errbuf[AV_ERROR_MAX_STRING_SIZE];  // Buffer to store the error message
							av_strerror(ret, errbuf, sizeof(errbuf));
							fprintf(stderr, "Error: %s\n", errbuf);

							fprintf(stderr, "Failed to get buffer for resampled audio frame\n");
							av_frame_unref(audio_frame);
							break;
						}
						
						float* fIn0 = (float*)audio_frame->data[0];
						float* fIn1 = (float*)audio_frame->data[1];
						float* fOut0 = (float*)resampledAudioFrame->data[0];
						float* fOut1 = (float*)resampledAudioFrame->data[1];

						for (int i = 0; i < nb_samples; i++)
						{
							fOut0[i] = fIn0[i];
							fOut1[i] = fIn1[i];
						}

						if (avcodec_send_frame(audioEncoderContext, resampledAudioFrame) < 0) {
							fprintf(stderr, "Failed to send resampled audio frame for encoding\n");
							av_frame_unref(resampledAudioFrame);
							break;
						}
					}

					av_packet_unref(packet);	
					av_frame_unref(audio_frame);
					av_frame_unref(resampledAudioFrame);

					while (avcodec_receive_packet(audioEncoderContext, packet) >= 0) {
						AVRational frameRate = audioEncoderContext->framerate;// the frame rate of your video stream;

																				  // Calculate PTS based on frame number and frame rate
			//			int64_t mypts = av_rescale_q(outputFrameNumber, frameRate, audioEncoderContext->time_base);

						int64_t frameTime;
						int64_t frameDuration;

						frameDuration = audioStreamOut->time_base.den / 30.0f; // i.e. 25
						frameTime = outputPacketNumber * frameDuration;
						packet->pts = frameTime / audioStreamOut->time_base.num;
						packet->duration = frameDuration;

						packet->dts = packet->pts;
						packet->stream_index = audioStreamOut->index;

						if (av_interleaved_write_frame(outputFormatContext, packet) < 0) {
							fprintf(stderr, "Failed to write packet to output file\n");
							av_packet_unref(packet);
							break;
						}
						outputPacketNumber++;
						av_packet_unref(packet);
					}
				}
			}
			av_packet_unref(packet);
		}

		/*
		// Flush the encoder
		avcodec_send_frame(outputVideoCodecContext, NULL);
		while (avcodec_receive_packet(outputVideoCodecContext, packet) >= 0) {
			// Write the packet to the output file
			if (av_interleaved_write_frame(outputFormatContext, packet) < 0) {
				fprintf(stderr, "Failed to write packet to output file\n");
				av_packet_unref(packet);
				break;
			}

			av_packet_unref(packet);
		}
		*/

		// Write trailer
		av_write_trailer(outputFormatContext);

		// Clean up
		avcodec_free_context(&outputVideoCodecContext);

		av_frame_free(&resampledAudioFrame);
		av_frame_free(&yuvFrame);
		av_frame_free(&rgbFrame);
		av_frame_free(&decodedFrame);
		
		avio_closep(&outputFormatContext->pb);
		avformat_free_context(outputFormatContext);
		sws_freeContext(swsContextRGB);
		sws_freeContext(swsContextYUV);

		av_packet_free(&packet);
		return 0;
	}

	static int decode_interrupt_cb(void *userdata)
	{
		SDLffMpeg* player = static_cast<SDLffMpeg*>(userdata);
		return player->quit;
	}

	static int thread_func(void *unused)
	{
		return 0;
	}

	SDLffMpeg()
	{
		speed = 1;
		skipspeed = 100;
		currTime = 0;
		pause = false;
		loop = false;
		quit = false;
		sdlWindow = nullptr;
		sdlRender = nullptr;
		sdlTextureYUV = nullptr;

		bFirst = true;
		bFirstFrame = true;
		firstPacketTime = 0;
		target_time_ms = 0;

		ifmt_ctx = NULL;
		
		videoindex = -1;
		audioindex = -1;

		sprintf_s(in_filename, "wildlife.mp4\0");

		frame_width = 1280;
		frame_height = 720;

		pic_count = 0;


		// video decoder
		video_codec = nullptr;
		video_codecContent = nullptr;

		// audio decoder
		audio_codec = nullptr;
		audio_codecContent = nullptr;

		int ret;

		ifmt_ctx = avformat_alloc_context();
		ifmt_ctx->interrupt_callback.callback = &SDLffMpeg::decode_interrupt_cb;
		ifmt_ctx->interrupt_callback.opaque = this;

		if ((ret = avformat_open_input(&ifmt_ctx, in_filename, 0, 0)) < 0) {
			printf("Could not open input file.");
			return;
		}

		if ((ret = avformat_find_stream_info(ifmt_ctx, 0)) < 0) {
			printf("Failed to retrieve input stream information");
			return;
		}

		videoindex = -1;
		videoStream = nullptr;
		audioStream = nullptr;
		for (int i = 0; i < ifmt_ctx->nb_streams; i++) { 
			if (ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
			{
				videoindex = i;
				videoStream = ifmt_ctx->streams[i];
			}
			else if (ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
			{
				audioindex = i;
				audioStream = ifmt_ctx->streams[i];
			}
		}

		// keyframe is not the same nb_frames in videostream

		/*
		keyframeCount = 0;		
		AVPacket* my_packet;
		my_packet = av_packet_alloc();
		for (unsigned int i = 0; i < videoStream->nb_frames; i++) {

			if (av_read_frame(ifmt_ctx, my_packet) < 0) {
				break; // End of stream or error
			}

			if (my_packet->stream_index == videoindex && (my_packet->flags & AV_PKT_FLAG_KEY)) {
				keyframeCount++;
			}
			av_packet_unref(my_packet);
		}
		*/

		//////////////////////////////////////////////////////////////

		// Calculate video duration in milliseconds
		int64_t durationMillis = static_cast<int64_t>(videoStream->duration * av_q2d(videoStream->time_base) * 1000);


		// Print video information 
		std::cout << "Video duration (s): " << (videoStream->duration * av_q2d(videoStream->time_base)) << " seconds" << std::endl;
		std::cout << "Number of frames: " << videoStream->nb_frames << std::endl;

		std::cout << "audio duration (s): " << (audioStream->duration * av_q2d(audioStream->time_base)) << " seconds" << std::endl;
		std::cout << "Number of frames: " << audioStream->nb_frames << std::endl;

		printf("\nInput Video===========================\n");
		av_dump_format(ifmt_ctx, 0, in_filename, 0);  // general information
		printf("\n======================================\n");

		// finding decoder for video
		video_codec = avcodec_find_decoder(ifmt_ctx->streams[videoindex]->codecpar->codec_id);

		// allocating decoder content
		video_codecContent = avcodec_alloc_context3(nullptr);// video_codec);
		// setup decoder parameters
		avcodec_parameters_to_context(video_codecContent, ifmt_ctx->streams[videoindex]->codecpar);

		frame_width = ifmt_ctx->streams[videoindex]->codecpar->width;
		frame_height = ifmt_ctx->streams[videoindex]->codecpar->height;
		
		// associating decoder codec with its content
		if (avcodec_open2(video_codecContent, video_codec, nullptr))
		{
			printf("could not open codec!\n");
			return;
		}

		// finding decoder for audio
		audio_codec = avcodec_find_decoder(ifmt_ctx->streams[audioindex]->codecpar->codec_id);

		// allocating decoder content
		audio_codecContent = avcodec_alloc_context3(nullptr);// audio_codec);
		// setup decoder parameters
		avcodec_parameters_to_context(audio_codecContent, ifmt_ctx->streams[audioindex]->codecpar);
		
		// associating decoder codec with its content
		if (avcodec_open2(audio_codecContent, audio_codec, nullptr))
		{
			printf("could not open codec!\n");
			return;
		}

	
		pkt = av_packet_alloc();

		video_frame_rgb = av_frame_alloc();
		video_frame = av_frame_alloc();
		audio_frame = av_frame_alloc();

		video_frame_rgb->width = frame_width;
		video_frame_rgb->height = frame_height;
		video_frame_rgb->format = AV_PIX_FMT_RGB24;

		// Allocate buffer for the RGB frame data
		ret = av_frame_get_buffer(video_frame_rgb, 32);

		// initialize
		initSdl();

		video_pkt_queue.mutex = SDL_CreateMutex();
		audio_pkt_queue.mutex = SDL_CreateMutex();

		InitAudio();



		startTime = av_gettime();
	}

	~SDLffMpeg()
	{
		closeSDL();
	}

	void InitAudio()
	{
		SDL_LockMutex(audio_pkt_queue.mutex);
		SDL_ClearQueuedAudio(1);
		this->audio_pkt_queue.first_pkt = NULL;
		this->audio_pkt_queue.last_pkt = NULL;
		this->audio_pkt_queue.nb_packets = 0;
		SDL_UnlockMutex(audio_pkt_queue.mutex);

		SDL_CloseAudio();

		wanted_spec.freq = audio_codecContent->sample_rate * speed; 
		wanted_spec.format = AUDIO_F32LSB; // AUDIO_S16SYS;// AUDIO_F32LSB; // audio_codecContent->sample_fmt;  
		wanted_spec.channels = audio_codecContent->ch_layout.nb_channels; 
		wanted_spec.silence = 0;
		wanted_spec.samples = audio_codecContent->frame_size; 
		wanted_spec.callback = fill_audio_pcm2; // this callback must also set userdata as "this" SDLffMpeg
		wanted_spec.userdata = this;
	
		// open audio
		if (SDL_OpenAudio(&wanted_spec, NULL) < 0) {
			printf("can't open audio.\n");
			return;
		}

		SDL_PauseAudio(0); // 1 : mute
	}

	Uint8* audio_chunk;
	Uint32  audio_len;
	Uint8* audio_pos;

	SDL_Window* sdlWindow ;
	SDL_Renderer* sdlRender;
	SDL_Texture* sdlTextureYUV;
	SDL_Texture* sdlTextureRGB;
	SDL_Texture* sdlTextTexture;
	SwsContext *swsContext;

	TTF_Font* font;
	AVFormatContext* ifmt_ctx ;
	AVPacket* pkt;
	AVFrame* video_frame, *audio_frame, *video_frame_rgb;

	int videoindex, audioindex;
	char in_filename [128];

	int frame_width ;
	int frame_height ;

	int pic_count ; // video frame count

	const AVCodec* video_codec ;
	AVCodecContext* video_codecContent ;

	const AVCodec* audio_codec ;
	AVCodecContext* audio_codecContent ;

	PacketQueue video_pkt_queue;
	PacketQueue audio_pkt_queue;

	int av_sync_type = AV_SYNC_AUDIO_MASTER;

	int64_t audio_callback_time;
	float audio_clock;
	float video_clock;

	//SDL spect for desired audio 
	SDL_AudioSpec wanted_spec;

	//sdl constructor
	int initSdl()
	{
		bool success = true;

		if (SDL_Init(SDL_INIT_VIDEO))
		{
			printf("init sdl error:%s\n", SDL_GetError());
			success = false;
		}

		sdlWindow = SDL_CreateWindow("decode video", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, frame_width/2, frame_height/2, SDL_WINDOW_SHOWN);
		if (sdlWindow == nullptr)
		{
			printf("create window error: %s\n", SDL_GetError());
			success = false;
		}

		sdlRender = SDL_CreateRenderer(sdlWindow, -1, 0);
		if (sdlRender == nullptr)
		{
			printf("init window Render error: %s\n", SDL_GetError());
			success = false;
		}

		TTF_Init();
		font = TTF_OpenFont("test.ttf", 24);
		if (!font) {
			fprintf(stderr, "Failed to load font: %s\n", TTF_GetError());
			return -1;
		}

		SDL_Color textColor = { 255, 255, 255 };
		textSurface = TTF_RenderText_Blended(font, "Hello, SDL!", textColor);  
		sdlTextTexture = SDL_CreateTextureFromSurface(sdlRender, textSurface);
		

		sdlTextureYUV = SDL_CreateTexture(sdlRender, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, frame_width, frame_height);
		sdlTextureRGB = SDL_CreateTexture(sdlRender, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, frame_width, frame_height);


		// the conversion from YUV to RGB
		swsContext = sws_getContext(frame_width, frame_height, (AVPixelFormat)(ifmt_ctx->streams[videoindex]->codecpar->format),
									frame_width, frame_height , AV_PIX_FMT_RGB24, SWS_BICUBIC, NULL, NULL, NULL);

		
		
		
		return success;
	}

	void restart()
	{
		pause = true;
		Sleep(10);
	
		bFirst = true;
		bFirstFrame = true;
		firstPacketTime = 0;

		SDL_LockMutex(video_pkt_queue.mutex);
        SDL_LockMutex(audio_pkt_queue.mutex);
		

		SDL_ClearQueuedAudio(1);


		this->video_pkt_queue.first_pkt = NULL;
		this->video_pkt_queue.last_pkt = NULL;
		this->video_pkt_queue.nb_packets = 0;

		this->audio_pkt_queue.first_pkt = NULL;
		this->audio_pkt_queue.last_pkt = NULL;
		this->audio_pkt_queue.nb_packets = 0;

		SDL_UnlockMutex(video_pkt_queue.mutex);
		SDL_UnlockMutex(audio_pkt_queue.mutex);
		
		int64_t timestamp = av_rescale(target_time_ms, AV_TIME_BASE, 1000);  // Convert to stream time base
		av_seek_frame(ifmt_ctx, -1, timestamp, AVSEEK_FLAG_BACKWARD);
		avcodec_flush_buffers(video_codecContent);
		
		SDL_RenderClear(sdlRender);


		video_codecContent->frame_num = 0;

		startTime = av_gettime();
		pause = false;
	}

	void closeSDL()
	{
		SDL_FreeSurface(textSurface);
		TTF_CloseFont(font);
		TTF_Quit();
		
		SDL_DestroyWindow(sdlWindow);
		sdlWindow = nullptr;
		SDL_DestroyRenderer(sdlRender);
		sdlRender = nullptr;
		SDL_DestroyTexture(sdlTextureYUV);
		sdlTextureYUV = nullptr;
		SDL_DestroyTexture(sdlTextureRGB);
		sdlTextureRGB = nullptr;
		SDL_Quit();
	}


	// processing audio data
	void funnyAudio(float* f32le, int nb_samples, int channels)
	{
		for (int i = 0; i < nb_samples; i++)
		{
		//  0 channel = left, 1 channel = right
		//	f32le[i * channels] = exp(-0.001 * float(i)) * 280;// *f32le[i * channels];
			f32le[i * channels + 1] *= 0.05f;
		}
	}

	void fltp_convert_to_f32le(float* f32le, float* fltp_l, float* fltp_r, int nb_samples, int channels)
	{
		for (int i = 0; i < nb_samples; i++)
		{
			f32le[i * channels] = fltp_l[i];
			f32le[i * channels + 1] =  fltp_r[i];
		}
	}

	//queue a packet
	void put_AVPacket_into_queue(PacketQueue *q, AVPacket* packet)
	{
		SDL_LockMutex(q->mutex);
		AVPacketList* temp = nullptr;
		temp = (AVPacketList*)av_malloc(sizeof(AVPacketList));
		if (!temp)
		{
			printf("malloc a AVPacketList error\n");
			return;
		}

		temp->pkt = *packet;
		temp->next = NULL;

		if (!q->last_pkt)
			q->first_pkt = temp;
		else
			q->last_pkt->next = temp;

		q->last_pkt = temp;
		q->nb_packets++;

		SDL_UnlockMutex(q->mutex);
	}

	static void packet_queue_get(PacketQueue* q, AVPacket *pkt2)
	{
		while (true)
		{
			AVPacketList* pkt1;
			// retreive packet from queue, return pkt2 as the result
			pkt1 = q->first_pkt;
			if (pkt1)
			{
				SDL_LockMutex(q->mutex);
				q->first_pkt = pkt1->next;

				if (!q->first_pkt)
					q->last_pkt = NULL;

				q->nb_packets--;
				SDL_UnlockMutex(q->mutex);

				*pkt2 = pkt1->pkt;

				av_free(pkt1);


				break;

			}
			else
				SDL_Delay(1);

		}

		return;
	}

	//sdl callback
	static void  fill_audio_pcm2(void* udata, Uint8* stream, int len) {

		SDLffMpeg* player = static_cast<SDLffMpeg*>(udata);

		//get current time
		player->audio_callback_time = av_gettime();

		//SDL 2.0
		SDL_memset(stream, 0, len);

		if (player->audio_len == 0)
			return;
		len = (len > player->audio_len ? player->audio_len : len);

		SDL_MixAudio(stream, player->audio_pos, len, SDL_MIX_MAXVOLUME);
		player->audio_len -= len;
	}

	//int delCunt = 0;
	static int video_playFunction(void * data)
	{

		SDLffMpeg* player = static_cast<SDLffMpeg*>(data);

		AVPacket video_packt = { 0 };
		int ret;
		while (!player->quit)
		{
			if (player->pause) continue;
			packet_queue_get(&player->video_pkt_queue, &video_packt);

			////////////////////////////////// send_packet /////////////////////////////////
			// decoding
			////
	
			AVRational timeBase = { 1, AV_TIME_BASE };
			int64_t ptsTime = av_rescale_q(video_packt.dts, player->videoStream->time_base, timeBase);
			

			printf("get queue time：%.3f\n", float(ptsTime) / 1000.0f);

			//// receiveing decoded frame
			// we cannot just send packet and not doing anything, the memory will be full
			ret = avcodec_send_packet(player->video_codecContent, &video_packt);
			if (ret < 0) {
				printf("Error sending a packet for decoding\n");
				continue;
			}
			
			while ((!player->quit)&&(ret >= 0)&&(!player->pause))
			{
				///////////////////////////// receive //////////////////////////////////
				// allocating each time

				ret = avcodec_receive_frame(player->video_codecContent, player->video_frame);
				if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
				{
					fprintf(stderr, "not enough data to decode\n");
					av_frame_unref(player->video_frame);
					break;
				}
				else if (ret < 0)
				{
					fprintf(stderr, "Error during decoding\n");
					av_frame_unref(player->video_frame);
					break;
				}

				if (player->bFirst)
				{
					// Check if the PTS of the video packet is close to the target timestamp
					int64_t video_packet_pts = av_rescale_q(video_packt.dts, player->videoStream->time_base, timeBase);
					int64_t time_difference = llabs(video_packet_pts / 1000 - player->target_time_ms);


					printf("target time:%.3f, video time：%.3f\n", float(player->target_time_ms), float(video_packet_pts) / 1000.0f);

					// Adjust this threshold based on your acceptable closeness
					int64_t threshold = 50;  // Adjust as needed
					if ((time_difference > threshold) && (float(video_packet_pts) / 1000.0f < float(player->target_time_ms)))
					{
						av_frame_unref(player->video_frame);
						continue;
					}
					else
						player->bFirst = false;
				}

				//printf(" frame num %3d\n", video_codecContent->frame_number);
				fflush(stdout);

				if (player->bFirstFrame)
				{
					player->startTime = av_gettime();
					player->firstPacketTime = av_rescale_q(video_packt.dts, player->videoStream->time_base, timeBase);
					player->bFirstFrame = false;
				}


				player->video_clock = 
					av_q2d(player->video_codecContent->time_base) * player->video_codecContent->ticks_per_frame * 1000 * player->video_codecContent->frame_num;
	
				float duration = av_q2d(player->video_codecContent->time_base) * player->video_codecContent->ticks_per_frame * 1000;
				int64_t ptsTime = av_rescale_q(video_packt.dts, player->videoStream->time_base, timeBase);

				
				// Convert YUV to RGB
				convertYUVtoRGB(player->frame_width, player->frame_height, player->video_frame, player->video_frame_rgb, player->swsContext);

				// Modify the image content (for example, invert colors)
//				for (int i = 0; i < player->frame_height * player->frame_width * 3; i++) {
//					player->video_frame_rgb->data[0][i] = 255 - player->video_frame_rgb->data[0][i];
//				}

				// Update SDL texture with modified RGB frame
				SDL_UpdateTexture(player->sdlTextureRGB, NULL, player->video_frame_rgb->data[0], player->video_frame_rgb->linesize[0]);

				// or we can update sdl texture with original YUV frame
				// SDL_UpdateTexture(player->sdlTextureYUV, NULL, player->video_frame->data[0], player->video_frame->linesize[0]);

				int64_t nowTime = av_gettime() - player->startTime;	
				nowTime += player->firstPacketTime;
			
				nowTime *= player->speed;
				player->currTime = int(ptsTime/1000.0f);
				printf("frame ID：%d, play time: %.3f, frame time: %.3f ", player->video_codecContent->frame_num, float(nowTime)/1000.0f, float(ptsTime)/1000.0f);

				if (ptsTime > nowTime) {
					unsigned usec = ptsTime - nowTime;
					av_usleep(usec);
					printf("delay (ms): %.3f", float(usec) / 1000.0f);
				}
				printf("\n");

				SDL_RenderClear(player->sdlRender);

				// render image texture
				SDL_RenderCopy(player->sdlRender, player->sdlTextureRGB, NULL, nullptr);
				// or we can render yuv
				// SDL_RenderCopy(player->sdlRender, player->sdlTextureYUV, NULL, nullptr);

				// render text texture
				SDL_Rect textRect = { 50, 50, player->textSurface->w, player->textSurface->h };
				SDL_RenderCopy(player->sdlRender, player->sdlTextTexture, NULL, &textRect);

				//send buffer to the front 
				SDL_RenderPresent(player->sdlRender);

				if (player->oneframe)
				{
					player->oneframe = false;
					player->pause = true;
				}

				av_frame_unref(player->video_frame);
			}
			av_packet_unref(&video_packt);
		}
		return 0;
	}
	static int audio_playFunction(void* data)
	{
		SDLffMpeg* player = static_cast<SDLffMpeg*>(data);
		AVPacket audio_packt = { 0 };
		int ret;
		while (!player->quit)
		{
			if (player->pause) continue;

			if (player->bFirstFrame)
				continue;

			packet_queue_get(&player->audio_pkt_queue, &audio_packt);


			/////////////////////////////////// send packet
			ret = avcodec_send_packet(player->audio_codecContent, &audio_packt);
			if (ret < 0) {
				fprintf(stderr, "Error submitting the packet to the decoder\n");
				exit(1);
			}

			if (player->bFirst)	
				av_packet_unref(&audio_packt);
				

			// read all the output frames (in general there may be any number of them 
			while ((!player->quit)&&(ret >= 0))
			{
				if (player->pause) continue;

				//receiving decoded frame
				ret = avcodec_receive_frame(player->audio_codecContent, player->audio_frame);
				if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
					break;
				else if (ret < 0) {
					fprintf(stderr, "Error during decoding\n");
					break;
				}
				
				int data_size = av_get_bytes_per_sample(player->audio_codecContent->sample_fmt);
				if (data_size < 0) {
					// This should not occur, checking just for paranoia 
					fprintf(stderr, "Failed to calculate data size\n");
					break;
				}

				int pcm_buffer_size = data_size * player->audio_frame->nb_samples * player->audio_codecContent->ch_layout.nb_channels;
				uint8_t* pcm_buffer = (uint8_t*)malloc(pcm_buffer_size);
				memset(pcm_buffer, 0, pcm_buffer_size);

				//convert to two channel PCM (left and right) from AAC decoded data 
				player->fltp_convert_to_f32le((float*)pcm_buffer, (float*)player->audio_frame->data[0], (float*)player->audio_frame->data[1],
					player->audio_frame->nb_samples, player->audio_codecContent->ch_layout.nb_channels);
			
				player->funnyAudio((float*)pcm_buffer, player->audio_frame->nb_samples, player->audio_codecContent->ch_layout.nb_channels);
				
				//Set audio buffer (PCM data)
				player->audio_chunk = pcm_buffer;
				player->audio_len = pcm_buffer_size;
				player->audio_pos = player->audio_chunk;

				player->audio_clock = player->audio_frame->pts * av_q2d(player->audio_codecContent->time_base) * 1000;
			
				while (player->audio_len > 0)//Wait until finish
					SDL_Delay(1);

				free(pcm_buffer);
			}
			av_packet_unref(&audio_packt);
		}

		return 0;
	}

	// it will run till everything is loaded into the queue
	static int open_fileFunction(void* data)
	{
		SDLffMpeg* player = static_cast<SDLffMpeg*>(data);
		player->frameReadCount = 0;
		while (!player->quit)
		{
			while (av_read_frame(player->ifmt_ctx, player->pkt) >= 0)
			{
					
				// always queue
				if (player->pkt->stream_index == player->videoindex) {
					int64_t video_packet_pts = av_rescale_q(player->pkt->pts, player->ifmt_ctx->streams[player->videoindex]->time_base, { 1, AV_TIME_BASE });
					int64_t time_difference = llabs(video_packet_pts / 1000 - player->target_time_ms);


					printf("queue pkt：%.3f\n", float(video_packet_pts) / 1000.0f);

					//queue for video
					player->put_AVPacket_into_queue(&player->video_pkt_queue, player->pkt);
				}
				else if (player->pkt->stream_index == player->audioindex)
				{
					//queu for audio
					player->put_AVPacket_into_queue(&player->audio_pkt_queue, player->pkt);
				}
				else
					av_packet_unref(player->pkt); 

				player->frameReadCount++;
			}
		}

		return 0;
	}
};

int main()
{
	SDLffMpeg player;

	SDL_CreateThread(SDLffMpeg::open_fileFunction, "open_file", &player);
	SDL_CreateThread(SDLffMpeg::video_playFunction, "video_play", &player);
	SDL_CreateThread(SDLffMpeg::audio_playFunction, "audio_play", &player);
	
	SDL_PauseAudio(0); // 1 : mute
	
	SDL_Event e;
	while (player.quit == false)
	{
		while ((!player.quit)&&(SDL_PollEvent(&e) != 0))
		{
			// Handle specific event types
			switch (e.type) {
			case SDL_QUIT:
				player.quit = true;
				break;
			case SDL_KEYDOWN:
				// Handle key press
				std::cout << "Key pressed: " << SDL_GetKeyName(e.key.keysym.sym) << std::endl;
				if (!strcmp(SDL_GetKeyName(e.key.keysym.sym), "Q")) 
				{	
					player.quit = true;
				}
				else if (!strcmp(SDL_GetKeyName(e.key.keysym.sym), "Right"))
				{
					// forward only means continue to the target timestamp
					player.target_time_ms = player.currTime + player.skipspeed;
					player.bFirst = true;
					player.pause = false;
					player.bFirstFrame = true;
					player.oneframe = true;

					std::cout << "forward" << std::endl;
				}
				else if (!strcmp(SDL_GetKeyName(e.key.keysym.sym), "Left"))
				{
					player.target_time_ms =  player.currTime - player.skipspeed;

					// backward needs to av_seek_frame, so it needs to queue again
					if (player.target_time_ms < 0)
						player.target_time_ms = 0;
					player.oneframe = true;
					player.restart();

					std::cout << "backward" << std::endl;
				}
				else if (!strcmp(SDL_GetKeyName(e.key.keysym.sym), "R"))
				{
					player.target_time_ms = 0;
					player.restart();
				}
				else if (!strcmp(SDL_GetKeyName(e.key.keysym.sym), "P"))
				{
					player.bFirstFrame = true;
					player.pause = !player.pause;

					if (player.pause)
					{
						SDL_Color textColor = { 255, 255, 255 };
						const char* newText = "puase";
						SDL_FreeSurface(player.textSurface);
						player.textSurface = TTF_RenderText_Blended(player.font, newText, textColor);
						SDL_DestroyTexture(player.sdlTextTexture);
						player.sdlTextTexture = SDL_CreateTextureFromSurface(player.sdlRender, player.textSurface);
					}
					else
					{

						SDL_Color textColor = { 255, 255, 255 };
						char newText[5];
						
						switch (player.speed)
						{
						case 1: sprintf(newText, "1x\0"); break;
						case 2: sprintf(newText, "2x\0"); break;
						case 3: sprintf(newText, "3x\0"); break;
						}
						SDL_FreeSurface(player.textSurface);
						player.textSurface = TTF_RenderText_Blended(player.font, newText, textColor);
						SDL_DestroyTexture(player.sdlTextTexture);
						player.sdlTextTexture = SDL_CreateTextureFromSurface(player.sdlRender, player.textSurface);
					}
				}
				else if (!strcmp(SDL_GetKeyName(e.key.keysym.sym), "1"))
				{
					SDL_Color textColor = { 255, 255, 255 };

					if (!player.pause)
					{
						const char* newText = "1x";
						SDL_FreeSurface(player.textSurface);
						player.textSurface = TTF_RenderText_Blended(player.font, newText, textColor);
						SDL_DestroyTexture(player.sdlTextTexture);
						player.sdlTextTexture = SDL_CreateTextureFromSurface(player.sdlRender, player.textSurface);
					}

					bool bPause = player.pause;
					player.skipspeed = 100;
					player.speed = 1;

					player.pause = true;
					Sleep(100);
					player.InitAudio();

					player.target_time_ms = player.currTime;
					player.restart();
					player.pause = bPause;
				}
				else if (!strcmp(SDL_GetKeyName(e.key.keysym.sym), "2"))
				{
					SDL_Color textColor = { 255, 255, 255 };

					if (!player.pause)
					{
						const char* newText = "2x";
						SDL_FreeSurface(player.textSurface);
						player.textSurface = TTF_RenderText_Blended(player.font, newText, textColor);
						SDL_DestroyTexture(player.sdlTextTexture);
						player.sdlTextTexture = SDL_CreateTextureFromSurface(player.sdlRender, player.textSurface);
					}
					
					bool bPause = player.pause;
					player.skipspeed = 500;
					player.speed = 2;


					player.pause = true;
					Sleep(100);
					player.InitAudio();

					player.target_time_ms = player.currTime;
					player.restart();
					player.pause = bPause;
				}
				else if (!strcmp(SDL_GetKeyName(e.key.keysym.sym), "3"))
				{
					SDL_Color textColor = { 255, 255, 255 };

					if (!player.pause)
					{
						const char* newText = "3x";
						SDL_FreeSurface(player.textSurface);
						player.textSurface = TTF_RenderText_Blended(player.font, newText, textColor);
						SDL_DestroyTexture(player.sdlTextTexture);
						player.sdlTextTexture = SDL_CreateTextureFromSurface(player.sdlRender, player.textSurface);
					}

					bool bPause = player.pause;
					player.skipspeed = 1000;
					player.speed = 3;

					player.pause = true;
					Sleep(100);
					player.InitAudio();

					player.target_time_ms = player.currTime;
					player.restart();
					player.pause = bPause;
				}
				else if (!strcmp(SDL_GetKeyName(e.key.keysym.sym), "O")) // export
				{
					player.bFirstFrame = true;
					player.target_time_ms = 0;
					player.restart();
					player.pause = true;

					char *outputFileName = "output.mp4";
					player.Output(outputFileName);
				}
				break;
			}
		}
	}

	Sleep(100); // wait for the thread to stop getting packet

	SDL_DestroyMutex(player.video_pkt_queue.mutex);

	avformat_close_input(&player.ifmt_ctx);
	avcodec_free_context(&player.video_codecContent);
	player.video_codecContent = NULL;
	av_frame_free(&player.video_frame);
	av_frame_free(&player.video_frame_rgb);

	SDL_DestroyMutex(player.audio_pkt_queue.mutex);
	avcodec_free_context(&player.audio_codecContent);
	player.audio_codecContent = NULL;
	av_frame_free(&player.audio_frame);

	SDL_CloseAudio();

	av_packet_free(&player.pkt);
	return 0;
}

