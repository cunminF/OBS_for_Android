/* 阶段 3-2 前置探针，要回答的三条问题（W / Q / S）见 gl_share_probe.h 头部注释。 */

#include "gl_share_probe.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
/* 顺序有讲究：gl2ext.h 自己不带 GLenum/GL_APIENTRY 这些基础定义（它假定 gl2.h 已在前面），
 * 必须先包含 gl31.h（→ gl3.h → gl2.h），否则整片 typedef 解析失败。 */
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>
#include <android/log.h>

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QEventLoop>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLWidget>
#include <QSurfaceFormat>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include <QStringList>

#include <dlfcn.h>

#include <cstring>
#include <functional>

#define PPLOG(...) __android_log_print(ANDROID_LOG_INFO, "32-PROBE", "%s", __VA_ARGS__)

namespace {

constexpr int kSide = 64;

struct Color {
	uint8_t r, g, b, a;
	const char *name;
};

/* 谁画的就用哪个颜色，读回来一眼可知内容有没有真的穿过上下文边界。 */
const Color kC1{200, 30, 40, 255, "C1"};  /* 裸上下文 A 画的：share group 正例 */
const Color kC2{30, 200, 90, 255, "C2"};  /* A 后改的：验 EGLImage 是否随帧传播 */
const Color kC4{220, 180, 20, 255, "C4"}; /* 与 Qt 同组的外部上下文画的：模拟 OBS 侧 */
const Color kC5{70, 210, 210, 255, "C5"}; /* Qt 上下文画的：反向（OBS 采 Qt） */

struct Rep {
	QStringList lines;
	bool ok = true;
	void add(const QString &s) { lines << s; }
	void fail(const QString &s)
	{
		lines << ("FAIL " + s);
		ok = false;
	}
	void note(const QString &s) { lines << ("注 " + s); }
};

QString eglErrStr()
{
	return QStringLiteral("0x%1").arg((unsigned)eglGetError(), 0, 16);
}

QString px(const uint8_t *p)
{
	return QStringLiteral("%1,%2,%3,%4").arg(p[0]).arg(p[1]).arg(p[2]).arg(p[3]);
}

QString want(const Color &c)
{
	return QStringLiteral("%1=%2,%3,%4,%5").arg(c.name).arg(c.r).arg(c.g).arg(c.b).arg(c.a);
}

bool closeTo(const uint8_t *p, const Color &c, int tol = 4)
{
	return qAbs(int(p[0]) - int(c.r)) <= tol && qAbs(int(p[1]) - int(c.g)) <= tol &&
	       qAbs(int(p[2]) - int(c.b)) <= tol && qAbs(int(p[3]) - int(c.a)) <= tol;
}

const char *kVS = "#version 310 es\n"
		  "in vec2 a_pos;\n"
		  "out vec2 v_uv;\n"
		  "void main(){ v_uv = a_pos*0.5+0.5; gl_Position = vec4(a_pos,0.0,1.0); }\n";

const char *kFS = "#version 310 es\n"
		  "precision mediump float;\n"
		  "in vec2 v_uv;\n"
		  "uniform sampler2D u_tex;\n"
		  "out vec4 frag_color;\n"
		  "void main(){ frag_color = texture(u_tex, v_uv); }\n";

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

/* 着色器程序**不是**可共享对象，每个上下文必须自己建一份。 */
GLuint makeSamplerProgram(Rep &r, const char *who)
{
	GLuint vs = 0, fs = 0, prog = 0;
	QString err;
	if (!compileStage(GL_VERTEX_SHADER, kVS, &vs, &err)) {
		r.fail(QStringLiteral("%1 顶点着色器失败: %2").arg(who, err));
		return 0;
	}
	if (!compileStage(GL_FRAGMENT_SHADER, kFS, &fs, &err)) {
		r.fail(QStringLiteral("%1 片元着色器失败: %2").arg(who, err));
		glDeleteShader(vs);
		return 0;
	}
	prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glBindAttribLocation(prog, 0, "a_pos");
	glLinkProgram(prog);
	glDeleteShader(vs);
	glDeleteShader(fs);
	GLint linked = GL_FALSE;
	glGetProgramiv(prog, GL_LINK_STATUS, &linked);
	if (linked == GL_FALSE) {
		r.fail(QStringLiteral("%1 着色器程序链接失败").arg(who));
		glDeleteProgram(prog);
		return 0;
	}
	glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);
	return prog;
}

GLuint makeRgba8Texture()
{
	GLuint tex = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kSide, kSide, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return tex;
}

bool fillTexture(GLuint tex, const Color &c, QString *err)
{
	GLuint fbo = 0;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	bool ok = true;
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		*err = QStringLiteral("写入用 FBO 不完整 0x%1").arg((unsigned)glCheckFramebufferStatus(GL_FRAMEBUFFER), 0, 16);
		ok = false;
	} else {
		glViewport(0, 0, kSide, kSide);
		glClearColor(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glFinish();
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &fbo);
	return ok;
}

/* 用 sampler 把 srcTex 画进一张临时 FBO，读回中心像素。
 * 不能直接采样"正 attach 到自己身上的纹理"（反馈环），所以目标另建一张。 */
