/**************************************************************************/
/*  ffmpeg_video_stream.h                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             EIRTeam.FFmpeg                             */
/*                         https://ph.eirteam.moe                         */
/**************************************************************************/
/* Copyright (c) 2023-present Álex Román (EIRTeam) & contributors.        */
/*                                                                        */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#ifndef FFMPEG_VIDEO_STREAM_H
#define FFMPEG_VIDEO_STREAM_H

#include "video_decoder.h"
#include "zero_copy_gl.h"

#ifdef GDEXTENSION

// Headers for building as GDExtension plug-in.
#include <godot_cpp/classes/texture2drd.hpp>
#include <godot_cpp/classes/video_stream.hpp>
#include <godot_cpp/classes/video_stream_playback.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/templates/list.hpp>
#include <godot_cpp/templates/vector.hpp>

using namespace godot;

#else

#include "core/object/ref_counted.h"
#include "scene/resources/atlas_texture.h"
#include "scene/resources/texture_rd.h"
#include "scene/resources/video_stream.h"

#endif

class YUVGPUConverter : public RefCounted {
	RID shader;
	Ref<Image> yuv_plane_images[4];
	RID yuv_plane_textures[4];
	RID yuv_planes_uniform_sets[4];
	RID pipeline;
	Ref<Texture2DRD> out_texture;
	RID out_uniform_set;
	Vector2i frame_size;

	struct PushConstant {
		uint32_t use_alpha;
		uint8_t padding[11];
	} push_constant;

private:
	void _ensure_pipeline();
	Error _ensure_plane_textures();
	Error _ensure_output_texture();
	RID _create_uniform_set(const RID &p_texture_rd_rid, int p_shader_set);
	void _upload_plane_images();
	void _clear_texture_internal();
	void _convert_internal();

public:
	void set_plane_image(int p_plane_idx, Ref<Image> p_image);
	Vector2i get_frame_size() const;
	void set_frame_size(const Vector2i &p_frame_size);
	void convert();
	Ref<Texture2D> get_output_texture() const;
	void clear_output_texture();

	YUVGPUConverter();
	~YUVGPUConverter();
};

// We have to use this function redirection system for GDExtension because the naming conventions
// for the functions we are supposed to override are different there
#include "gdextension_build/func_redirect.h"
class FFmpegVideoStreamPlayback : public VideoStreamPlayback {
	GDCLASS(FFmpegVideoStreamPlayback, VideoStreamPlayback);

	const int LENIENCE_BEFORE_SEEK = 2500;
	double playback_position = 0.0f;
	// Playback speed multiplier applied to the update delta (1.0 = normal).
	double playback_speed = 1.0;

	Ref<VideoDecoder> decoder;
	List<Ref<DecodedFrame>> available_frames;
	List<Ref<DecodedAudioFrame>> available_audio_frames;
	Ref<DecodedFrame> last_frame;
#ifndef FFMPEG_MT_GPU_UPLOAD
	Ref<ImageTexture> last_frame_texture;
#endif
	Ref<Image> last_frame_image;
	Ref<ImageTexture> texture;
	Ref<Texture2DRD> yuv_texture;
	// GL Compatibility path (no RenderingDevice): the raw YUV420P planes are
	// uploaded into plain R8 ImageTextures (0: Y full res, 1: U, 2: V half res)
	// so a consumer material shader can do the YUV->RGB conversion on the GPU.
	Ref<ImageTexture> yuv_gl_textures[3];
	bool use_gl_yuv = false;
	// Zero-copy route (Linux GL Compatibility + DRM hardware decoder): the
	// decoder's DMABuf frames are imported as two GL textures (Y R8, UV RG8)
	// exposed as RIDs; no download, no upload. Falls back to the plane upload
	// route above if an import fails.
	bool use_zero_copy = false;
	bool zero_copy_failed = false;
	ZeroCopyGL::Importer zero_copy;
	// Frames whose DMABuf the GPU may still be reading (the two most recently
	// displayed frames); returned to the decoder only after that window.
	Ref<DecodedFrame> retired_frames[2];
	bool looping = false;
	bool buffering = false;
	int frames_processed = 0;
	// Every decoded frame taken from the queue (shown or skipped over while
	// catching up). Divided by wall time this is the rate the decoder actually
	// sustains for this stream - the input for the Strain meter in Godot.
	int64_t frames_consumed = 0;
	void seek_into_sync();
	double get_current_frame_time();
	bool check_next_frame_valid(Ref<DecodedFrame> p_decoded_frame);
	bool check_next_audio_frame_valid(Ref<DecodedAudioFrame> p_decoded_frame);
	bool paused = false;
	bool playing = false;
	bool just_seeked = false;

