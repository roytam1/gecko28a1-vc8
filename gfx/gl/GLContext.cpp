/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* vim: set ts=8 sts=4 et sw=4 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "GLContext.h"
#include "GLBlitHelper.h"
#include "GLBlitTextureImageHelper.h"

#include "gfxCrashReporterUtils.h"
#include "gfxPlatform.h"
#include "gfxUtils.h"
#include "GLContextProvider.h"
#include "GLTextureImage.h"
#include "nsPrintfCString.h"
#include "nsThreadUtils.h"
#include "prenv.h"
#include "prlink.h"
#include "ScopedGLHelpers.h"
#include "SurfaceStream.h"
#include "GfxTexturesReporter.h"
#include "TextureGarbageBin.h"
#include "gfx2DGlue.h"

#include "OGLShaderProgram.h" // for ShaderProgramType

#include "mozilla/DebugOnly.h"
#include "mozilla/Preferences.h"
#include "mozilla/SSE.h"
#if _MSC_VER == 1400
#include <emmintrin.h>
extern "C" {
  extern __m128i _mm_shuffle_epi8(__m128i a, __m128i b);
}
#else
#include <tmmintrin.h>
#endif

#ifdef XP_MACOSX
#include <CoreServices/CoreServices.h>
#include "gfxColor.h"
#endif

#if defined(MOZ_WIDGET_COCOA)
#include "nsCocoaFeatures.h"
#endif

using namespace mozilla::gfx;
using namespace mozilla::layers;