bool sampleTexture(GLuint srcTex, GLuint prog, uint8_t out[4], GLenum *bindErr, QString *err)
{
	static const GLfloat quad[] = {-1, -1, 3, -1, -1, 3};
	GLuint fbo = 0, dst = 0, vao = 0, vbo = 0;

	glBindTexture(GL_TEXTURE_2D, srcTex);
	*bindErr = glGetError();

	glGenTextures(1, &dst);
	glBindTexture(GL_TEXTURE_2D, dst);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kSide, kSide, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);

	bool ok = true;
	if (prog == 0) {
		*err = QStringLiteral("没有可用的 sampler 程序");
		ok = false;
	} else if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		*err = QStringLiteral("采样目标 FBO 不完整");
		ok = false;
	} else {
		glViewport(0, 0, kSide, kSide);
		glUseProgram(prog);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, srcTex);
		glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);
		glGenVertexArrays(1, &vao);
		glBindVertexArray(vao);
		glGenBuffers(1, &vbo);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glFinish();
		glReadPixels(kSide / 2, kSide / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (vbo)
		glDeleteBuffers(1, &vbo);
	if (vao)
		glDeleteVertexArrays(1, &vao);
	if (fbo)
		glDeleteFramebuffers(1, &fbo);
	if (dst)
		glDeleteTextures(1, &dst);
	glUseProgram(0);
	return ok;
}

const EGLint kConfigAttribs[] = {
    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT | EGL_WINDOW_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_NONE};

EGLConfig chooseConfig(EGLDisplay dpy, Rep &r, const char *who)
{
	EGLConfig cfg = nullptr;
	EGLint n = 0;
	if (eglChooseConfig(dpy, kConfigAttribs, &cfg, 1, &n) != EGL_TRUE || n < 1) {
		r.fail(QStringLiteral("%1: eglChooseConfig 失败 err=%2 n=%3").arg(who, eglErrStr()).arg(n));
		return nullptr;
	}
	return cfg;
}

EGLContext createCtx(EGLDisplay dpy, EGLConfig cfg, EGLContext share, Rep &r, const char *who)
{
	const EGLint attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 1, EGL_NONE};
	EGLContext ctx = eglCreateContext(dpy, cfg, share, attribs);
	if (ctx == EGL_NO_CONTEXT) {
		const EGLint fallback[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
		ctx = eglCreateContext(dpy, cfg, share, fallback);
		if (ctx != EGL_NO_CONTEXT)
			r.note(QStringLiteral("%1: 3.1 建不出来，按 3.0 建成").arg(who));
	}
	if (ctx == EGL_NO_CONTEXT)
		r.fail(QStringLiteral("%1: eglCreateContext 失败 err=%2").arg(who, eglErrStr()));
	return ctx;
}

bool makeCurrent(EGLDisplay dpy, EGLSurface surf, EGLContext ctx, Rep &r, const char *who)
{
	if (eglMakeCurrent(dpy, surf, surf, ctx) != EGL_TRUE) {
		r.fail(QStringLiteral("%1: eglMakeCurrent 失败 err=%2").arg(who, eglErrStr()));
		return false;
	}
	return true;
}

bool makeCurrentTwo(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx, Rep &r, const char *who)
{
	if (eglMakeCurrent(dpy, draw, read, ctx) != EGL_TRUE) {
		r.fail(QStringLiteral("%1: eglMakeCurrent(draw,read) 失败 err=%2").arg(who, eglErrStr()));
		return false;
	}
	return true;
}

EGLSurface makePbuffer(EGLDisplay dpy, EGLConfig cfg, Rep &r, const char *who)
{
	const EGLint attribs[] = {EGL_WIDTH, kSide, EGL_HEIGHT, kSide, EGL_NONE};
	EGLSurface s = eglCreatePbufferSurface(dpy, cfg, attribs);
	if (s == EGL_NO_SURFACE)
		r.fail(QStringLiteral("%1: eglCreatePbufferSurface 失败 err=%2").arg(who, eglErrStr()));
	return s;
}

QSurfaceFormat es3Format()
{
	QSurfaceFormat f;
	f.setRenderableType(QSurfaceFormat::OpenGLES);
	f.setVersion(3, 1);
	f.setDepthBufferSize(24);
	f.setStencilBufferSize(8);
	f.setSamples(0);
	return f;
}

void dumpExtensions(Rep &r, EGLDisplay dpy)
{
	const char *glx = (const char *)glGetString(GL_EXTENSIONS);
	const QByteArray gle = glx ? QByteArray(glx) : QByteArray();
	const char *eglsRaw = eglQueryString(dpy, EGL_EXTENSIONS);
	const QByteArray egls = eglsRaw ? QByteArray(eglsRaw) : QByteArray();

	static const char *wantedGl[] = {"GL_OES_EGL_image", "GL_OES_EGL_image_external"};
	static const char *wantedEgl[] = {"EGL_KHR_image_base", "EGL_KHR_image", "EGL_KHR_gl_texture_2D_image",
					  "EGL_KHR_create_context"};
	QStringList glHave, glMissing, eglHave, eglMissing;
	for (const char *w : wantedGl)
		(gle.contains(w) ? glHave : glMissing) << w;
	for (const char *w : wantedEgl)
		(egls.contains(w) ? eglHave : eglMissing) << w;
	r.add(QStringLiteral("扩展 GL 有: %1").arg(glHave.isEmpty() ? QStringLiteral("(无)") : glHave.join(' ')));
	if (!glMissing.isEmpty())
		r.add(QStringLiteral("扩展 GL 缺: %1").arg(glMissing.join(' ')));
	r.add(QStringLiteral("扩展 EGL 有: %1").arg(eglHave.isEmpty() ? QStringLiteral("(无)") : eglHave.join(' ')));
	if (!eglMissing.isEmpty())
		r.add(QStringLiteral("扩展 EGL 缺: %1").arg(eglMissing.join(' ')));
}

