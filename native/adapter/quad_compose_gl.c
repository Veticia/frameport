// SPDX-License-Identifier: GPL-3.0-only
// Included by frame_adapter.c after snapshot_gl.c (whose swapchain image tracking it uses). projection_only=2 (GLES
// sessions only): before a frame with a projection layer and more layers is reduced to the projection layer alone,
// the quad layers submitted after the projection layer are drawn into its eye images, so menus and overlays drawn as
// quads still show. For runtimes that refuse multi-layer GLES frames (AXRB 1.0.4's emulator: -2 for every one;
// Path of the Warrior's menus are a VrApi cylinder the VrApi bridge turns into a quad, Accounting+ uses OVROverlay
// quads).
//  * Drawn on the game's thread, in xrEndFrame, on its current GL context (the one the runtime reads the images back
//    from), with the eye image the game just released as the target: one textured quad per eye and quad layer, at
//    the eye's view and FOV (lm_quad_clip). No depth test: a quad is drawn over the scene (as a compositor does).
//  * Blending follows the quad's flags: XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT blends (premultiplied, or
//    straight alpha with UNPREMULTIPLIED_ALPHA_BIT), else the quad is opaque. Array quad swapchains are skipped.
//  * Every GL state it touches is restored and its errors are cleared. A failure (shader, framebuffer) is logged once
//    and turns the drawing off for the process; the frame still goes out as the projection layer alone.
#define QC_GL_FUNCS(X) X(glCreateShader) X(glShaderSource) X(glCompileShader) X(glGetShaderiv) X(glGetShaderInfoLog) \
    X(glCreateProgram) X(glAttachShader) X(glLinkProgram) X(glGetProgramiv) X(glDeleteShader) X(glUseProgram) \
    X(glGetUniformLocation) X(glUniform4fv) X(glUniform1i) X(glUniform1f) X(glGenVertexArrays) X(glBindVertexArray) \
    X(glGenFramebuffers) X(glBindFramebuffer) X(glFramebufferTexture2D) X(glFramebufferTextureLayer) \
    X(glCheckFramebufferStatus) X(glViewport) X(glGetIntegerv) X(glGetBooleanv) X(glIsEnabled) X(glEnable) \
    X(glDisable) X(glBlendFuncSeparate) X(glBlendEquationSeparate) X(glColorMask) X(glActiveTexture) \
    X(glBindTexture) X(glGenSamplers) X(glSamplerParameteri) X(glBindSampler) X(glDrawArrays) X(glGetError)
#define QC_DECLARE(fn) static __typeof__(&fn) q_##fn;
QC_GL_FUNCS(QC_DECLARE)
#undef QC_DECLARE

static int qc_state;  // 0 untried, 1 ok, -1 off for this process
static EGLContext qc_context;  // the context the objects below belong to (VAOs and FBOs aren't shared)
static GLuint qc_program, qc_vao, qc_fb, qc_sampler;
static GLint qc_u_v, qc_u_tex, qc_u_opaque;

static const char *QC_VS =
    "#version 300 es\n"
    "uniform vec4 u_v[8];\n"  // 0-3 clip-space corners (BL, BR, TL, TR), 4-7 their texture coordinates
    "out vec2 v_uv;\n"
    "void main() { gl_Position = u_v[gl_VertexID]; v_uv = u_v[4 + gl_VertexID].xy; }\n";
static const char *QC_FS =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "uniform float u_opaque;\n"
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "void main() { vec4 c = texture(u_tex, v_uv); o_color = vec4(c.rgb, max(c.a, u_opaque)); }\n";

static void qc_off(const char *why) {
    qc_state = -1;
    LOG("projection_only=2: %s, quad layers are dropped instead of drawn into the main picture", why);
}

static GLuint qc_shader(GLenum type, const char *src) {
    GLuint s = q_glCreateShader(type);
    q_glShaderSource(s, 1, &src, NULL);
    q_glCompileShader(s);
    GLint ok = 0;
    q_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[256] = {0};
        q_glGetShaderInfoLog(s, sizeof(log) - 1, NULL, log);
        LOG("projection_only=2: shader: %s", log);
        q_glDeleteShader(s);
        return 0;
    }
    return s;
}

