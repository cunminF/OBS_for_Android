// 阶段 1 门票：GLES 3.x 最小验证。
//
// 这一步要替整个「方案 A：移植 libobs-opengl 到 GLES」兜底，所以只碰原生 EGL/GLES，
// 不借 Qt 的渲染封装 —— libobs 将来就是自己 eglMakeCurrent，Qt 帮不上忙。
// 覆盖：EGL 显示/配置/pbuffer → GLES 3.1 上下文 → #version 310 es 着色器 →
//       纹理 FBO → 全屏三角形 → glReadPixels 比对像素 → 关键扩展探测。

#include "gles_probe.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <android/log.h>

#include <QStringList>

#define GLLOG(...) __android_log_print(ANDROID_LOG_INFO, "M0-GLES", __VA_ARGS__)

namespace {

const char *kVertexSrc =
    "#version 310 es\n"
    "in vec2 a_pos;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = a_pos * 0.5 + 0.5;\n"
    "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

// 故意用 ES 专属写法：precision 必填、无 gl_FragColor、in/out 代替 attribute/varying。
const char *kFragmentSrc =
    "#version 310 es\n"
    "precision mediump float;\n"
    "in vec2 v_uv;\n"
    "out vec4 frag_color;\n"
    "void main() {\n"
    "    frag_color = vec4(v_uv.x, v_uv.y, 0.5, 1.0);\n"
    "}\n";

const int kSide = 64;

struct GlReport {
    QStringList lines;
    bool ok = true;
    void add(const QString &s) { lines << s; }
    void fail(const QString &s)
    {
        lines << ("FAIL " + s);
        ok = false;
    }
};

bool compileStage(GLenum type, const char *src, GLuint *out, QString *err)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint status = GL_FALSE;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
    if (status == GL_FALSE) {
        GLint len = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        QByteArray buf(len > 1 ? len : 1, '\0');
        glGetShaderInfoLog(sh, buf.size(), nullptr, buf.data());
        *err = QString::fromUtf8(buf.constData()).trimmed();
        glDeleteShader(sh);
        return false;
    }
    *out = sh;
    return true;
}

} // namespace