/* NDK 的 eglext.h 把 eglCreateImageKHR/eglDestroyImageKHR 的原型关在 EGL_EGLEXT_PROTOTYPES 里，
 * 默认不导出（实测：不加这个宏直接调就报"undeclared identifier，did you mean eglCreateImage"）。
 * 所以走运行时解析 —— 和 libobs-opengl/gl-android.c 的 get_proc_address 同一套：
 * 先 eglGetProcAddress，取不到再 dlsym(RTLD_DEFAULT)（我们的 .so 本来就链了 libEGL）。 */
void *resolveEglExt(const char *name)
{
	void *addr = (void *)(intptr_t)eglGetProcAddress(name);
	if (!addr)
		addr = dlsym(RTLD_DEFAULT, name);
	return addr;
}

struct EglImageApi {
	PFNEGLCREATEIMAGEKHRPROC create = nullptr;
	PFNEGLDESTROYIMAGEKHRPROC destroy = nullptr;
	bool ok() const { return create && destroy; }
};

EglImageApi loadEglImageApi(Rep &r)
{
	EglImageApi api;
	api.create = (PFNEGLCREATEIMAGEKHRPROC)resolveEglExt("eglCreateImageKHR");
	api.destroy = (PFNEGLDESTROYIMAGEKHRPROC)resolveEglExt("eglDestroyImageKHR");
	r.add(QStringLiteral("解析 EGLImage 入口：create=%1 destroy=%2")
		      .arg(api.create ? QStringLiteral("有") : QStringLiteral("无"),
			   api.destroy ? QStringLiteral("有") : QStringLiteral("无")));
	return api;
}

/* ==========================================================================
   Q 部分的宿主控件：把事实记在成员上，判定逻辑放在外面，paintGL 只做一次实验
   ========================================================================== */
class ShareProbeWidget : public QOpenGLWidget {
public:
	ShareProbeWidget(QWidget *host, const char *tagName) : QOpenGLWidget(host), tag(tagName)
	{
		setFormat(es3Format());
	}

	const char *tag;
	bool painted = false;
	GLuint fbo = 0;
	GLint fboBound = -1;
	WId wid = 0;
	QOpenGLContext *ctx = nullptr;
	EGLContext eglCtx = EGL_NO_CONTEXT;
	EGLDisplay eglDpy = EGL_NO_DISPLAY;
	EGLSurface eglDraw = EGL_NO_SURFACE;
	EGLSurface eglRead = EGL_NO_SURFACE;
	QString glVersion;
	bool glValid = false;

	/* paintGL 里对当前 DRAW 面的 eglQuerySurface 实测 —— 判"这块面是不是本控件自己的原生窗口"
	 * 靠的是它的宽高/类型，不是"两个 widget 的面句柄不相等"（0 也是一种不相等）。 */
	GLint drawW = -1;
	GLint drawH = -1;
	GLint drawType = -1;
	EGLint drawQueryErr = EGL_SUCCESS;
	QString drawQueryNote;

	std::function<void(ShareProbeWidget &)> onFirstPaint;
	std::function<void()> afterPaint;

protected:
	void initializeGL() override {}

	void paintGL() override
	{
		if (!painted) {
			painted = true;
			collectFacts();
			if (onFirstPaint)
				onFirstPaint(*this);
		}
		glClearColor(0.05f, 0.6f, 0.35f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		if (afterPaint)
			afterPaint();
	}

private:
	void collectFacts()
	{
		ctx = QOpenGLContext::currentContext();
		fbo = defaultFramebufferObject();
		glValid = QOpenGLWidget::isValid();
		wid = winId();
		eglCtx = eglGetCurrentContext();
		eglDpy = eglGetCurrentDisplay();
		eglDraw = eglGetCurrentSurface(EGL_DRAW);
		eglRead = eglGetCurrentSurface(EGL_READ);

		/* 面句柄本身说明不了什么，得问这块面有多大、是什么类型。先清一次错误位，
		 * 否则读到的是上一段代码留下的旧错误。 */
		eglGetError();
		if (eglDraw != EGL_NO_SURFACE) {
			const bool okW = eglQuerySurface(eglDpy, eglDraw, EGL_WIDTH, &drawW) == EGL_TRUE;
			const bool okH = eglQuerySurface(eglDpy, eglDraw, EGL_HEIGHT, &drawH) == EGL_TRUE;
			const bool okT = eglQuerySurface(eglDpy, eglDraw, EGL_SURFACE_TYPE, &drawType) == EGL_TRUE;
			drawQueryErr = eglGetError();
			if (!(okW && okH && okT))
				drawQueryNote = QStringLiteral("查询返回 W=%1 H=%2 T=%3").arg(okW).arg(okH).arg(okT);
		} else {
			drawQueryNote = QStringLiteral("当前没有 DRAW 面（EGL_NO_SURFACE）");
		}

		const char *v = (const char *)glGetString(GL_VERSION);
		glVersion = v ? QString::fromUtf8(v) : QStringLiteral("(NULL)");
		/* paintGL 进来时 Qt 已经把本控件的 FBO 绑上了，这里只读回来看是不是那一张 */
		GLint b = -1;
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &b);
		fboBound = b;
	}
};

