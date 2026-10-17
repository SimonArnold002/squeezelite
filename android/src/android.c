/* 
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2015, triode1@btinternet.com
 *      Ralph Irving 2015-2025, ralph_irving@hotmail.com
 *  
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Additions (c) Paul Hermann, 2015-2025 under the same license terms
 *   -Control of Raspberry pi GPIO for amplifier power
 *   -Launch script on power status change from LMS
 */

#include "squeezelite.h"
#include <jni.h>
#include <signal.h>

extern struct outputstate output;
extern struct buffer *outputbuf;

static JavaVM *jvm = NULL;
static jclass clazz = 0;
static jobject obj = 0;

extern int PaOpenSLES_ENABLED;
extern int PaAAudio_ENABLED;
extern int LibAAudio_init();

static void sighandler(int signum) {
	slimproto_stop();

	// remove ourselves in case above does not work, second SIGINT will cause non gracefull shutdown
	signal(signum, SIG_DFL);
}

/**
 Catch SEGV, log, and exit (normally). This is to prevent Android showing errors when stopping.
 */
static void segv_handler(int sig) {
	LOG_ERROR("SEGV/ABRT!");
	exit(0);
}

static void init_jvm(JNIEnv * env, jobject jobj) {
	jvm = NULL;
	if (JNI_OK!=(*env)->GetJavaVM(env, &jvm)) {
		LOG_ERROR("Failed to get java VM");
		return;
	}
	obj = (*env)->NewGlobalRef(env, jobj);
	clazz = (*env)->NewGlobalRef(env, (*env)->GetObjectClass(env, obj));
}

#if DSD
static log_level loglevel = lERROR; // for the functions below; start() shadows it with its parameter

static void set_dsd_loglevel(log_level level) {
	loglevel = level;
}

/*
 Native DSD on FiiO players. FiiO's Android adds AudioFormat.ENCODING_DSD_32BIT; an AudioTrack
 opened with it is routed to the DAC's DSD path. The track rate is the DSD bit rate / 32, and
 the data is the DSD_U16_LE byte stream (16-bit L/R interleave, earliest bit in the MSB),
 packed by output_pa.c. Only the DSD writer thread in output_pa.c calls the dsd_track functions.
 */
static int dsd_encoding = 0;
static JNIEnv *dsd_env = NULL;
static jobject dsd_track = NULL;
static jbyteArray dsd_array = NULL;
static jint dsd_array_len = 0;
static jmethodID dsd_write_id, dsd_head_id;

static bool jni_failed(JNIEnv *env, const char *what) {
	if ((*env)->ExceptionCheck(env)) {
		(*env)->ExceptionDescribe(env);
		(*env)->ExceptionClear(env);
		LOG_ERROR("DSD AudioTrack: %s failed", what);
		return true;
	}
	return false;
}

// FiiO's ENCODING_DSD_32BIT, or 0 when this Android does not have it
static int find_dsd_encoding(JNIEnv *env) {
	jclass fmt = (*env)->FindClass(env, "android/media/AudioFormat");
	if (!fmt) {
		(*env)->ExceptionClear(env);
		return 0;
	}
	jfieldID id = (*env)->GetStaticFieldID(env, fmt, "ENCODING_DSD_32BIT", "I");
	int encoding = 0;
	if (id) {
		encoding = (*env)->GetStaticIntField(env, fmt, id);
	} else {
		(*env)->ExceptionClear(env);
	}
	(*env)->DeleteLocalRef(env, fmt);
	return encoding;
}

static jobject call_builder(JNIEnv *env, jobject builder, const char *name, const char *ret, jint arg) {
	jclass cls = (*env)->GetObjectClass(env, builder);
	char sig[96];
	snprintf(sig, sizeof(sig), "(I)%s", ret);
	jobject next = (*env)->CallObjectMethod(env, builder, (*env)->GetMethodID(env, cls, name, sig), arg);
	(*env)->DeleteLocalRef(env, cls);
	return next;
}

