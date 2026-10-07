/**************************************************************************/
/*  zero_copy_gl.cpp                                                      */
/**************************************************************************/
/*  Fire Geni addition to EIRTeam.FFmpeg (MIT, same terms as the fork).   */
/**************************************************************************/

#include "zero_copy_gl.h"

#ifdef GDEXTENSION
#include "gdextension_build/gdex_print.h"
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#else
#include "core/os/os.h"
#include "servers/rendering_server.h"
#endif

#if defined(__linux__) && !defined(__ANDROID__)
#define ZERO_COPY_SUPPORTED 1
#include <dlfcn.h>
#include <vector>
#include <stdint.h>
#include <string.h>
extern "C" {
#include "libavutil/hwcontext_drm.h"
}
#endif

#ifdef ZERO_COPY_SUPPORTED

// Minimal EGL / GLES declarations (avoids a build dependency on the EGL and
// GLES dev headers; values are from the Khronos registry).
typedef void *EGLDisplay;
typedef void *EGLContext;
typedef void *EGLImageKHR;
typedef int32_t EGLint;
typedef unsigned int EGLBoolean;
typedef uint64_t EGLuint64KHR;
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;

#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_IMAGE_KHR ((EGLImageKHR)0)
#define EGL_NONE 0x3038
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_EXTENSIONS 0x3055
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444

#define GL_NO_ERROR 0
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_LINEAR 0x2601
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_UNSIGNED_BYTE 0x1401
#define GL_RED 0x1903
#define GL_RG 0x8227
#define GL_R8 0x8229
#define GL_RG8 0x822B

// drm_fourcc.h equivalents
#define ZC_FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define ZC_DRM_FORMAT_R8 ZC_FOURCC('R', '8', ' ', ' ')
#define ZC_DRM_FORMAT_GR88 ZC_FOURCC('G', 'R', '8', '8')
#define ZC_DRM_FORMAT_MOD_INVALID ((((uint64_t)0xff) << 56) | ((1ULL << 56) - 1))
#define ZC_DRM_FORMAT_MOD_VENDOR_BROADCOM 0x07
#define ZC_DRM_FORMAT_MOD_BROADCOM_SAND128 4
static inline bool zc_is_sand128(uint64_t m) {
	// vendor in the top byte, SAND128 = 4 in the low 8 bits, column height in the 48 bits between
	return (m >> 56) == ZC_DRM_FORMAT_MOD_VENDOR_BROADCOM && (m & 0xff) == ZC_DRM_FORMAT_MOD_BROADCOM_SAND128;
}

typedef void *(*PFN_eglGetProcAddress)(const char *);
typedef EGLDisplay (*PFN_eglGetCurrentDisplay)(void);
typedef EGLContext (*PFN_eglGetCurrentContext)(void);
typedef const char *(*PFN_eglQueryString)(EGLDisplay, EGLint);
typedef EGLint (*PFN_eglGetError)(void);
typedef EGLImageKHR (*PFN_eglCreateImageKHR)(EGLDisplay, EGLContext, unsigned int, void *, const EGLint *);
typedef EGLBoolean (*PFN_eglDestroyImageKHR)(EGLDisplay, EGLImageKHR);
typedef EGLBoolean (*PFN_eglQueryDmaBufModifiersEXT)(EGLDisplay, EGLint, EGLint, EGLuint64KHR *, EGLBoolean *, EGLint *);
typedef void (*PFN_glEGLImageTargetTexture2DOES)(GLenum, void *);
typedef void (*PFN_glGenTextures)(GLsizei, GLuint *);
typedef void (*PFN_glDeleteTextures)(GLsizei, const GLuint *);
typedef void (*PFN_glBindTexture)(GLenum, GLuint);
typedef void (*PFN_glActiveTexture)(GLenum);
typedef void (*PFN_glTexParameteri)(GLenum, GLenum, GLint);
typedef void (*PFN_glGetIntegerv)(GLenum, GLint *);
typedef GLenum (*PFN_glGetError)(void);
typedef void (*PFN_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);