QString runGlesProbe()
{
    GlReport r;

    r.add(QStringLiteral("=== GLES 3.x 最小验证 ==="));

    // --- 1. EGL display + 版本 -------------------------------------------
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) {
        r.fail(QStringLiteral("eglGetDisplay(EGL_DEFAULT_DISPLAY) 失败: 0x%x").arg((unsigned)eglGetError(), 0, 16));
        return r.lines.join('\n');
    }
    EGLint major = 0, minor = 0;
    if (eglInitialize(dpy, &major, &minor) != EGL_TRUE) {
        r.fail(QStringLiteral("eglInitialize 失败: 0x%x").arg((unsigned)eglGetError(), 0, 16));
        return r.lines.join('\n');
    }
    r.add(QStringLiteral("EGL %1.%2  client=%3").arg(major).arg(minor).arg(eglQueryString(dpy, EGL_VERSION)));

    // --- 2. 选一个可离屏渲染的 ES3.1 配置 ---------------------------------
    const EGLint cfgAttribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg = nullptr;
    EGLint ncfg = 0;
    if (eglChooseConfig(dpy, cfgAttribs, &cfg, 1, &ncfg) != EGL_TRUE || ncfg < 1) {
        r.fail(QStringLiteral("无 ES3 + 8888 + PBUFFER 配置 (err=0x%1, n=%2)")
                   .arg((unsigned)eglGetError(), 0, 16).arg(ncfg));
        eglTerminate(dpy);
        return r.lines.join('\n');
    }
    r.add(QStringLiteral("config OK: ES3 + RGBA8 + depth24 + stencil8"));

    EGLint winAttribs[] = { EGL_WIDTH, kSide, EGL_HEIGHT, kSide, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, winAttribs);
    if (surf == EGL_NO_SURFACE) {
        r.fail(QStringLiteral("eglCreatePbufferSurface 失败: 0x%x").arg((unsigned)eglGetError(), 0, 16));
        eglTerminate(dpy);
        return r.lines.join('\n');
    }

    // --- 3. GLES 3.1 上下文（拿不到就退回 3.0，如实记录） ------------------
    EGLContext ctx = EGL_NO_CONTEXT;
    const EGLint ctx31[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 1, EGL_NONE };
    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx31);
    int wantMinor = 1;
    if (ctx == EGL_NO_CONTEXT) {
        eglGetError();
        const EGLint ctx30[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
        ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx30);
        wantMinor = 0;
        r.add(QStringLiteral("注意: 3.1 上下文创建失败，退回 3.0"));
    }
    if (ctx == EGL_NO_CONTEXT) {
        r.fail(QStringLiteral("无法创建 GLES3 上下文: 0x%x").arg((unsigned)eglGetError(), 0, 16));
        eglDestroySurface(dpy, surf);
        eglTerminate(dpy);
        return r.lines.join('\n');
    }
    if (eglMakeCurrent(dpy, surf, surf, ctx) != EGL_TRUE) {
        r.fail(QStringLiteral("eglMakeCurrent 失败: 0x%x").arg((unsigned)eglGetError(), 0, 16));
        eglDestroyContext(dpy, ctx);
        eglDestroySurface(dpy, surf);
        eglTerminate(dpy);
        return r.lines.join('\n');
    }
    r.add(QStringLiteral("context OK (请求 3.%1): %2").arg(wantMinor).arg((const char *)glGetString(GL_VERSION)));
    r.add(QStringLiteral("GLSL: %1").arg((const char *)glGetString(GL_SHADING_LANGUAGE_VERSION)));
    r.add(QStringLiteral("renderer: %1 / %2").arg((const char *)glGetString(GL_VENDOR)).arg((const char *)glGetString(GL_RENDERER)));

    GLint maj = 0, min = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &maj);
    glGetIntegerv(GL_MINOR_VERSION, &min);
    if (maj < 3 || (maj == 3 && min < 1))
        r.fail(QStringLiteral("实际 GLES 版本 %1.%2 < 3.1（方案 A 依赖 3.1 的 UBO/VAO）").arg(maj).arg(min));
    else
        r.add(QStringLiteral("GLES %1.%2 满足 ≥3.1").arg(maj).arg(min));

    GLint maxTex = 0, maxCb = 0, maxVi = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
    glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &maxCb);
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &maxVi);
    r.add(QStringLiteral("limits: maxTex=%1 colorAttach=%2 vertexAttribs=%3").arg(maxTex).arg(maxCb).arg(maxVi));

    // --- 4. #version 310 es 着色器 ---------------------------------------
    GLuint vs = 0, fs = 0;
    QString err;
    if (!compileStage(GL_VERTEX_SHADER, kVertexSrc, &vs, &err))
        r.fail(QStringLiteral("顶点着色器编译失败: %1").arg(err));
    if (!compileStage(GL_FRAGMENT_SHADER, kFragmentSrc, &fs, &err))
        r.fail(QStringLiteral("片元着色器编译失败: %1").arg(err));

    GLuint prog = 0;
    if (vs && fs) {
        prog = glCreateProgram();
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glBindAttribLocation(prog, 0, "a_pos");
        glLinkProgram(prog);
        GLint linked = GL_FALSE;
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        if (linked == GL_FALSE) {
            GLint len = 0;
            glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
            QByteArray buf(len > 1 ? len : 1, '\0');
            glGetProgramInfoLog(prog, buf.size(), nullptr, buf.data());
            r.fail(QStringLiteral("链接失败: %1").arg(QString::fromUtf8(buf.constData()).trimmed()));
            glDeleteProgram(prog);
            prog = 0;
        } else {
            r.add(QStringLiteral("#version 310 es 着色器编译+链接通过"));
        }
    }
    if (vs)
        glDeleteShader(vs);
    if (fs)
        glDeleteShader(fs);

    // --- 5. 纹理 FBO + 绘制 + 读回 ---------------------------------------
    if (prog) {
        GLuint tex = 0, fbo = 0, vao = 0, vbo = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kSide, kSide, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) {
            r.fail(QStringLiteral("FBO 不完整: 0x%x").arg((unsigned)st, 0, 16));
        } else {
            static const GLfloat quad[] = { -1, -1, 3, -1, -1, 3 }; // 单个放大三角形覆盖全屏
            glGenVertexArrays(1, &vao);
            glBindVertexArray(vao);
            glGenBuffers(1, &vbo);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

            glViewport(0, 0, kSide, kSide);
            glUseProgram(prog);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glFinish();

            unsigned char px[4] = { 0, 0, 0, 0 };
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glReadPixels(kSide / 2, kSide / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);

            // 中心点 v_uv≈(0.5,0.5) → (127,127,128)
            const bool pass = px[0] > 110 && px[0] < 145 && px[1] > 110 && px[1] < 145 && px[2] > 110 && px[2] < 145 && px[3] > 240;
            if (pass)
                r.add(QStringLiteral("离屏 FBO 绘制+glReadPixels 正确: 中心像素 RGBA(%1,%2,%3,%4)")
                          .arg(px[0]).arg(px[1]).arg(px[2]).arg(px[3]));
            else
                r.fail(QStringLiteral("读回像素不符预期: RGBA(%1,%2,%3,%4)，期望约 (127,127,128,255)")
                           .arg(px[0]).arg(px[1]).arg(px[2]).arg(px[3]));
        }

        GLenum glerr = GL_NO_ERROR;
        int shown = 0;
        while ((glerr = glGetError()) != GL_NO_ERROR && shown++ < 4)
            r.add(QStringLiteral("glGetError: 0x%1").arg((unsigned)glerr, 0, 16));

        if (vbo)
            glDeleteBuffers(1, &vbo);
        if (vao)
            glDeleteVertexArrays(1, &vao);
        if (fbo)
            glDeleteFramebuffers(1, &fbo);
        if (tex)
            glDeleteTextures(1, &tex);
        if (prog)
            glDeleteProgram(prog);
    }

    // --- 6. 方案 A 关心的扩展 --------------------------------------------
    {
        QByteArray exts((const char *)glGetString(GL_EXTENSIONS));
        static const char *wanted[] = {
            "GL_EXT_color_buffer_float",       // 16F/32F 渲染目标（部分特效/滤镜）
            "GL_EXT_color_buffer_half_float",  // 低一档的替代
            "GL_OES_EGL_image_external",       // 相机/硬解零拷贝上屏
            "GL_EXT_texture_format_BGRA8888",  // OBS 内部大量按 BGRA 走
            "GL_NV_fence",
            "GL_EXT_discard_framebuffer"
        };
        QStringList have, missing;
        for (const char *w : wanted)
            (exts.contains(w) ? have : missing) << w;
        r.add(QStringLiteral("扩展 有: %1").arg(have.isEmpty() ? QStringLiteral("(无)") : have.join(' ')));
        if (!missing.isEmpty())
            r.add(QStringLiteral("扩展 缺: %1").arg(missing.join(' ')));
    }
    {
        QByteArray eext = eglQueryString(dpy, EGL_EXTENSIONS) ? eglQueryString(dpy, EGL_EXTENSIONS) : "";
        const bool imgBase = eext.contains("EGL_KHR_image_base") || eext.contains("EGL_KHR_image");
        r.add(QStringLiteral("EGL image 零拷贝: %1").arg(imgBase ? QStringLiteral("可用") : QStringLiteral("不可用")));
    }

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglDestroySurface(dpy, surf);
    eglTerminate(dpy);

    r.add(r.ok ? QStringLiteral("=== 结论：通过，可以开工 libobs-opengl 的 GLES 移植 ===")
               : QStringLiteral("=== 结论：未通过，见上面 FAIL ==="));

    const QString out = r.lines.join('\n');
    for (const QString &l : r.lines)
        GLLOG("%s", l.toUtf8().constData());
    return out;
}