	Ref<YUVGPUConverter> yuv_converter;

private:
	bool is_paused_internal() const;
	void update_internal(double p_delta);
	bool is_playing_internal() const;
	void set_paused_internal(bool p_paused);
	void play_internal();
	void stop_internal();
	void seek_internal(double p_time);
	double get_length_internal() const;
	Ref<Texture2D> get_texture_internal() const;
	double get_playback_position_internal() const;
	int get_mix_rate_internal() const;
	int get_channels_internal() const;

protected:
	void clear();
	static void _bind_methods();

public:
	Error load(Ref<FileAccess> p_file_access);

	// GL Compatibility YUV plane access (valid when has_yuv_textures() is true).
	bool has_yuv_textures() const;
	Ref<Texture2D> get_yuv_texture(int p_plane_idx) const;

	// Playback speed multiplier (1.0 = normal). Scales the frame clock, so the
	// decoder is consumed faster/slower accordingly; note decode cost scales
	// with speed. Values < 0 are clamped to 0 (frozen).
	void set_playback_speed(double p_speed);
	double get_playback_speed() const;

	// True while frames come from a hardware decoder (Linux DRM/V4L2 request).
	bool is_hardware_decoding() const;

	// Zero-copy GL textures (valid when has_zerocopy_textures() is true):
	// plane 0 = Y (R8), plane 1 = interleaved UV (RG8). Bind them to a
	// ShaderMaterial with RenderingServer.material_set_param(material_rid,
	// "y_tex" / "uv_tex", rid).
	bool has_zerocopy_textures() const;
	RID get_zerocopy_texture_rid(int p_plane_idx) const;
	bool is_zero_copy() const;

	// Strain-meter inputs: cumulative decoded frames consumed, the stream's
	// nominal frame rate, the frame size and how far the clock currently trails
	// the decoder (ms, 0 when the decoder keeps up).
	int64_t get_frames_consumed() const { return frames_consumed; }
	double get_stream_fps() const;
	Vector2i get_video_size() const;
	double get_decoder_lag_ms() const;

	STREAM_FUNC_REDIRECT_0_CONST(bool, is_paused);
	STREAM_FUNC_REDIRECT_1(void, update, double, p_delta);
	STREAM_FUNC_REDIRECT_0_CONST(bool, is_playing);
	STREAM_FUNC_REDIRECT_1(void, set_paused, bool, p_paused);
	STREAM_FUNC_REDIRECT_0(void, play);
	STREAM_FUNC_REDIRECT_0(void, stop);
	STREAM_FUNC_REDIRECT_1(void, seek, double, p_time);
	STREAM_FUNC_REDIRECT_0_CONST(double, get_length);
	STREAM_FUNC_REDIRECT_0_CONST(Ref<Texture2D>, get_texture);
	STREAM_FUNC_REDIRECT_0_CONST(double, get_playback_position);
	STREAM_FUNC_REDIRECT_0_CONST(int, get_mix_rate);
	STREAM_FUNC_REDIRECT_0_CONST(int, get_channels);
	FFmpegVideoStreamPlayback();
	~FFmpegVideoStreamPlayback();
};

class FFmpegVideoStream : public VideoStream {
	GDCLASS(FFmpegVideoStream, VideoStream);

	// Most recently instantiated playback, exposed to scripts so they can
	// reach the GL YUV plane textures (VideoStreamPlayer does not expose its
	// internal playback object).
	Ref<FFmpegVideoStreamPlayback> last_playback;

protected:
	static void _bind_methods();
	Ref<VideoStreamPlayback> instantiate_playback_internal() {
		Ref<FileAccess> fa = FileAccess::open(get_file(), FileAccess::READ);
		if (!fa.is_valid()) {
			return Ref<VideoStreamPlayback>();
		}
		Ref<FFmpegVideoStreamPlayback> pb;
		pb.instantiate();
		if (pb->load(fa) != OK) {
			return nullptr;
		}
		last_playback = pb;
		return pb;
	}

public:
	Ref<FFmpegVideoStreamPlayback> get_last_playback() const { return last_playback; }
	STREAM_FUNC_REDIRECT_0(Ref<VideoStreamPlayback>, instantiate_playback);
};

#endif // FFMPEG_VIDEO_STREAM_H