struct ZCFunctions {
	void *lib = nullptr;
	PFN_eglGetProcAddress eglGetProcAddress = nullptr;
	PFN_eglGetCurrentDisplay eglGetCurrentDisplay = nullptr;
	PFN_eglGetCurrentContext eglGetCurrentContext = nullptr;
	PFN_eglQueryString eglQueryString = nullptr;
	PFN_eglGetError eglGetError = nullptr;
	PFN_eglCreateImageKHR eglCreateImageKHR = nullptr;
	PFN_eglDestroyImageKHR eglDestroyImageKHR = nullptr;
	PFN_eglQueryDmaBufModifiersEXT eglQueryDmaBufModifiersEXT = nullptr;
	PFN_glEGLImageTargetTexture2DOES glEGLImageTargetTexture2DOES = nullptr;
	PFN_glGenTextures glGenTextures = nullptr;
	PFN_glDeleteTextures glDeleteTextures = nullptr;
	PFN_glBindTexture glBindTexture = nullptr;
	PFN_glActiveTexture glActiveTexture = nullptr;
	PFN_glTexParameteri glTexParameteri = nullptr;
	PFN_glGetIntegerv glGetIntegerv = nullptr;
	PFN_glGetError glGetError = nullptr;
	PFN_glTexImage2D glTexImage2D = nullptr;
	EGLDisplay display = EGL_NO_DISPLAY;
};

static ZCFunctions zc;
static bool zc_probed = false;
static bool zc_available = false;
static const char *zc_reason = "not probed";

template <typename T>
static T zc_proc(const char *p_name) {
	void *p = zc.eglGetProcAddress ? zc.eglGetProcAddress(p_name) : nullptr;
	if (p == nullptr && zc.lib != nullptr) {
		p = dlsym(zc.lib, p_name);
	}
	if (p == nullptr) {
		p = dlsym(RTLD_DEFAULT, p_name);
	}
	return reinterpret_cast<T>(p);
}

