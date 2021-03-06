#include <stdio.h>
#include <signal.h>

#include "player.h"

#define TEST_FILE_TFCARD "/mnt/extSdCard/clear.ts"

static int av_sync_type = AV_SYNC_AUDIO_MASTER;
GlobalContext global_context;

static void sigterm_handler(int sig) {
	av_log(NULL, AV_LOG_ERROR, "sigterm_handler : sig is %d \n", sig);
	exit(123);
}

static void ffmpeg_log_callback(void *ptr, int level, const char *fmt,
		va_list vl) {
	//__android_log_vprint(ANDROID_LOG_DEBUG, "FFmpeg", fmt, vl);
}

double get_master_clock() {
	if (av_sync_type == AV_SYNC_VIDEO_MASTER) {
		return get_video_clock();
	} else if (av_sync_type == AV_SYNC_AUDIO_MASTER) {
		return get_audio_clock();
	} else {
		return get_audio_clock();
	}
}

void* open_media(void *argv) {
	int i;
	int err = 0;
	int framecnt;
	AVFormatContext *fmt_ctx = NULL;
	AVDictionaryEntry *dict = NULL;
	AVPacket pkt;
	bool firstPacket = true;
	int video_stream_index = -1;
	int audio_stream_index = -1;
	pthread_t thread1;
	pthread_t thread2;

	// register INT/TERM signal
	signal(SIGINT, sigterm_handler); /* Interrupt (ANSI).    */
	signal(SIGTERM, sigterm_handler); /* Termination (ANSI).  */

	av_log_set_callback(ffmpeg_log_callback);

	// set log level
	av_log_set_level(AV_LOG_WARNING);

	/* register all codecs, demux and protocols */
	avfilter_register_all();
	av_register_all();
	avformat_network_init();

	fmt_ctx = avformat_alloc_context();

	err = avformat_open_input(&fmt_ctx, TEST_FILE_TFCARD, NULL, NULL);
	if (err < 0) {
		char errbuf[64];
		av_strerror(err, errbuf, 64);
		av_log(NULL, AV_LOG_ERROR, "avformat_open_input : err is %d , %s\n",
				err, errbuf);
		err = -1;
		goto failure;
	}

	if ((err = avformat_find_stream_info(fmt_ctx, NULL)) < 0) {
		av_log(NULL, AV_LOG_ERROR, "avformat_find_stream_info : err is %d \n",
				err);
		err = -1;
		goto failure;
	}

	// search video stream in all streams.
	for (i = 0; i < fmt_ctx->nb_streams; i++) {
		// because video stream only one, so found and stop.
		if (fmt_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
			video_stream_index = i;
			break;
		}
	}

	// search audio stream in all streams.
	for (i = 0; i < fmt_ctx->nb_streams; i++) {
		// we used the first audio stream
		if (fmt_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
			audio_stream_index = i;
			break;
		}
	}

	// if no video and audio, exit
	if (-1 == video_stream_index) {
		goto failure;
	}

	// if no audio, exit
	if (-1 == audio_stream_index) {
		goto failure;
	}

	// open video
	if (-1 != video_stream_index) {
		global_context.vcodec_ctx = fmt_ctx->streams[video_stream_index]->codec;
		global_context.vstream = fmt_ctx->streams[video_stream_index];
		global_context.vcodec = avcodec_find_decoder(
				global_context.vcodec_ctx->codec_id);
		if (NULL == global_context.vcodec) {
			av_log(NULL, AV_LOG_ERROR,
					"avcodec_find_decoder video failure. \n");
			goto failure;
		}

		if (avcodec_open2(global_context.vcodec_ctx, global_context.vcodec,
				NULL) < 0) {
			av_log(NULL, AV_LOG_ERROR, "avcodec_open2 failure. \n");
			goto failure;
		}

		if ((global_context.vcodec_ctx->width > 0)
				&& (global_context.vcodec_ctx->height > 0)) {

			setBuffersGeometry(global_context.vcodec_ctx->width,
					global_context.vcodec_ctx->height);
		}

		av_log(NULL, AV_LOG_ERROR, "video : width is %d, height is %d . \n",
				global_context.vcodec_ctx->width,
				global_context.vcodec_ctx->height);

		// init picture queue lock and cond
		pthread_mutex_init(&global_context.pictq_mutex, NULL);
		pthread_cond_init(&global_context.pictq_cond, NULL);

		pthread_mutex_init(&global_context.timer_mutex, NULL);
		pthread_cond_init(&global_context.timer_cond, NULL);

	}

	// open audio
	if (-1 != audio_stream_index) {
		global_context.acodec_ctx = fmt_ctx->streams[audio_stream_index]->codec;
		global_context.astream = fmt_ctx->streams[audio_stream_index];
		global_context.acodec = avcodec_find_decoder(
				global_context.acodec_ctx->codec_id);
		//av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1,
		//	&global_context.acodec, 0);
		if (NULL == global_context.acodec) {
			av_log(NULL, AV_LOG_ERROR, "avcodec_find_decoder failure. \n");
			err = -1;
			goto failure;
		}

		//av_opt_set_int(global_context.acodec_ctx, "refcounted_frames", 1, 0);
		if (avcodec_open2(global_context.acodec_ctx, global_context.acodec,
				NULL) < 0) {
			av_log(NULL, AV_LOG_ERROR, "avcodec_open2 failure. \n");
			err = -1;
			goto failure;
		}
	}

	// opensl es init
	createEngine();
	createBufferQueueAudioPlayer();

	// init frame time
	global_context.frame_timer = (double) av_gettime() / 1000000.0;
	global_context.frame_last_delay = 40e-3;

	// init video current pts
	global_context.video_current_pts_time = av_gettime();

	global_context.pictq_rindex = global_context.pictq_windex = 0;

	// init audio and video packet queue
	packet_queue_init(&global_context.video_queue);
	packet_queue_init(&global_context.audio_queue);

	if (-1 != video_stream_index) {
		pthread_create(&thread1, NULL, video_thread, NULL);
		pthread_create(&thread2, NULL, picture_thread, NULL);
	}

	// read url media data circle
	while ((av_read_frame(fmt_ctx, &pkt) == 0) && (!global_context.quit)) {
		if (pkt.stream_index == video_stream_index) {
			packet_queue_put(&global_context.video_queue, &pkt);
		} else if (pkt.stream_index == audio_stream_index) {
			packet_queue_put(&global_context.audio_queue, &pkt);
			if (firstPacket) {
				firstPacket = false;
				fireOnPlayer();
			}
		} else {
			av_free_packet(&pkt);
		}
	}

	// wait exit
	while (!global_context.quit) {
		usleep(1000);
	}

	failure:

	if (fmt_ctx) {
		avformat_close_input(&fmt_ctx);
		avformat_free_context(fmt_ctx);
	}

	avformat_network_deinit();

	return 0;
}