/* Qt 会不会把"用户自己 create 的 QOpenGLContext"和 widget 的上下文收进同一个 share group。 */
void runQtUserContextTest(ShareProbeWidget &w, Rep &r)
{
	QOffscreenSurface oss;
	oss.setFormat(es3Format());
	oss.create();
	if (!oss.isValid()) {
		r.note(QStringLiteral("Q2 跳过：QOffscreenSurface 建不出来（这版 Qt/Android 不支持离屏面？）"));
		return;
	}
	QOpenGLContext user;
	user.setFormat(es3Format());
	if (!user.create()) {
		r.note(QStringLiteral("Q2 跳过：用户 QOpenGLContext::create() 失败"));
		return;
	}
	if (!user.makeCurrent(&oss)) {
		r.note(QStringLiteral("Q2 跳过：用户上下文对离屏面 makeCurrent 失败 err=%1").arg(eglErrStr()));
		return;
	}
	EGLContext userEgl = eglGetCurrentContext();
	r.add(QStringLiteral("Q2：用户 QOpenGLContext 的 EGLContext=%1，widget 的=%2 → %3")
		      .arg((quintptr)userEgl, 0, 16)
		      .arg((quintptr)w.eglCtx, 0, 16)
		      .arg(userEgl == w.eglCtx
				   ? QStringLiteral("同一个（Qt 已把 GL 上下文收敛成一个，路线 A 也许不需要额外共享）")
				   : QStringLiteral("不是同一个（需要显式 share，见 Q3）")));
	r.add(QStringLiteral("Q2：user.shareContext=%1，widget.context()->shareContext=%2")
		      .arg((quintptr)user.shareContext(), 0, 16)
		      .arg((quintptr)(w.ctx ? w.ctx->shareContext() : nullptr), 0, 16));
	user.doneCurrent();
	if (w.ctx)
		w.ctx->makeCurrent(w.windowHandle()); /* 归还：paintGL 后半段还得在 widget 的上下文里 */
	oss.destroy();
}

/* 与 Qt 上下文同组的"外部上下文" ↔ Qt 双向纹理可见性 —— 这就是路线 A 的接线假设。 */
void runQtShareExperiment(ShareProbeWidget &w, Rep &r)
{
	EGLDisplay dpy = eglGetCurrentDisplay();
	EGLContext qtCtx = eglGetCurrentContext();
	EGLSurface draw = eglGetCurrentSurface(EGL_DRAW);
	EGLSurface read = eglGetCurrentSurface(EGL_READ);
	if (dpy == EGL_NO_DISPLAY || qtCtx == EGL_NO_CONTEXT) {
		r.fail(QStringLiteral("Q3 无法判定：paintGL 里 eglGetCurrentDisplay/Context 是空的"));
		return;
	}
	r.add(QStringLiteral("Q3 起点：paintGL 里 Qt 有 current 的 display/context，DRAW=%1 READ=%2")
		      .arg((quintptr)draw, 0, 16)
		      .arg((quintptr)read, 0, 16));

	EGLConfig cfg = chooseConfig(dpy, r, "Q3");
	if (!cfg)
		return;
	EGLSurface pb = makePbuffer(dpy, cfg, r, "Q3");
	EGLContext raw = createCtx(dpy, cfg, qtCtx, r, "Q3(以 Qt 的 context 为 share 源)");
	if (raw == EGL_NO_CONTEXT || pb == EGL_NO_SURFACE) {
		r.fail(QStringLiteral("Q3 判负：拿 Qt 的 EGLContext 当 share 源都建不出上下文 → "
				       "路线 A 不能靠'OBS 共享 Qt 的上下文'，得换 EGLImage 或路线 B"));
		return;
	}
	r.add(QStringLiteral("Q3 关键事实：以 Qt 的 EGLContext 为 share 源建上下文**成功** "
			    "→ OBS 侧只要把 gl-android.c:116 的 EGL_NO_CONTEXT 换成它，纹理名就能直接互通"));

	GLuint progQt = 0, progRaw = 0;
	QString err;
	GLuint texRaw = 0, texQt = 0;

	if (!makeCurrent(dpy, pb, raw, r, "Q3(raw)"))
		goto out;
	progRaw = makeSamplerProgram(r, "Q3-raw");
	texRaw = makeRgba8Texture();
	if (!fillTexture(texRaw, kC4, &err)) {
		r.fail(QStringLiteral("Q3 无法判定：外部上下文填纹理失败 %1").arg(err));
		goto out;
	}
	if (!makeCurrentTwo(dpy, draw, read, qtCtx, r, "Q3(回到 Qt)"))
		goto out;
	progQt = makeSamplerProgram(r, "Q3-qt");
	{
		uint8_t out1[4] = {0, 0, 0, 0};
		GLenum bindErr = GL_NO_ERROR;
		QString serr;
		if (progQt && sampleTexture(texRaw, progQt, out1, &bindErr, &serr)) {
			if (closeTo(out1, kC4))
				r.add(QStringLiteral("Q3a PASS：与 Qt 同组的外部上下文建的纹理，Qt 上下文采到 %1 "
						     "（= OBS 画 / Qt 显示可行）")
					      .arg(px(out1)));
			else
				r.fail(QStringLiteral("Q3a FAIL：Qt 上下文读到 %1，期望 %2（bindErr=0x%3）")
					       .arg(px(out1), want(kC4)).arg((unsigned)bindErr, 0, 16));
		} else
			r.fail(QStringLiteral("Q3a 无法判定：%1").arg(serr));
	}

	texQt = makeRgba8Texture();
	if (fillTexture(texQt, kC5, &err)) {
		if (makeCurrent(dpy, pb, raw, r, "Q3(raw 反向)")) {
			uint8_t out2[4] = {0, 0, 0, 0};
			GLenum b2 = GL_NO_ERROR;
			QString s2;
			if (progRaw && sampleTexture(texQt, progRaw, out2, &b2, &s2)) {
				if (closeTo(out2, kC5))
					r.add(QStringLiteral("Q3b PASS：Qt 上下文建的纹理，同组外部上下文采到 %1 "
							     "（= Qt 出遮罩 / OBS 采，反向也通）")
						  .arg(px(out2)));
				else
					r.fail(QStringLiteral("Q3b FAIL：外部上下文读到 %1，期望 %2（bindErr=0x%3）")
						   .arg(px(out2), want(kC5)).arg((unsigned)b2, 0, 16));
			} else
				r.fail(QStringLiteral("Q3b 无法判定：%1").arg(s2));
			makeCurrentTwo(dpy, draw, read, qtCtx, r, "Q3(再次回到 Qt)");
		}
	} else
		r.fail(QStringLiteral("Q3b 无法判定：Qt 上下文填纹理失败 %1").arg(err));

out:
	/* 程序/纹理都是上下文所属 share group 的对象，销毁上下文一起回收，
	 * 这里只恢复 Qt 的 current 绑定并销毁外部上下文。 */
	eglMakeCurrent(dpy, pb, pb, EGL_NO_CONTEXT);
	eglDestroyContext(dpy, raw);
	eglDestroySurface(dpy, pb);
	makeCurrentTwo(dpy, draw, read, qtCtx, r, "Q3(收尾恢复 Qt)");
	(void)progQt;
	(void)progRaw;
	(void)texRaw;
	(void)texQt;
	(void)w;
}

} // namespace