static jobject build(JNIEnv *env, jobject builder, const char *sig) {
	jclass cls = (*env)->GetObjectClass(env, builder);
	jobject built = (*env)->CallObjectMethod(env, builder, (*env)->GetMethodID(env, cls, "build", sig));
	(*env)->DeleteLocalRef(env, cls);
	return built;
}

bool dsd_track_open(unsigned rate, unsigned buffer_bytes, unsigned chunk_bytes) {
	JNIEnv *env;
	jclass cls;
	jobject b, ret, attrs = NULL, format = NULL, track = NULL;

	if (!jvm || !dsd_encoding) return false;
	if ((*jvm)->AttachCurrentThread(jvm, &env, NULL) != JNI_OK) {
		LOG_ERROR("DSD AudioTrack: failed to attach thread");
		return false;
	}
	dsd_env = env;

	cls = (*env)->FindClass(env, "android/media/AudioAttributes$Builder");
	b = (*env)->NewObject(env, cls, (*env)->GetMethodID(env, cls, "<init>", "()V"));
	(*env)->DeleteLocalRef(env, cls);
	if (jni_failed(env, "AudioAttributes.Builder")) goto fail;
	ret = call_builder(env, b, "setUsage", "Landroid/media/AudioAttributes$Builder;", 1); // USAGE_MEDIA
	(*env)->DeleteLocalRef(env, ret);
	ret = call_builder(env, b, "setContentType", "Landroid/media/AudioAttributes$Builder;", 2); // CONTENT_TYPE_MUSIC
	(*env)->DeleteLocalRef(env, ret);
	attrs = build(env, b, "()Landroid/media/AudioAttributes;");
	(*env)->DeleteLocalRef(env, b);
	if (jni_failed(env, "AudioAttributes")) goto fail;

	cls = (*env)->FindClass(env, "android/media/AudioFormat$Builder");
	b = (*env)->NewObject(env, cls, (*env)->GetMethodID(env, cls, "<init>", "()V"));
	(*env)->DeleteLocalRef(env, cls);
	if (jni_failed(env, "AudioFormat.Builder")) goto fail;
	ret = call_builder(env, b, "setEncoding", "Landroid/media/AudioFormat$Builder;", dsd_encoding);
	(*env)->DeleteLocalRef(env, ret);
	ret = call_builder(env, b, "setSampleRate", "Landroid/media/AudioFormat$Builder;", rate);
	(*env)->DeleteLocalRef(env, ret);
	ret = call_builder(env, b, "setChannelMask", "Landroid/media/AudioFormat$Builder;", 0xC); // CHANNEL_OUT_STEREO
	(*env)->DeleteLocalRef(env, ret);
	format = build(env, b, "()Landroid/media/AudioFormat;");
	(*env)->DeleteLocalRef(env, b);
	if (jni_failed(env, "AudioFormat")) goto fail;

	cls = (*env)->FindClass(env, "android/media/AudioTrack");
	track = (*env)->NewObject(env, cls, (*env)->GetMethodID(env, cls, "<init>",
							  "(Landroid/media/AudioAttributes;Landroid/media/AudioFormat;III)V"),
							  attrs, format, (jint)buffer_bytes, 1 /* MODE_STREAM */, 0 /* AUDIO_SESSION_ID_GENERATE */);
	if (jni_failed(env, "AudioTrack")) {
		(*env)->DeleteLocalRef(env, cls);
		goto fail;
	}
	dsd_write_id = (*env)->GetMethodID(env, cls, "write", "([BII)I");
	dsd_head_id = (*env)->GetMethodID(env, cls, "getPlaybackHeadPosition", "()I");
	if ((*env)->CallIntMethod(env, track, (*env)->GetMethodID(env, cls, "getState", "()I")) != 1) { // STATE_INITIALIZED
		(*env)->DeleteLocalRef(env, cls);
		LOG_ERROR("DSD AudioTrack: not initialised at %u", rate);
		(*env)->CallVoidMethod(env, track, (*env)->GetMethodID(env, (*env)->GetObjectClass(env, track), "release", "()V"));
		goto fail;
	}
	(*env)->CallVoidMethod(env, track, (*env)->GetMethodID(env, cls, "play", "()V"));
	(*env)->DeleteLocalRef(env, cls);
	if (jni_failed(env, "play")) goto fail;

	dsd_track = (*env)->NewGlobalRef(env, track);
	dsd_array = (*env)->NewGlobalRef(env, (*env)->NewByteArray(env, chunk_bytes));
	dsd_array_len = chunk_bytes;
	(*env)->DeleteLocalRef(env, track);
	(*env)->DeleteLocalRef(env, attrs);
	(*env)->DeleteLocalRef(env, format);
	LOG_INFO("DSD AudioTrack open: rate %u, encoding %d, buffer %u bytes", rate, dsd_encoding, buffer_bytes);
	return true;

fail:
	if (track) (*env)->DeleteLocalRef(env, track);
	if (attrs) (*env)->DeleteLocalRef(env, attrs);
	if (format) (*env)->DeleteLocalRef(env, format);
	(*jvm)->DetachCurrentThread(jvm);
	dsd_env = NULL;
	return false;
}