static bool zc_probe_once() {
	if (zc_probed) {
		return zc_available;
	}
	zc_probed = true;
	zc_available = false;

	if (OS::get_singleton()->get_environment("FIREGENI_ZEROCOPY") == "0") {
		zc_reason = "disabled by FIREGENI_ZEROCOPY=0";
		return false;
	}
	if (RenderingServer::get_singleton() == nullptr) {
		zc_reason = "no RenderingServer";
		return false;
	}
	if (RenderingServer::get_singleton()->get_rendering_device() != nullptr) {
		zc_reason = "RenderingDevice renderer (GL Compatibility only)";
		return false;
	}
	if (!RenderingServer::get_singleton()->has_method("texture_create_from_native_handle")) {
		zc_reason = "Godot too old: no texture_create_from_native_handle";
		return false;
	}
	zc.lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
	if (zc.lib == nullptr) {
		zc.lib = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
	}
	if (zc.lib == nullptr) {
		zc_reason = "libEGL not loadable";
		return false;
	}
	zc.eglGetProcAddress = (PFN_eglGetProcAddress)dlsym(zc.lib, "eglGetProcAddress");
	zc.eglGetCurrentDisplay = (PFN_eglGetCurrentDisplay)dlsym(zc.lib, "eglGetCurrentDisplay");
	zc.eglGetCurrentContext = (PFN_eglGetCurrentContext)dlsym(zc.lib, "eglGetCurrentContext");
	zc.eglQueryString = (PFN_eglQueryString)dlsym(zc.lib, "eglQueryString");
	zc.eglGetError = (PFN_eglGetError)dlsym(zc.lib, "eglGetError");
	if (!zc.eglGetProcAddress || !zc.eglGetCurrentDisplay || !zc.eglGetCurrentContext || !zc.eglQueryString) {
		zc_reason = "libEGL lacks core entry points";
		return false;
	}
	zc.display = zc.eglGetCurrentDisplay();
	if (zc.display == EGL_NO_DISPLAY || zc.eglGetCurrentContext() == EGL_NO_CONTEXT) {
		// Godot on X11 with the GLX driver, or we are not on the GL thread.
		zc_reason = "no EGL context current on this thread (GLX renderer? run Godot with --rendering-driver opengl3_es)";
		return false;
	}
	const char *exts = zc.eglQueryString(zc.display, EGL_EXTENSIONS);
	if (exts == nullptr || strstr(exts, "EGL_EXT_image_dma_buf_import") == nullptr || strstr(exts, "EGL_EXT_image_dma_buf_import_modifiers") == nullptr) {
		zc_reason = "EGL_EXT_image_dma_buf_import(_modifiers) missing";
		return false;
	}
	zc.eglCreateImageKHR = zc_proc<PFN_eglCreateImageKHR>("eglCreateImageKHR");
	zc.eglDestroyImageKHR = zc_proc<PFN_eglDestroyImageKHR>("eglDestroyImageKHR");
	zc.eglQueryDmaBufModifiersEXT = zc_proc<PFN_eglQueryDmaBufModifiersEXT>("eglQueryDmaBufModifiersEXT");
	zc.glEGLImageTargetTexture2DOES = zc_proc<PFN_glEGLImageTargetTexture2DOES>("glEGLImageTargetTexture2DOES");
	zc.glGenTextures = zc_proc<PFN_glGenTextures>("glGenTextures");
	zc.glDeleteTextures = zc_proc<PFN_glDeleteTextures>("glDeleteTextures");
	zc.glBindTexture = zc_proc<PFN_glBindTexture>("glBindTexture");
	zc.glActiveTexture = zc_proc<PFN_glActiveTexture>("glActiveTexture");
	zc.glTexParameteri = zc_proc<PFN_glTexParameteri>("glTexParameteri");
	zc.glGetIntegerv = zc_proc<PFN_glGetIntegerv>("glGetIntegerv");
	zc.glGetError = zc_proc<PFN_glGetError>("glGetError");
	zc.glTexImage2D = zc_proc<PFN_glTexImage2D>("glTexImage2D");
	if (!zc.eglCreateImageKHR || !zc.eglDestroyImageKHR || !zc.eglQueryDmaBufModifiersEXT || !zc.glEGLImageTargetTexture2DOES ||
			!zc.glGenTextures || !zc.glDeleteTextures || !zc.glBindTexture || !zc.glActiveTexture || !zc.glTexParameteri || !zc.glGetIntegerv || !zc.glGetError || !zc.glTexImage2D) {
		zc_reason = "EGL/GL entry points missing";
		return false;
	}
	// The decoder's tiled layout must be importable per plane.
	for (int f = 0; f < 2; f++) {
		EGLint fourcc = f == 0 ? (EGLint)ZC_DRM_FORMAT_R8 : (EGLint)ZC_DRM_FORMAT_GR88;
		EGLuint64KHR mods[64];
		EGLBoolean ext_only[64];
		EGLint n = 0;
		bool sand = false;
		if (zc.eglQueryDmaBufModifiersEXT(zc.display, fourcc, 64, mods, ext_only, &n)) {
			for (EGLint i = 0; i < n; i++) {
				if (zc_is_sand128(mods[i])) {
					sand = true;
				}
			}
		}
		if (!sand) {
			zc_reason = f == 0 ? "driver cannot import R8 with the SAND128 modifier" : "driver cannot import GR88 with the SAND128 modifier";
			return false;
		}
	}
	zc_available = true;
	zc_reason = "ok";
	print_line("Zero-copy video: EGL dma-buf import available (SAND128 R8/GR88)");
	return true;
}

bool ZeroCopyGL::is_available() {
	return zc_probe_once();
}

const char *ZeroCopyGL::unavailable_reason() {
	zc_probe_once();
	return zc_reason;
}