/* ==========================================================================
   W：纯 EGL，跑在非 GUI 线程
   ========================================================================== */
QString runGlShareProbe()
{
	Rep r;
	r.add(QStringLiteral("=== 3-2 探针 W：跨 EGL 上下文的纹理可见性（纯 EGL，不借 Qt）==="));

	EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (dpy == EGL_NO_DISPLAY) {
		r.fail(QStringLiteral("eglGetDisplay 失败 err=%1").arg(eglErrStr()));
		return r.lines.join('\n');
	}
	EGLint major = 0, minor = 0;
	/* 壳里 Qt、libobs 都已经用过 EGL_DEFAULT_DISPLAY。本函数**故意不调 eglTerminate**：
	 * terminate 会把别人的 display 一起废掉（OBS 视频线程的上下文首当其冲）。 */
	if (eglInitialize(dpy, &major, &minor) != EGL_TRUE) {
		r.fail(QStringLiteral("eglInitialize 失败 err=%1").arg(eglErrStr()));
		return r.lines.join('\n');
	}
	r.add(QStringLiteral("EGL %1.%2 就绪（与 Qt/libobs 共用同一个 default display）").arg(major).arg(minor));

	EGLConfig cfg = chooseConfig(dpy, r, "W");
	if (!cfg)
		goto out_lines;

	{
		EGLContext ctxA = createCtx(dpy, cfg, EGL_NO_CONTEXT, r, "A(不共享)");
		EGLSurface pbA = makePbuffer(dpy, cfg, r, "A");
		EGLContext ctxB = EGL_NO_CONTEXT, ctxC = EGL_NO_CONTEXT;
		EGLSurface pbB = EGL_NO_SURFACE, pbC = EGL_NO_SURFACE;
		GLuint progA = 0, progB = 0, progC = 0;
		GLuint texA = 0, texImg = 0, texConsumer = 0;
		EGLImageKHR image = EGL_NO_IMAGE_KHR;
		EglImageApi eglImage = loadEglImageApi(r);
		QString err;

		do {
			if (ctxA == EGL_NO_CONTEXT || pbA == EGL_NO_SURFACE)
				break;
			if (!makeCurrent(dpy, pbA, ctxA, r, "A"))
				break;
			progA = makeSamplerProgram(r, "A");
			dumpExtensions(r, dpy);

			texA = makeRgba8Texture();
			if (!fillTexture(texA, kC1, &err)) {
				r.fail(QStringLiteral("A 填色失败: %1").arg(err));
				break;
			}
			texImg = makeRgba8Texture();
			if (!fillTexture(texImg, kC1, &err)) {
				r.fail(QStringLiteral("A 填色(第二张，给 EGLImage)失败: %1").arg(err));
				break;
			}
			r.add(QStringLiteral("A(不共享) 建了两张纹理 name=%1 / %2，都填 %3")
				      .arg(texA).arg(texImg).arg(want(kC1)));

			/* --- W1：B 与 A 同组，应当看得见 A 的纹理名 --- */
			ctxB = createCtx(dpy, cfg, ctxA, r, "B(与A共享)");
			pbB = makePbuffer(dpy, cfg, r, "B");
			if (ctxB != EGL_NO_CONTEXT && pbB != EGL_NO_SURFACE && makeCurrent(dpy, pbB, ctxB, r, "B")) {
				progB = makeSamplerProgram(r, "B");
				uint8_t out[4] = {0, 0, 0, 0};
				GLenum bindErr = GL_NO_ERROR;
				QString serr;
				if (progB && sampleTexture(texA, progB, out, &bindErr, &serr)) {
					if (closeTo(out, kC1))
						r.add(QStringLiteral("W1 正例 PASS：共享上下文 B 采到 A 的纹理 = %1（bindErr=0x%2）")
								  .arg(px(out)).arg((unsigned)bindErr, 0, 16));
					else
						r.fail(QStringLiteral("W1 正例 FAIL：共享上下文 B 读到 %1，期望 %2（bindErr=0x%3）")
							   .arg(px(out), want(kC1)).arg((unsigned)bindErr, 0, 16));
				} else
					r.fail(QStringLiteral("W1 正例无法判定：%1").arg(serr));
			}

			/* --- W2：C 谁都不共享，必须读不到（负对照，否则 W1 没有区分度） --- */
			ctxC = createCtx(dpy, cfg, EGL_NO_CONTEXT, r, "C(不共享)");
			pbC = makePbuffer(dpy, cfg, r, "C");
			if (ctxC == EGL_NO_CONTEXT || pbC == EGL_NO_SURFACE || !makeCurrent(dpy, pbC, ctxC, r, "C"))
				break;
			progC = makeSamplerProgram(r, "C");
			{
				uint8_t out[4] = {0, 0, 0, 0};
				GLenum bindErr = GL_NO_ERROR;
				QString serr;
				if (progC && sampleTexture(texA, progC, out, &bindErr, &serr)) {
					if (closeTo(out, kC1))
						r.fail(QStringLiteral("W2 负对照 FAIL：不共享也读到了 %1 —— 这台设备的 GL "
								       "把纹理名当进程全局，W1 的结论没有区分度").arg(px(out)));
					else
						r.add(QStringLiteral("W2 负对照 PASS：不共享读到 %1 ≠ %2（bindErr=0x%3）"
								     "→ 纹理名确实只在同一个 share group 内可见")
							  .arg(px(out), want(kC1)).arg((unsigned)bindErr, 0, 16));
				} else
					r.note(QStringLiteral("W2 负对照无法判定：%1").arg(serr));
			}

			/* --- W3：EGLImage 跨非共享上下文（退路，也是零拷贝正解的候选） --- */
			if (!eglImage.ok()) {
				r.fail(QStringLiteral("W3 无法判定：本机解析不到 eglCreateImageKHR/eglDestroyImageKHR 入口"));
				break;
			}
			if (makeCurrent(dpy, pbA, ctxA, r, "A(建 image 前)")) {
				const EGLClientBuffer buf = (EGLClientBuffer)(uintptr_t)texImg;
				image = eglImage.create(dpy, ctxA, EGL_GL_TEXTURE_2D_KHR, buf, nullptr);
				r.add(QStringLiteral("eglCreateImageKHR(ctx=A) → %1 err=%2")
					      .arg(image == EGL_NO_IMAGE_KHR ? QStringLiteral("失败") : QStringLiteral("成功"), eglErrStr()));
				if (image == EGL_NO_IMAGE_KHR) {
					image = eglImage.create(dpy, EGL_NO_CONTEXT, EGL_GL_TEXTURE_2D_KHR, buf, nullptr);
					r.add(QStringLiteral("eglCreateImageKHR(ctx=NO_CONTEXT) → %1 err=%2")
							  .arg(image == EGL_NO_IMAGE_KHR ? QStringLiteral("失败") : QStringLiteral("成功"),
							       eglErrStr()));
				}
			}
			if (image == EGL_NO_IMAGE_KHR) {
				r.fail(QStringLiteral("W3 EGLImage 路线不可用：两次 eglCreateImageKHR 都失败 "
						       "→ 共享上下文是路线 A 唯一可行的传纹理方式"));
				break;
			}

			auto *targetFn =
				(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(intptr_t)eglGetProcAddress("glEGLImageTargetTexture2DOES");
			r.add(QStringLiteral("eglGetProcAddress(\"glEGLImageTargetTexture2DOES\") → %1")
				      .arg(targetFn ? QStringLiteral("非空") : QStringLiteral("NULL")));
			if (!targetFn) {
				r.fail(QStringLiteral("W3 无法判定：取不到 glEGLImageTargetTexture2DOES 入口"));
				break;
			}

			if (!makeCurrent(dpy, pbC, ctxC, r, "C(贴 image 前)"))
				break;
			glGetError();
			texConsumer = makeRgba8Texture();
			targetFn(GL_TEXTURE_2D, (GLeglImageOES)image);
			{
				const GLenum terr = glGetError();
				r.add(QStringLiteral("C 里 glEGLImageTargetTexture2DOES → glGetError=0x%1")
					      .arg((unsigned)terr, 0, 16));
				if (terr != GL_NO_ERROR) {
					r.fail(QStringLiteral("W3 判负：贴上 EGLImage 就报错，零拷贝退路在本机不可用"));
					break;
				}
				uint8_t out[4] = {0, 0, 0, 0};
				GLenum bindErr = GL_NO_ERROR;
				QString serr;
				if (!(progC && sampleTexture(texConsumer, progC, out, &bindErr, &serr))) {
					r.fail(QStringLiteral("W3 无法判定：%1").arg(serr));
					break;
				}
				if (closeTo(out, kC1))
					r.add(QStringLiteral("W3 正例 PASS：不共享的 C 通过 EGLImage 采到 %1 "
							     "（跨上下文零拷贝可用，是不依赖 share 的第二条路）")
						  .arg(px(out)));
				else
					r.fail(QStringLiteral("W3 正例 FAIL：EGLImage 贴上了但读到 %1，期望 %2（bindErr=0x%3）")
						   .arg(px(out), want(kC1)).arg((unsigned)bindErr, 0, 16));

				/* --- W4：内容改了要不要重建 image（预览每帧都是新内容） --- */
				if (makeCurrent(dpy, pbA, ctxA, r, "A(改内容)") && fillTexture(texImg, kC2, &err)) {
					if (makeCurrent(dpy, pbC, ctxC, r, "C(读改动)")) {
						uint8_t out2[4] = {0, 0, 0, 0};
						GLenum b2 = GL_NO_ERROR;
						QString s2;
						if (progC && sampleTexture(texConsumer, progC, out2, &b2, &s2)) {
							if (closeTo(out2, kC2))
								r.add(QStringLiteral("W4 PASS：A 改成 %1 后，C 经同一张 image 读到 %2 "
										     "→ image 不用每帧重建").arg(want(kC2), px(out2)));
							else if (closeTo(out2, kC1))
								r.note(QStringLiteral("W4 未传播：C 仍读到 %1（期望 %2）"
										     "→ 每帧得销毁重建 image，或需要显式同步")
									       .arg(px(out2), want(kC2)));
							else
								r.fail(QStringLiteral("W4 读到第三个值 %1（期望 %2 或 %3）")
									   .arg(px(out2), want(kC2), want(kC1)));
						}
					}
				} else
					r.fail(QStringLiteral("W4 无法判定：A 改内容或切回上下文失败 %1").arg(err));
			}
		} while (false);

		if (image != EGL_NO_IMAGE_KHR && eglImage.destroy)
			eglImage.destroy(dpy, image);
		eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (ctxC != EGL_NO_CONTEXT)
			eglDestroyContext(dpy, ctxC);
		if (ctxB != EGL_NO_CONTEXT)
			eglDestroyContext(dpy, ctxB);
		if (ctxA != EGL_NO_CONTEXT)
			eglDestroyContext(dpy, ctxA);
		if (pbC != EGL_NO_SURFACE)
			eglDestroySurface(dpy, pbC);
		if (pbB != EGL_NO_SURFACE)
			eglDestroySurface(dpy, pbB);
		if (pbA != EGL_NO_SURFACE)
			eglDestroySurface(dpy, pbA);
		(void)progA;
		(void)progB;
		(void)progC;
		(void)texConsumer;
	}

out_lines:
	r.add(r.ok ? QStringLiteral("=== 结论：W 通过，见上面 W1/W2/W3/W4 各行 ===")
		   : QStringLiteral("=== 结论：W 未通过，见上面 FAIL ==="));

	for (const QString &l : r.lines)
		PPLOG(l.toUtf8().constData());
	return r.lines.join('\n');
}

/* ==========================================================================
   Q + S：QOpenGLWidget 事实，跑在 GUI 线程
   ========================================================================== */
QString runQtGlProbe(QWidget *host)
{
	Rep r;
	r.add(QStringLiteral("=== 3-2 探针 Q：QOpenGLWidget 在 Qt 6.9.3 + Android 上画在哪儿 ==="));

	if (!host) {
		r.fail(QStringLiteral("没有宿主控件，Q/S 部分整体跳过"));
	} else {
		auto *w1 = new ShareProbeWidget(host, "W1");
		auto *w2 = new ShareProbeWidget(host, "W2");
		w1->setGeometry(8, 8, 150, 110);
		w2->setGeometry(8, 130, 150, 110);
		w1->setObjectName("obsProbe1");
		w2->setObjectName("obsProbe2");

		QEventLoop loop;
		auto step = [&loop, w1, w2]() {
			if (w1->painted && w2->painted)
				loop.quit();
		};
		w1->afterPaint = step;
		w2->afterPaint = step;
		w1->onFirstPaint = [&r](ShareProbeWidget &self) {
			runQtUserContextTest(self, r);
			runQtShareExperiment(self, r);
		};

		w1->show();
		w2->show();
		w1->raise();
		w2->raise();
		QTimer::singleShot(6000, &loop, &QEventLoop::quit);
		loop.exec();

		r.add(QStringLiteral("宿主 host->winId()=%1 windowHandle=%2")
			      .arg((quintptr)host->winId(), 0, 16)
			      .arg((quintptr)host->windowHandle(), 0, 16));
		for (ShareProbeWidget *w : {w1, w2}) {
			if (!w->painted) {
				r.fail(QStringLiteral("%1 从未进过 paintGL（6 秒超时）—— 这版 Qt/这台设备上 "
						       "QOpenGLWidget 没走 GL 路径绘制，下面所有 Q 判据都不成立")
						   .arg(w->tag));
				continue;
			}
			r.add(QStringLiteral("%1：winId=%2 windowHandle=%3 isValid=%4 尺寸=%5x%6")
				      .arg(w->tag)
				      .arg((quintptr)w->wid, 0, 16)
				      .arg((quintptr)w->windowHandle(), 0, 16)
				      .arg((int)w->glValid)
				      .arg(w->width())
				      .arg(w->height()));
			r.add(QStringLiteral("%1：defaultFramebufferObject=%2 paintGL 里 GL_FRAMEBUFFER_BINDING=%3 → %4")
				      .arg(w->tag)
				      .arg(w->fbo)
				      .arg(w->fboBound)
				      .arg(w->fboBound == (GLint)w->fbo
						       ? QStringLiteral("绑的就是本控件那张离屏 FBO")
						       : (w->fboBound == 0
							      ? QStringLiteral("实际绑的是 0（窗口后缓冲），"
									       "和 defaultFramebufferObject 不一致")
							      : QStringLiteral("绑的是别的一张，不是 defaultFramebufferObject"))));
			r.add(QStringLiteral("%1：QOpenGLContext=%2 EGLContext=%3 EGL_DRAW=%4 EGL_READ=%5")
				      .arg(w->tag)
				      .arg((quintptr)w->ctx, 0, 16)
				      .arg((quintptr)w->eglCtx, 0, 16)
				      .arg((quintptr)w->eglDraw, 0, 16)
				      .arg((quintptr)w->eglRead, 0, 16));
			r.add(QStringLiteral("%1：DRAW 面实测 %2x%3 surface_type=0x%4 err=0x%5 %6")
				      .arg(w->tag)
				      .arg(w->drawW)
				      .arg(w->drawH)
				      .arg((int)w->drawType, 0, 16)
				      .arg((unsigned)w->drawQueryErr, 0, 16)
				      .arg(w->drawQueryNote.isEmpty()
						   ? QStringLiteral("（本控件几何=%1x%2 设备像素）")
							     .arg(int(w->width() * w->devicePixelRatioF()))
							     .arg(int(w->height() * w->devicePixelRatioF()))
						   : w->drawQueryNote));
			r.add(QStringLiteral("%1：GL_VERSION=%2").arg(w->tag, w->glVersion));
		}

		/* S：整个 Qt 进程是不是只有一块原生 EGL surface —— "没有原生子窗口"那句的实测。
		 * 判据只能用面的**尺寸与类型**：句柄不相等不能说明任何事，因为 EGL_NO_SURFACE(0)
		 * 也是一种"不相等"，而它的意思是"当时根本没有窗口面 current"。上一轮就是因为把
		 * W1=0fd0 / W2=0 读成"各有独立原生面"，打出一条不成立的"前提被推翻"。 */
		if (w1->painted && w2->painted) {
			const int dpW1 = int(w1->width() * w1->devicePixelRatioF());
			const int dpH1 = int(w1->height() * w1->devicePixelRatioF());
			const bool noSurf[2] = {w1->eglDraw == EGL_NO_SURFACE, w2->eglDraw == EGL_NO_SURFACE};

			/* 一个面算不算"本控件自己的原生窗口"：尺寸正好等于该控件的设备像素几何。
			 * Qt 的顶层窗口是整个屏幕，离屏 pbuffer 也不会刚好等于控件尺寸。 */
			const bool selfSized[2] = {
				w1->drawW == dpW1 && w1->drawH == dpH1,
				w2->drawW == (int)(w2->width() * w2->devicePixelRatioF()) &&
					w2->drawH == (int)(w2->height() * w2->devicePixelRatioF())};

			if (noSurf[0] && noSurf[1])
				r.add(QStringLiteral("S 结论：两个 widget 在 paintGL 里都没有 current 的窗口面（各自画进自己的 FBO）"
						     " → 计划里那句\"没有原生子窗口\"成立；QTToGSWindow 在这里没有对应物，"
						     "应当继续留着 success=false 而不是硬凑一个窗口句柄"));
			else if (selfSized[0] || selfSized[1])
				r.fail(QStringLiteral("S 前提被推翻：至少一个 widget 的 DRAW 面尺寸 == 它自己的设备像素几何"
						       "（W1 %1x%2 vs %3x%4 / W2 %5x%6 vs %7x%8）→ Qt 真给了原生子窗口，"
						       "路线 B 仍然开着，得回去重看 8.16 的对照表")
						   .arg(w1->drawW).arg(w1->drawH).arg(dpW1).arg(dpH1)
						   .arg(w2->drawW).arg(w2->drawH)
						   .arg(int(w2->width() * w2->devicePixelRatioF()))
						   .arg(int(w2->height() * w2->devicePixelRatioF())));
			else if (noSurf[0] != noSurf[1])
				r.note(QStringLiteral("S 未判定：两个 widget 各采到一次、但一份有 current 面一份没有"
						      "（W1 DRAW=%1 %2x%3 / W2 DRAW=%4 %5x%6），取样状态不可比 —— "
						      "\"每 widget 一块独立原生 surface\"既没证成也没证伪。"
						      "这条不影响路线选择（A 由计划原文定死），只是别拿本轮数据说事")
					       .arg((quintptr)w1->eglDraw, 0, 16).arg(w1->drawW).arg(w1->drawH)
					       .arg((quintptr)w2->eglDraw, 0, 16).arg(w2->drawW).arg(w2->drawH));
			else
				r.note(QStringLiteral("S 未判定：两边都有 current 面但尺寸都不等于各自几何"
						      "（W1 %1x%2 / W2 %3x%4，句柄%5）→ 更像共用同一块面或各自一张 pbuffer，"
						      "不足以判\"独立原生子窗口\"")
					       .arg(w1->drawW).arg(w1->drawH).arg(w2->drawW).arg(w2->drawH)
					       .arg(w1->eglDraw == w2->eglDraw ? QStringLiteral("相同")
									       : QStringLiteral("不同")));
		} else {
			r.note(QStringLiteral("S 判据缺失：至少一个探针 widget 没画过"));
		}

		/* 折腾完一圈 eglMakeCurrent，Qt 还能不能画 —— 决定"能不能在 paintGL 里切上下文"的代价 */
		const QImage img = w1->glValid ? w1->grabFramebuffer() : QImage();
		if (img.isNull()) {
			r.fail(QStringLiteral("收尾：w1->grabFramebuffer() 返回空图 —— 上面那些 eglMakeCurrent 之后 "
					       "Qt 已经画不出这个控件"));
		} else {
			r.add(QStringLiteral("收尾：grabFramebuffer %1x%2 中心像素 %3（期望 ≈ paintGL 的 clearColor "
					     "13,153,89）→ %4")
				      .arg(img.width())
				      .arg(img.height())
				      .arg(img.pixelColor(img.width() / 2, img.height() / 2).name(QColor::HexRgb),
					   QStringLiteral("对照见脚本判据")));
		}

		delete w1;
		delete w2;
		QCoreApplication::processEvents();
	}

	r.add(r.ok ? QStringLiteral("=== 结论：Q 通过，见上面 Q/S 各行 ===")
		   : QStringLiteral("=== 结论：Q 未通过，见上面 FAIL ==="));

	for (const QString &l : r.lines)
		PPLOG(l.toUtf8().constData());
	return r.lines.join('\n');
}