// blocking write of the whole buffer; returns false on a write error
bool dsd_track_write(const u8_t *buf, unsigned len) {
	JNIEnv *env = dsd_env;
	if (!env || !dsd_track || len > (unsigned)dsd_array_len) return false;
	(*env)->SetByteArrayRegion(env, dsd_array, 0, len, (const jbyte *)buf);
	unsigned done = 0;
	while (done < len) {
		jint n = (*env)->CallIntMethod(env, dsd_track, dsd_write_id, dsd_array, (jint)done, (jint)(len - done));
		if (jni_failed(env, "write")) return false;
		if (n < 0) {
			LOG_ERROR("DSD AudioTrack: write error %d", n);
			return false;
		}
		done += n;
	}
	return true;
}

// frames (at the track rate) the AudioTrack has played
unsigned dsd_track_position(void) {
	JNIEnv *env = dsd_env;
	if (!env || !dsd_track) return 0;
	jint pos = (*env)->CallIntMethod(env, dsd_track, dsd_head_id);
	if (jni_failed(env, "getPlaybackHeadPosition")) return 0;
	return (unsigned)pos;
}

void dsd_track_close(void) {
	JNIEnv *env = dsd_env;
	if (!env) return;
	if (dsd_track) {
		jclass cls = (*env)->GetObjectClass(env, dsd_track);
		(*env)->CallVoidMethod(env, dsd_track, (*env)->GetMethodID(env, cls, "stop", "()V"));
		jni_failed(env, "stop");
		(*env)->CallVoidMethod(env, dsd_track, (*env)->GetMethodID(env, cls, "release", "()V"));
		jni_failed(env, "release");
		(*env)->DeleteLocalRef(env, cls);
		(*env)->DeleteGlobalRef(env, dsd_track);
		dsd_track = NULL;
	}
	if (dsd_array) {
		(*env)->DeleteGlobalRef(env, dsd_array);
		dsd_array = NULL;
	}
	(*jvm)->DetachCurrentThread(jvm);
	dsd_env = NULL;
	LOG_INFO("DSD AudioTrack closed");
}
#endif

void send_output_state_to_app(u8_t spdif, u8_t dac) {
    if (!jvm || !obj || !clazz) {
		return;
	}
	JNIEnv *env;
	bool detached = JNI_EDETACHED == (*jvm)->GetEnv(jvm, &env, JNI_VERSION_1_6);
	if (detached) {
		if (JNI_OK!=(*jvm)->AttachCurrentThread(jvm, &env, NULL)) {
			LOG_ERROR("Failed to get attach current thread");
			return;
		}
	}
	jmethodID method = (*env)->GetMethodID(env, clazz, "outputChanged", "(II)V");
	if (method) {
		(*env)->CallVoidMethod(env, obj, method, spdif, dac);
	}
	if (detached) {
		(*jvm)->DetachCurrentThread(jvm);
	}
}