bool ZeroCopyGL::Importer::_ensure_textures(int p_width, int p_height) {
	if (textures_ready && p_width == width && p_height == height) {
		return true;
	}
	if (textures_ready) {
		// Size change mid-stream (never for our content): rebuild.
		release();
	}
	zc.glGenTextures(2, gl_tex);
	if (gl_tex[0] == 0 || gl_tex[1] == 0) {
		return false;
	}
	width = p_width;
	height = p_height;
	// Give both textures "video black" storage (Y=16, U=V=128) until the first
	// frame is imported, so an early sample does not read an incomplete texture.
	{
		GLint prev_active = GL_TEXTURE0;
		GLint prev_bound = 0;
		zc.glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
		zc.glActiveTexture(GL_TEXTURE0);
		zc.glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_bound);
		int cw = (width + 1) / 2, ch = (height + 1) / 2;
		std::vector<uint8_t> y_black;
		y_black.resize(width * height);
		memset(y_black.data(), 16, y_black.size());
		std::vector<uint8_t> uv_gray;
		uv_gray.resize(cw * ch * 2);
		memset(uv_gray.data(), 128, uv_gray.size());
		for (int i = 0; i < 2; i++) {
			zc.glBindTexture(GL_TEXTURE_2D, gl_tex[i]);
			if (i == 0) {
				zc.glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, y_black.data());
			} else {
				zc.glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, cw, ch, 0, GL_RG, GL_UNSIGNED_BYTE, uv_gray.data());
			}
			zc.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			zc.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			zc.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			zc.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		}
		zc.glBindTexture(GL_TEXTURE_2D, (GLuint)prev_bound);
		zc.glActiveTexture((GLenum)prev_active);
		while (zc.glGetError() != GL_NO_ERROR) {
		}
	}
	// Wrap the GL texture ids as Godot textures. Called through Variant so the
	// plugin also compiles against a godot-cpp without this 4.2+ binding.
	RenderingServer *rs = RenderingServer::get_singleton();
	const int tex_type_2d = 0; // RenderingServer::TEXTURE_TYPE_2D
	const int layered_type = 0; // RenderingServer::TEXTURE_LAYERED_2D_ARRAY (unused for 2D)
	Variant r0 = rs->call("texture_create_from_native_handle", tex_type_2d, (int)Image::FORMAT_R8, (int64_t)gl_tex[0], width, height, 1, 1, layered_type);
	Variant r1 = rs->call("texture_create_from_native_handle", tex_type_2d, (int)Image::FORMAT_RG8, (int64_t)gl_tex[1], (width + 1) / 2, (height + 1) / 2, 1, 1, layered_type);
	rids[0] = r0;
	rids[1] = r1;
	if (!rids[0].is_valid() || !rids[1].is_valid()) {
		print_line("Zero-copy video: texture_create_from_native_handle failed");
		release();
		return false;
	}
	textures_ready = true;
	return true;
}

void ZeroCopyGL::Importer::_destroy_images() {
	for (int i = 0; i < 2; i++) {
		if (egl_img[i] != nullptr) {
			zc.eglDestroyImageKHR(zc.display, (EGLImageKHR)egl_img[i]);
			egl_img[i] = nullptr;
		}
	}
}