// GL entry points + this context's objects. 1 = ready.
static int qc_setup(void) {
    if (qc_state < 0) return 0;
    if (!qc_state) {
        if (!snap_load_gl()) { qc_off("OpenGL ES not available"); return 0; }
        void *gles = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
        int ok = gles != NULL;
#define QC_RESOLVE(fn) ok &= gles && (q_##fn = (__typeof__(q_##fn))dlsym(gles, #fn)) != NULL;
        QC_GL_FUNCS(QC_RESOLVE)
#undef QC_RESOLVE
        if (!ok) { qc_off("GL functions missing"); return 0; }
        qc_state = 1;
    }
    EGLContext ctx = s_eglGetCurrentContext();
    if (!ctx) return 0;
    if (ctx == qc_context && qc_program) return 1;
    // a new context (or the first): build the program and objects there (the old ones belonged to another context)
    GLint old_program = 0;
    q_glGetIntegerv(GL_CURRENT_PROGRAM, &old_program);
    GLuint vs = qc_shader(GL_VERTEX_SHADER, QC_VS), fs = qc_shader(GL_FRAGMENT_SHADER, QC_FS);
    GLuint program = 0;
    if (vs && fs) {
        program = q_glCreateProgram();
        q_glAttachShader(program, vs);
        q_glAttachShader(program, fs);
        q_glLinkProgram(program);
        GLint linked = 0;
        q_glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) program = 0;
    }
    if (vs) q_glDeleteShader(vs);
    if (fs) q_glDeleteShader(fs);
    if (!program) { while (q_glGetError() != GL_NO_ERROR) {} qc_off("shader program failed"); return 0; }
    qc_program = program;
    qc_u_v = q_glGetUniformLocation(program, "u_v");
    qc_u_tex = q_glGetUniformLocation(program, "u_tex");
    qc_u_opaque = q_glGetUniformLocation(program, "u_opaque");
    q_glGenVertexArrays(1, &qc_vao);
    q_glGenFramebuffers(1, &qc_fb);
    q_glGenSamplers(1, &qc_sampler);
    q_glSamplerParameteri(qc_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);  // no mipmaps needed (and none required)
    q_glSamplerParameteri(qc_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    q_glSamplerParameteri(qc_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    q_glSamplerParameteri(qc_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    q_glUseProgram((GLuint)old_program);
    qc_context = ctx;
    return 1;
}

// The GL texture (and size) of the image the game last acquired (= just released) from a swapchain; 0 if unknown.
static GLuint qc_image(XrSwapchain handle, uint32_t *w, uint32_t *h, uint32_t *array_size) {
    GLuint tex = 0;
    pthread_mutex_lock(&snap_lock);
    int i = snap_find(handle, 0);
    if (i >= 0 && snap_chains[i].count && snap_chains[i].last_acquired < snap_chains[i].count) {
        tex = snap_chains[i].images[snap_chains[i].last_acquired];
        *w = snap_chains[i].width, *h = snap_chains[i].height, *array_size = snap_chains[i].array_size;
    }
    pthread_mutex_unlock(&snap_lock);
    return tex;
}

static void qc_compose(const XrFrameEndInfo *info, uint32_t proj_index) {
    if (projection_only != 2 || !info || !info->layers) return;
    const XrCompositionLayerProjection *proj = (const XrCompositionLayerProjection *)info->layers[proj_index];
    if (!proj || proj->viewCount < 2 || !proj->views) return;
    uint32_t quads = 0;
    for (uint32_t i = proj_index + 1; i < info->layerCount; ++i)
        if (info->layers[i] && info->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_QUAD) ++quads;
    if (!quads || !qc_setup()) return;

    // the game's state we change
    GLint draw_fb, viewport[4], program, vao, active_tex, tex2d, sampler, src_rgb, dst_rgb, src_a, dst_a, eq_rgb, eq_a;
    GLboolean color_mask[4];
    const GLenum caps[] = {GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_SCISSOR_TEST, GL_CULL_FACE,
                           GL_SAMPLE_ALPHA_TO_COVERAGE, GL_RASTERIZER_DISCARD};
    GLboolean cap_on[sizeof(caps) / sizeof(caps[0])];
    q_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
    q_glGetIntegerv(GL_VIEWPORT, viewport);
    q_glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    q_glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    q_glGetIntegerv(GL_ACTIVE_TEXTURE, &active_tex);
    q_glActiveTexture(GL_TEXTURE0);
    q_glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex2d);
    q_glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
    q_glGetIntegerv(GL_BLEND_SRC_RGB, &src_rgb);
    q_glGetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
    q_glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_a);
    q_glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_a);
    q_glGetIntegerv(GL_BLEND_EQUATION_RGB, &eq_rgb);
    q_glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &eq_a);
    q_glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
    for (size_t c = 0; c < sizeof(caps) / sizeof(caps[0]); ++c) {
        cap_on[c] = q_glIsEnabled(caps[c]);
        q_glDisable(caps[c]);
    }
    q_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    q_glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    q_glUseProgram(qc_program);
    q_glBindVertexArray(qc_vao);
    q_glBindSampler(0, qc_sampler);
    q_glUniform1i(qc_u_tex, 0);
    q_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, qc_fb);

    PFN_xrLocateSpace locate = (PFN_xrLocateSpace)lookup(active_instance, "xrLocateSpace");
    int drawn = 0;
    for (uint32_t eye = 0; eye < 2; ++eye) {
        const XrCompositionLayerProjectionView *view = &proj->views[eye];
        uint32_t tw = 0, th = 0, tarray = 1;
        GLuint target = qc_image(view->subImage.swapchain, &tw, &th, &tarray);
        if (!target) continue;
        if (tarray > 1)
            q_glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, 0,
                                        (GLint)view->subImage.imageArrayIndex);
        else
            q_glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
        if (q_glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            qc_off("eye image not drawable (framebuffer incomplete)");
            break;
        }
        const XrRect2Di *r = &view->subImage.imageRect;
        q_glViewport(r->offset.x, r->offset.y, r->extent.width, r->extent.height);
        for (uint32_t i = proj_index + 1; i < info->layerCount; ++i) {
            const XrCompositionLayerQuad *q = (const XrCompositionLayerQuad *)info->layers[i];
            if (!q || q->type != XR_TYPE_COMPOSITION_LAYER_QUAD) continue;
            if (q->eyeVisibility == XR_EYE_VISIBILITY_LEFT && eye != 0) continue;
            if (q->eyeVisibility == XR_EYE_VISIBILITY_RIGHT && eye != 1) continue;
            uint32_t sw = 0, sh = 0, sarray = 1;
            GLuint source = qc_image(q->subImage.swapchain, &sw, &sh, &sarray);
            if (!source || sarray > 1 || !sw || !sh) continue;
            XrPosef pose = q->pose;  // the quad in the projection layer's space
            if (q->space != proj->space) {
                XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
                if (!locate || XR_FAILED(locate(q->space, proj->space, info->displayTime, &loc))) continue;
                pose = lm_pose_mul(loc.pose, q->pose);
            }
            float v[8][4];
            lm_quad_clip(pose, q->size, view->pose, view->fov, v);
            const XrRect2Di *s = &q->subImage.imageRect;
            float u0 = (float)s->offset.x / sw, u1 = (float)(s->offset.x + s->extent.width) / sw;
            float v0 = (float)s->offset.y / sh, v1 = (float)(s->offset.y + s->extent.height) / sh;
            for (int k = 0; k < 4; ++k) {
                v[4 + k][0] = k & 1 ? u1 : u0;
                v[4 + k][1] = k & 2 ? v1 : v0;  // GL textures: v grows upwards, the quad's top is +Y
                v[4 + k][2] = v[4 + k][3] = 0;
            }
            q_glUniform4fv(qc_u_v, 8, &v[0][0]);
            const int blend = (q->layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) != 0;
            q_glUniform1f(qc_u_opaque, blend ? 0.0f : 1.0f);
            if (blend) {
                q_glEnable(GL_BLEND);
                if (q->layerFlags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT)
                    q_glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                else
                    q_glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            } else {
                q_glDisable(GL_BLEND);
            }
            q_glBindTexture(GL_TEXTURE_2D, source);
            q_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            ++drawn;
        }
    }

    // the game's state back
    q_glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    q_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)draw_fb);
    q_glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    q_glBindTexture(GL_TEXTURE_2D, (GLuint)tex2d);
    q_glBindSampler(0, (GLuint)sampler);
    q_glActiveTexture((GLenum)active_tex);
    q_glBindVertexArray((GLuint)vao);
    q_glUseProgram((GLuint)program);
    q_glBlendFuncSeparate((GLenum)src_rgb, (GLenum)dst_rgb, (GLenum)src_a, (GLenum)dst_a);
    q_glBlendEquationSeparate((GLenum)eq_rgb, (GLenum)eq_a);
    q_glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
    for (size_t c = 0; c < sizeof(caps) / sizeof(caps[0]); ++c)
        if (cap_on[c]) q_glEnable(caps[c]); else q_glDisable(caps[c]);
    GLenum err, first = GL_NO_ERROR;
    while ((err = q_glGetError()) != GL_NO_ERROR) if (first == GL_NO_ERROR) first = err;
    static int logged;
    if (drawn && !logged++) LOG("projection_only=2: %u quad layer(s) drawn into the main picture", quads);
    if (first != GL_NO_ERROR) {
        static int errors;
        if (errors++ < 3) LOG("projection_only=2: GL error 0x%x while drawing quads", first);
    }
}