void send_volume_to_app(u32_t left, u32_t right) {
    if (!jvm || !obj || !clazz) {
		return;
	}
	JNIEnv *env;
	bool detached = JNI_EDETACHED == (*jvm)->GetEnv(jvm, &env, JNI_VERSION_1_6);
	if (detached) {
		if (JNI_OK!=(*jvm)->AttachCurrentThread(jvm, &env, NULL)) {
			LOG_ERROR("Failed to get attach current thread");
			return;
		}
	}
	jmethodID method = (*env)->GetMethodID(env, clazz, "volumeChanged", "(II)V");
	if (method) {
		(*env)->CallVoidMethod(env, obj, method, left, right);
	}
	if (detached) {
		(*jvm)->DetachCurrentThread(jvm);
	}
}

void send_playback_state_to_app(void) {
	if (!jvm || !obj || !clazz) {
		return;
	}
	// Callers do not hold the output lock
	mutex_lock(outputbuf->mutex);
	bool playing = output.state != OUTPUT_OFF && output.state != OUTPUT_STOPPED;
	mutex_unlock(outputbuf->mutex);

	JNIEnv *env;
	bool detached = JNI_EDETACHED == (*jvm)->GetEnv(jvm, &env, JNI_VERSION_1_6);
	if (detached) {
		if (JNI_OK!=(*jvm)->AttachCurrentThread(jvm, &env, NULL)) {
			LOG_ERROR("Failed to get attach current thread");
			return;
		}
	}
	jmethodID method = (*env)->GetMethodID(env, clazz, "playbackStateChanged", "(Z)V");
	if (method) {
		(*env)->CallVoidMethod(env, obj, method, (jboolean)(playing ? JNI_TRUE : JNI_FALSE));
	} else {
		// A failed GetMethodID leaves an exception pending, and the next JNI call from this
		// thread would then abort the VM. Happens if the native library is newer than the java.
		(*env)->ExceptionClear(env);
		LOG_ERROR("playbackStateChanged not found");
	}
	if (detached) {
		(*jvm)->DetachCurrentThread(jvm);
	}
}

void send_connection_state_to_app(const char *address) {
	if (!jvm || !obj || !clazz) {
		return;
	}
	JNIEnv *env;
	bool detached = JNI_EDETACHED == (*jvm)->GetEnv(jvm, &env, JNI_VERSION_1_6);
	if (detached) {
		if (JNI_OK!=(*jvm)->AttachCurrentThread(jvm, &env, NULL)) {
			LOG_ERROR("Failed to get attach current thread");
			return;
		}
	}
	jmethodID method = (*env)->GetMethodID(env, clazz, "connectionStateChanged", "(Ljava/lang/String;)V");
	if (method) {
		jstring jStr = (*env)->NewStringUTF(env, address);
		(*env)->CallVoidMethod(env, obj, method, jStr);
		(*env)->DeleteLocalRef(env, jStr);
	}
	if (detached) {
		(*jvm)->DetachCurrentThread(jvm);
	}
}