bool ZeroCopyGL::Importer::bind_frame(const AVFrame *p_frame) {
	if (p_frame == nullptr || p_frame->format != AV_PIX_FMT_DRM_PRIME || p_frame->data[0] == nullptr) {
		return false;
	}
	if (!zc_probe_once()) {
		return false;
	}
	const AVDRMFrameDescriptor *desc = (const AVDRMFrameDescriptor *)p_frame->data[0];
	if (desc->nb_layers != 1 || desc->layers[0].nb_planes != 2) {
		print_line(vformat("Zero-copy video: unexpected DRM layout (%d layers, %d planes)", desc->nb_layers, desc->nb_layers > 0 ? desc->layers[0].nb_planes : 0));
		return false;
	}
	if (!_ensure_textures(p_frame->width, p_frame->height)) {
		return false;
	}

	// Create the two single-plane images first; only on success rebind, so a
	// failure leaves the previous frame on screen.
	EGLImageKHR new_img[2] = { EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR };
	for (int pl = 0; pl < 2; pl++) {
		const AVDRMPlaneDescriptor &plane = desc->layers[0].planes[pl];
		const AVDRMObjectDescriptor &obj = desc->objects[plane.object_index];
		EGLint attr[32];
		int n = 0;
		attr[n++] = EGL_WIDTH;
		attr[n++] = pl == 0 ? p_frame->width : (p_frame->width + 1) / 2;
		attr[n++] = EGL_HEIGHT;
		attr[n++] = pl == 0 ? p_frame->height : (p_frame->height + 1) / 2;
		attr[n++] = EGL_LINUX_DRM_FOURCC_EXT;
		attr[n++] = (EGLint)(pl == 0 ? ZC_DRM_FORMAT_R8 : ZC_DRM_FORMAT_GR88);
		attr[n++] = EGL_DMA_BUF_PLANE0_FD_EXT;
		attr[n++] = obj.fd;
		attr[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
		attr[n++] = (EGLint)plane.offset;
		attr[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
		attr[n++] = (EGLint)plane.pitch;
		if (obj.format_modifier != ZC_DRM_FORMAT_MOD_INVALID) {
			attr[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
			attr[n++] = (EGLint)(obj.format_modifier & 0xffffffffu);
			attr[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
			attr[n++] = (EGLint)(obj.format_modifier >> 32);
		}
		attr[n++] = EGL_NONE;
		new_img[pl] = zc.eglCreateImageKHR(zc.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attr);
		if (new_img[pl] == EGL_NO_IMAGE_KHR) {
			print_line(vformat("Zero-copy video: eglCreateImageKHR failed for plane %d (egl error 0x%x)", pl, zc.eglGetError ? zc.eglGetError() : 0));
			for (int k = 0; k < pl; k++) {
				zc.eglDestroyImageKHR(zc.display, new_img[k]);
			}
			return false;
		}
	}

	// Bind, preserving Godot's GL texture state.
	GLint prev_active = GL_TEXTURE0;
	GLint prev_bound = 0;
	zc.glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
	zc.glActiveTexture(GL_TEXTURE0);
	zc.glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_bound);
	while (zc.glGetError() != GL_NO_ERROR) {
	}
	bool ok = true;
	for (int pl = 0; pl < 2; pl++) {
		zc.glBindTexture(GL_TEXTURE_2D, gl_tex[pl]);
		zc.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, new_img[pl]);
		zc.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		zc.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		zc.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		zc.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		GLenum err = zc.glGetError();
		if (err != GL_NO_ERROR) {
			print_line(vformat("Zero-copy video: glEGLImageTargetTexture2DOES failed for plane %d (gl error 0x%x)", pl, err));
			ok = false;
		}
	}
	zc.glBindTexture(GL_TEXTURE_2D, (GLuint)prev_bound);
	zc.glActiveTexture((GLenum)prev_active);

	if (!ok) {
		for (int pl = 0; pl < 2; pl++) {
			zc.eglDestroyImageKHR(zc.display, new_img[pl]);
		}
		return false;
	}
	// The textures now reference the new storage; in-flight GPU work keeps its
	// own buffer references, so the old images can go right away.
	_destroy_images();
	egl_img[0] = new_img[0];
	egl_img[1] = new_img[1];
	return true;
}

void ZeroCopyGL::Importer::release() {
	if (!zc_probed || !zc_available) {
		return;
	}
	_destroy_images();
	for (int i = 0; i < 2; i++) {
		if (rids[i].is_valid()) {
			RenderingServer::get_singleton()->free_rid(rids[i]);
			rids[i] = RID();
		}
	}
	if (gl_tex[0] != 0 || gl_tex[1] != 0) {
		zc.glDeleteTextures(2, gl_tex);
		gl_tex[0] = gl_tex[1] = 0;
	}
	textures_ready = false;
	width = height = 0;
}

#else // !ZERO_COPY_SUPPORTED

bool ZeroCopyGL::is_available() {
	return false;
}
const char *ZeroCopyGL::unavailable_reason() {
	return "not supported on this platform";
}
bool ZeroCopyGL::Importer::_ensure_textures(int, int) {
	return false;
}
void ZeroCopyGL::Importer::_destroy_images() {}
bool ZeroCopyGL::Importer::bind_frame(const AVFrame *) {
	return false;
}
void ZeroCopyGL::Importer::release() {}

#endif
