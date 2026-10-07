/**************************************************************************/
/*  zero_copy_gl.h                                                        */
/**************************************************************************/
/*  Fire Geni addition to EIRTeam.FFmpeg (MIT, same terms as the fork).   */
/**************************************************************************/

#ifndef ZERO_COPY_GL_H
#define ZERO_COPY_GL_H

// Zero-copy path for hardware-decoded frames on Linux / GL Compatibility:
// a DRM_PRIME frame (DMABuf, e.g. the Raspberry Pi 5 rpivid HEVC decoder's
// SAND128-tiled NV12) is imported straight into two plain GL_TEXTURE_2D
// textures (Y as R8, interleaved UV as RG8) through EGL_EXT_image_dma_buf_import,
// and those GL texture ids are wrapped into Godot textures with
// RenderingServer.texture_create_from_native_handle(). No CPU download, no
// texture upload; the consumer shader does the NV12->RGB conversion.
//
// Everything here must run on the thread that owns Godot's GL context (the
// main thread with the default single-threaded renderer; probe() verifies
// that a context is current). libEGL is loaded with dlopen so the plugin
// keeps no link-time dependency and still loads on machines without EGL.

#ifdef GDEXTENSION
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/rid.hpp>
using namespace godot;
#else
#include "core/templates/rid.h"
#endif

extern "C" {
#include "libavutil/frame.h"
}

class ZeroCopyGL {
public:
	// One-time capability probe (cached). True when: Linux, FIREGENI_ZEROCOPY != "0",
	// an EGL context is current on this thread, the dma-buf import extensions are
	// present and the driver lists a BROADCOM_SAND128 modifier for R8 and GR88.
	static bool is_available();
	static const char *unavailable_reason();

	// Per-playback importer: owns two GL textures + their Godot RIDs and the
	// currently bound EGLImages.
	class Importer {
		unsigned int gl_tex[2] = { 0, 0 };
		void *egl_img[2] = { nullptr, nullptr }; // EGLImageKHR of the frame currently bound
		RID rids[2];
		int width = 0;
		int height = 0;
		bool textures_ready = false;

		bool _ensure_textures(int p_width, int p_height);
		void _destroy_images();

	public:
		// Create the GL textures + Godot RIDs up front (video-black content) so
		// consumers can bind them before the first frame arrives.
		bool prepare(int p_width, int p_height) { return _ensure_textures(p_width, p_height); }
		// Import p_frame (AV_PIX_FMT_DRM_PRIME) into the two textures. The caller
		// must keep p_frame referenced until the GPU has finished reading it (we
		// recommend two frames of retirement). Returns false on any failure; the
		// previous binding then stays intact.
		bool bind_frame(const AVFrame *p_frame);
		RID get_rid(int p_plane) const { return p_plane >= 0 && p_plane < 2 ? rids[p_plane] : RID(); }
		bool has_textures() const { return textures_ready; }
		int get_width() const { return width; }
		int get_height() const { return height; }
		// Releases GL/EGL/Godot resources; call on the GL thread.
		void release();
		~Importer() { release(); }
	};
};

#endif // ZERO_COPY_GL_H