JNIEXPORT void JNICALL Java_org_lyrion_squeezelite_Library_start(JNIEnv *env, jobject jobj, jstring lms_param, jstring mac_param, jstring name_param, jint idle, jint fixed_vol, jint loglevel, jint mobile_network, jint buffer_size, jint force_opensles) {
	const char *server = (*env)->GetStringUTFChars(env, lms_param, NULL);
	const char *mac_str = (*env)->GetStringUTFChars(env, mac_param, NULL);
	char *output_device = "default";
	char *include_codecs = NULL;
	char *exclude_codecs = "";
	const char *name = (*env)->GetStringUTFChars(env, name_param, NULL);
	char *namefile = NULL;
	const char *modelname = "SqueezeLiteAndroid";
	u8_t mac[6];
	unsigned stream_buf_size = buffer_size>0 ? buffer_size*1024*1024 : STREAMBUF_SIZE;
	unsigned output_buf_size = 0; // set later
	unsigned rates[MAX_SUPPORTED_SAMPLERATES] = { 0 };
	unsigned rate_delay = 0;
	char *resample = NULL;
	char *output_params = NULL;
#if DSD
	unsigned dsd_delay = 0;
	dsd_format dsd_outfmt = PCM;
#endif
	log_level log_output = loglevel;
	log_level log_stream = loglevel;
	log_level log_decode = loglevel;
	log_level log_slimproto = loglevel;

	if (!force_opensles && LibAAudio_init()) {
		PaOpenSLES_ENABLED = 0;
		PaAAudio_ENABLED = 1;
		LOG_DEBUG("Using AAudio");
	} else {
		PaOpenSLES_ENABLED = 1;
		PaAAudio_ENABLED = 0;
		LOG_DEBUG("Using OpenSL ES");
	}

	if (mac_str) {
		int byte = 0;
		char *tmp;
		char *t = strtok(mac_str, ":");
		while (t && byte < 6) {
			mac[byte++] = (u8_t)strtoul(t, &tmp, 16);
			t = strtok(NULL, ":");
		}
	}

	init_jvm(env, jobj);
#if DSD
	// FiiO players take native DSD through an AudioTrack; elsewhere DSD is converted to PCM
	set_dsd_loglevel(loglevel);
	dsd_encoding = find_dsd_encoding(env);
	if (dsd_encoding) {
		dsd_outfmt = DSD_U16_LE;
		LOG_INFO("native DSD available (encoding %d)", dsd_encoding);
	}
#endif
	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
#if defined(SIGQUIT)
	signal(SIGQUIT, sighandler);
#endif
#if defined(SIGHUP)
	signal(SIGHUP, sighandler);
#endif
	signal(SIGSEGV, segv_handler);
	signal(SIGABRT, segv_handler);

#if USE_SSL && !LINKALL && !NO_SSLSYM
	ssl_loaded = load_ssl_symbols();
#endif

	// set the output buffer size if not specified on the command line, take account of resampling
	if (!output_buf_size) {
		output_buf_size = OUTPUTBUF_SIZE;
		if (resample) {
			unsigned scale = 8;
			if (rates[0]) {
				scale = rates[0] / 44100;
				if (scale > 8) scale = 8;
				if (scale < 1) scale = 1;
			}
			output_buf_size *= scale;
		}
	}

	stream_init(log_stream, stream_buf_size);

	output_init_pa(log_output, output_device, output_buf_size, output_params, rates, rate_delay, (unsigned int)idle, 0!=fixed_vol);

#if DSD
	dsd_init(dsd_outfmt, dsd_delay);
#endif

	decode_init(log_decode, include_codecs, exclude_codecs);

#if RESAMPLE
	if (resample) {
		process_init(resample);
	}
#endif

	slimproto(log_slimproto, !server || strlen(server)==0 ? NULL : server, mac, name, namefile, modelname, 0, mobile_network);

	decode_close();
	stream_close();

	output_close_pa();

#if USE_SSL && !LINKALL && !NO_SSLSYM
	free_ssl_symbols();
#endif

	(*env)->ReleaseStringUTFChars(env, lms_param, server);
	(*env)->ReleaseStringUTFChars(env, mac_param, mac_str);
	(*env)->ReleaseStringUTFChars(env, name_param, name);
}

JNIEXPORT void JNICALL Java_org_lyrion_squeezelite_Library_stop(JNIEnv * env, jobject jobj) {
	slimproto_stop();
}