namespace mozilla {
namespace gl {

#ifdef DEBUG
unsigned GLContext::sCurrentGLContextTLS = -1;
#endif

uint32_t GLContext::sDebugMode = 0;


#define MAX_SYMBOL_LENGTH 128
#define MAX_SYMBOL_NAMES 5

// should match the order of GLExtensions, and be null-terminated.
static const char *sExtensionNames[] = {
    "GL_EXT_framebuffer_object",
    "GL_ARB_framebuffer_object",
    "GL_ARB_texture_rectangle",
    "GL_EXT_bgra",
    "GL_EXT_texture_format_BGRA8888",
    "GL_OES_depth24",
    "GL_OES_depth32",
    "GL_OES_stencil8",
    "GL_OES_texture_npot",
    "GL_ARB_depth_texture",
    "GL_OES_depth_texture",
    "GL_OES_packed_depth_stencil",
    "GL_IMG_read_format",
    "GL_EXT_read_format_bgra",
    "GL_APPLE_client_storage",
    "GL_ARB_texture_non_power_of_two",
    "GL_ARB_pixel_buffer_object",
    "GL_ARB_ES2_compatibility",
    "GL_ARB_ES3_compatibility",
    "GL_OES_texture_float",
    "GL_OES_texture_float_linear",
    "GL_ARB_texture_float",
    "GL_EXT_unpack_subimage",
    "GL_OES_standard_derivatives",
    "GL_EXT_texture_filter_anisotropic",
    "GL_EXT_texture_compression_s3tc",
    "GL_EXT_texture_compression_dxt1",
    "GL_ANGLE_texture_compression_dxt3",
    "GL_ANGLE_texture_compression_dxt5",
    "GL_AMD_compressed_ATC_texture",
    "GL_IMG_texture_compression_pvrtc",
    "GL_EXT_framebuffer_blit",
    "GL_ANGLE_framebuffer_blit",
    "GL_EXT_framebuffer_multisample",
    "GL_ANGLE_framebuffer_multisample",
    "GL_OES_rgb8_rgba8",
    "GL_ARB_robustness",
    "GL_EXT_robustness",
    "GL_ARB_sync",
    "GL_OES_EGL_image",
    "GL_OES_EGL_sync",
    "GL_OES_EGL_image_external",
    "GL_EXT_packed_depth_stencil",
    "GL_OES_element_index_uint",
    "GL_OES_vertex_array_object",
    "GL_ARB_vertex_array_object",
    "GL_APPLE_vertex_array_object",
    "GL_ARB_draw_buffers",
    "GL_EXT_draw_buffers",
    "GL_EXT_gpu_shader4",
    "GL_EXT_blend_minmax",
    "GL_ARB_draw_instanced",
    "GL_EXT_draw_instanced",
    "GL_NV_draw_instanced",
    "GL_ARB_instanced_arrays",
    "GL_NV_instanced_arrays",
    "GL_ANGLE_instanced_arrays",
    "GL_EXT_occlusion_query_boolean",
    "GL_ARB_occlusion_query2",
    "GL_EXT_transform_feedback",
    "GL_NV_transform_feedback",
    "GL_ANGLE_depth_texture",
    "GL_EXT_sRGB",
    "GL_EXT_texture_sRGB",
    "GL_ARB_framebuffer_sRGB",
    "GL_EXT_framebuffer_sRGB",
    "GL_KHR_debug",
    nullptr
};

static bool
ParseGLVersion(GLContext* gl, unsigned int* version)
{
    GLenum error = gl->fGetError();
    if (error != LOCAL_GL_NO_ERROR) {
        MOZ_ASSERT(false, "An OpenGL error has been triggered before.");
        return false;
    }

    /**
     * B2G emulator bug work around: The emulator implements OpenGL ES 2.0 on
     * OpenGL 3.2. The bug is that GetIntegerv(LOCAL_GL_{MAJOR,MINOR}_VERSION)
     * returns OpenGL 3.2 instead of generating an error.
     */
    if (!gl->IsGLES())
    {
        /**
         * OpenGL 3.1 and OpenGL ES 3.0 both introduce GL_{MAJOR,MINOR}_VERSION
         * with GetIntegerv. So we first try those constants even though we
         * might not have an OpenGL context supporting them, has this is a
         * better way than parsing GL_VERSION.
         */
        GLint majorVersion = 0;
        GLint minorVersion = 0;

        gl->fGetIntegerv(LOCAL_GL_MAJOR_VERSION, &majorVersion);
        gl->fGetIntegerv(LOCAL_GL_MINOR_VERSION, &minorVersion);

        // If it's not an OpenGL (ES) 3.0 context, we will have an error
        error = gl->fGetError();
        if (error == LOCAL_GL_NO_ERROR &&
            majorVersion > 0 &&
            minorVersion >= 0)
        {
            *version = majorVersion * 100 + minorVersion * 10;
            return true;
        }
    }

    /**
     * We were not able to use GL_{MAJOR,MINOR}_VERSION, so we parse
     * GL_VERSION.
     *
     *
     * OpenGL 2.x, 3.x, 4.x specifications:
     *  The VERSION and SHADING_LANGUAGE_VERSION strings are laid out as follows:
     *
     *    <version number><space><vendor-specific information>
     *
     *  The version number is either of the form major_number.minor_number or
     *  major_number.minor_number.release_number, where the numbers all have
     *  one or more digits.
     *
     *
     * OpenGL ES 2.0, 3.0 specifications:
     *  The VERSION string is laid out as follows:
     *
     *     "OpenGL ES N.M vendor-specific information"
     *
     *  The version number is either of the form major_number.minor_number or
     *  major_number.minor_number.release_number, where the numbers all have
     *  one or more digits.
     *
     *
     * Note:
     *  We don't care about release_number.
     */
    const char* versionString = (const char*)gl->fGetString(LOCAL_GL_VERSION);

    error = gl->fGetError();
    if (error != LOCAL_GL_NO_ERROR) {
        MOZ_ASSERT(false, "glGetString(GL_VERSION) has generated an error");
        return false;
    } else if (!versionString) {
        MOZ_ASSERT(false, "glGetString(GL_VERSION) has returned 0");
        return false;
    }

    const char kGLESVersionPrefix[] = "OpenGL ES ";
    if (strncmp(versionString, kGLESVersionPrefix, strlen(kGLESVersionPrefix)) == 0) {
        versionString += strlen(kGLESVersionPrefix);
    }

    const char* itr = versionString;
    char* end = nullptr;
    int majorVersion = (int)strtol(itr, &end, 10);

    if (!end) {
        MOZ_ASSERT(false, "Failed to parse the GL major version number.");
        return false;
    } else if (*end != '.') {
        MOZ_ASSERT(false, "Failed to parse GL's major-minor version number separator.");
        return false;
    }

    // we skip the '.' between the major and the minor version
    itr = end + 1;

    end = nullptr;

    int minorVersion = (int)strtol(itr, &end, 10);
    if (!end) {
        MOZ_ASSERT(false, "Failed to parse GL's minor version number.");
        return false;
    }

    if (majorVersion <= 0 || majorVersion >= 100) {
        MOZ_ASSERT(false, "Invalid major version.");
        return false;
    } else if (minorVersion < 0 || minorVersion >= 10) {
        MOZ_ASSERT(false, "Invalid minor version.");
        return false;
    }

    *version = (unsigned int)(majorVersion * 100 + minorVersion * 10);
    return true;
}

GLContext::GLContext(const SurfaceCaps& caps,
          GLContext* sharedContext,
          bool isOffscreen)
  : mInitialized(false),
    mIsOffscreen(isOffscreen),
    mIsGlobalSharedContext(false),
    mContextLost(false),
    mVersion(0),
    mProfile(ContextProfile::Unknown),
    mVendor(-1),
    mRenderer(-1),
    mHasRobustness(false),
#ifdef DEBUG
    mGLError(LOCAL_GL_NO_ERROR),
#endif
    mSharedContext(sharedContext),
    mFlipped(false),
    mCaps(caps),
    mScreen(nullptr),
    mLockedSurface(nullptr),
    mMaxTextureSize(0),
    mMaxCubeMapTextureSize(0),
    mMaxTextureImageSize(0),
    mMaxRenderbufferSize(0),
    mNeedsTextureSizeChecks(false),
    mWorkAroundDriverBugs(true)
{
    mOwningThread = NS_GetCurrentThread();

    mReadTextureImagePrograms[0] = 0;
    mReadTextureImagePrograms[1] = 0;
    mReadTextureImagePrograms[2] = 0;
    mReadTextureImagePrograms[3] = 0;
}

GLContext::~GLContext() {
    NS_ASSERTION(IsDestroyed(), "GLContext implementation must call MarkDestroyed in destructor!");
#ifdef DEBUG
    if (mSharedContext) {
        GLContext *tip = mSharedContext;
        while (tip->mSharedContext)
            tip = tip->mSharedContext;
        tip->SharedContextDestroyed(this);
        tip->ReportOutstandingNames();
    } else {
        ReportOutstandingNames();
    }
#endif
}

bool
GLContext::InitWithPrefix(const char *prefix, bool trygl)
{
    ScopedGfxFeatureReporter reporter("GL Context");

    if (mInitialized) {
        reporter.SetSuccessful();
        return true;
    }

    mWorkAroundDriverBugs = gfxPlatform::GetPlatform()->WorkAroundDriverBugs();

    SymLoadStruct symbols[] = {
        { (PRFuncPtr*) &mSymbols.fActiveTexture, { "ActiveTexture", "ActiveTextureARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fAttachShader, { "AttachShader", "AttachShaderARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fBindAttribLocation, { "BindAttribLocation", "BindAttribLocationARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fBindBuffer, { "BindBuffer", "BindBufferARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fBindTexture, { "BindTexture", "BindTextureARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fBlendColor, { "BlendColor", nullptr } },
        { (PRFuncPtr*) &mSymbols.fBlendEquation, { "BlendEquation", nullptr } },
        { (PRFuncPtr*) &mSymbols.fBlendEquationSeparate, { "BlendEquationSeparate", "BlendEquationSeparateEXT", nullptr } },
        { (PRFuncPtr*) &mSymbols.fBlendFunc, { "BlendFunc", nullptr } },
        { (PRFuncPtr*) &mSymbols.fBlendFuncSeparate, { "BlendFuncSeparate", "BlendFuncSeparateEXT", nullptr } },
        { (PRFuncPtr*) &mSymbols.fBufferData, { "BufferData", nullptr } },
        { (PRFuncPtr*) &mSymbols.fBufferSubData, { "BufferSubData", nullptr } },
        { (PRFuncPtr*) &mSymbols.fClear, { "Clear", nullptr } },
        { (PRFuncPtr*) &mSymbols.fClearColor, { "ClearColor", nullptr } },
        { (PRFuncPtr*) &mSymbols.fClearStencil, { "ClearStencil", nullptr } },
        { (PRFuncPtr*) &mSymbols.fColorMask, { "ColorMask", nullptr } },
        { (PRFuncPtr*) &mSymbols.fCompressedTexImage2D, {"CompressedTexImage2D", nullptr} },
        { (PRFuncPtr*) &mSymbols.fCompressedTexSubImage2D, {"CompressedTexSubImage2D", nullptr} },
        { (PRFuncPtr*) &mSymbols.fCullFace, { "CullFace", nullptr } },
        { (PRFuncPtr*) &mSymbols.fDetachShader, { "DetachShader", "DetachShaderARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fDepthFunc, { "DepthFunc", nullptr } },
        { (PRFuncPtr*) &mSymbols.fDepthMask, { "DepthMask", nullptr } },
        { (PRFuncPtr*) &mSymbols.fDisable, { "Disable", nullptr } },
        { (PRFuncPtr*) &mSymbols.fDisableVertexAttribArray, { "DisableVertexAttribArray", "DisableVertexAttribArrayARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fDrawArrays, { "DrawArrays", nullptr } },
        { (PRFuncPtr*) &mSymbols.fDrawElements, { "DrawElements", nullptr } },
        { (PRFuncPtr*) &mSymbols.fEnable, { "Enable", nullptr } },
        { (PRFuncPtr*) &mSymbols.fEnableVertexAttribArray, { "EnableVertexAttribArray", "EnableVertexAttribArrayARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fFinish, { "Finish", nullptr } },
        { (PRFuncPtr*) &mSymbols.fFlush, { "Flush", nullptr } },
        { (PRFuncPtr*) &mSymbols.fFrontFace, { "FrontFace", nullptr } },
        { (PRFuncPtr*) &mSymbols.fGetActiveAttrib, { "GetActiveAttrib", "GetActiveAttribARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fGetActiveUniform, { "GetActiveUniform", "GetActiveUniformARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fGetAttachedShaders, { "GetAttachedShaders", "GetAttachedShadersARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fGetAttribLocation, { "GetAttribLocation", "GetAttribLocationARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fGetIntegerv, { "GetIntegerv", nullptr } },
        { (PRFuncPtr*) &mSymbols.fGetFloatv, { "GetFloatv", nullptr } },
        { (PRFuncPtr*) &mSymbols.fGetBooleanv, { "GetBooleanv", nullptr } },
        { (PRFuncPtr*) &mSymbols.fGetBufferParameteriv, { "GetBufferParameteriv", "GetBufferParameterivARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fGetError, { "GetError", nullptr } },
        { (PRFuncPtr*) &mSymbols.fGetProgramiv, { "GetProgramiv", "GetProgramivARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fGetProgramInfoLog, { "GetProgramInfoLog", "GetProgramInfoLogARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fTexParameteri, { "TexParameteri", nullptr } },
        { (PRFuncPtr*) &mSymbols.fTexParameteriv, { "TexParameteriv", nullptr } },
        { (PRFuncPtr*) &mSymbols.fTexParameterf, { "TexParameterf", nullptr } },
        { (PRFuncPtr*) &mSymbols.fGetString, { "GetString", nullptr } },
        { (PRFuncPtr*) &mSymbols.fGetTexParameterfv, { "GetTexParameterfv", nullptr } },
        { (PRFuncPtr*) &mSymbols.fGetTexParameteriv, { "GetTexParameteriv", nullptr } },
        { (PRFuncPtr*) &mSymbols.fGetUniformfv, { "GetUniformfv", "GetUniformfvARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fGetUniformiv, { "GetUniformiv", "GetUniformivARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fGetUniformLocation, { "GetUniformLocation", "GetUniformLocationARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fGetVertexAttribfv, { "GetVertexAttribfv", "GetVertexAttribfvARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fGetVertexAttribiv, { "GetVertexAttribiv", "GetVertexAttribivARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fGetVertexAttribPointerv, { "GetVertexAttribPointerv", nullptr } },
        { (PRFuncPtr*) &mSymbols.fHint, { "Hint", nullptr } },
        { (PRFuncPtr*) &mSymbols.fIsBuffer, { "IsBuffer", "IsBufferARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fIsEnabled, { "IsEnabled", nullptr } },
        { (PRFuncPtr*) &mSymbols.fIsProgram, { "IsProgram", "IsProgramARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fIsShader, { "IsShader", "IsShaderARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fIsTexture, { "IsTexture", "IsTextureARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fLineWidth, { "LineWidth", nullptr } },
        { (PRFuncPtr*) &mSymbols.fLinkProgram, { "LinkProgram", "LinkProgramARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fPixelStorei, { "PixelStorei", nullptr } },
        { (PRFuncPtr*) &mSymbols.fPolygonOffset, { "PolygonOffset", nullptr } },
        { (PRFuncPtr*) &mSymbols.fReadPixels, { "ReadPixels", nullptr } },
        { (PRFuncPtr*) &mSymbols.fSampleCoverage, { "SampleCoverage", nullptr } },
        { (PRFuncPtr*) &mSymbols.fScissor, { "Scissor", nullptr } },
        { (PRFuncPtr*) &mSymbols.fStencilFunc, { "StencilFunc", nullptr } },
        { (PRFuncPtr*) &mSymbols.fStencilFuncSeparate, { "StencilFuncSeparate", "StencilFuncSeparateEXT", nullptr } },
        { (PRFuncPtr*) &mSymbols.fStencilMask, { "StencilMask", nullptr } },
        { (PRFuncPtr*) &mSymbols.fStencilMaskSeparate, { "StencilMaskSeparate", "StencilMaskSeparateEXT", nullptr } },
        { (PRFuncPtr*) &mSymbols.fStencilOp, { "StencilOp", nullptr } },
        { (PRFuncPtr*) &mSymbols.fStencilOpSeparate, { "StencilOpSeparate", "StencilOpSeparateEXT", nullptr } },
        { (PRFuncPtr*) &mSymbols.fTexImage2D, { "TexImage2D", nullptr } },
        { (PRFuncPtr*) &mSymbols.fTexSubImage2D, { "TexSubImage2D", nullptr } },
        { (PRFuncPtr*) &mSymbols.fUniform1f, { "Uniform1f", nullptr } },
        { (PRFuncPtr*) &mSymbols.fUniform1fv, { "Uniform1fv", nullptr } },
        { (PRFuncPtr*) &mSymbols.fUniform1i, { "Uniform1i", nullptr } },
        { (PRFuncPtr*) &mSymbols.fUniform1iv, { "Uniform1iv", nullptr } },
        { (PRFuncPtr*) &mSymbols.fUniform2f, { "Uniform2f", nullptr } },
        { (PRFuncPtr*) &mSymbols.fUniform2fv, { "Uniform2fv", nullptr } },
        { (PRFuncPtr*) &mSymbols.fUniform2i, { "Uniform2i", nullptr } },
        { (PRFuncPtr*) &mSymbols.fUniform2iv, { "Uniform2iv", nullptr } },
        { (PRFuncPtr*) &mSymbols.fUniform3f, { "Uniform3f", nullptr } },
        { (PRFuncPtr*) &mSymbols.fUniform3fv, { "Uniform3fv", nullptr } },
        { (PRFuncPtr*) &mSymbols.fUniform3i, { "Uniform3i", nullptr } },
        { (PRFuncPtr*) &mSymbols.fUniform3iv, { "Uniform3iv", nullptr } },
        { (PRFuncPtr*) &mSymbols.fUniform4f, { "Uniform4f", nullptr } },
        { (PRFuncPtr*) &mSymbols.fUniform4fv, { "Uniform4fv", nullptr } },
        { (PRFuncPtr*) &mSymbols.fUniform4i, { "Uniform4i", nullptr } },
        { (PRFuncPtr*) &mSymbols.fUniform4iv, { "Uniform4iv", nullptr } },
        { (PRFuncPtr*) &mSymbols.fUniformMatrix2fv, { "UniformMatrix2fv", nullptr } },
        { (PRFuncPtr*) &mSymbols.fUniformMatrix3fv, { "UniformMatrix3fv", nullptr } },
        { (PRFuncPtr*) &mSymbols.fUniformMatrix4fv, { "UniformMatrix4fv", nullptr } },
        { (PRFuncPtr*) &mSymbols.fUseProgram, { "UseProgram", nullptr } },
        { (PRFuncPtr*) &mSymbols.fValidateProgram, { "ValidateProgram", nullptr } },
        { (PRFuncPtr*) &mSymbols.fVertexAttribPointer, { "VertexAttribPointer", nullptr } },
        { (PRFuncPtr*) &mSymbols.fVertexAttrib1f, { "VertexAttrib1f", nullptr } },
        { (PRFuncPtr*) &mSymbols.fVertexAttrib2f, { "VertexAttrib2f", nullptr } },
        { (PRFuncPtr*) &mSymbols.fVertexAttrib3f, { "VertexAttrib3f", nullptr } },
        { (PRFuncPtr*) &mSymbols.fVertexAttrib4f, { "VertexAttrib4f", nullptr } },
        { (PRFuncPtr*) &mSymbols.fVertexAttrib1fv, { "VertexAttrib1fv", nullptr } },
        { (PRFuncPtr*) &mSymbols.fVertexAttrib2fv, { "VertexAttrib2fv", nullptr } },
        { (PRFuncPtr*) &mSymbols.fVertexAttrib3fv, { "VertexAttrib3fv", nullptr } },
        { (PRFuncPtr*) &mSymbols.fVertexAttrib4fv, { "VertexAttrib4fv", nullptr } },
        { (PRFuncPtr*) &mSymbols.fViewport, { "Viewport", nullptr } },
        { (PRFuncPtr*) &mSymbols.fCompileShader, { "CompileShader", nullptr } },
        { (PRFuncPtr*) &mSymbols.fCopyTexImage2D, { "CopyTexImage2D", nullptr } },
        { (PRFuncPtr*) &mSymbols.fCopyTexSubImage2D, { "CopyTexSubImage2D", nullptr } },
        { (PRFuncPtr*) &mSymbols.fGetShaderiv, { "GetShaderiv", nullptr } },
        { (PRFuncPtr*) &mSymbols.fGetShaderInfoLog, { "GetShaderInfoLog", nullptr } },
        { (PRFuncPtr*) &mSymbols.fGetShaderSource, { "GetShaderSource", nullptr } },
        { (PRFuncPtr*) &mSymbols.fShaderSource, { "ShaderSource", nullptr } },
        { (PRFuncPtr*) &mSymbols.fVertexAttribPointer, { "VertexAttribPointer", nullptr } },
        { (PRFuncPtr*) &mSymbols.fBindFramebuffer, { "BindFramebuffer", "BindFramebufferEXT", nullptr } },
        { (PRFuncPtr*) &mSymbols.fBindRenderbuffer, { "BindRenderbuffer", "BindRenderbufferEXT", nullptr } },
        { (PRFuncPtr*) &mSymbols.fCheckFramebufferStatus, { "CheckFramebufferStatus", "CheckFramebufferStatusEXT", nullptr } },
        { (PRFuncPtr*) &mSymbols.fFramebufferRenderbuffer, { "FramebufferRenderbuffer", "FramebufferRenderbufferEXT", nullptr } },
        { (PRFuncPtr*) &mSymbols.fFramebufferTexture2D, { "FramebufferTexture2D", "FramebufferTexture2DEXT", nullptr } },
        { (PRFuncPtr*) &mSymbols.fGenerateMipmap, { "GenerateMipmap", "GenerateMipmapEXT", nullptr } },
        { (PRFuncPtr*) &mSymbols.fGetFramebufferAttachmentParameteriv, { "GetFramebufferAttachmentParameteriv", "GetFramebufferAttachmentParameterivEXT", nullptr } },
        { (PRFuncPtr*) &mSymbols.fGetRenderbufferParameteriv, { "GetRenderbufferParameteriv", "GetRenderbufferParameterivEXT", nullptr } },
        { (PRFuncPtr*) &mSymbols.fIsFramebuffer, { "IsFramebuffer", "IsFramebufferEXT", nullptr } },
        { (PRFuncPtr*) &mSymbols.fIsRenderbuffer, { "IsRenderbuffer", "IsRenderbufferEXT", nullptr } },
        { (PRFuncPtr*) &mSymbols.fRenderbufferStorage, { "RenderbufferStorage", "RenderbufferStorageEXT", nullptr } },

        { (PRFuncPtr*) &mSymbols.fGenBuffers, { "GenBuffers", "GenBuffersARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fGenTextures, { "GenTextures", nullptr } },
        { (PRFuncPtr*) &mSymbols.fCreateProgram, { "CreateProgram", "CreateProgramARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fCreateShader, { "CreateShader", "CreateShaderARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fGenFramebuffers, { "GenFramebuffers", "GenFramebuffersEXT", nullptr } },
        { (PRFuncPtr*) &mSymbols.fGenRenderbuffers, { "GenRenderbuffers", "GenRenderbuffersEXT", nullptr } },

        { (PRFuncPtr*) &mSymbols.fDeleteBuffers, { "DeleteBuffers", "DeleteBuffersARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fDeleteTextures, { "DeleteTextures", "DeleteTexturesARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fDeleteProgram, { "DeleteProgram", "DeleteProgramARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fDeleteShader, { "DeleteShader", "DeleteShaderARB", nullptr } },
        { (PRFuncPtr*) &mSymbols.fDeleteFramebuffers, { "DeleteFramebuffers", "DeleteFramebuffersEXT", nullptr } },
        { (PRFuncPtr*) &mSymbols.fDeleteRenderbuffers, { "DeleteRenderbuffers", "DeleteRenderbuffersEXT", nullptr } },

        { nullptr, { nullptr } },

    };

    mInitialized = LoadSymbols(&symbols[0], trygl, prefix);
    MakeCurrent();
    if (mInitialized) {
        unsigned int version = 0;

        bool parseSuccess = ParseGLVersion(this, &version);

        if (version >= mVersion) {
            mVersion = version;
        } else if (parseSuccess) {
            NS_WARNING("Parsed version less than expected.");
            mInitialized = false;
        }
    }

    // Load OpenGL ES 2.0 symbols, or desktop if we aren't using ES 2.
    if (mInitialized) {
        if (IsGLES2()) {
            SymLoadStruct symbols_ES2[] = {
                { (PRFuncPtr*) &mSymbols.fGetShaderPrecisionFormat, { "GetShaderPrecisionFormat", nullptr } },
                { (PRFuncPtr*) &mSymbols.fClearDepthf, { "ClearDepthf", nullptr } },
                { (PRFuncPtr*) &mSymbols.fDepthRangef, { "DepthRangef", nullptr } },
                { nullptr, { nullptr } },
            };

            if (!LoadSymbols(&symbols_ES2[0], trygl, prefix)) {
                NS_ERROR("OpenGL ES 2.0 supported, but symbols could not be loaded.");
                mInitialized = false;
            }
        } else {
            SymLoadStruct symbols_desktop[] = {
                { (PRFuncPtr*) &mSymbols.fClearDepth, { "ClearDepth", nullptr } },
                { (PRFuncPtr*) &mSymbols.fDepthRange, { "DepthRange", nullptr } },
                { (PRFuncPtr*) &mSymbols.fReadBuffer, { "ReadBuffer", nullptr } },
                { (PRFuncPtr*) &mSymbols.fMapBuffer, { "MapBuffer", nullptr } },
                { (PRFuncPtr*) &mSymbols.fUnmapBuffer, { "UnmapBuffer", nullptr } },
                { (PRFuncPtr*) &mSymbols.fPointParameterf, { "PointParameterf", nullptr } },
                { (PRFuncPtr*) &mSymbols.fDrawBuffer, { "DrawBuffer", nullptr } },
                { nullptr, { nullptr } },
            };

            if (!LoadSymbols(&symbols_desktop[0], trygl, prefix)) {
                NS_ERROR("Desktop symbols failed to load.");
                mInitialized = false;
            }
        }
    }

    const char *glVendorString = nullptr;
    const char *glRendererString = nullptr;

    if (mInitialized) {
        // The order of these strings must match up with the order of the enum
        // defined in GLContext.h for vendor IDs
        glVendorString = (const char *)fGetString(LOCAL_GL_VENDOR);
        if (!glVendorString)
            mInitialized = false;

        const char *vendorMatchStrings[VendorOther] = {
                "Intel",
                "NVIDIA",
                "ATI",
                "Qualcomm",
                "Imagination",
                "nouveau"
        };

        mVendor = VendorOther;
        for (int i = 0; i < VendorOther; ++i) {
            if (DoesStringMatch(glVendorString, vendorMatchStrings[i])) {
                mVendor = i;
                break;
            }
        }

        // The order of these strings must match up with the order of the enum
        // defined in GLContext.h for renderer IDs
        glRendererString = (const char *)fGetString(LOCAL_GL_RENDERER);
        if (!glRendererString)
            mInitialized = false;

        const char *rendererMatchStrings[RendererOther] = {
                "Adreno 200",
                "Adreno 205",
                "Adreno (TM) 205",
                "Adreno (TM) 320",
                "PowerVR SGX 530",
                "PowerVR SGX 540",
                "NVIDIA Tegra",
                "Android Emulator"
        };

        mRenderer = RendererOther;
        for (int i = 0; i < RendererOther; ++i) {
            if (DoesStringMatch(glRendererString, rendererMatchStrings[i])) {
                mRenderer = i;
                break;
            }
        }
    }


#ifdef DEBUG
    if (PR_GetEnv("MOZ_GL_DEBUG"))
        sDebugMode |= DebugEnabled;

    // enables extra verbose output, informing of the start and finish of every GL call.
    // useful e.g. to record information to investigate graphics system crashes/lockups
    if (PR_GetEnv("MOZ_GL_DEBUG_VERBOSE"))
        sDebugMode |= DebugTrace;

    // aborts on GL error. Can be useful to debug quicker code that is known not to generate any GL error in principle.
    if (PR_GetEnv("MOZ_GL_DEBUG_ABORT_ON_ERROR"))
        sDebugMode |= DebugAbortOnError;
#endif

    if (mInitialized) {
#ifdef DEBUG
        static bool firstRun = true;
        if (firstRun && DebugMode()) {
            const char *vendors[VendorOther] = {
                "Intel",
                "NVIDIA",
                "ATI",
                "Qualcomm"
            };

            MOZ_ASSERT(glVendorString);
            if (mVendor < VendorOther) {
                printf_stderr("OpenGL vendor ('%s') recognized as: %s\n",
                              glVendorString, vendors[mVendor]);
            } else {
                printf_stderr("OpenGL vendor ('%s') unrecognized\n", glVendorString);
            }
        }
        firstRun = false;
#endif

        InitExtensions();
        InitFeatures();

        // Disable extensions with partial or incorrect support.
        if (WorkAroundDriverBugs()) {
            if (Renderer() == RendererAdrenoTM320) {
                MarkUnsupported(GLFeature::standard_derivatives);
            }

#ifdef XP_MACOSX
            // The Mac Nvidia driver, for versions up to and including 10.8, don't seem
            // to properly support this.  See 814839
            // this has been fixed in Mac OS X 10.9. See 907946
            if (Vendor() == gl::GLContext::VendorNVIDIA &&
                !nsCocoaFeatures::OnMavericksOrLater())
            {
                MarkUnsupported(GLFeature::depth_texture);
            }
#endif
            // ANGLE's divisor support is busted. (see bug 916816)
            if (IsANGLE()) {
                MarkUnsupported(GLFeature::instanced_arrays);
            }
        }

        NS_ASSERTION(!IsExtensionSupported(GLContext::ARB_pixel_buffer_object) ||
                     (mSymbols.fMapBuffer && mSymbols.fUnmapBuffer),
                     "ARB_pixel_buffer_object supported without glMapBuffer/UnmapBuffer being available!");

        if (SupportsRobustness()) {
            mHasRobustness = false;

            if (IsExtensionSupported(ARB_robustness)) {
                SymLoadStruct robustnessSymbols[] = {
                    { (PRFuncPtr*) &mSymbols.fGetGraphicsResetStatus, { "GetGraphicsResetStatusARB", nullptr } },
                    { nullptr, { nullptr } },
                };

                if (!LoadSymbols(&robustnessSymbols[0], trygl, prefix)) {
                    NS_ERROR("GL supports ARB_robustness without supplying GetGraphicsResetStatusARB.");

                    mSymbols.fGetGraphicsResetStatus = nullptr;
                } else {
                    mHasRobustness = true;
                }
            }
            if (!IsExtensionSupported(ARB_robustness) &&
                IsExtensionSupported(EXT_robustness)) {
                SymLoadStruct robustnessSymbols[] = {
                    { (PRFuncPtr*) &mSymbols.fGetGraphicsResetStatus, { "GetGraphicsResetStatusEXT", nullptr } },
                    { nullptr, { nullptr } },
                };

                if (!LoadSymbols(&robustnessSymbols[0], trygl, prefix)) {
                    NS_ERROR("GL supports EXT_robustness without supplying GetGraphicsResetStatusEXT.");

                    mSymbols.fGetGraphicsResetStatus = nullptr;
                } else {
                    mHasRobustness = true;
                }
            }

            if (!mHasRobustness) {
                MarkUnsupported(GLFeature::robustness);
            }
        }

        // Check for aux symbols based on extensions
        if (IsSupported(GLFeature::framebuffer_blit))
        {
            SymLoadStruct auxSymbols[] = {
                {
                    (PRFuncPtr*) &mSymbols.fBlitFramebuffer,
                    {
                        "BlitFramebuffer",
                        "BlitFramebufferEXT",
                        "BlitFramebufferANGLE",
                        nullptr
                    }
                },
                { nullptr, { nullptr } },
            };
            if (!LoadSymbols(&auxSymbols[0], trygl, prefix)) {
                NS_ERROR("GL supports framebuffer_blit without supplying glBlitFramebuffer");

                MarkUnsupported(GLFeature::framebuffer_blit);
                mSymbols.fBlitFramebuffer = nullptr;
            }
        }

        if (IsSupported(GLFeature::framebuffer_multisample))
        {
            SymLoadStruct auxSymbols[] = {
                {
                    (PRFuncPtr*) &mSymbols.fRenderbufferStorageMultisample,
                    {
                        "RenderbufferStorageMultisample",
                        "RenderbufferStorageMultisampleEXT",
                        "RenderbufferStorageMultisampleANGLE",
                        nullptr
                    }
                },
                { nullptr, { nullptr } },
            };
            if (!LoadSymbols(&auxSymbols[0], trygl, prefix)) {
                NS_ERROR("GL supports framebuffer_multisample without supplying glRenderbufferStorageMultisample");

                MarkUnsupported(GLFeature::framebuffer_multisample);
                mSymbols.fRenderbufferStorageMultisample = nullptr;
            }
        }

        if (IsExtensionSupported(ARB_sync)) {
            SymLoadStruct syncSymbols[] = {
                { (PRFuncPtr*) &mSymbols.fFenceSync,      { "FenceSync",      nullptr } },
                { (PRFuncPtr*) &mSymbols.fIsSync,         { "IsSync",         nullptr } },
                { (PRFuncPtr*) &mSymbols.fDeleteSync,     { "DeleteSync",     nullptr } },
                { (PRFuncPtr*) &mSymbols.fClientWaitSync, { "ClientWaitSync", nullptr } },
                { (PRFuncPtr*) &mSymbols.fWaitSync,       { "WaitSync",       nullptr } },
                { (PRFuncPtr*) &mSymbols.fGetInteger64v,  { "GetInteger64v",  nullptr } },
                { (PRFuncPtr*) &mSymbols.fGetSynciv,      { "GetSynciv",      nullptr } },
                { nullptr, { nullptr } },
            };

            if (!LoadSymbols(&syncSymbols[0], trygl, prefix)) {
                NS_ERROR("GL supports ARB_sync without supplying its functions.");

                MarkExtensionUnsupported(ARB_sync);
                mSymbols.fFenceSync = nullptr;
                mSymbols.fIsSync = nullptr;
                mSymbols.fDeleteSync = nullptr;
                mSymbols.fClientWaitSync = nullptr;
                mSymbols.fWaitSync = nullptr;
                mSymbols.fGetInteger64v = nullptr;
                mSymbols.fGetSynciv = nullptr;
            }
        }

        if (IsExtensionSupported(OES_EGL_image)) {
            SymLoadStruct imageSymbols[] = {
                { (PRFuncPtr*) &mSymbols.fEGLImageTargetTexture2D, { "EGLImageTargetTexture2DOES", nullptr } },
                { (PRFuncPtr*) &mSymbols.fEGLImageTargetRenderbufferStorage, { "EGLImageTargetRenderbufferStorageOES", nullptr } },
                { nullptr, { nullptr } },
            };

            if (!LoadSymbols(&imageSymbols[0], trygl, prefix)) {
                NS_ERROR("GL supports OES_EGL_image without supplying its functions.");

                MarkExtensionUnsupported(OES_EGL_image);
                mSymbols.fEGLImageTargetTexture2D = nullptr;
                mSymbols.fEGLImageTargetRenderbufferStorage = nullptr;
            }
        }

        if (IsExtensionSupported(ARB_vertex_array_object) ||
            IsExtensionSupported(OES_vertex_array_object)) {
            SymLoadStruct vaoSymbols[] = {
                { (PRFuncPtr*) &mSymbols.fIsVertexArray, { "IsVertexArray", "IsVertexArrayOES", nullptr } },
                { (PRFuncPtr*) &mSymbols.fGenVertexArrays, { "GenVertexArrays", "GenVertexArraysOES", nullptr } },
                { (PRFuncPtr*) &mSymbols.fBindVertexArray, { "BindVertexArray", "BindVertexArrayOES", nullptr } },
                { (PRFuncPtr*) &mSymbols.fDeleteVertexArrays, { "DeleteVertexArrays", "DeleteVertexArraysOES", nullptr } },
                { nullptr, { nullptr } },
            };

            if (!LoadSymbols(&vaoSymbols[0], trygl, prefix)) {
                NS_ERROR("GL supports Vertex Array Object without supplying its functions.");

                MarkUnsupported(GLFeature::vertex_array_object);
                mSymbols.fIsVertexArray = nullptr;
                mSymbols.fGenVertexArrays = nullptr;
                mSymbols.fBindVertexArray = nullptr;
                mSymbols.fDeleteVertexArrays = nullptr;
            }
        }
        else if (IsExtensionSupported(APPLE_vertex_array_object)) {
            /*
             * separate call to LoadSymbols with APPLE_vertex_array_object to work around
             * a driver bug : the IsVertexArray symbol (without suffix) can be present but unusable.
             */
            SymLoadStruct vaoSymbols[] = {
                { (PRFuncPtr*) &mSymbols.fIsVertexArray, { "IsVertexArrayAPPLE", nullptr } },
                { (PRFuncPtr*) &mSymbols.fGenVertexArrays, { "GenVertexArraysAPPLE", nullptr } },
                { (PRFuncPtr*) &mSymbols.fBindVertexArray, { "BindVertexArrayAPPLE", nullptr } },
                { (PRFuncPtr*) &mSymbols.fDeleteVertexArrays, { "DeleteVertexArraysAPPLE", nullptr } },
                { nullptr, { nullptr } },
            };

            if (!LoadSymbols(&vaoSymbols[0], trygl, prefix)) {
                NS_ERROR("GL supports Vertex Array Object without supplying its functions.");

                MarkUnsupported(GLFeature::vertex_array_object);
                mSymbols.fIsVertexArray = nullptr;
                mSymbols.fGenVertexArrays = nullptr;
                mSymbols.fBindVertexArray = nullptr;
                mSymbols.fDeleteVertexArrays = nullptr;
            }
        }

        if (IsSupported(GLFeature::draw_instanced)) {
            SymLoadStruct drawInstancedSymbols[] = {
                { (PRFuncPtr*) &mSymbols.fDrawArraysInstanced,
                  { "DrawArraysInstanced",
                    "DrawArraysInstancedARB",
                    "DrawArraysInstancedEXT",
                    "DrawArraysInstancedNV",
                    "DrawArraysInstancedANGLE",
                    nullptr
                  }
                },
                { (PRFuncPtr*) &mSymbols.fDrawElementsInstanced,
                  { "DrawElementsInstanced",
                    "DrawElementsInstancedARB",
                    "DrawElementsInstancedEXT",
                    "DrawElementsInstancedNV",
                    "DrawElementsInstancedANGLE",
                    nullptr
                  }
                },
                { nullptr, { nullptr } },
            };

            if (!LoadSymbols(drawInstancedSymbols, trygl, prefix)) {
                NS_ERROR("GL supports instanced draws without supplying its functions.");

                MarkUnsupported(GLFeature::draw_instanced);
                mSymbols.fDrawArraysInstanced = nullptr;
                mSymbols.fDrawElementsInstanced = nullptr;
            }
        }

        if (IsSupported(GLFeature::instanced_arrays)) {
            SymLoadStruct instancedArraySymbols[] = {
                { (PRFuncPtr*) &mSymbols.fVertexAttribDivisor,
                  { "VertexAttribDivisor",
                    "VertexAttribDivisorARB",
                    "VertexAttribDivisorNV",
                    "VertexAttribDivisorANGLE",
                    nullptr
                  }
                },
                { nullptr, { nullptr } },
            };

            if (!LoadSymbols(instancedArraySymbols, trygl, prefix)) {
                NS_ERROR("GL supports array instanced without supplying it function.");

                MarkUnsupported(GLFeature::instanced_arrays);
                mSymbols.fVertexAttribDivisor = nullptr;
            }
        }

        if (IsSupported(GLFeature::transform_feedback)) {
            SymLoadStruct transformFeedbackSymbols[] = {
                { (PRFuncPtr*) &mSymbols.fBindBufferBase,
                  { "BindBufferBase",
                    "BindBufferBaseEXT",
                    "BindBufferBaseNV",
                    nullptr
                  }
                },
                { (PRFuncPtr*) &mSymbols.fBindBufferRange,
                  { "BindBufferRange",
                    "BindBufferRangeEXT",
                    "BindBufferRangeNV",
                    nullptr
                  }
                },
                { (PRFuncPtr*) &mSymbols.fBeginTransformFeedback,
                  { "BeginTransformFeedback",
                    "BeginTransformFeedbackEXT",
                    "BeginTransformFeedbackNV",
                    nullptr
                  }
                },
                { (PRFuncPtr*) &mSymbols.fEndTransformFeedback,
                  { "EndTransformFeedback",
                    "EndTransformFeedbackEXT",
                    "EndTransformFeedbackNV",
                    nullptr
                  }
                },
                { (PRFuncPtr*) &mSymbols.fTransformFeedbackVaryings,
                  { "TransformFeedbackVaryings",
                    "TransformFeedbackVaryingsEXT",
                    "TransformFeedbackVaryingsNV",
                    nullptr
                  }
                },
                { (PRFuncPtr*) &mSymbols.fGetTransformFeedbackVarying,
                  { "GetTransformFeedbackVarying",
                    "GetTransformFeedbackVaryingEXT",
                    "GetTransformFeedbackVaryingNV",
                    nullptr
                  }
                },
                { (PRFuncPtr*) &mSymbols.fGetIntegeri_v,
                  { "GetIntegeri_v",
                    "GetIntegerIndexedvEXT",
                    "GetIntegerIndexedvNV",
                    nullptr
                  }
                },
                { nullptr, { nullptr } },
            };

            if (!LoadSymbols(transformFeedbackSymbols, trygl, prefix)) {
                NS_ERROR("GL supports transform feedback without supplying its functions.");

                MarkUnsupported(GLFeature::transform_feedback);
                MarkUnsupported(GLFeature::bind_buffer_offset);
                mSymbols.fBindBufferBase = nullptr;
                mSymbols.fBindBufferRange = nullptr;
                mSymbols.fBeginTransformFeedback = nullptr;
                mSymbols.fEndTransformFeedback = nullptr;
                mSymbols.fTransformFeedbackVaryings = nullptr;
                mSymbols.fGetTransformFeedbackVarying = nullptr;
                mSymbols.fGetIntegeri_v = nullptr;
            }
        }

        if (IsSupported(GLFeature::bind_buffer_offset)) {
            SymLoadStruct bindBufferOffsetSymbols[] = {
                { (PRFuncPtr*) &mSymbols.fBindBufferOffset,
                  { "BindBufferOffset",
                    "BindBufferOffsetEXT",
                    "BindBufferOffsetNV",
                    nullptr
                  }
                },
                { nullptr, { nullptr } },
            };

            if (!LoadSymbols(bindBufferOffsetSymbols, trygl, prefix)) {
                NS_ERROR("GL supports BindBufferOffset without supplying its function.");

                MarkUnsupported(GLFeature::bind_buffer_offset);
                mSymbols.fBindBufferOffset = nullptr;
            }
        }

        if (IsSupported(GLFeature::query_objects)) {
            SymLoadStruct queryObjectsSymbols[] = {
                { (PRFuncPtr*) &mSymbols.fBeginQuery, { "BeginQuery", "BeginQueryEXT", nullptr } },
                { (PRFuncPtr*) &mSymbols.fGenQueries, { "GenQueries", "GenQueriesEXT", nullptr } },
                { (PRFuncPtr*) &mSymbols.fDeleteQueries, { "DeleteQueries", "DeleteQueriesEXT", nullptr } },
                { (PRFuncPtr*) &mSymbols.fEndQuery, { "EndQuery", "EndQueryEXT", nullptr } },
                { (PRFuncPtr*) &mSymbols.fGetQueryiv, { "GetQueryiv", "GetQueryivEXT", nullptr } },
                { (PRFuncPtr*) &mSymbols.fGetQueryObjectuiv, { "GetQueryObjectuiv", "GetQueryObjectuivEXT", nullptr } },
                { (PRFuncPtr*) &mSymbols.fIsQuery, { "IsQuery", "IsQueryEXT", nullptr } },
                { nullptr, { nullptr } },
            };

            if (!LoadSymbols(queryObjectsSymbols, trygl, prefix)) {
                NS_ERROR("GL supports query objects without supplying its functions.");

                MarkUnsupported(GLFeature::query_objects);
                MarkUnsupported(GLFeature::get_query_object_iv);
                MarkUnsupported(GLFeature::occlusion_query);
                MarkUnsupported(GLFeature::occlusion_query_boolean);
                MarkUnsupported(GLFeature::occlusion_query2);
                mSymbols.fBeginQuery = nullptr;
                mSymbols.fGenQueries = nullptr;
                mSymbols.fDeleteQueries = nullptr;
                mSymbols.fEndQuery = nullptr;
                mSymbols.fGetQueryiv = nullptr;
                mSymbols.fGetQueryObjectuiv = nullptr;
                mSymbols.fIsQuery = nullptr;
            }
        }

        if (IsSupported(GLFeature::get_query_object_iv)) {
            SymLoadStruct queryObjectsSymbols[] = {
                { (PRFuncPtr*) &mSymbols.fGetQueryObjectiv, { "GetQueryObjectiv", "GetQueryObjectivEXT", nullptr } },
                { nullptr, { nullptr } },
            };

            if (!LoadSymbols(queryObjectsSymbols, trygl, prefix)) {
                NS_ERROR("GL supports query objects iv getter without supplying its function.");

                MarkUnsupported(GLFeature::get_query_object_iv);
                mSymbols.fGetQueryObjectiv = nullptr;
            }
        }

        if (IsSupported(GLFeature::draw_buffers)) {
            SymLoadStruct drawBuffersSymbols[] = {
                { (PRFuncPtr*) &mSymbols.fDrawBuffers, { "DrawBuffers", nullptr } },
                { nullptr, { nullptr } },
            };

            if (!LoadSymbols(drawBuffersSymbols, trygl, prefix)) {
                NS_ERROR("GL supports draw_buffers without supplying its functions.");

                MarkUnsupported(GLFeature::draw_buffers);
                mSymbols.fDrawBuffers = nullptr;
            }
        }

        if (IsExtensionSupported(KHR_debug)) {
            SymLoadStruct extSymbols[] = {
                { (PRFuncPtr*) &mSymbols.fDebugMessageControl,  { "DebugMessageControl",  "DebugMessageControlKHR",  nullptr } },
                { (PRFuncPtr*) &mSymbols.fDebugMessageInsert,   { "DebugMessageInsert",   "DebugMessageInsertKHR",   nullptr } },
                { (PRFuncPtr*) &mSymbols.fDebugMessageCallback, { "DebugMessageCallback", "DebugMessageCallbackKHR", nullptr } },
                { (PRFuncPtr*) &mSymbols.fGetDebugMessageLog,   { "GetDebugMessageLog",   "GetDebugMessageLogKHR",   nullptr } },
                { (PRFuncPtr*) &mSymbols.fGetPointerv,          { "GetPointerv",          "GetPointervKHR",          nullptr } },
                { (PRFuncPtr*) &mSymbols.fPushDebugGroup,       { "PushDebugGroup",       "PushDebugGroupKHR",       nullptr } },
                { (PRFuncPtr*) &mSymbols.fPopDebugGroup,        { "PopDebugGroup",        "PopDebugGroupKHR",        nullptr } },
                { (PRFuncPtr*) &mSymbols.fObjectLabel,          { "ObjectLabel",          "ObjectLabelKHR",          nullptr } },
                { (PRFuncPtr*) &mSymbols.fGetObjectLabel,       { "GetObjectLabel",       "GetObjectLabelKHR",       nullptr } },
                { (PRFuncPtr*) &mSymbols.fObjectPtrLabel,       { "ObjectPtrLabel",       "ObjectPtrLabelKHR",       nullptr } },
                { (PRFuncPtr*) &mSymbols.fGetObjectPtrLabel,    { "GetObjectPtrLabel",    "GetObjectPtrLabelKHR",    nullptr } },
                { nullptr, { nullptr } },
            };

            if (!LoadSymbols(&extSymbols[0], trygl, prefix)) {
                NS_ERROR("GL supports KHR_debug without supplying its functions.");

                MarkExtensionUnsupported(KHR_debug);
                mSymbols.fDebugMessageControl  = nullptr;
                mSymbols.fDebugMessageInsert   = nullptr;
                mSymbols.fDebugMessageCallback = nullptr;
                mSymbols.fGetDebugMessageLog   = nullptr;
                mSymbols.fGetPointerv          = nullptr;
                mSymbols.fPushDebugGroup       = nullptr;
                mSymbols.fPopDebugGroup        = nullptr;
                mSymbols.fObjectLabel          = nullptr;
                mSymbols.fGetObjectLabel       = nullptr;
                mSymbols.fObjectPtrLabel       = nullptr;
                mSymbols.fGetObjectPtrLabel    = nullptr;
            }
        }

        // Load developer symbols, don't fail if we can't find them.
        SymLoadStruct auxSymbols[] = {
                { (PRFuncPtr*) &mSymbols.fGetTexImage, { "GetTexImage", nullptr } },
                { (PRFuncPtr*) &mSymbols.fGetTexLevelParameteriv, { "GetTexLevelParameteriv", nullptr } },
                { nullptr, { nullptr } },
        };
        bool warnOnFailures = DebugMode();
        LoadSymbols(&auxSymbols[0], trygl, prefix, warnOnFailures);
    }

    if (mInitialized) {
        GLint v[4];

        fGetIntegerv(LOCAL_GL_SCISSOR_BOX, v);
        mScissorStack.AppendElement(nsIntRect(v[0], v[1], v[2], v[3]));

        fGetIntegerv(LOCAL_GL_VIEWPORT, v);
        mViewportStack.AppendElement(nsIntRect(v[0], v[1], v[2], v[3]));

        raw_fGetIntegerv(LOCAL_GL_MAX_TEXTURE_SIZE, &mMaxTextureSize);
        raw_fGetIntegerv(LOCAL_GL_MAX_CUBE_MAP_TEXTURE_SIZE, &mMaxCubeMapTextureSize);
        raw_fGetIntegerv(LOCAL_GL_MAX_RENDERBUFFER_SIZE, &mMaxRenderbufferSize);

#ifdef XP_MACOSX
        if (mWorkAroundDriverBugs) {
            if (mVendor == VendorIntel) {
                // see bug 737182 for 2D textures, bug 684882 for cube map textures.
                mMaxTextureSize        = std::min(mMaxTextureSize,        4096);
                mMaxCubeMapTextureSize = std::min(mMaxCubeMapTextureSize, 512);
                // for good measure, we align renderbuffers on what we do for 2D textures
                mMaxRenderbufferSize   = std::min(mMaxRenderbufferSize,   4096);
                mNeedsTextureSizeChecks = true;
            } else if (mVendor == VendorNVIDIA) {
                SInt32 major, minor;
                OSErr err1 = ::Gestalt(gestaltSystemVersionMajor, &major);
                OSErr err2 = ::Gestalt(gestaltSystemVersionMinor, &minor);

                if (err1 != noErr || err2 != noErr ||
                    major < 10 || (major == 10 && minor < 8)) {
                    // See bug 877949.
                    mMaxTextureSize = std::min(mMaxTextureSize, 4096);
                    mMaxRenderbufferSize = std::min(mMaxRenderbufferSize, 4096);
                }
                else {
                    // See bug 879656.  8192 fails, 8191 works.
                    mMaxTextureSize = std::min(mMaxTextureSize, 8191);
                    mMaxRenderbufferSize = std::min(mMaxRenderbufferSize, 8191);
                }
                // Part of the bug 879656, but it also doesn't hurt the 877949
                mNeedsTextureSizeChecks = true;
            }
        }
#endif
#ifdef MOZ_X11
        if (mWorkAroundDriverBugs &&
            mVendor == VendorNouveau) {
            // see bug 814716. Clamp MaxCubeMapTextureSize at 2K for Nouveau.
            mMaxCubeMapTextureSize = std::min(mMaxCubeMapTextureSize, 2048);
            mNeedsTextureSizeChecks = true;
        }
#endif

        mMaxTextureImageSize = mMaxTextureSize;

        mMaxSamples = 0;
        if (IsSupported(GLFeature::framebuffer_multisample)) {
            fGetIntegerv(LOCAL_GL_MAX_SAMPLES, (GLint*)&mMaxSamples);
        }

        // We're ready for final setup.
        fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, 0);

        if (mCaps.any)
            DetermineCaps();

        UpdatePixelFormat();
        UpdateGLFormats(mCaps);

        mTexGarbageBin = new TextureGarbageBin(this);

        MOZ_ASSERT(IsCurrent());
    }

    if (mInitialized)
        reporter.SetSuccessful();
    else {
        // if initialization fails, ensure all symbols are zero, to avoid hard-to-understand bugs
        mSymbols.Zero();
        NS_WARNING("InitWithPrefix failed!");
    }

    mVersionString = nsPrintfCString("%u.%u.%u", mVersion / 100, (mVersion / 10) % 10, mVersion % 10);

    return mInitialized;
}

void
GLContext::InitExtensions()
{
    MakeCurrent();
    const char* extensions = (const char*)fGetString(LOCAL_GL_EXTENSIONS);
    if (!extensions)
        return;

#ifdef DEBUG
    static bool firstRun = true;
#else
    // Non-DEBUG, so never spew.
    const bool firstRun = false;
#endif

    InitializeExtensionsBitSet(mAvailableExtensions, extensions, sExtensionNames, firstRun && DebugMode());

    if (WorkAroundDriverBugs() &&
        Vendor() == VendorQualcomm) {

        // Some Adreno drivers do not report GL_OES_EGL_sync, but they really do support it.
        MarkExtensionSupported(OES_EGL_sync);
    }

    if (WorkAroundDriverBugs() &&
        Renderer() == RendererAndroidEmulator) {
        // the Android emulator, which we use to run B2G reftests on,
        // doesn't expose the OES_rgb8_rgba8 extension, but it seems to
        // support it (tautologically, as it only runs on desktop GL).
        MarkExtensionSupported(OES_rgb8_rgba8);
    }

#ifdef DEBUG
    firstRun = false;
#endif
}

void
GLContext::PlatformStartup()
{
  RegisterStrongMemoryReporter(new GfxTexturesReporter());
}

// Common code for checking for both GL extensions and GLX extensions.
bool
GLContext::ListHasExtension(const GLubyte *extensions, const char *extension)
{
    // fix bug 612572 - we were crashing as we were calling this function with extensions==null
    if (extensions == nullptr || extension == nullptr)
        return false;

    const GLubyte *start;
    GLubyte *where, *terminator;

    /* Extension names should not have spaces. */
    where = (GLubyte *) strchr(extension, ' ');
    if (where || *extension == '\0')
        return false;

    /*
     * It takes a bit of care to be fool-proof about parsing the
     * OpenGL extensions string. Don't be fooled by sub-strings,
     * etc.
     */
    start = extensions;
    for (;;) {
        where = (GLubyte *) strstr((const char *) start, extension);
        if (!where) {
            break;
        }
        terminator = where + strlen(extension);
        if (where == start || *(where - 1) == ' ') {
            if (*terminator == ' ' || *terminator == '\0') {
                return true;
            }
        }
        start = terminator;
    }
    return false;
}

void GLContext::ApplyFilterToBoundTexture(GraphicsFilter aFilter)
{
    ApplyFilterToBoundTexture(LOCAL_GL_TEXTURE_2D, aFilter);
}

void GLContext::ApplyFilterToBoundTexture(GLuint aTarget,
                                          GraphicsFilter aFilter)
{
    if (aFilter == GraphicsFilter::FILTER_NEAREST) {
        fTexParameteri(aTarget, LOCAL_GL_TEXTURE_MIN_FILTER, LOCAL_GL_NEAREST);
        fTexParameteri(aTarget, LOCAL_GL_TEXTURE_MAG_FILTER, LOCAL_GL_NEAREST);
    } else {
        fTexParameteri(aTarget, LOCAL_GL_TEXTURE_MIN_FILTER, LOCAL_GL_LINEAR);
        fTexParameteri(aTarget, LOCAL_GL_TEXTURE_MAG_FILTER, LOCAL_GL_LINEAR);
    }
}


void
GLContext::DetermineCaps()
{
    PixelBufferFormat format = QueryPixelFormat();

    SurfaceCaps caps;
    caps.color = !!format.red && !!format.green && !!format.blue;
    caps.bpp16 = caps.color && format.ColorBits() == 16;
    caps.alpha = !!format.alpha;
    caps.depth = !!format.depth;
    caps.stencil = !!format.stencil;
    caps.antialias = format.samples > 1;
    caps.preserve = true;

    mCaps = caps;
}

PixelBufferFormat
GLContext::QueryPixelFormat()
{
    PixelBufferFormat format;

    ScopedBindFramebuffer autoFB(this, 0);

    fGetIntegerv(LOCAL_GL_RED_BITS  , &format.red  );
    fGetIntegerv(LOCAL_GL_GREEN_BITS, &format.green);
    fGetIntegerv(LOCAL_GL_BLUE_BITS , &format.blue );
    fGetIntegerv(LOCAL_GL_ALPHA_BITS, &format.alpha);

    fGetIntegerv(LOCAL_GL_DEPTH_BITS, &format.depth);
    fGetIntegerv(LOCAL_GL_STENCIL_BITS, &format.stencil);

    fGetIntegerv(LOCAL_GL_SAMPLES, &format.samples);

    return format;
}

void
GLContext::UpdatePixelFormat()
{
    PixelBufferFormat format = QueryPixelFormat();
#ifdef DEBUG
    const SurfaceCaps& caps = Caps();
    MOZ_ASSERT(!caps.any, "Did you forget to DetermineCaps()?");

    MOZ_ASSERT(caps.color == !!format.red);
    MOZ_ASSERT(caps.color == !!format.green);
    MOZ_ASSERT(caps.color == !!format.blue);

    MOZ_ASSERT(caps.alpha == !!format.alpha);

    // These we either must have if they're requested, or
    // we can have if they're not.
    MOZ_ASSERT(caps.depth == !!format.depth || !caps.depth);
    MOZ_ASSERT(caps.stencil == !!format.stencil || !caps.stencil);

    MOZ_ASSERT(caps.antialias == (format.samples > 1));
#endif
    mPixelFormat = new PixelBufferFormat(format);
}

GLFormats
GLContext::ChooseGLFormats(const SurfaceCaps& caps) const
{
    GLFormats formats;

    // If we're on ES2 hardware and we have an explicit request for 16 bits of color or less
    // OR we don't support full 8-bit color, return a 4444 or 565 format.
    bool bpp16 = caps.bpp16;
    if (IsGLES2()) {
        if (!IsExtensionSupported(OES_rgb8_rgba8))
            bpp16 = true;
    } else {
        // RGB565 is uncommon on desktop, requiring ARB_ES2_compatibility.
        // Since it's also vanishingly useless there, let's not support it.
        bpp16 = false;
    }

    if (bpp16) {
        MOZ_ASSERT(IsGLES2());
        if (caps.alpha) {
            formats.color_texInternalFormat = LOCAL_GL_RGBA;
            formats.color_texFormat = LOCAL_GL_RGBA;
            formats.color_texType   = LOCAL_GL_UNSIGNED_SHORT_4_4_4_4;
            formats.color_rbFormat  = LOCAL_GL_RGBA4;
        } else {
            formats.color_texInternalFormat = LOCAL_GL_RGB;
            formats.color_texFormat = LOCAL_GL_RGB;
            formats.color_texType   = LOCAL_GL_UNSIGNED_SHORT_5_6_5;
            formats.color_rbFormat  = LOCAL_GL_RGB565;
        }
    } else {
        formats.color_texType = LOCAL_GL_UNSIGNED_BYTE;

        if (caps.alpha) {
            formats.color_texInternalFormat = IsGLES2() ? LOCAL_GL_RGBA : LOCAL_GL_RGBA8;
            formats.color_texFormat = LOCAL_GL_RGBA;
            formats.color_rbFormat  = LOCAL_GL_RGBA8;
        } else {
            formats.color_texInternalFormat = IsGLES2() ? LOCAL_GL_RGB : LOCAL_GL_RGB8;
            formats.color_texFormat = LOCAL_GL_RGB;
            formats.color_rbFormat  = LOCAL_GL_RGB8;
        }
    }

    uint32_t msaaLevel = Preferences::GetUint("gl.msaa-level", 2);
    GLsizei samples = msaaLevel * msaaLevel;
    samples = std::min(samples, mMaxSamples);

    // Bug 778765.
    if (WorkAroundDriverBugs() && samples == 1) {
        samples = 0;
    }
    formats.samples = samples;


    // Be clear that these are 0 if unavailable.
    formats.depthStencil = 0;
    if (!IsGLES2() || IsExtensionSupported(OES_packed_depth_stencil)) {
        formats.depthStencil = LOCAL_GL_DEPTH24_STENCIL8;
    }

    formats.depth = 0;
    if (IsGLES2()) {
        if (IsExtensionSupported(OES_depth24)) {
            formats.depth = LOCAL_GL_DEPTH_COMPONENT24;
        } else {
            formats.depth = LOCAL_GL_DEPTH_COMPONENT16;
        }
    } else {
        formats.depth = LOCAL_GL_DEPTH_COMPONENT24;
    }

    formats.stencil = LOCAL_GL_STENCIL_INDEX8;

    return formats;
}

bool
GLContext::IsFramebufferComplete(GLuint fb, GLenum* pStatus)
{
    MOZ_ASSERT(fb);

    ScopedBindFramebuffer autoFB(this, fb);
    MOZ_ASSERT(fIsFramebuffer(fb));

    GLenum status = fCheckFramebufferStatus(LOCAL_GL_FRAMEBUFFER);
    if (pStatus)
        *pStatus = status;

    return status == LOCAL_GL_FRAMEBUFFER_COMPLETE;
}

void
GLContext::AttachBuffersToFB(GLuint colorTex, GLuint colorRB,
                             GLuint depthRB, GLuint stencilRB,
                             GLuint fb, GLenum target)
{
    MOZ_ASSERT(fb);
    MOZ_ASSERT( !(colorTex && colorRB) );

    ScopedBindFramebuffer autoFB(this, fb);
    MOZ_ASSERT(fIsFramebuffer(fb)); // It only counts after being bound.

    if (colorTex) {
        MOZ_ASSERT(fIsTexture(colorTex));
        MOZ_ASSERT(target == LOCAL_GL_TEXTURE_2D ||
                   target == LOCAL_GL_TEXTURE_RECTANGLE_ARB);
        fFramebufferTexture2D(LOCAL_GL_FRAMEBUFFER,
                              LOCAL_GL_COLOR_ATTACHMENT0,
                              target,
                              colorTex,
                              0);
    } else if (colorRB) {
        MOZ_ASSERT(fIsRenderbuffer(colorRB));
        fFramebufferRenderbuffer(LOCAL_GL_FRAMEBUFFER,
                                 LOCAL_GL_COLOR_ATTACHMENT0,
                                 LOCAL_GL_RENDERBUFFER,
                                 colorRB);
    }

    if (depthRB) {
        MOZ_ASSERT(fIsRenderbuffer(depthRB));
        fFramebufferRenderbuffer(LOCAL_GL_FRAMEBUFFER,
                                 LOCAL_GL_DEPTH_ATTACHMENT,
                                 LOCAL_GL_RENDERBUFFER,
                                 depthRB);
    }

    if (stencilRB) {
        MOZ_ASSERT(fIsRenderbuffer(stencilRB));
        fFramebufferRenderbuffer(LOCAL_GL_FRAMEBUFFER,
                                 LOCAL_GL_STENCIL_ATTACHMENT,
                                 LOCAL_GL_RENDERBUFFER,
                                 stencilRB);
    }
}

bool
GLContext::AssembleOffscreenFBs(const GLuint colorMSRB,
                                const GLuint depthRB,
                                const GLuint stencilRB,
                                const GLuint texture,
                                GLuint* drawFB_out,
                                GLuint* readFB_out)
{
    if (!colorMSRB && !texture) {
        MOZ_ASSERT(!depthRB && !stencilRB);

        if (drawFB_out)
            *drawFB_out = 0;
        if (readFB_out)
            *readFB_out = 0;

        return true;
    }

    ScopedBindFramebuffer autoFB(this);

    GLuint drawFB = 0;
    GLuint readFB = 0;

    if (texture) {
        readFB = 0;
        fGenFramebuffers(1, &readFB);
        BindFB(readFB);
        fFramebufferTexture2D(LOCAL_GL_FRAMEBUFFER,
                              LOCAL_GL_COLOR_ATTACHMENT0,
                              LOCAL_GL_TEXTURE_2D,
                              texture,
                              0);
    }

    if (colorMSRB) {
        drawFB = 0;
        fGenFramebuffers(1, &drawFB);
        BindFB(drawFB);
        fFramebufferRenderbuffer(LOCAL_GL_FRAMEBUFFER,
                                 LOCAL_GL_COLOR_ATTACHMENT0,
                                 LOCAL_GL_RENDERBUFFER,
                                 colorMSRB);
    } else {
        drawFB = readFB;
    }
    MOZ_ASSERT(GetFB() == drawFB);

    if (depthRB) {
        fFramebufferRenderbuffer(LOCAL_GL_FRAMEBUFFER,
                                 LOCAL_GL_DEPTH_ATTACHMENT,
                                 LOCAL_GL_RENDERBUFFER,
                                 depthRB);
    }

    if (stencilRB) {
        fFramebufferRenderbuffer(LOCAL_GL_FRAMEBUFFER,
                                 LOCAL_GL_STENCIL_ATTACHMENT,
                                 LOCAL_GL_RENDERBUFFER,
                                 stencilRB);
    }

    // We should be all resized.  Check for framebuffer completeness.
    GLenum status;
    bool isComplete = true;

    if (!IsFramebufferComplete(drawFB, &status)) {
        NS_WARNING("DrawFBO: Incomplete");
  #ifdef DEBUG
        if (DebugMode()) {
            printf_stderr("Framebuffer status: %X\n", status);
        }
  #endif
        isComplete = false;
    }

    if (!IsFramebufferComplete(readFB, &status)) {
        NS_WARNING("ReadFBO: Incomplete");
  #ifdef DEBUG
        if (DebugMode()) {
            printf_stderr("Framebuffer status: %X\n", status);
        }
  #endif
        isComplete = false;
    }

    if (drawFB_out) {
        *drawFB_out = drawFB;
    } else if (drawFB) {
        NS_RUNTIMEABORT("drawFB created when not requested!");
    }

    if (readFB_out) {
        *readFB_out = readFB;
    } else if (readFB) {
        NS_RUNTIMEABORT("readFB created when not requested!");
    }

    return isComplete;
}



bool
GLContext::PublishFrame()
{
    MOZ_ASSERT(mScreen);

    if (!mScreen->PublishFrame(OffscreenSize()))
        return false;

    return true;
}

SharedSurface*
GLContext::RequestFrame()
{
    MOZ_ASSERT(mScreen);

    return mScreen->Stream()->SwapConsumer();
}



void
GLContext::ClearSafely()
{
    // bug 659349 --- we must be very careful here: clearing a GL framebuffer is nontrivial, relies on a lot of state,
    // and in the case of the backbuffer of a WebGL context, state is exposed to scripts.
    //
    // The code here is taken from WebGLContext::ForceClearFramebufferWithDefaultValues, but I didn't find a good way of
    // sharing code with it. WebGL's code is somewhat performance-critical as it is typically called on every frame, so
    // WebGL keeps track of GL state to avoid having to query it everytime, and also tries to only do work for actually
    // present buffers (e.g. stencil buffer). Doing that here seems like premature optimization,
    // as ClearSafely() is called only when e.g. a canvas is resized, not on every animation frame.

    realGLboolean scissorTestEnabled;
    realGLboolean ditherEnabled;
    realGLboolean colorWriteMask[4];
    realGLboolean depthWriteMask;
    GLint stencilWriteMaskFront, stencilWriteMaskBack;
    GLfloat colorClearValue[4];
    GLfloat depthClearValue;
    GLint stencilClearValue;

    // save current GL state
    fGetBooleanv(LOCAL_GL_SCISSOR_TEST, &scissorTestEnabled);
    fGetBooleanv(LOCAL_GL_DITHER, &ditherEnabled);
    fGetBooleanv(LOCAL_GL_COLOR_WRITEMASK, colorWriteMask);
    fGetBooleanv(LOCAL_GL_DEPTH_WRITEMASK, &depthWriteMask);
    fGetIntegerv(LOCAL_GL_STENCIL_WRITEMASK, &stencilWriteMaskFront);
    fGetIntegerv(LOCAL_GL_STENCIL_BACK_WRITEMASK, &stencilWriteMaskBack);
    fGetFloatv(LOCAL_GL_COLOR_CLEAR_VALUE, colorClearValue);
    fGetFloatv(LOCAL_GL_DEPTH_CLEAR_VALUE, &depthClearValue);
    fGetIntegerv(LOCAL_GL_STENCIL_CLEAR_VALUE, &stencilClearValue);

    // prepare GL state for clearing
    fDisable(LOCAL_GL_SCISSOR_TEST);
    fDisable(LOCAL_GL_DITHER);
    PushViewportRect(nsIntRect(0, 0, OffscreenSize().width, OffscreenSize().height));

    fColorMask(1, 1, 1, 1);
    fClearColor(0.f, 0.f, 0.f, 0.f);

    fDepthMask(1);
    fClearDepth(1.0f);

    fStencilMask(0xffffffff);
    fClearStencil(0);

    // do clear
    fClear(LOCAL_GL_COLOR_BUFFER_BIT |
           LOCAL_GL_DEPTH_BUFFER_BIT |
           LOCAL_GL_STENCIL_BUFFER_BIT);

    // restore GL state after clearing
    fColorMask(colorWriteMask[0],
               colorWriteMask[1],
               colorWriteMask[2],
               colorWriteMask[3]);
    fClearColor(colorClearValue[0],
                colorClearValue[1],
                colorClearValue[2],
                colorClearValue[3]);

    fDepthMask(depthWriteMask);
    fClearDepth(depthClearValue);

    fStencilMaskSeparate(LOCAL_GL_FRONT, stencilWriteMaskFront);
    fStencilMaskSeparate(LOCAL_GL_BACK, stencilWriteMaskBack);
    fClearStencil(stencilClearValue);

    PopViewportRect();

    if (ditherEnabled)
        fEnable(LOCAL_GL_DITHER);
    else
        fDisable(LOCAL_GL_DITHER);

    if (scissorTestEnabled)
        fEnable(LOCAL_GL_SCISSOR_TEST);
    else
        fDisable(LOCAL_GL_SCISSOR_TEST);

}

void
GLContext::MarkDestroyed()
{
    if (IsDestroyed())
        return;

    if (MakeCurrent()) {
        DestroyScreenBuffer();

        mBlitHelper = nullptr;
        mBlitTextureImageHelper = nullptr;

        fDeleteProgram(mReadTextureImagePrograms[0]);
        fDeleteProgram(mReadTextureImagePrograms[1]);
        fDeleteProgram(mReadTextureImagePrograms[2]);
        fDeleteProgram(mReadTextureImagePrograms[3]);

        mTexGarbageBin->GLContextTeardown();
    } else {
        NS_WARNING("MakeCurrent() failed during MarkDestroyed! Skipping GL object teardown.");
    }

    mSymbols.Zero();
}

static void SwapRAndBComponents(gfxImageSurface* surf)
{
  uint8_t *row = surf->Data();
  size_t rowBytes = surf->Width()*4;
  size_t rows = surf->Height();

#ifdef MOZILLA_MAY_SUPPORT_SSSE3
  const __m128i swap_mask = _mm_set_epi8(15, 12, 13, 14,
                                         11,  8,  9, 10,
                                          7,  4,  5,  6,
                                          3,  0,  1,  2);
#endif

  while (rows) {

    size_t w = rowBytes;
    uint8_t *d = row;

#ifdef MOZILLA_MAY_SUPPORT_SSSE3
    if (mozilla::supports_ssse3()) {
      while (w >= 4 && ((uintptr_t)d & 15)) {
        d[0] ^= d[2];
        d[2] ^= d[0];
        d[0] ^= d[2];
        d += 4;
        w -= 4;
      }

      while (w >= 16) {
        _mm_store_si128((__m128i*)d,
                        _mm_shuffle_epi8(_mm_load_si128((__m128i*)d), swap_mask));
        d += 16;
        w -= 16;
      }
    }
#endif

    while (w >= 4) {
      d[0] ^= d[2];
      d[2] ^= d[0];
      d[0] ^= d[2];
      d += 4;
      w -= 4;
    }

    row += surf->Stride();
    --rows;
  }
}

static already_AddRefed<gfxImageSurface> YInvertImageSurface(gfxImageSurface* aSurf)
{
  gfxIntSize size = aSurf->GetSize();
  nsRefPtr<gfxImageSurface> temp = new gfxImageSurface(size, aSurf->Format());
  nsRefPtr<gfxContext> ctx = new gfxContext(temp);
  ctx->SetOperator(gfxContext::OPERATOR_SOURCE);
  ctx->Scale(1.0, -1.0);
  ctx->Translate(-gfxPoint(0.0, size.height));
  ctx->SetSource(aSurf);
  ctx->Paint();
  return temp.forget();
}

already_AddRefed<gfxImageSurface>
GLContext::GetTexImage(GLuint aTexture, bool aYInvert, SurfaceFormat aFormat)
{
    MakeCurrent();
    GuaranteeResolve();
    fActiveTexture(LOCAL_GL_TEXTURE0);
    fBindTexture(LOCAL_GL_TEXTURE_2D, aTexture);

    gfxIntSize size;
    fGetTexLevelParameteriv(LOCAL_GL_TEXTURE_2D, 0, LOCAL_GL_TEXTURE_WIDTH, &size.width);
    fGetTexLevelParameteriv(LOCAL_GL_TEXTURE_2D, 0, LOCAL_GL_TEXTURE_HEIGHT, &size.height);

    nsRefPtr<gfxImageSurface> surf = new gfxImageSurface(size, gfxImageFormatARGB32);
    if (!surf || surf->CairoStatus()) {
        return nullptr;
    }

    uint32_t currentPackAlignment = 0;
    fGetIntegerv(LOCAL_GL_PACK_ALIGNMENT, (GLint*)&currentPackAlignment);
    if (currentPackAlignment != 4) {
        fPixelStorei(LOCAL_GL_PACK_ALIGNMENT, 4);
    }
    fGetTexImage(LOCAL_GL_TEXTURE_2D, 0, LOCAL_GL_RGBA, LOCAL_GL_UNSIGNED_BYTE, surf->Data());
    if (currentPackAlignment != 4) {
        fPixelStorei(LOCAL_GL_PACK_ALIGNMENT, currentPackAlignment);
    }

    if (aFormat == FORMAT_R8G8B8A8 || aFormat == FORMAT_R8G8B8X8) {
      SwapRAndBComponents(surf);
    }

    if (aYInvert) {
      surf = YInvertImageSurface(surf);
    }
    return surf.forget();
}

static float
gReadTextureImageVerts[4*4] = {
    -1.0f, -1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 0.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 1.0f,
     1.0f,  1.0f, 0.0f, 1.0f
};

static float*
ReadTextureVertexArray()
{
    return gReadTextureImageVerts;
}

static float
gReadTextureImageTexcoords[2*4] = {
    0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f
};

static float*
ReadTextureTexCoordArray(float aWidth, float aHeight, bool aFlip)
{
    const float u0 = 0.0f;
    const float u1 = aWidth;
    const float v0 = (aFlip) ? aHeight : 0.0f;
    const float v1 = (aFlip) ? 0.0f : aHeight;

    float* uvs = gReadTextureImageTexcoords;
    uvs[0] = u0;
    uvs[1] = v0;
    uvs[2] = u1;
    uvs[3] = v0;
    uvs[4] = u0;
    uvs[5] = v1;
    uvs[6] = u1;
    uvs[7] = v1;

    return uvs;
}

static const char*
gReadTextureImageVS =
    "attribute vec4 aVertex;\n"
    "attribute vec2 aTexCoord;\n"
    "varying vec2 vTexCoord;\n"
    "void main() { gl_Position = aVertex; vTexCoord = aTexCoord; }";

static const char*
gReadTextureImageFS[] = {
    /* TEXTURE_2D */
    "#ifdef GL_ES\n"
    "precision mediump float;\n"
    "#endif\n"
    "varying vec2 vTexCoord;\n"
    "uniform sampler2D uTexture;\n"
    "void main() { gl_FragColor = texture2D(uTexture, vTexCoord); }"
    ,
    /* TEXTURE_2D with R/B swizzling */
    "#ifdef GL_ES\n"
    "precision mediump float;\n"
    "#endif\n"
    "varying vec2 vTexCoord;\n"
    "uniform sampler2D uTexture;\n"
    "void main() { gl_FragColor = texture2D(uTexture, vTexCoord).bgra; }"
    ,
    /* TEXTURE_EXTERNAL */
    "#extension GL_OES_EGL_image_external : require\n"
    "#ifdef GL_ES\n"
    "precision mediump float;\n"
    "#endif\n"
    "varying vec2 vTexCoord;\n"
    "uniform samplerExternalOES uTexture;\n"
    "void main() { gl_FragColor = texture2D(uTexture, vTexCoord); }"
    ,
    /* TEXTURE_RECTANGLE */
    "#extension GL_ARB_texture_rectangle\n"
    "#ifdef GL_ES\n"
    "precision mediump float;\n"
    "#endif\n"
    "varying vec2 vTexCoord;\n"
    "uniform sampler2DRect uTexture;\n"
    "void main() { gl_FragColor = texture2DRect(uTexture, vTexCoord).bgra; }"
};

GLuint
GLContext::TextureImageProgramFor(GLenum aTextureTarget, int aShader) {
    int variant = 0;
    if (aTextureTarget == LOCAL_GL_TEXTURE_2D &&
        (aShader == layers::BGRALayerProgramType ||
         aShader == layers::BGRXLayerProgramType))
    {   // Need to swizzle R/B.
        variant = 1;
    } else if (aTextureTarget == LOCAL_GL_TEXTURE_EXTERNAL) {
        variant = 2;
    } else if (aTextureTarget == LOCAL_GL_TEXTURE_RECTANGLE) {
        variant = 3;
    }

    /* This might be overkill, but assure that we don't access out-of-bounds */
    MOZ_ASSERT((size_t) variant < ArrayLength(mReadTextureImagePrograms));
    if (!mReadTextureImagePrograms[variant]) {
        GLuint vs = fCreateShader(LOCAL_GL_VERTEX_SHADER);
        fShaderSource(vs, 1, (const GLchar**) &gReadTextureImageVS, NULL);
        fCompileShader(vs);

        GLuint fs = fCreateShader(LOCAL_GL_FRAGMENT_SHADER);
        fShaderSource(fs, 1, (const GLchar**) &gReadTextureImageFS[variant], NULL);
        fCompileShader(fs);

        GLuint program = fCreateProgram();
        fAttachShader(program, vs);
        fAttachShader(program, fs);
        fBindAttribLocation(program, 0, "aVertex");
        fBindAttribLocation(program, 1, "aTexCoord");
        fLinkProgram(program);

        GLint success;
        fGetProgramiv(program, LOCAL_GL_LINK_STATUS, &success);

        if (!success) {
            fDeleteProgram(program);
            program = 0;
        }

        fDeleteShader(vs);
        fDeleteShader(fs);

        mReadTextureImagePrograms[variant] = program;
    }

    return mReadTextureImagePrograms[variant];
}

static bool
DidGLErrorOccur(GLContext* aGL, const char* str)
{
    GLenum error = aGL->fGetError();
    if (error != LOCAL_GL_NO_ERROR) {
        printf_stderr("GL ERROR: %s (0x%04x) %s\n",
                      aGL->GLErrorToString(error), error, str);
        return true;
    }

    return false;
}

bool
GLContext::ReadBackPixelsIntoSurface(gfxImageSurface* aSurface, const gfxIntSize& aSize) {
    GLint oldPackAlignment;
    fGetIntegerv(LOCAL_GL_PACK_ALIGNMENT, &oldPackAlignment);

    if (oldPackAlignment != 4)
        fPixelStorei(LOCAL_GL_PACK_ALIGNMENT, 4);

    fReadPixels(0, 0, aSize.width, aSize.height,
                LOCAL_GL_RGBA, LOCAL_GL_UNSIGNED_BYTE,
                aSurface->Data());

    bool result = DidGLErrorOccur(this, "when reading pixels into surface");

    if (oldPackAlignment != 4)
        fPixelStorei(LOCAL_GL_PACK_ALIGNMENT, oldPackAlignment);

    return result;
}

#define CLEANUP_IF_GLERROR_OCCURRED(x)                                      \
    if (DidGLErrorOccur(this, (x))) {                                       \
        isurf = nullptr;                                                    \
        break;                                                              \
    }

already_AddRefed<gfxImageSurface>
GLContext::ReadTextureImage(GLuint aTextureId,
                            GLenum aTextureTarget,
                            const gfxIntSize& aSize,
    /* ShaderProgramType */ int aShaderProgram,
                            bool aYInvert)
{
    // Check aShaderProgram is in bounds for a layers::ShaderProgramType
    MOZ_ASSERT(0 <= aShaderProgram && aShaderProgram < NumProgramTypes);

    if (aTextureTarget != LOCAL_GL_TEXTURE_2D &&
        aTextureTarget != LOCAL_GL_TEXTURE_EXTERNAL &&
        aTextureTarget != LOCAL_GL_TEXTURE_RECTANGLE_ARB)
    {
        printf_stderr("ReadTextureImage target is not TEXTURE_2D || "
                      "TEXTURE_EXTERNAL || TEXTURE_RECTANGLE\n");
        return nullptr;
    }

    MakeCurrent();

    /* Allocate resulting image surface */
    nsRefPtr<gfxImageSurface> isurf;
    isurf = new gfxImageSurface(aSize, gfxImageFormatARGB32);
    if (!isurf || isurf->CairoStatus()) {
        isurf = nullptr;
        return isurf.forget();
    }

    realGLboolean oldBlend, oldScissor;
    GLint oldrb, oldfb, oldprog, oldTexUnit, oldTex;
    GLuint rb, fb;

    do {
        /* Save current GL state */
        oldBlend = fIsEnabled(LOCAL_GL_BLEND);
        oldScissor = fIsEnabled(LOCAL_GL_SCISSOR_TEST);

        fGetIntegerv(LOCAL_GL_RENDERBUFFER_BINDING, &oldrb);
        fGetIntegerv(LOCAL_GL_FRAMEBUFFER_BINDING, &oldfb);
        fGetIntegerv(LOCAL_GL_CURRENT_PROGRAM, &oldprog);
        fGetIntegerv(LOCAL_GL_ACTIVE_TEXTURE, &oldTexUnit);
        fActiveTexture(LOCAL_GL_TEXTURE0);
        switch (aTextureTarget) {
        case LOCAL_GL_TEXTURE_2D:
            fGetIntegerv(LOCAL_GL_TEXTURE_BINDING_2D, &oldTex);
            break;
        case LOCAL_GL_TEXTURE_EXTERNAL:
            fGetIntegerv(LOCAL_GL_TEXTURE_BINDING_EXTERNAL, &oldTex);
            break;
        case LOCAL_GL_TEXTURE_RECTANGLE:
            fGetIntegerv(LOCAL_GL_TEXTURE_BINDING_RECTANGLE, &oldTex);
            break;
        default: /* Already checked above */
            break;
        }

        /* Set required GL state */
        fDisable(LOCAL_GL_BLEND);
        fDisable(LOCAL_GL_SCISSOR_TEST);

        PushViewportRect(nsIntRect(0, 0, aSize.width, aSize.height));

        /* Setup renderbuffer */
        fGenRenderbuffers(1, &rb);
        fBindRenderbuffer(LOCAL_GL_RENDERBUFFER, rb);

        GLenum rbInternalFormat =
            IsGLES2()
                ? (IsExtensionSupported(OES_rgb8_rgba8) ? LOCAL_GL_RGBA8 : LOCAL_GL_RGBA4)
                : LOCAL_GL_RGBA;
        fRenderbufferStorage(LOCAL_GL_RENDERBUFFER, rbInternalFormat, aSize.width, aSize.height);
        CLEANUP_IF_GLERROR_OCCURRED("when binding and creating renderbuffer");

        /* Setup framebuffer */
        fGenFramebuffers(1, &fb);
        fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, fb);
        fFramebufferRenderbuffer(LOCAL_GL_FRAMEBUFFER, LOCAL_GL_COLOR_ATTACHMENT0,
                                 LOCAL_GL_RENDERBUFFER, rb);
        CLEANUP_IF_GLERROR_OCCURRED("when binding and creating framebuffer");

        if (fCheckFramebufferStatus(LOCAL_GL_FRAMEBUFFER) != LOCAL_GL_FRAMEBUFFER_COMPLETE) {
            printf_stderr("framebuffer is incomplete\n");
            break; //goto cleanup;
        }

        /* Setup vertex and fragment shader */
        layers::ShaderProgramType shaderProgram = (ShaderProgramType) aShaderProgram;
        GLuint program = TextureImageProgramFor(aTextureTarget, shaderProgram);
        if (!program) {
            printf_stderr("failed to compile program for texture target %u and"
                          " shader program type %d\n",
                          aTextureTarget, aShaderProgram);
            break; // goto cleanup;
        }

        fUseProgram(program);
        CLEANUP_IF_GLERROR_OCCURRED("when using program");
        fUniform1i(fGetUniformLocation(program, "uTexture"), 0);
        CLEANUP_IF_GLERROR_OCCURRED("when setting uniform location");

        /* Setup quad geometry */
        fBindBuffer(LOCAL_GL_ARRAY_BUFFER, 0);
        fEnableVertexAttribArray(0);
        fEnableVertexAttribArray(1);

        float w = (aTextureTarget == LOCAL_GL_TEXTURE_RECTANGLE) ? (float) aSize.width : 1.0f;
        float h = (aTextureTarget == LOCAL_GL_TEXTURE_RECTANGLE) ? (float) aSize.height : 1.0f;
        fVertexAttribPointer(0, 4, LOCAL_GL_FLOAT, LOCAL_GL_FALSE, 0, ReadTextureVertexArray());
        fVertexAttribPointer(1, 2, LOCAL_GL_FLOAT, LOCAL_GL_FALSE, 0, ReadTextureTexCoordArray(w, h, aYInvert));

        /* Bind the texture */
        if (aTextureId) {
            fBindTexture(aTextureTarget, aTextureId);
            CLEANUP_IF_GLERROR_OCCURRED("when binding texture");
        }

        /* Draw quad */
        fClearColor(1.0f, 0.0f, 1.0f, 1.0f);
        fClear(LOCAL_GL_COLOR_BUFFER_BIT);
        CLEANUP_IF_GLERROR_OCCURRED("when clearing color buffer");

        fDrawArrays(LOCAL_GL_TRIANGLE_STRIP, 0, 4);
        CLEANUP_IF_GLERROR_OCCURRED("when drawing texture");

        fDisableVertexAttribArray(1);
        fDisableVertexAttribArray(0);

        /* Read-back draw results */
        ReadBackPixelsIntoSurface(isurf, aSize);
        CLEANUP_IF_GLERROR_OCCURRED("when reading pixels into surface");
    } while (false);

    /* Restore GL state */
//cleanup:
    fBindRenderbuffer(LOCAL_GL_RENDERBUFFER, oldrb);
    fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, oldfb);
    fUseProgram(oldprog);

    // note that deleting 0 has no effect in any of these calls
    fDeleteRenderbuffers(1, &rb);
    fDeleteFramebuffers(1, &fb);

    if (oldBlend)
        fEnable(LOCAL_GL_BLEND);

    if (oldScissor)
        fEnable(LOCAL_GL_SCISSOR_TEST);

    if (aTextureId)
        fBindTexture(aTextureTarget, oldTex);

    if (oldTexUnit != LOCAL_GL_TEXTURE0)
        fActiveTexture(oldTexUnit);

    PopViewportRect();

    return isurf.forget();
}

#undef CLEANUP_IF_GLERROR_OCCURRED

static bool
GetActualReadFormats(GLContext* gl, GLenum destFormat, GLenum destType,
                     GLenum& readFormat, GLenum& readType)
{
    if (destFormat == LOCAL_GL_RGBA &&
        destType == LOCAL_GL_UNSIGNED_BYTE)
    {
        readFormat = destFormat;
        readType = destType;
        return true;
    }

    bool fallback = true;
    if (gl->IsGLES2()) {
        GLenum auxFormat = 0;
        GLenum auxType = 0;

        gl->fGetIntegerv(LOCAL_GL_IMPLEMENTATION_COLOR_READ_FORMAT, (GLint*)&auxFormat);
        gl->fGetIntegerv(LOCAL_GL_IMPLEMENTATION_COLOR_READ_TYPE, (GLint*)&auxType);

        if (destFormat == auxFormat &&
            destType == auxType)
        {
            fallback = false;
        }
    } else {
        switch (destFormat) {
            case LOCAL_GL_RGB: {
                if (destType == LOCAL_GL_UNSIGNED_SHORT_5_6_5_REV)
                    fallback = false;
                break;
            }
            case LOCAL_GL_BGRA: {
                if (destType == LOCAL_GL_UNSIGNED_INT_8_8_8_8_REV)
                    fallback = false;
                break;
            }
        }
    }

    if (fallback) {
        readFormat = LOCAL_GL_RGBA;
        readType = LOCAL_GL_UNSIGNED_BYTE;
        return false;
    } else {
        readFormat = destFormat;
        readType = destType;
        return true;
    }
}

void
GLContext::ReadScreenIntoImageSurface(gfxImageSurface* dest)
{
    ScopedBindFramebuffer autoFB(this, 0);

    ReadPixelsIntoImageSurface(dest);
}

TemporaryRef<SourceSurface>
GLContext::ReadPixelsToSourceSurface(const gfx::IntSize &aSize)
{
    // XXX we should do this properly one day without using the gfxImageSurface
    RefPtr<DataSourceSurface> dataSourceSurface =
        Factory::CreateDataSourceSurface(aSize, gfx::FORMAT_B8G8R8A8);
    nsRefPtr<gfxImageSurface> surf =
        new gfxImageSurface(dataSourceSurface->GetData(),
                            gfxIntSize(aSize.width, aSize.height),
                            dataSourceSurface->Stride(),
                            gfxImageFormatARGB32);
    ReadPixelsIntoImageSurface(surf);
    dataSourceSurface->MarkDirty();

    return dataSourceSurface;
}

void
GLContext::ReadPixelsIntoImageSurface(gfxImageSurface* dest)
{
    MakeCurrent();
    MOZ_ASSERT(dest->GetSize() != gfxIntSize(0, 0));

    /* gfxImageFormatARGB32:
     * RGBA+UByte: be[RGBA], le[ABGR]
     * RGBA+UInt: le[RGBA]
     * BGRA+UInt: le[BGRA]
     * BGRA+UIntRev: le[ARGB]
     *
     * gfxImageFormatRGB16_565:
     * RGB+UShort: le[rrrrrggg,gggbbbbb]
     */
    bool hasAlpha = dest->Format() == gfxImageFormatARGB32;

    int destPixelSize;
    GLenum destFormat;
    GLenum destType;

    switch (dest->Format()) {
        case gfxImageFormatRGB24: // XRGB
        case gfxImageFormatARGB32:
            destPixelSize = 4;
            // Needs host (little) endian ARGB.
            destFormat = LOCAL_GL_BGRA;
            destType = LOCAL_GL_UNSIGNED_INT_8_8_8_8_REV;
            break;

        case gfxImageFormatRGB16_565:
            destPixelSize = 2;
            destFormat = LOCAL_GL_RGB;
            destType = LOCAL_GL_UNSIGNED_SHORT_5_6_5_REV;
            break;

        default:
            MOZ_CRASH("Bad format.");
    }
    MOZ_ASSERT(dest->Stride() == dest->Width() * destPixelSize);

    GLenum readFormat = destFormat;
    GLenum readType = destType;
    bool needsTempSurf = !GetActualReadFormats(this,
                                               destFormat, destType,
                                               readFormat, readType);

    nsAutoPtr<gfxImageSurface> tempSurf;
    gfxImageSurface* readSurf = nullptr;
    int readPixelSize = 0;
    if (needsTempSurf) {
        if (DebugMode()) {
            NS_WARNING("Needing intermediary surface for ReadPixels. This will be slow!");
        }
        ImageFormat readFormatGFX;

        switch (readFormat) {
            case LOCAL_GL_RGBA:
            case LOCAL_GL_BGRA: {
                readFormatGFX = hasAlpha ? gfxImageFormatARGB32
                                         : gfxImageFormatRGB24;
                break;
            }
            case LOCAL_GL_RGB: {
                MOZ_ASSERT(readPixelSize == 2);
                MOZ_ASSERT(readType == LOCAL_GL_UNSIGNED_SHORT_5_6_5_REV);
                readFormatGFX = gfxImageFormatRGB16_565;
                break;
            }
            default: {
                MOZ_CRASH("Bad read format.");
            }
        }

        switch (readType) {
            case LOCAL_GL_UNSIGNED_BYTE: {
                MOZ_ASSERT(readFormat == LOCAL_GL_RGBA);
                readPixelSize = 4;
                break;
            }
            case LOCAL_GL_UNSIGNED_INT_8_8_8_8_REV: {
                MOZ_ASSERT(readFormat == LOCAL_GL_BGRA);
                readPixelSize = 4;
                break;
            }
            case LOCAL_GL_UNSIGNED_SHORT_5_6_5_REV: {
                MOZ_ASSERT(readFormat == LOCAL_GL_RGB);
                readPixelSize = 2;
                break;
            }
            default: {
                MOZ_CRASH("Bad read type.");
            }
        }

        tempSurf = new gfxImageSurface(dest->GetSize(), readFormatGFX, false);
        readSurf = tempSurf;
    } else {
        readPixelSize = destPixelSize;
        readSurf = dest;
    }
    MOZ_ASSERT(readPixelSize);

    GLint currentPackAlignment = 0;
    fGetIntegerv(LOCAL_GL_PACK_ALIGNMENT, &currentPackAlignment);

    if (currentPackAlignment != readPixelSize)
        fPixelStorei(LOCAL_GL_PACK_ALIGNMENT, readPixelSize);

    GLsizei width = dest->Width();
    GLsizei height = dest->Height();

    readSurf->Flush();
    fReadPixels(0, 0,
                width, height,
                readFormat, readType,
                readSurf->Data());
    readSurf->MarkDirty();

    if (currentPackAlignment != readPixelSize)
        fPixelStorei(LOCAL_GL_PACK_ALIGNMENT, currentPackAlignment);

    if (readSurf != dest) {
        MOZ_ASSERT(readFormat == LOCAL_GL_RGBA);
        MOZ_ASSERT(readType == LOCAL_GL_UNSIGNED_BYTE);
        // So we just copied in RGBA in big endian, or le: 0xAABBGGRR.
        // We want 0xAARRGGBB, so swap R and B:
        dest->Flush();
        SwapRAndBComponents(readSurf);
        dest->MarkDirty();

        gfxContext ctx(dest);
        ctx.SetOperator(gfxContext::OPERATOR_SOURCE);
        ctx.SetSource(readSurf);
        ctx.Paint();
    }

    // Check if GL is giving back 1.0 alpha for
    // RGBA reads to RGBA images from no-alpha buffers.
#ifdef XP_MACOSX
    if (WorkAroundDriverBugs() &&
        mVendor == VendorNVIDIA &&
        dest->Format() == gfxImageFormatARGB32 &&
        width && height)
    {
        GLint alphaBits = 0;
        fGetIntegerv(LOCAL_GL_ALPHA_BITS, &alphaBits);
        if (!alphaBits) {
            const uint32_t alphaMask = gfxPackedPixelNoPreMultiply(0xff,0,0,0);

            dest->Flush();
            uint32_t* itr = (uint32_t*)dest->Data();
            uint32_t testPixel = *itr;
            if ((testPixel & alphaMask) != alphaMask) {
                // We need to set the alpha channel to 1.0 manually.
                uint32_t* itrEnd = itr + width*height;  // Stride is guaranteed to be width*4.

                for (; itr != itrEnd; itr++) {
                    *itr |= alphaMask;
                }
            }
            dest->MarkDirty();
        }
    }
#endif
}

#ifdef MOZ_ENABLE_GL_TRACKING
void
GLContext::CreatedProgram(GLContext *aOrigin, GLuint aName)
{
    mTrackedPrograms.AppendElement(NamedResource(aOrigin, aName));
}

void
GLContext::CreatedShader(GLContext *aOrigin, GLuint aName)
{
    mTrackedShaders.AppendElement(NamedResource(aOrigin, aName));
}

void
GLContext::CreatedBuffers(GLContext *aOrigin, GLsizei aCount, GLuint *aNames)
{
    for (GLsizei i = 0; i < aCount; ++i) {
        mTrackedBuffers.AppendElement(NamedResource(aOrigin, aNames[i]));
    }
}

void
GLContext::CreatedQueries(GLContext *aOrigin, GLsizei aCount, GLuint *aNames)
{
    for (GLsizei i = 0; i < aCount; ++i) {
        mTrackedQueries.AppendElement(NamedResource(aOrigin, aNames[i]));
    }
}

void
GLContext::CreatedTextures(GLContext *aOrigin, GLsizei aCount, GLuint *aNames)
{
    for (GLsizei i = 0; i < aCount; ++i) {
        mTrackedTextures.AppendElement(NamedResource(aOrigin, aNames[i]));
    }
}

void
GLContext::CreatedFramebuffers(GLContext *aOrigin, GLsizei aCount, GLuint *aNames)
{
    for (GLsizei i = 0; i < aCount; ++i) {
        mTrackedFramebuffers.AppendElement(NamedResource(aOrigin, aNames[i]));
    }
}

void
GLContext::CreatedRenderbuffers(GLContext *aOrigin, GLsizei aCount, GLuint *aNames)
{
    for (GLsizei i = 0; i < aCount; ++i) {
        mTrackedRenderbuffers.AppendElement(NamedResource(aOrigin, aNames[i]));
    }
}

static void
RemoveNamesFromArray(GLContext *aOrigin, GLsizei aCount, const GLuint *aNames, nsTArray<GLContext::NamedResource>& aArray)
{
    for (GLsizei j = 0; j < aCount; ++j) {
        GLuint name = aNames[j];
        // name 0 can be ignored
        if (name == 0)
            continue;

        for (uint32_t i = 0; i < aArray.Length(); ++i) {
            if (aArray[i].name == name) {
                aArray.RemoveElementAt(i);
                break;
            }
        }
    }
}

void
GLContext::DeletedProgram(GLContext *aOrigin, GLuint aName)
{
    RemoveNamesFromArray(aOrigin, 1, &aName, mTrackedPrograms);
}

void
GLContext::DeletedShader(GLContext *aOrigin, GLuint aName)
{
    RemoveNamesFromArray(aOrigin, 1, &aName, mTrackedShaders);
}

void
GLContext::DeletedBuffers(GLContext *aOrigin, GLsizei aCount, GLuint *aNames)
{
    RemoveNamesFromArray(aOrigin, aCount, aNames, mTrackedBuffers);
}

void
GLContext::DeletedQueries(GLContext *aOrigin, GLsizei aCount, const GLuint *aNames)
{
    RemoveNamesFromArray(aOrigin, aCount, aNames, mTrackedQueries);
}

void
GLContext::DeletedTextures(GLContext *aOrigin, GLsizei aCount, GLuint *aNames)
{
    RemoveNamesFromArray(aOrigin, aCount, aNames, mTrackedTextures);
}

void
GLContext::DeletedFramebuffers(GLContext *aOrigin, GLsizei aCount, GLuint *aNames)
{
    RemoveNamesFromArray(aOrigin, aCount, aNames, mTrackedFramebuffers);
}

void
GLContext::DeletedRenderbuffers(GLContext *aOrigin, GLsizei aCount, GLuint *aNames)
{
    RemoveNamesFromArray(aOrigin, aCount, aNames, mTrackedRenderbuffers);
}

static void
MarkContextDestroyedInArray(GLContext *aContext, nsTArray<GLContext::NamedResource>& aArray)
{
    for (uint32_t i = 0; i < aArray.Length(); ++i) {
        if (aArray[i].origin == aContext)
            aArray[i].originDeleted = true;
    }
}

void
GLContext::SharedContextDestroyed(GLContext *aChild)
{
    MarkContextDestroyedInArray(aChild, mTrackedPrograms);
    MarkContextDestroyedInArray(aChild, mTrackedShaders);
    MarkContextDestroyedInArray(aChild, mTrackedTextures);
    MarkContextDestroyedInArray(aChild, mTrackedFramebuffers);
    MarkContextDestroyedInArray(aChild, mTrackedRenderbuffers);
    MarkContextDestroyedInArray(aChild, mTrackedBuffers);
    MarkContextDestroyedInArray(aChild, mTrackedQueries);
}

static void
ReportArrayContents(const char *title, const nsTArray<GLContext::NamedResource>& aArray)
{
    if (aArray.Length() == 0)
        return;

    printf_stderr("%s:\n", title);

    nsTArray<GLContext::NamedResource> copy(aArray);
    copy.Sort();

    GLContext *lastContext = nullptr;
    for (uint32_t i = 0; i < copy.Length(); ++i) {
        if (lastContext != copy[i].origin) {
            if (lastContext)
                printf_stderr("\n");
            printf_stderr("  [%p - %s] ", copy[i].origin, copy[i].originDeleted ? "deleted" : "live");
            lastContext = copy[i].origin;
        }
        printf_stderr("%d ", copy[i].name);
    }
    printf_stderr("\n");
}

void
GLContext::ReportOutstandingNames()
{
    if (!DebugMode())
        return;

    printf_stderr("== GLContext %p Outstanding ==\n", this);

    ReportArrayContents("Outstanding Textures", mTrackedTextures);
    ReportArrayContents("Outstanding Buffers", mTrackedBuffers);
    ReportArrayContents("Outstanding Queries", mTrackedQueries);
    ReportArrayContents("Outstanding Programs", mTrackedPrograms);
    ReportArrayContents("Outstanding Shaders", mTrackedShaders);
    ReportArrayContents("Outstanding Framebuffers", mTrackedFramebuffers);
    ReportArrayContents("Outstanding Renderbuffers", mTrackedRenderbuffers);
}

#endif /* DEBUG */


void
GLContext::GuaranteeResolve()
{
    if (mScreen) {
        mScreen->AssureBlitted();
    }
    fFinish();
}

const gfxIntSize&
GLContext::OffscreenSize() const
{
    MOZ_ASSERT(IsOffscreen());
    return mScreen->Size();
}

bool
GLContext::CreateScreenBufferImpl(const gfxIntSize& size, const SurfaceCaps& caps)
{
    GLScreenBuffer* newScreen = GLScreenBuffer::Create(this, size, caps);
    if (!newScreen)
        return false;

    if (!newScreen->Resize(size)) {
        delete newScreen;
        return false;
    }

    DestroyScreenBuffer();

    // This will rebind to 0 (Screen) if needed when
    // it falls out of scope.
    ScopedBindFramebuffer autoFB(this);

    mScreen = newScreen;

    return true;
}

bool
GLContext::ResizeScreenBuffer(const gfxIntSize& size)
{
    if (!IsOffscreenSizeAllowed(size))
        return false;

    return mScreen->Resize(size);
}


void
GLContext::DestroyScreenBuffer()
{
    delete mScreen;
    mScreen = nullptr;
}

void
GLContext::ForceDirtyScreen()
{
    ScopedBindFramebuffer autoFB(0);

    BeforeGLDrawCall();
    // no-op; just pretend we did something
    AfterGLDrawCall();
}

void
GLContext::CleanDirtyScreen()
{
    ScopedBindFramebuffer autoFB(0);

    BeforeGLReadCall();
    // no-op; we just want to make sure the Read FBO is updated if it needs to be
    AfterGLReadCall();
}

void
GLContext::EmptyTexGarbageBin()
{
   TexGarbageBin()->EmptyGarbage();
}

bool
GLContext::IsOffscreenSizeAllowed(const gfxIntSize& aSize) const {
  int32_t biggerDimension = std::max(aSize.width, aSize.height);
  int32_t maxAllowed = std::min(mMaxRenderbufferSize, mMaxTextureSize);
  return biggerDimension <= maxAllowed;
}

bool
GLContext::IsOwningThreadCurrent()
{
  return NS_GetCurrentThread() == mOwningThread;
}

void
GLContext::DispatchToOwningThread(nsIRunnable *event)
{
    // Before dispatching, we need to ensure we're not in the middle of
    // shutting down. Dispatching runnables in the middle of shutdown
    // (that is, when the main thread is no longer get-able) can cause them
    // to leak. See Bug 741319, and Bug 744115.
    nsCOMPtr<nsIThread> mainThread;
    if (NS_SUCCEEDED(NS_GetMainThread(getter_AddRefs(mainThread)))) {
        mOwningThread->Dispatch(event, NS_DISPATCH_NORMAL);
    }
}

GLBlitHelper*
GLContext::BlitHelper()
{
    if (!mBlitHelper) {
        mBlitHelper = new GLBlitHelper(this);
    }

    return mBlitHelper;
}

GLBlitTextureImageHelper*
GLContext::BlitTextureImageHelper()
{
    if (!mBlitTextureImageHelper) {
        mBlitTextureImageHelper = new GLBlitTextureImageHelper(this);
    }

    return mBlitTextureImageHelper;
}

bool
DoesStringMatch(const char* aString, const char *aWantedString)
{
    if (!aString || !aWantedString)
        return false;

    const char *occurrence = strstr(aString, aWantedString);

    // aWanted not found
    if (!occurrence)
        return false;

    // aWantedString preceded by alpha character
    if (occurrence != aString && isalpha(*(occurrence-1)))
        return false;

    // aWantedVendor followed by alpha character
    const char *afterOccurrence = occurrence + strlen(aWantedString);
    if (isalpha(*afterOccurrence))
        return false;

    return true;
}

} /* namespace gl */
} /* namespace mozilla */
