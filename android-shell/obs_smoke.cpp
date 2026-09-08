// 阶段 1 冒烟：证明交叉编译出来的 liblibobs.so 能在 Android 上真正跑起来。
//
// 做四件事：
//   1) 把 libobs 的数据（*.effect 等，见 CMake 里的 :/obsdata 资源）解到应用私有目录；
//   2) 按 obs-android.c 约定的布局摆好 OBS_ROOT_PATH/{lib,share/libobs}；
//   3) 把 blog 转发到 logcat（否则 libobs 的日志全进 stderr，抓不到）；
//   4) obs_startup() → 打印版本/已注册源类型 → obs_shutdown()。

#include "obs_smoke.h"
#include "obs_usb_host.h"   // A3：Java 宿主 + libobs USB 总线
#include "obs_audio_host.h" // A4：Java 宿主 + libobs 音频总线
#include "obs_display_host.h" // S1：Java SurfaceView → ANativeWindow 显示面宿主

#include <obs.h>
#include <graphics/vec4.h> // S1 的 draw 回调要给 solid effect 喂 vec4

#include <pthread.h> // S1 要证明"创建任务落点 == 出帧线程"

#include <android/log.h>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QStandardPaths>
#include <QThread>

#ifdef Q_OS_ANDROID
#include <QJniObject> // A1 冒烟查 RECORD_AUDIO 授权状态用
#endif

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h> // A2 用 open("/dev/null") 造一个"真实但不是 usbfs 节点"的 fd

#include <dlfcn.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#define SMK(...) __android_log_print(ANDROID_LOG_INFO, "OBS-SMOKE", __VA_ARGS__)

namespace {

// 必须是 2 的幂：libobsLog 里用 "head & (N-1)" 取模
constexpr int LOG_RING_N = 64;
char g_logRing[LOG_RING_N][512];
std::atomic<int> g_logRingHead{0};

struct Smoke {
    QStringList lines;
    bool ok = true;
    void add(const QString &s) { lines << s; }
    void fail(const QString &s)
    {
        lines << ("FAIL " + s);
        ok = false;
    }
};

void libobsLog(int lvl, const char *msg, va_list args, void *)
{
    char buf[2048];
    vsnprintf(buf, sizeof(buf), msg, args);
    const int prio = lvl <= LOG_ERROR ? ANDROID_LOG_ERROR : lvl <= LOG_WARNING ? ANDROID_LOG_WARN : ANDROID_LOG_INFO;
    __android_log_print(prio, "OBS-libobs", "%s", buf);

    // A2 要按插件日志判断"到底走了哪个失败分支"，所以顺手再存一份到环形缓冲。
    // 纯诊断工装：允许并发写时偶尔读到略微过期的内容（和 AudioProbe 计数同一取舍）。
    const int i = g_logRingHead.fetch_add(1, std::memory_order_relaxed) & (LOG_RING_N - 1);
    snprintf(g_logRing[i], sizeof(g_logRing[i]), "%s", buf);
}

void logRingReset()
{
    g_logRingHead.store(0, std::memory_order_relaxed);
}

bool logRingHas(const char *needle)
{
    const int head = g_logRingHead.load(std::memory_order_relaxed);
    for (int i = 0; i < (head < LOG_RING_N ? head : LOG_RING_N); i++) {
        if (strstr(g_logRing[i], needle))
            return true;
    }
    return false;
}

// 资源树 :/obsdata/** -> 磁盘 dstRoot/**；先整目录删掉，保证每次都是干净的
bool extractTree(const QString &resRoot, const QString &dstRoot, int *count)
{
    QDir(dstRoot).removeRecursively();
    if (!QDir().mkpath(dstRoot))
        return false;

    int n = 0;
    QDirIterator it(resRoot, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString src = it.next();
        const QString dst = dstRoot + src.mid(resRoot.length());
        QDir().mkpath(QFileInfo(dst).absolutePath());
        QFile::remove(dst);
        if (!QFile::copy(src, dst))
            return false;
        ++n;
    }
    *count = n;
    return true;
}

} // namespace

// 根目录 + libobs 数据解包 + OBS_ROOT_PATH，两个冒烟（M1/M2）共用
static bool prepareRoot(Smoke &s, QString *root)
{
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (appData.isEmpty()) {
        s.fail(QStringLiteral("拿不到 AppDataLocation"));
        return false;
    }
    *root = appData + "/obsroot";
    s.add(QStringLiteral("OBS_ROOT_PATH = %1").arg(*root));

    if (!QDir().mkpath(*root + "/lib"))
        s.fail(QStringLiteral("无法创建 %1").arg(*root + "/lib"));

    int files = 0;
    if (extractTree(QStringLiteral(":/obsdata"), *root + "/share/libobs", &files))
        s.add(QStringLiteral("libobs 数据解包 %1 个文件 → share/libobs").arg(files));
    else
        s.fail(QStringLiteral("解包 :/obsdata 失败（检查 CMake 资源是否打进了 APK）"));

    setenv("OBS_ROOT_PATH", root->toUtf8().constData(), 1);
    return true;
}

QString runObsSmoke()
{
    Smoke s;
    s.add(QStringLiteral("=== libobs 冒烟：obs_startup ==="));

    // --- 1. 根目录与数据 ---------------------------------------------------
    QString root;
    if (!prepareRoot(s, &root))
        return s.lines.join('\n');

    // --- 2. blog → logcat --------------------------------------------------
    base_set_log_handler(libobsLog, nullptr);

    // --- 3. startup --------------------------------------------------------
    const bool started = obs_startup("en-US", nullptr, nullptr);
    if (!started) {
        s.add(QStringLiteral("（失败原因看 logcat -s OBS-libobs）"));
        s.fail(QStringLiteral("obs_startup 返回 false"));
    } else {
        s.add(QStringLiteral("obs_startup 成功，libobs %1 (0x%2)")
                  .arg(obs_get_version_string())
                  .arg((quint32)obs_get_version(), 8, 16, QChar('0')));

        QStringList types;
        for (size_t i = 0;; i++) {
            const char *id = nullptr;
            if (!obs_enum_source_types(i, &id) || !id)
                break;
            types << QString::fromUtf8(id);
            if (types.size() > 64) // 防御：万一枚举不回 false
                break;
        }
        s.add(QStringLiteral("已注册源类型 %1 个: %2").arg(types.size()).arg(types.join(", ")));

        // 数据查找链自检：OBS_ROOT_PATH/share/libobs 必须能被 obs_find_data_file 解析出来
        for (const char *fx : {"default.effect", "color.effect"}) {
            char *path = obs_find_data_file(fx);
            if (path) {
                s.add(QStringLiteral("find_data_file(%1) → %2").arg(fx).arg(QString::fromUtf8(path)));
                bfree(path);
            } else {
                s.fail(QStringLiteral("find_data_file(%1) 找不到 —— share/libobs 解包或 OBS_ROOT_PATH 不对").arg(fx));
            }
        }

        obs_shutdown();
        s.add(QStringLiteral("obs_shutdown 完成（libobs 干净退出）"));
    }

    base_set_log_handler(nullptr, nullptr);

    s.add(s.ok ? QStringLiteral("=== 结论：libobs 在 Android 上可用 ===") : QStringLiteral("=== 结论：未通过 ==="));

    for (const QString &l : s.lines)
        SMK("%s", l.toUtf8().constData());
    return s.lines.join('\n');
}

// ---------------------------------------------------------------------------
// 里程碑 M2：obs_reset_video 真建起 GS 设备（gl-android.c 的 1x1 pbuffer + GLES 3.1
// 上下文），并把 share/libobs 下所有 effect 过一遍驱动编译，最后离屏渲染 + 回读像素。
// 注意 obs_reset_video 内部（obs.c 的 obs_init_graphics）就会编译 12 个基础 effect，
// 且 default_rect.effect 只在 GS_DEVICE_OPENGL 下是硬性要求，所以这一步同时是在验证
// gl-shaderparser 发出的 GLSL ES 3.1 能否被真实驱动接受。
// ---------------------------------------------------------------------------

static const char *videoErrName(int rc)
{
    switch (rc) {
    case OBS_VIDEO_SUCCESS:
        return "OBS_VIDEO_SUCCESS";
    case OBS_VIDEO_FAIL:
        return "OBS_VIDEO_FAIL";
    case OBS_VIDEO_NOT_SUPPORTED:
        return "OBS_VIDEO_NOT_SUPPORTED";
    case OBS_VIDEO_INVALID_PARAM:
        return "OBS_VIDEO_INVALID_PARAM";
    case OBS_VIDEO_CURRENTLY_ACTIVE:
        return "OBS_VIDEO_CURRENTLY_ACTIVE";
    case OBS_VIDEO_MODULE_NOT_FOUND:
        return "OBS_VIDEO_MODULE_NOT_FOUND";
    default:
        return "未知返回码";
    }
}

QString runObsRenderSmoke()
{
    Smoke s;
    s.add(QStringLiteral("=== 渲染冒烟 M2：GS 设备 + GLSL ES + 离屏回读 ==="));

    QString root;
    const bool prepared = prepareRoot(s, &root);
    bool started = false;

    if (prepared) {
        base_set_log_handler(libobsLog, nullptr);
        started = obs_startup("en-US", nullptr, nullptr);
        if (!started)
            s.fail(QStringLiteral("obs_startup 返回 false（细节看 logcat -s OBS-libobs）"));
    }

    if (started) {
        struct obs_video_info ovi = {};
        ovi.graphics_module = "libobs-opengl";
        ovi.fps_num = 60;
        ovi.fps_den = 1;
        ovi.base_width = 640;
        ovi.base_height = 360;
        ovi.output_width = 640;
        ovi.output_height = 360;
        ovi.output_format = VIDEO_FORMAT_RGBA;
        ovi.colorspace = VIDEO_CS_DEFAULT;
        ovi.range = VIDEO_RANGE_DEFAULT;
        ovi.scale_type = OBS_SCALE_BILINEAR;
        ovi.gpu_conversion = false; // 先不拉 YUV 转换路径，把设备与着色器通道跑通

        const int vrc = obs_reset_video(&ovi);
        s.add(QStringLiteral("obs_reset_video(libobs-opengl, 640x360@60 RGBA) → %1").arg(videoErrName(vrc)));
        if (vrc != OBS_VIDEO_SUCCESS)
            s.fail(QStringLiteral("obs_reset_video 未通过（libobs 侧的失败原因看 logcat -s OBS-libobs）"));

        if (vrc == OBS_VIDEO_SUCCESS) {
            obs_enter_graphics();
            s.add(QStringLiteral("设备就绪：type=%1 name=%2 默认 effect=%3")
                      .arg(gs_get_device_type())
                      .arg(QString::fromUtf8(gs_get_device_name()))
                      .arg(obs_get_base_effect(OBS_EFFECT_DEFAULT) ? "已编译" : "空"));
            obs_leave_graphics();

            // --- 全量 effect 编译扫描 --------------------------------------
            int total = 0;
            int good = 0;
            QStringList bad;
            QDirIterator it(root + "/share/libobs", {"*.effect"}, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString file = it.next();
                const QByteArray utf8 = file.toUtf8();
                ++total;
                obs_enter_graphics();
                gs_effect_t *eff = gs_effect_create_from_file(utf8.constData(), nullptr);
                if (eff) {
                    ++good;
                    gs_effect_destroy(eff);
                } else {
                    bad << QFileInfo(file).fileName();
                }
                obs_leave_graphics();
            }
            s.add(QStringLiteral("effect 编译：%1/%2 通过；失败：%3")
                      .arg(good)
                      .arg(total)
                      .arg(bad.isEmpty() ? QStringLiteral("无") : bad.join(", ")));
            if (good != total)
                s.fail(QStringLiteral("有 effect 编译不过"));

            // --- 离屏渲染 + 回读 -------------------------------------------
            const uint32_t W = 64;
            const uint32_t H = 64;
            struct vec4 red = {1.0f, 0.0f, 0.0f, 1.0f};
            struct vec4 black = {0.0f, 0.0f, 0.0f, 1.0f};
            uint8_t solid[4 * 4 * 4];
            for (int i = 0; i < 16; i++) {
                solid[i * 4 + 0] = 10;
                solid[i * 4 + 1] = 120;
                solid[i * 4 + 2] = 240;
                solid[i * 4 + 3] = 255;
            }

            obs_enter_graphics();
            gs_texrender_t *tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
            gs_stagesurf_t *stage = gs_stagesurface_create(W, H, GS_RGBA);
            const uint8_t *spr_data = solid;
            gs_texture_t *spr = gs_texture_create(4, 4, GS_RGBA, 1, &spr_data, 0);

            if (!tr || !stage || !spr) {
                s.fail(QStringLiteral("离屏对象创建失败：texrender=%1 stage=%2 tex=%3")
                           .arg((quintptr)tr != 0).arg((quintptr)stage != 0).arg((quintptr)spr != 0));
            }

            auto grab = [&](const char *tag) -> QString {
                gs_stage_texture(stage, gs_texrender_get_texture(tr));
                uint8_t *px = nullptr;
                uint32_t linesize = 0;
                if (!gs_stagesurface_map(stage, &px, &linesize))
                    return QStringLiteral("%1: gs_stagesurface_map 失败").arg(tag);
                const QString line = QStringLiteral("%1: 首像素=(%2,%3,%4,%5) linesize=%6")
                                         .arg(tag)
                                         .arg(px[0])
                                         .arg(px[1])
                                         .arg(px[2])
                                         .arg(px[3])
                                         .arg(linesize);
                gs_stagesurface_unmap(stage);
                return line;
            };

            if (tr && stage && spr) {
                bool clear_ok = false;
                bool draw_ok = false;

                // 步骤 A：只 clear，验证 FBO + stage + 回读这条通道
                if (gs_texrender_begin(tr, W, H)) {
                    gs_clear(GS_CLEAR_COLOR, &red, 1.0f, 0);
                    gs_texrender_end(tr);
                    const QString info = grab("clear");
                    s.add(info);
                    clear_ok = info.contains(QStringLiteral("(255,0,0,255)"));
                } else {
                    s.fail(QStringLiteral("gs_texrender_begin 失败"));
                }

                // 步骤 B：照搬 obs-video.c:305-317 的真实姿势，用 default.effect 画两次
                //   B1 采样解码开 + 帧缓冲编码开 → 期望字节原样往返（这两个转换必须互相抵消）
                //   B2 只关帧缓冲编码 → 期望与 B1 不同，用来证明 GL_EXT_sRGB_write_control
                //      那个开关真的在改 GL 状态，而不是只在 device 里记账
                // 注意 gs_effect_set_texture 与 _srgb 的差别在 GL 后端是实打实的：
                // gl-subsystem.c 的 device_load_texture_internal 据此设
                // GL_TEXTURE_SRGB_DECODE_EXT = GL_SKIP_DECODE_EXT / GL_DECODE_EXT。
                gs_effect_t *eff = obs_get_base_effect(OBS_EFFECT_DEFAULT);
                gs_technique_t *tech = eff ? gs_effect_get_technique(eff, "Draw") : nullptr;
                if (!tech) {
                    s.fail(QStringLiteral("步骤 B 跳过：default.effect=%1 Draw technique=%2")
                               .arg(eff ? "有" : "空")
                               .arg(tech ? "有" : "空"));
                } else {
                    auto drawOnce = [&](bool encode_to_srgb) -> QString {
                        gs_texrender_reset(tr);
                        if (!gs_texrender_begin(tr, W, H))
                            return QStringLiteral("gs_texrender_begin 失败");

                        gs_enable_blending(false);
                        gs_clear(GS_CLEAR_COLOR, &black, 0.0f, 0);
                        gs_ortho(0.0f, (float)W, 0.0f, (float)H, -100.0f, 100.0f);
                        gs_effect_set_texture_srgb(gs_effect_get_param_by_name(eff, "image"), spr);

                        gs_enable_framebuffer_srgb(encode_to_srgb);
                        const size_t passes = gs_technique_begin(tech);
                        for (size_t i = 0; i < passes; i++) {
                            gs_technique_begin_pass(tech, i);
                            gs_draw_sprite(spr, 0, W, H);
                            gs_technique_end_pass(tech);
                        }
                        gs_technique_end(tech);
                        gs_enable_framebuffer_srgb(false);

                        gs_enable_blending(true);
                        gs_texrender_end(tr);
                        return grab("draw");
                    };

                    const QString on = drawOnce(true);
                    const QString off = drawOnce(false);
                    s.add(QStringLiteral("default.effect/Draw 编码开：%1").arg(on));
                    s.add(QStringLiteral("default.effect/Draw 编码关：%1").arg(off));

                    draw_ok = on.contains(QStringLiteral("(10,120,240,255)"));
                    if (!draw_ok)
                        s.fail(QStringLiteral("采样解码 + 帧缓冲编码没抵消成原值 —— GLSL ES 通道或 sRGB 两端的开关有问题"));
                    if (on == off)
                        s.fail(QStringLiteral("编码开/关两次结果完全一样 —— GL_FRAMEBUFFER_SRGB（GL_EXT_sRGB_write_control）"
                                              "没真正生效"));
                    else
                        s.add(QStringLiteral("GL_FRAMEBUFFER_SRGB 开关有效（关掉编码后结果变成线性写入，与桌面一致）"));
                }

                if (!clear_ok)
                    s.fail(QStringLiteral("clear 回读不是纯红 —— FBO/stagesurface 通道有问题"));
                if (clear_ok && draw_ok)
                    s.add(QStringLiteral("离屏渲染 + GLSL ES 着色器绘制 + 像素回读全部正确"));
            }

            // --- 步骤 C：GS_BGRA 动态纹理 上传→回读 字节往返 -------------------
            // 只验一件事：Android 把 BGRA 系的外部格式换成 GL_RGBA、上传前在 CPU 侧交换
            // R/B（见 gl-subsystem.h 的 convert_gs_upload_format），而回读仍用 GL_BGRA_EXT，
            // 所以整条往返的字节序必须和桌面一模一样。
            {
                uint8_t in_px[4 * 4 * 4];
                for (int i = 0; i < 16; i++) {
                    in_px[i * 4 + 0] = 240; // B
                    in_px[i * 4 + 1] = 120; // G
                    in_px[i * 4 + 2] = 10;  // R
                    in_px[i * 4 + 3] = 255; // A
                }

                gs_texture_t *dyn = gs_texture_create(4, 4, GS_BGRA, 1, nullptr, GS_DYNAMIC);
                gs_stagesurf_t *dyn_stage = gs_stagesurface_create(4, 4, GS_BGRA);
                QString res;
                bool rt_ok = false;

                if (!dyn || !dyn_stage) {
                    res = QStringLiteral("对象创建失败：tex=%1 stage=%2")
                              .arg((quintptr)dyn != 0)
                              .arg((quintptr)dyn_stage != 0);
                } else {
                    uint8_t *lp = nullptr;
                    uint32_t ls = 0;
                    const bool mapped = gs_texture_map(dyn, &lp, &ls);
                    if (mapped) {
                        for (size_t b = 0; b < sizeof(in_px); b++)
                            lp[b] = in_px[b];
                        gs_texture_unmap(dyn);
                        gs_stage_texture(dyn_stage, dyn);

                        uint8_t *px = nullptr;
                        uint32_t linesize = 0;
                        if (gs_stagesurface_map(dyn_stage, &px, &linesize)) {
                            QString bytes;
                            for (int i = 0; i < 4; i++)
                                bytes += QStringLiteral("%1,").arg(px[i]);
                            rt_ok = px[0] == 240 && px[1] == 120 && px[2] == 10 && px[3] == 255;
                            res = QStringLiteral("写 (240,120,10,255) → 读 [%1] linesize=%2 → %3")
                                      .arg(bytes.left(bytes.size() - 1))
                                      .arg(linesize)
                                      .arg(rt_ok ? QStringLiteral("字节一致") : QStringLiteral("不一致"));
                            gs_stagesurface_unmap(dyn_stage);
                        } else {
                            res = QStringLiteral("map 成功但回读 gs_stagesurface_map 失败");
                        }
                    } else {
                        res = QStringLiteral("gs_texture_map 失败（GS_DYNAMIC 的 unpack buffer 路径）");
                    }
                }

                s.add(QStringLiteral("GS_BGRA 字节往返：%1").arg(res));
                if (!rt_ok)
                    s.fail(QStringLiteral("GS_BGRA 往返与写入不一致 —— convert_gs_upload_format / gl_swap_rb_bgra / "
                                          "回读格式三者之一有问题"));

                if (dyn)
                    gs_texture_destroy(dyn);
                if (dyn_stage)
                    gs_stagesurface_destroy(dyn_stage);
            }

            if (spr)
                gs_texture_destroy(spr);
            if (stage)
                gs_stagesurface_destroy(stage);
            if (tr)
                gs_texrender_destroy(tr);
            obs_leave_graphics();
        }

        obs_shutdown();
        s.add(QStringLiteral("obs_shutdown 完成（graphics 线程与设备干净退出）"));
        base_set_log_handler(nullptr, nullptr);
    }

    s.add(s.ok ? QStringLiteral("=== 结论：M2 通过 —— GLES 后端能建设备、编 effect、渲染并回读 ===")
               : QStringLiteral("=== 结论：M2 未通过 ==="));

    for (const QString &l : s.lines)
        SMK("%s", l.toUtf8().constData());
    return s.lines.join('\n');
}

// ---------------------------------------------------------------------------
// 里程碑 M3：插件链路。
//   1) obs_load_all_modules() 能不能在 Android 上把插件 .so dlopen 进来（模块搜索路径
//      由 obs-android.c 的 OBS_ROOT_PATH/lib + share/obs/obs-plugins/%module% 约定）；
//   2) 插件注册的类型（color source / obs_x264 / encode_sink）是否真的可创建；
//   3) 视频线程带着真实源跑起来，并把一帧帧 RGBA 喂给 x264 编出 H.264 码流。
// ---------------------------------------------------------------------------

// 插件模块名（不带前后缀）。进 APK 时叫 lib<name>.so（androiddeployqt 硬性要求 EXTRA_LIBS
// 的文件名以 "lib" 开头，实测不过就报 "must begin with \"lib\""），挂给 libobs 时保留这个
// 名字即可 —— make_data_directory()（obs-module.c:630）会去掉 "lib" 前缀再找一遍 %module% 资源目录。
static const char *const kObsPlugins[] = {"image-source", "obs-x264", "obs-encode-sink", "android-audio",
                                          "android-capture"};
constexpr int kObsPluginCount = int(sizeof(kObsPlugins) / sizeof(kObsPlugins[0]));

// 实测：Qt6/AGP 默认不解包 native 库 —— 装机目录 lib/x86_64/ 是空的，dladdr 拿到的是
// ".../base.apk!/lib/x86_64/liblibobs.so" 这种带 "!/" 的虚拟路径，既不能 access 也不能 symlink。
// 所以这里走另一条路：插件 .so 同时打成 Qt 资源（:/obspluginlibs），运行时解到
// $OBS_ROOT_PATH/lib/ 让 libobs 自己去 glob。**Android 15 允不允许 dlopen 应用私有可写目录里
// 的文件，正是这一轮要测的东西** —— 允许则 M3 直接过；不允许则 os_dlopen 的 dlerror 会进
// logcat，届时改用 extractNativeLibs(legacy packaging) 让库落到真实只读目录再软链过去。
static void stagePlugins(Smoke &s, const QString &root)
{
    Dl_info di = {};
    if (dladdr((const void *) obs_startup, &di) && di.dli_fname)
        s.add(QStringLiteral("dladdr(obs_startup) = %1").arg(QString::fromUtf8(di.dli_fname)));
    else
        s.add(QStringLiteral("dladdr(obs_startup) 失败（只影响信息性输出，不影响加载路径）"));

    int files = 0;
    if (!extractTree(QStringLiteral(":/obspluginlibs"), root + "/lib", &files))
        s.fail(QStringLiteral("解包 :/obspluginlibs 失败（检查 CMake 资源是否打进 APK）"));

    for (const char *name : kObsPlugins) {
        const QString so = root + "/lib/lib" + QString::fromUtf8(name) + ".so";
        QFileInfo fi(so);
        if (!fi.exists()) {
            s.fail(QStringLiteral("解包后没有 %1").arg(so));
            continue;
        }
        // 0644：可写目录里的私有文件，不给任何 exec/other-write 位，避免多一个变量
        if (chmod(so.toUtf8().constData(), 0644) != 0) {
            const int err = errno;
            s.add(QStringLiteral("chmod(0644, %1) 失败：%2").arg(so).arg(QString::fromUtf8(strerror(err))));
        }
        s.add(QStringLiteral("待 dlopen：%1  %2 字节  perms=%3")
                  .arg(so)
                  .arg(fi.size())
                  .arg(fi.permissions() & QFile::WriteOwner ? "owner-writable" : "ro"));
    }
    s.add(QStringLiteral("插件 .so 解包 %1/%2 个 → %3/lib").arg(files).arg(kObsPluginCount).arg(root));
    if (files != kObsPluginCount)
        s.fail(QStringLiteral("有插件没解包出来，obs_load_all_modules 必然漏加载"));
}

static QStringList enumTypeIds(bool (*enum_fn)(size_t, const char **))
{
    QStringList out;
    for (size_t i = 0;; i++) {
        const char *id = nullptr;
        if (!enum_fn(i, &id) || !id)
            break;
        out << QString::fromUtf8(id);
        if (out.size() > 256)
            break;
    }
    return out;
}

struct NalStats {
    int sps = 0;
    int pps = 0;
    int idr = 0;
    int other = 0;

    int total() const { return sps + pps + idr + other; }
};

// Annex-B 起止码扫描：只数 NAL 类型，不做完整解析（够用：证明码流是真 H.264 而非噪声）
static NalStats scanNals(const QByteArray &buf)
{
    NalStats st;
    for (int i = 0; i + 3 < buf.size();) {
        int sc = 0;
        const auto b = [&buf](int k) { return (unsigned char) buf[k]; };
        if (b(i) == 0 && b(i + 1) == 0 && b(i + 2) == 0 && b(i + 3) == 1)
            sc = 4;
        else if (b(i) == 0 && b(i + 1) == 0 && b(i + 2) == 1)
            sc = 3;
        if (!sc) {
            i++;
            continue;
        }
        switch (b(i + sc) & 0x1F) {
        case 7:
            st.sps++;
            break;
        case 8:
            st.pps++;
            break;
        case 5:
            st.idr++;
            break;
        default:
            st.other++;
            break;
        }
        i += sc + 1;
    }
    return st;
}

QString runObsModuleSmoke()
{
    Smoke s;
    s.add(QStringLiteral("=== 插件冒烟 M3：模块加载 + color source + x264 编码 ==="));

    QString root;
    const bool prepared = prepareRoot(s, &root);
    bool started = false;

    if (prepared) {
        int pluginDataFiles = 0;
        if (extractTree(QStringLiteral(":/obsplugindata"), root + "/share/obs/obs-plugins", &pluginDataFiles))
            s.add(QStringLiteral("插件资源解包 %1 个文件 → share/obs/obs-plugins").arg(pluginDataFiles));
        else
            s.fail(QStringLiteral("解包 :/obsplugindata 失败（检查 CMake 资源是否打进 APK）"));

        stagePlugins(s, root);

        base_set_log_handler(libobsLog, nullptr);
        started = obs_startup("en-US", nullptr, nullptr);
        if (!started)
            s.fail(QStringLiteral("obs_startup 返回 false（细节看 logcat -s OBS-libobs）"));
    }

    if (!started) {
        s.add(s.ok ? QStringLiteral("=== 结论：M3 未开始（startup 就没过）===") : QStringLiteral("=== 结论：M3 未通过 ==="));
        for (const QString &l : s.lines)
            SMK("%s", l.toUtf8().constData());
        return s.lines.join('\n');
    }

    // --- 1. 模块加载链 ------------------------------------------------------
    obs_load_all_modules();
    obs_post_load_modules();

    const QStringList srcTypes = enumTypeIds(obs_enum_source_types);
    const QStringList encTypes = enumTypeIds(obs_enum_encoder_types);
    const QStringList outTypes = enumTypeIds(obs_enum_output_types);
    s.add(QStringLiteral("源类型 %1 个：%2").arg(srcTypes.size()).arg(srcTypes.join(", ")));
    s.add(QStringLiteral("编码器类型：%1").arg(encTypes.join(", ")));
    s.add(QStringLiteral("输出类型：%1").arg(outTypes.join(", ")));

    for (const char *want : {"color_source", "image_source"}) {
        if (!srcTypes.contains(QString::fromUtf8(want)))
            s.fail(QStringLiteral("image-source 没注册 %1 —— 模块没加载成功").arg(want));
    }
    if (!encTypes.contains("obs_x264"))
        s.fail(QStringLiteral("obs-x264 没注册 obs_x264 —— 模块没加载成功"));
    if (!outTypes.contains("encode_sink"))
        s.fail(QStringLiteral("obs-encode-sink 没注册 encode_sink —— 模块没加载成功"));

    const bool chain_ok = s.ok;

    // --- 2. 视频线程 + 真实源 -----------------------------------------------
    struct obs_video_info ovi = {};
    ovi.graphics_module = "libobs-opengl";
    ovi.fps_num = 30;
    ovi.fps_den = 1;
    ovi.base_width = 320;
    ovi.base_height = 180;
    ovi.output_width = 320;
    ovi.output_height = 180;
    // 标签必须写 BGRA 而不是 RGBA：混合纹理与 CPU 侧帧都是 GS_BGRA 的内存序 (B,G,R,A)
    // （obs.c:358/396/425 恒用 GS_BGRA，obs-video.c:758 的 copy_rgbx_frame 是纯 memcpy），
    // 而编码器侧的转换是 video-io.c:346 用 video->info.format 当 "from" 建 swscale
    // （video-scaler-ffmpeg.c:46: VIDEO_FORMAT_RGBA → AV_PIX_FMT_RGBA）。
    // 上一轮标成 RGBA，实测 0xFFE04828 编出来的码流解回像素是 (224,72,41) —— R/B 反了。
    ovi.output_format = VIDEO_FORMAT_BGRA;
    ovi.colorspace = VIDEO_CS_DEFAULT;
    ovi.range = VIDEO_RANGE_DEFAULT;
    ovi.scale_type = OBS_SCALE_BICUBIC;
    ovi.gpu_conversion = false;

    const int vrc = obs_reset_video(&ovi);
    const QString fmtName =
        ovi.output_format == VIDEO_FORMAT_BGRA ? QStringLiteral("BGRA") : QStringLiteral("RGBA");
    s.add(QStringLiteral("obs_reset_video(320x180@30, output_format=%1) → %2")
              .arg(fmtName)
              .arg(videoErrName(vrc)));
    if (vrc != OBS_VIDEO_SUCCESS)
        s.fail(QStringLiteral("视频线程没起来"));

    obs_source_t *src = nullptr;
    const uint64_t framesBefore = (vrc == OBS_VIDEO_SUCCESS) ? video_output_get_total_frames(obs_get_video()) : 0;

    if (vrc == OBS_VIDEO_SUCCESS && chain_ok) {
        // vec4_from_rgba 是 memcpy 到 u[4]，小端下 u[0]=R … u[3]=A，所以这个整数的
        // 字节序是 0xAABBGGRR：下面故意选 R=0x28 G=0x48 B=0xE0 A=0xFF，回读像素能反证。
        obs_data_t *cs = obs_data_create();
        obs_data_set_int(cs, "color", 0xFFE04828LL);
        obs_data_set_int(cs, "width", 320);
        obs_data_set_int(cs, "height", 180);
        src = obs_source_create("color_source", "m3-color", cs, nullptr);
        obs_data_release(cs);

        if (!src) {
            s.fail(QStringLiteral("obs_source_create(color_source) 返回空"));
        } else {
            obs_set_output_source(0, src);
            s.add(QStringLiteral("color_source 已挂到输出通道 0（R=0x28 G=0x48 B=0xE0 A=0xFF, 320x180）"));
            // 把"源本身有没有被识别成 320x180 的活跃源"单独记下来：万一以后又读到全黑，
            // 能一眼分清是"源没进链"还是"回读姿势不对"。
            const uint32_t sw = obs_source_get_width(src);
            const uint32_t sh = obs_source_get_height(src);
            s.add(QStringLiteral("obs_source_get_size → %1x%2；active=%3 showing=%4 enabled=%5")
                      .arg(sw)
                      .arg(sh)
                      .arg(obs_source_active(src) ? "true" : "false")
                      .arg(obs_source_showing(src) ? "true" : "false")
                      .arg(obs_source_enabled(src) ? "true" : "false"));
            if (sw != 320 || sh != 180)
                s.fail(QStringLiteral("color source 报告的尺寸不是 320x180 —— settings 没生效"));
        }
    }

    // --- 3. x264 编码链 -----------------------------------------------------
    obs_encoder_t *enc = nullptr;
    obs_output_t *out = nullptr;
    const QString streamPath = root + "/m3-encode.h264";

    if (src) {
        QFile::remove(streamPath);

        obs_data_t *es = obs_data_create();
        obs_data_set_string(es, "rate_control", "CBR");
        obs_data_set_int(es, "bitrate", 1500);
        obs_data_set_int(es, "keyint_sec", 1);
        obs_data_set_string(es, "preset", "ultrafast");
        obs_data_set_string(es, "tune", "zerolatency");
        obs_data_set_string(es, "profile", "baseline");

        enc = obs_video_encoder_create("obs_x264", "m3-x264", es, nullptr);
        if (!enc) {
            s.fail(QStringLiteral("obs_video_encoder_create(obs_x264) 返回空"));
        } else {
            // 顺序有讲究：obs_encoder_update() 才会去调插件的 create()，而 x264 的
            // create() 里直接 video_output_get_info(obs_encoder_video(encoder))，
            // 所以必须先把视频输出挂上去。
            obs_encoder_set_video(enc, obs_get_video());
            obs_encoder_update(enc, es);
        }
        obs_data_release(es);
    }

    if (enc) {
        obs_data_t *os = obs_data_create();
        obs_data_set_string(os, "path", streamPath.toUtf8().constData());
        out = obs_output_create("encode_sink", "m3-sink", os, nullptr);
        obs_data_release(os);

        if (!out) {
            s.fail(QStringLiteral("obs_output_create(encode_sink) 返回空"));
        } else {
            obs_output_set_video_encoder(out, enc);
            const bool outStarted = obs_output_start(out);
            s.add(QStringLiteral("obs_output_start(encode_sink) → %1").arg(outStarted ? "true" : "false"));
            if (!outStarted)
                s.fail(QStringLiteral("输出启动失败：%1")
                           .arg(QString::fromUtf8(obs_output_get_last_error(out) ? obs_output_get_last_error(out) : "无错误串")));

            int waited = 0;
            while (waited < 3000 && obs_encoder_get_encoded_frames(enc) < 10) {
                QThread::msleep(50);
                waited += 50;
            }

            const uint32_t encFrames = obs_encoder_get_encoded_frames(enc);
            const uint64_t framesAfter = video_output_get_total_frames(obs_get_video());
            s.add(QStringLiteral("视频线程产出帧数增量 = %1（%2 → %3）")
                      .arg(framesAfter - framesBefore)
                      .arg(framesBefore)
                      .arg(framesAfter));
            s.add(QStringLiteral("x264 已编码 %1 帧，耗时 %2 ms；编码器错误=%3")
                      .arg(encFrames)
                      .arg(waited)
                      .arg(QString::fromUtf8(obs_encoder_get_last_error(enc) ? obs_encoder_get_last_error(enc) : "无")));
            if (framesAfter <= framesBefore)
                s.fail(QStringLiteral("视频线程一帧都没出 —— color source 挂上去后 tick 停了"));
            if (encFrames == 0)
                s.fail(QStringLiteral("x264 一帧都没编 —— 渲染→video_output→编码器这条链断了"));

            uint8_t *extra = nullptr;
            size_t extraSize = 0;
            if (obs_encoder_get_extra_data(enc, &extra, &extraSize))
                s.add(QStringLiteral("obs_encoder_get_extra_data：%1 字节（SPS/PPS in-band）").arg(extraSize));
            else
                s.add(QStringLiteral("obs_encoder_get_extra_data：无（x264 走 Annex-B 带内参数集）"));

            obs_output_stop(out);
            for (int i = 0; i < 60 && obs_output_active(out); i++)
                QThread::msleep(50);
        }
    }

    // --- 4. 合成结果回读（证明 color source 真在画面里）----------------------
    if (src) {
        obs_enter_graphics();
        // obs_get_main_texture() 返回视频线程上一帧的合成结果（render_texture），
        // 前提是该线程已把 texture_rendered 置位 —— 拿不到就说明合成链没跑完。
        gs_texture_t *mainTex = obs_get_main_texture();
        QString line;
        bool nonBlack = false;
        if (!mainTex) {
            line = QStringLiteral("obs_get_main_texture 返回空（texture_rendered 未置位）");
        } else {
            const uint32_t tw = gs_texture_get_width(mainTex);
            const uint32_t th = gs_texture_get_height(mainTex);
            // 混合纹理恒为 GS_BGRA（obs.c:358/425），和 ovi.output_format 无关。
            // stagesurface 的格式必须和它一致：gl-stagesurf.c:97 对不上就直接 return false，
            // 而 gs_stage_texture() 是 void，于是 map 出来一整张全 0 —— 上一轮"全黑"就是这么来的。
            const enum gs_color_format fmt = gs_texture_get_color_format(mainTex);
            gs_stagesurf_t *stage = gs_stagesurface_create(tw, th, fmt);
            gs_stage_texture(stage, mainTex);
            uint8_t *px = nullptr;
            uint32_t linesize = 0;
            if (stage && gs_stagesurface_map(stage, &px, &linesize)) {
                const uint8_t *cp = px + (th / 2) * linesize + (tw / 2) * 4;
                // color=0xFFE04828 → R=0x28(40) G=0x48(72) B=0xE0(224) A=0xFF(255)。
                // 内存字节序跟 stagesurface 的格式走：GS_BGRA 是 (B,G,R,A)，GS_RGBA 是 (R,G,B,A)。
                // 只作信息项 —— shader/sRGB 往返可能让字节差一点，"全黑"才是硬失败。
                const bool bgra = (fmt == GS_BGRA);
                const int want0 = bgra ? 224 : 40;
                const int want2 = bgra ? 40 : 224;
                const bool matches = std::abs(int(cp[0]) - want0) <= 8 && std::abs(int(cp[1]) - 72) <= 8 &&
                                     std::abs(int(cp[2]) - want2) <= 8 && std::abs(int(cp[3]) - 255) <= 8;
                line = QStringLiteral("%1x%2 format=%3(%4) 中心像素=(%5,%6,%7,%8) 期望≈(%9,%10,%11,255) → %12  linesize=%13")
                           .arg(tw)
                           .arg(th)
                           .arg((int) fmt)
                           .arg(bgra ? QStringLiteral("GS_BGRA") : QStringLiteral("GS_RGBA"))
                           .arg(cp[0])
                           .arg(cp[1])
                           .arg(cp[2])
                           .arg(cp[3])
                           .arg(want0)
                           .arg(72)
                           .arg(want2)
                           .arg(matches ? QStringLiteral("吻合") : QStringLiteral("不吻合"))
                           .arg(linesize);
                nonBlack = cp[0] || cp[1] || cp[2];
                gs_stagesurface_unmap(stage);
            } else {
                line = QStringLiteral("stagesurface(%1x%2 format=%3) 创建或 map 失败").arg(tw).arg(th).arg((int) fmt);
            }
            if (stage)
                gs_stagesurface_destroy(stage);
        }
        obs_leave_graphics();
        s.add(QStringLiteral("obs_get_main_texture 回读：%1").arg(line));
        if (!nonBlack)
            s.fail(QStringLiteral("合成画面全黑 —— color source 没被渲染进主纹理"));
    }

    // --- 5. 码流核对 + 收尾 --------------------------------------------------
    if (out) {
        obs_output_release(out);

        QFile f(streamPath);
        if (!f.open(QIODevice::ReadOnly)) {
            s.fail(QStringLiteral("打不开码流文件 %1").arg(streamPath));
        } else {
            const QByteArray buf = f.readAll();
            const NalStats st = scanNals(buf);
            s.add(QStringLiteral("码流 %1 字节，NAL：SPS=%2 PPS=%3 IDR=%4 其它=%5")
                      .arg(buf.size())
                      .arg(st.sps)
                      .arg(st.pps)
                      .arg(st.idr)
                      .arg(st.other));
            if (buf.size() < 64 || st.total() == 0)
                s.fail(QStringLiteral("码流太短或没有 NAL —— x264 输出的不是可辨识的 H.264 Annex-B"));
            if (!st.sps || !st.pps || !st.idr)
                s.fail(QStringLiteral("缺 SPS/PPS/IDR 之一（sps=%1 pps=%2 idr=%3）").arg(st.sps).arg(st.pps).arg(st.idr));
        }
    }

    if (src) {
        obs_set_output_source(0, nullptr);
        obs_source_release(src);
    }
    if (enc)
        obs_encoder_release(enc);

    obs_shutdown();
    s.add(QStringLiteral("obs_shutdown 完成（插件卸载 + 视频线程干净退出）"));
    base_set_log_handler(nullptr, nullptr);

    s.add(s.ok ? QStringLiteral("=== 结论：M3 通过 —— 插件加载链、真实源渲染、x264 出流全部成立 ===")
               : QStringLiteral("=== 结论：M3 未通过 ==="));

    for (const QString &l : s.lines)
        SMK("%s", l.toUtf8().constData());
    return s.lines.join('\n');
}

// ---------------------------------------------------------------------------
// 阶段 2 · A1：AAudio 采集源。要证明的是"设备麦克风 → AAudio → obs_source_output_audio
// → libobs 音频链"这条路成立。
//
// 两条本树实测/读码得到的前提，缺一不可：
//   1) libobs 的主音频链不需要平台音频后端：media-io/audio-io.c:353-390 的
//      audio_output_open() 只校验参数 + 分配 + pthread_create(audio_thread)，不开播放设备。
//      AAudio 只在往扬声器"监听"时才需要（阶段 5）。
//   2) 但源的 activate 回调**只在视频线程里被调**：obs-source.c 的 obs_source_video_tick()
//      (:1394 起) 里比较 activate_refs 变化后才 activate_source() (:1444)。
//      obs_source_active() (:4809) 返回的只是 activate_refs != 0，不代表回调已经跑过。
//      → 不 obs_reset_video 就没有视频线程，采集源永远不会 activate。
//      这一条是被实测打出来的：第一版这里故意不建视频，结果"active=true 但插件零日志零帧"。
// ---------------------------------------------------------------------------

namespace {
struct AudioProbe {
    int callbacks = 0;
    quint64 frames = 0;
    int planes = 0;
    float peak = 0.0f;
    quint64 first_ts = 0;
    quint64 last_ts = 0;
};

// 采集线程写、主线程读：诊断工装，允许读到略微过期的计数
AudioProbe g_ap;

void audioProbeCb(void *, obs_source_t *, const struct audio_data *audio, bool)
{
    if (!audio || !audio->frames)
        return;

    g_ap.callbacks++;
    g_ap.frames += audio->frames;
    if (!g_ap.first_ts)
        g_ap.first_ts = audio->timestamp;
    g_ap.last_ts = audio->timestamp;

    int planes = 0;
    for (int p = 0; p < MAX_AV_PLANES; p++) {
        if (!audio->data[p])
            continue;
        planes++;
        // 主音频格式恒为 AUDIO_FORMAT_FLOAT_PLANAR（obs.c:1630），一个 plane 就是一路 float
        const float *f = reinterpret_cast<const float *>(audio->data[p]);
        for (uint32_t i = 0; i < audio->frames; i++) {
            const float v = f[i] < 0.0f ? -f[i] : f[i];
            if (v > g_ap.peak)
                g_ap.peak = v;
        }
    }
    if (planes > g_ap.planes)
        g_ap.planes = planes;
}
} // namespace

QString runObsAudioSmoke()
{
    Smoke s;
    s.add(QStringLiteral("=== 音频冒烟 A1：AAudio 采集源 → libobs 音频链 ==="));

    QString root;
    const bool prepared = prepareRoot(s, &root);
    bool started = false;

    if (prepared) {
        stagePlugins(s, root);
        base_set_log_handler(libobsLog, nullptr);
        started = obs_startup("en-US", nullptr, nullptr);
        if (!started)
            s.fail(QStringLiteral("obs_startup 返回 false（细节看 logcat -s OBS-libobs）"));
    }

    if (!started) {
        s.add(QStringLiteral("=== 结论：A1 未通过（startup 就没过）==="));
        for (const QString &l : s.lines)
            SMK("%s", l.toUtf8().constData());
        return s.lines.join('\n');
    }

    // --- 1. 源类型注册 ---
    obs_load_all_modules();
    obs_post_load_modules();

    const QStringList srcTypes = enumTypeIds(obs_enum_source_types);
    const bool registered = srcTypes.contains("android_audio_input");
    s.add(QStringLiteral("源类型 %1 个，android_audio_input 已注册 = %2")
              .arg(srcTypes.size())
              .arg(registered ? "true" : "false"));
    if (!registered)
        s.fail(QStringLiteral("android-audio 没注册 android_audio_input —— 模块没加载成功"));

    // --- 2. RECORD_AUDIO 授权状态（把"没权限→静音"和"设备没麦"分开）---
#ifdef Q_OS_ANDROID
    {
        bool granted = false;
        // Qt 6.9.3 的暴露方式（javap org.qtproject.qt.android.QtNative 实测）：
        //   static Context getContext() / static Activity activity() —— 不是 Kotlin 属性名 context()
        QJniObject ctx;
        const char *const getters[] = {"getContext", "activity"};
        for (const char *g : getters) {
            ctx = QJniObject::callStaticObjectMethod("org/qtproject/qt/android/QtNative", g,
                                                     "()Landroid/content/Context;");
            if (ctx.isValid())
                break;
        }
        if (!ctx.isValid()) {
            s.add(QStringLiteral("RECORD_AUDIO 授权：QtNative.getContext()/activity() 都拿不到 Context，未查"));
        } else {
            QJniObject perm = QJniObject::fromString(QStringLiteral("android.permission.RECORD_AUDIO"));
            const jint st = ctx.callMethod<jint>("checkSelfPermission", "(Ljava/lang/String;)I", perm.object<jstring>());
            granted = (st == 0);
            s.add(QStringLiteral("RECORD_AUDIO 授权 = %1（PackageManager 约定 0=GRANTED / -1=DENIED）")
                      .arg(granted ? QStringLiteral("GRANTED") : QStringLiteral("DENIED")));
        }
    }
#else
    s.add(QStringLiteral("RECORD_AUDIO 授权：非 Android 平台，跳过"));
#endif

    // --- 3. libobs 的音频线程（软件混音，不需要平台播放设备）---
    struct obs_audio_info ai = {};
    ai.samples_per_sec = 48000;
    ai.speakers = SPEAKERS_STEREO;

    const bool audio_ok = obs_reset_audio(&ai);
    s.add(QStringLiteral("obs_reset_audio(48000, SPEAKERS_STEREO) → %1").arg(audio_ok ? "true" : "false"));
    if (!audio_ok)
        s.fail(QStringLiteral("libobs 的音频线程起不来"));

    // --- 3b. 视频线程：不建它，采集源的 activate 永远不会被调（见本节头注释）---
    struct obs_video_info ovi = {};
    ovi.graphics_module = "libobs-opengl";
    ovi.fps_num = 30;
    ovi.fps_den = 1;
    ovi.base_width = 320;
    ovi.base_height = 180;
    ovi.output_width = 320;
    ovi.output_height = 180;
    ovi.output_format = VIDEO_FORMAT_BGRA;
    ovi.colorspace = VIDEO_CS_DEFAULT;
    ovi.range = VIDEO_RANGE_DEFAULT;
    ovi.scale_type = OBS_SCALE_BICUBIC;
    ovi.gpu_conversion = false;

    const int vrc = obs_reset_video(&ovi);
    s.add(QStringLiteral("obs_reset_video(320x180@30) → %1").arg(videoErrName(vrc)));
    if (vrc != OBS_VIDEO_SUCCESS)
        s.fail(QStringLiteral("没有视频线程就没有 tick，源的 activate 回调不会被调用"));

    // --- 4. 建源 + 挂回调 + 进主通道 ---
    obs_source_t *src = obs_source_create("android_audio_input", "aaudio-probe", nullptr, nullptr);
    if (!src) {
        s.fail(QStringLiteral("obs_source_create(android_audio_input) 返回空"));
    } else {
        g_ap = AudioProbe();
        obs_source_add_audio_capture_callback(src, audioProbeCb, nullptr);
        obs_set_output_source(0, src);

        // activate 是在视频线程的 tick 里补调的，不是 obs_set_output_source 里同步调的
        QThread::msleep(400);
        s.add(QStringLiteral("源状态（等视频线程 tick 过之后）：active=%1 showing=%2 enabled=%3")
                  .arg(obs_source_active(src) ? "true" : "false")
                  .arg(obs_source_showing(src) ? "true" : "false")
                  .arg(obs_source_enabled(src) ? "true" : "false"));

        // 等第一帧，最多 4 秒；再多收 500 ms 让统计有代表性
        for (int i = 0; i < 40 && g_ap.frames == 0; i++)
            QThread::msleep(100);
        QThread::msleep(500);

        const double seconds = (double) g_ap.frames / 48000.0;
        const double ts_span_ms = (g_ap.last_ts > g_ap.first_ts) ? (double) (g_ap.last_ts - g_ap.first_ts) / 1e6 : 0.0;
        s.add(QStringLiteral("AAudio 采集：%1 次回调 / %2 帧 ≈ %3 ms / plane=%4 / 峰值 %5 / 时间戳跨度 %6 ms")
                  .arg(g_ap.callbacks)
                  .arg(g_ap.frames)
                  .arg(seconds * 1000.0, 0, 'f', 1)
                  .arg(g_ap.planes)
                  .arg((double) g_ap.peak, 0, 'f', 5)
                  .arg(ts_span_ms, 0, 'f', 1));

        if (g_ap.frames == 0) {
            s.fail(QStringLiteral("一帧音频都没收到 —— AAudio 采集链没通（原因看 logcat -s OBS-libobs 里 "
                                  "\"aaaudio:\" 那几行：打不开流 = 权限/无设备，开了流但零帧 = 读不到数据）"));
        } else if (g_ap.peak <= 0.0001f) {
            // 不判失败：授权被拒时 Android 会给一路静音；模拟器也可能没有真实麦克风
            s.add(QStringLiteral("峰值≈0：链路通了但内容是静音 —— 授权 DENIED 就是权限问题，"
                                 "GRANTED 就是这台设备没往流里送声音"));
        }

        // 拆链：这一步同时验证 deactivate 停采集线程不会卡死
        obs_set_output_source(0, nullptr);
        obs_source_remove_audio_capture_callback(src, audioProbeCb, nullptr);
        obs_source_release(src);
        s.add(QStringLiteral("拆链完成（deactivate 停线程 + 摘回调 + release 都没卡）"));
    }

    obs_shutdown();
    s.add(QStringLiteral("obs_shutdown 完成（音频线程干净退出）"));
    base_set_log_handler(nullptr, nullptr);

    s.add(s.ok ? QStringLiteral("=== 结论：A1 通过 —— AAudio 采集源把真实音频帧送进了 libobs 音频链 ===")
               : QStringLiteral("=== 结论：A1 未通过 ==="));

    for (const QString &l : s.lines)
        SMK("%s", l.toUtf8().constData());
    return s.lines.join('\n');
}

// ---------------------------------------------------------------------------
// 阶段 2 · A2：USB fd 传递基建。
//
// 计划 §四 阶段 2 的第 1 项（USB 声卡）和第 2 项（USB Webcam）共用同一条通道：
// Java 侧 UsbManager 申请权限并 openDevice() → UsbDeviceConnection.getFileDescriptor()
// → 把这个 int fd 传进 native → libusb_wrap_sys_device()（libusb 的 Android 无 root 模式）。
// libuvc 0.0.7 已经把后半段打包成 uvc_wrap()，所以插件里只需一个 usb_fd 设置。
//
// OTG 真机拿不到（唯一可用设备是 MuMu 模拟器），所以这一轮只测模拟器上**可观测**的部分：
//   a) 静态链了 libusb/libuvc/libjpeg 的插件 .so 能不能在 Android 15 上 dlopen 成功；
//   b) Java 侧 UsbManager 的枚举调用能不能正常返回（模拟器上数量应为 0，但调用本身要成立）；
//   c) 插件对 fd 的三种取值各走各的分支，且分支结果可观测：
//        fd = 0    → 设计上"等 Java 下发"的空闲态：不许报错、不许让源创建失败
//        fd = -1   → Java 侧 getFileDescriptor() 的失败返回值（授权被拒/设备已拔），
//                    插件必须在进 libusb 之前就拦下并单独喊一声。
//                    （原本设想把 -1 直接喂给 libusb 看它短路返回 NOT_SUPPORTED，实测
//                     否掉了：那样等于把"用户拒授权"伪装成"还没给 fd"，是最难查的一类 bug）
//        fd = 真实但非 usbfs 节点的 fd → 要走到 libusb 内部才失败，
//                    这一条是唯一能在无设备时证明"真的进到 libusb 里面了"的手段
//   d) 三种情况拆链都不卡死。
// 首轮实测：第三条打出 "uvc: uvc_init 失败 -1 (I/O error)"，静态链进来的 libusb 确实执行了；
// 这台机器上没有可用 usbfs 后端，所以 uvc_wrap 的错误码仍然测不到。
// ---------------------------------------------------------------------------

namespace {
struct UsbCase {
    bool created = false;
    int w = 0, h = 0;
    bool sawIdle = false;
    bool sawBadFd = false;
    bool sawWrap = false;
    bool sawInitFail = false;
};

// 建一个 android_video_input 源、塞进主通道等 activate、把插件走到哪个分支读回来。
// 判分支只能靠日志（插件不会把内部错误码暴露在 obs API 上），所以这里配了 logRingHas。
UsbCase runUsbFdCase(Smoke &s, const char *tag, qint64 fd)
{
    UsbCase r;

    obs_data_t *st = obs_data_create();
    obs_data_set_int(st, "usb_fd", fd);
    obs_data_set_int(st, "width", 640);
    obs_data_set_int(st, "height", 480);
    obs_data_set_int(st, "fps", 30);
    obs_data_set_string(st, "video_format", "yuyv");

    logRingReset();

    obs_source_t *src = obs_source_create("android_video_input", tag, st, nullptr);
    obs_data_release(st);
    if (!src) {
        s.fail(QStringLiteral("%1: obs_source_create 返回空（fd=%2）").arg(tag).arg(fd));
        return r;
    }
    r.created = true;

    obs_set_output_source(0, src);
    QThread::msleep(700); // activate 是视频线程 tick 里补调的，不是 set_output_source 同步调的

    r.w = obs_source_get_width(src);
    r.h = obs_source_get_height(src);
    r.sawIdle = logRingHas("还没有 USB fd");
    r.sawBadFd = logRingHas("拿到的 fd 是"); // 2-3 起这条也可能是总线给的，措辞已改成中性
    r.sawWrap = logRingHas("uvc_wrap(");
    r.sawInitFail = logRingHas("uvc_init 失败");

    obs_set_output_source(0, nullptr);
    obs_source_release(src);
    QThread::msleep(150);
    return r;
}

void reportUsbCase(Smoke &s, const char *tag, qint64 fd, const UsbCase &r)
{
    s.add(QStringLiteral("  %1（fd=%2）：创建=%3 帧尺寸=%4x%5 空闲态=%6 坏fd已拦=%7 进到 uvc_wrap=%8 uvc_init 失败=%9")
              .arg(tag)
              .arg(fd)
              .arg(r.created ? "ok" : "NULL")
              .arg(r.w)
              .arg(r.h)
              .arg(r.sawIdle ? "是" : "否")
              .arg(r.sawBadFd ? "是" : "否")
              .arg(r.sawWrap ? "是" : "否")
              .arg(r.sawInitFail ? "是" : "否"));
}

/* A3：只填设备名（不填 fd），看插件是不是真的通过 libobs 总线问到了答案。
 * 分支判据同样是日志——插件不会把"桥通没通"暴露在 obs API 上。 */
struct UsbBridgeCase {
    bool created = false;
    int w = 0, h = 0;
    bool sawNoBus = false;    // "USB 总线不可用"
    bool sawNotListed = false; // "不在当前 USB 清单里"
    bool sawBusFd = false;    // "已通过总线拿到"
    bool sawIdle = false;
    bool sawBadFd = false;
    bool sawWrap = false;
};

UsbBridgeCase runUsbDeviceCase(Smoke &s, const char *tag, const QString &device)
{
    UsbBridgeCase r;

    obs_data_t *st = obs_data_create();
    obs_data_set_string(st, "usb_device", device.toUtf8().constData());
    obs_data_set_int(st, "width", 640);
    obs_data_set_int(st, "height", 480);
    obs_data_set_int(st, "fps", 30);
    obs_data_set_string(st, "video_format", "yuyv");

    logRingReset();

    obs_source_t *src = obs_source_create("android_video_input", tag, st, nullptr);
    obs_data_release(st);
    if (!src) {
        s.fail(QStringLiteral("%1: obs_source_create 返回空（usb_device=%2）").arg(tag).arg(device));
        return r;
    }
    r.created = true;

    obs_set_output_source(0, src);
    QThread::msleep(700); // activate 在视频线程 tick 里补调

    r.w = obs_source_get_width(src);
    r.h = obs_source_get_height(src);
    r.sawNoBus = logRingHas("USB 总线不可用");
    r.sawNotListed = logRingHas("不在当前 USB 清单里");
    r.sawBusFd = logRingHas("已通过总线拿到");
    r.sawIdle = logRingHas("还没有 USB fd");
    r.sawBadFd = logRingHas("拿到的 fd 是");
    r.sawWrap = logRingHas("uvc_wrap(");

    obs_set_output_source(0, nullptr);
    obs_source_release(src);
    QThread::msleep(150);
    return r;
}
} // namespace

QString runObsUsbSmoke()
{
    Smoke s;
    s.add(QStringLiteral("=== USB 冒烟 A2：fd 传递基建 + android-capture 无设备路径 ==="));

    QString root;
    const bool prepared = prepareRoot(s, &root);
    bool started = false;

    if (prepared) {
        stagePlugins(s, root);
        base_set_log_handler(libobsLog, nullptr);
        started = obs_startup("en-US", nullptr, nullptr);
        if (!started)
            s.fail(QStringLiteral("obs_startup 返回 false（细节看 logcat -s OBS-libobs）"));
    }

    if (!started) {
        s.add(QStringLiteral("=== 结论：A2 未通过（startup 就没过）==="));
        for (const QString &l : s.lines)
            SMK("%s", l.toUtf8().constData());
        return s.lines.join('\n');
    }

    // --- 1. 插件加载：静态库全解析成功才会有这个源类型 ---
    obs_load_all_modules();
    obs_post_load_modules();

    const QStringList srcTypes = enumTypeIds(obs_enum_source_types);
    const bool registered = srcTypes.contains("android_video_input");
    s.add(QStringLiteral("源类型 %1 个，android_video_input 已注册 = %2")
              .arg(srcTypes.size())
              .arg(registered ? "true" : "false"));
    if (!registered)
        s.fail(QStringLiteral("android-capture 没注册 android_video_input —— 模块 dlopen 或 "
                              "obs_module_load 没成功（静态 libusb/libuvc/libjpeg 没链上也会这样）"));

    // --- 2. Java 侧 UsbManager 枚举（fd 的上游）---
    int usbDevices = -1;
#ifdef Q_OS_ANDROID
    {
        // Qt 6.9.3 暴露方式（A1 用 javap 实测过）：QtNative.getContext() / activity()
        QJniObject ctx;
        const char *const getters[] = {"getContext", "activity"};
        for (const char *g : getters) {
            ctx = QJniObject::callStaticObjectMethod("org/qtproject/qt/android/QtNative", g,
                                                     "()Landroid/content/Context;");
            if (ctx.isValid())
                break;
        }
        if (!ctx.isValid()) {
            s.fail(QStringLiteral("拿不到 Context，UsbManager 没查"));
        } else {
            QJniObject pkgMgr = ctx.callObjectMethod("getPackageManager",
                                                     "()Landroid/content/pm/PackageManager;");
            if (pkgMgr.isValid()) {
                QJniObject feat = QJniObject::fromString(QStringLiteral("android.hardware.usb.host"));
                const jboolean has = pkgMgr.callMethod<jboolean>("hasSystemFeature", "(Ljava/lang/String;)Z",
                                                              feat.object<jstring>());
                s.add(QStringLiteral("hasSystemFeature(android.hardware.usb.host) = %1").arg(has ? "true" : "false"));
            }

            QJniObject svcName = QJniObject::fromString(QStringLiteral("usb"));
            QJniObject usbMgr = ctx.callObjectMethod("getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;",
                                                     svcName.object<jstring>());
            if (!usbMgr.isValid()) {
                s.fail(QStringLiteral("getSystemService(USB_SERVICE) 返回空"));
            } else {
                QJniObject list = usbMgr.callObjectMethod("getDeviceList", "()Ljava/util/HashMap;");
                if (!list.isValid()) {
                    s.fail(QStringLiteral("getDeviceList() 返回空引用"));
                } else {
                    usbDevices = list.callMethod<jint>("size", "()I");
                    s.add(QStringLiteral("UsbManager 枚举成功，当前 USB 设备数 = %1").arg(usbDevices));

                    if (usbDevices > 0) {
                        // 有设备就把身份 + 授权状态逐条打出来，为阶段 2 第 3 项（设备选择 +
                        // 权限申请）铺路。这里刻意用 Iterator 而不是 toArray：Qt 6.9 的
                        // QJniObject 没有 getObjectElement（那是 QJniArray 的成员，实测编译报过错）。
                        QJniObject keys = list.callObjectMethod("keySet", "()Ljava/util/Set;");
                        QJniObject it = keys.callObjectMethod("iterator", "()Ljava/util/Iterator;");
                        if (!it.isValid()) {
                            s.add(QStringLiteral("  keySet()/iterator() 拿不到有效引用，设备清单未打印"));
                        } else {
                            for (int i = 0; i < 8 && it.callMethod<jboolean>("hasNext", "()Z"); i++) {
                                QJniObject name = it.callObjectMethod("next", "()Ljava/lang/Object;");
                                const QString devName = name.toString();
                                QJniObject dev = list.callObjectMethod(
                                    "get", "(Ljava/lang/Object;)Ljava/lang/Object;", name.object<jobject>());
                                if (!dev.isValid()) {
                                    s.add(QStringLiteral("  设备[%1] = %2（get() 引用无效，vid/pid 未查）")
                                              .arg(i)
                                              .arg(devName));
                                    continue;
                                }
                                const jint vid = dev.callMethod<jint>("getVendorId", "()I");
                                const jint pid = dev.callMethod<jint>("getProductId", "()I");
                                const jboolean perm = usbMgr.callMethod<jboolean>(
                                    "hasPermission", "(Landroid/hardware/usb/UsbDevice;)Z", dev.object<jobject>());
                                s.add(QStringLiteral("  设备[%1] = %2  vid:pid=%3:%4  hasPermission=%5")
                                          .arg(i)
                                          .arg(devName)
                                          .arg(vid, 4, 16, QLatin1Char('0'))
                                          .arg(pid, 4, 16, QLatin1Char('0'))
                                          .arg(perm ? "true" : "false"));
                            }
                        }
                    }
                }
            }
        }
    }
#else
    s.add(QStringLiteral("UsbManager 枚举：非 Android 平台，跳过"));
#endif

    if (usbDevices == 0)
        s.add(QStringLiteral("模拟器上没有 USB 设备 —— 真实取流路径（2-1b USB 声卡 / 2-2 真摄像头）"
                             "仍然只能等 OTG 真机，见 plan.md 8.8 第 10 项"));

    // --- 3. 视频线程：没有 tick 就没有 activate（A1 实测）---
    struct obs_video_info ovi = {};
    ovi.graphics_module = "libobs-opengl";
    ovi.fps_num = 30;
    ovi.fps_den = 1;
    ovi.base_width = 320;
    ovi.base_height = 180;
    ovi.output_width = 320;
    ovi.output_height = 180;
    ovi.output_format = VIDEO_FORMAT_BGRA;
    ovi.colorspace = VIDEO_CS_DEFAULT;
    ovi.range = VIDEO_RANGE_DEFAULT;
    ovi.scale_type = OBS_SCALE_BICUBIC;
    ovi.gpu_conversion = false;

    const int vrc = obs_reset_video(&ovi);
    s.add(QStringLiteral("obs_reset_video(320x180@30) → %1").arg(videoErrName(vrc)));
    if (vrc != OBS_VIDEO_SUCCESS)
        s.fail(QStringLiteral("没有视频线程就没有 tick，插件的 activate 不会被调用，三个 fd 分支都测不到"));

    if (vrc == OBS_VIDEO_SUCCESS) {
        // 一个真实存在、但明显不是 usbfs 设备节点的 fd：让插件真的走到 libusb 内部的
        // ioctl 才失败，这样才能区分"链上了但没进去"和"根本没链上"
        const int bogus = ::open("/dev/null", O_RDONLY);
        s.add(QStringLiteral("分支用例（插件日志全文见 logcat -s OBS-libobs）："));

        const UsbCase idle = runUsbFdCase(s, "usb-idle", 0);
        reportUsbCase(s, "空闲态", 0, idle);

        const UsbCase neg = runUsbFdCase(s, "usb-negfd", -1);
        reportUsbCase(s, "fd=-1 ", -1, neg);

        const UsbCase junk = runUsbFdCase(s, "usb-junkfd", bogus);
        reportUsbCase(s, "/dev/null", bogus, junk);

        if (bogus >= 0)
            ::close(bogus);

        // fd=0：安静地等，不许报错、不许进 libusb、不许出帧
        if (!idle.created || !idle.sawIdle)
            s.fail(QStringLiteral("fd=0 没走到空闲分支（创建=%1 日志=%2）—— activate 没跑或分支被改坏")
                       .arg(idle.created ? "ok" : "NULL")
                       .arg(idle.sawIdle ? "有" : "无"));
        if (idle.sawBadFd || idle.sawInitFail)
            s.fail(QStringLiteral("fd=0 却走进了坏 fd / libusb 分支——默认值和失败值没分清"));
        if (idle.w != 0 || idle.h != 0)
            s.fail(QStringLiteral("fd=0 却收到了 %1x%2 的帧 —— 空闲态没拦住").arg(idle.w).arg(idle.h));

        // fd=-1：必须在插件层就拦下并单喊一声，绝不把"授权被拒"送到 libusb 去碰运气
        if (!neg.created || !neg.sawBadFd)
            s.fail(QStringLiteral("fd=-1 没走到坏 fd 分支（创建=%1 日志=%2）—— Java 的失败返回值会被当成'还没给 fd'藏起来")
                       .arg(neg.created ? "ok" : "NULL")
                       .arg(neg.sawBadFd ? "有" : "无"));
        if (neg.sawIdle || neg.sawWrap || neg.sawInitFail)
            s.fail(QStringLiteral("fd=-1 走进了空闲态或 libusb —— 三条分支又糊回去了"));
        if (neg.w != 0)
            s.fail(QStringLiteral("fd=-1 却出了帧"));

        // fd=/dev/null：真实存在但不是 usbfs 节点，唯一能证明"真的进到静态链进来的
        // libusb 里面了"的用例。uvc_wrap 报错和 uvc_init 就失败都是确定的可观测结果。
        if (!junk.sawWrap && !junk.sawInitFail)
            s.fail(QStringLiteral("fd=/dev/null 没有任何 uvc 层日志 —— 静态链进来的 libusb/uvc 可能压根没被执行"));
        if (junk.w != 0)
            s.fail(QStringLiteral("fd=/dev/null 却出了帧"));

        if (junk.sawInitFail && !junk.sawWrap)
            s.add(QStringLiteral("注意：这台机器上 libusb_init 就失败了（大概率没有可用 usbfs 后端），"
                                 "所以 uvc_wrap 的错误码是**没测到**的，真机接上才能确认"));
    }

    obs_shutdown();
    s.add(QStringLiteral("obs_shutdown 完成（三个源的 deactivate 都没卡）"));
    base_set_log_handler(nullptr, nullptr);

    s.add(s.ok ? QStringLiteral("=== 结论：A2 通过 —— 带静态 USB 栈的插件可加载，fd 传递基建的三条分支都可观测 ===")
               : QStringLiteral("=== 结论：A2 未通过 ==="));

    for (const QString &l : s.lines)
        SMK("%s", l.toUtf8().constData());
    return s.lines.join('\n');
}

// ---------------------------------------------------------------------------
// 阶段 2 · A3：USB 热插拔监听 + 授权请求 + 设备选择属性页。
//
// A2 测的是"fd 进了插件之后怎么走"，A3 测的是"fd 怎么从 Java 走到插件"这条桥：
//   Java ObsUsbHost（UsbManager / BroadcastReceiver）
//     → 壳 obs_usb_host.cpp（QJniObject 回调表）
//       → libobs obs_android_usb_*（总线）
//         → android-capture 按设备名要 fd
// 模拟器上插不进真设备（plan.md 8.8 第 10 项），所以这一轮能测的是"链路每一段是否真的被执行、
// 失败时说的是不是真话"，测不到的仍然是"真摄像头出帧"。三段判据都取自已核对过的源码：
//   Java：requestPermission()/openFd() 对不在清单里的设备返回 -1
//   libobs：obs-android.c 的包装在总线未注册时返回失败值
//   插件：uvc-input.c 的 uvc_bridge_acquire 三条分支各有独特措辞的日志
// ---------------------------------------------------------------------------

QString runObsUsbHostSmoke()
{
    Smoke s;
    QString detail;
    s.add(QStringLiteral("=== USB 宿主冒烟 A3：Java 宿主 + libobs 总线 + 属性页 + 设备名取 fd ==="));

    const char *const bogus = "/dev/bus/usb/999/999"; // 不可能存在的设备名，专门用来验失败路径

    // --- 0. 冒烟跑在哪个线程：广播是在 Android 主线程 looper 上投递的，
    //        如果冒烟本身就占着主线程，下面的热插拔用例就是工装测不到而不是链路坏 ---
    bool onMainLooper = false;
#ifdef Q_OS_ANDROID
    {
        const QJniObject mainL = QJniObject::callStaticObjectMethod("android/os/Looper", "getMainLooper",
                                                                    "()Landroid/os/Looper;");
        const QJniObject myL = QJniObject::callStaticObjectMethod("android/os/Looper", "myLooper",
                                                                  "()Landroid/os/Looper;");
        onMainLooper = mainL.isValid() && myL.isValid() && myL.object<jobject>() == mainL.object<jobject>();
    }
#endif
    s.add(QStringLiteral("冒烟所在线程：Looper.myLooper() %1 getMainLooper() → %2")
              .arg(onMainLooper ? "==" : "!=")
              .arg(onMainLooper ? "热插拔广播要等本线程让出 looper 才会投递" : "非主线程，广播可与本冒烟并发投递"));

    // --- 1. 未装桥时总线必须一律报"不可用"，而不是"没有设备" ---
    struct obs_android_usb_device devs[8];
    const bool readyBefore = obs_android_usb_ready();
    const int enumBefore = obs_android_usb_enum_devices(devs, 8);
    const int openBefore = obs_android_usb_open(bogus);
    const int permBefore = obs_android_usb_request_permission(bogus);
    obs_android_usb_close(-1); // 必须是干净的 no-op
    obs_android_usb_close(0);
    s.add(QStringLiteral("未装桥：ready=%1 enum=%2 open=%3 requestPermission=%4（期望 false / -1 / -1 / -1）")
              .arg(readyBefore ? "true" : "false")
              .arg(enumBefore)
              .arg(openBefore)
              .arg(permBefore));
    if (readyBefore || enumBefore != -1 || openBefore != -1 || permBefore != -1)
        s.fail(QStringLiteral("总线未注册却给出了非失败值 —— 包装函数没判空，插件会以为“设备在但打不开”"));

    // --- 2. 装桥 ---
    const bool installed = obsUsbHostInstall(&detail);
    const bool readyAfter = obs_android_usb_ready();
    s.add(QStringLiteral("obsUsbHostInstall → %1：%2（装后 ready=%3）")
              .arg(installed ? "true" : "false")
              .arg(detail)
              .arg(readyAfter ? "true" : "false"));
    if (installed != readyAfter)
        s.fail(QStringLiteral("install=%1 但 obs_android_usb_ready=%2 —— 回调表置位与安装结果不一致")
                   .arg(installed ? "true" : "false")
                   .arg(readyAfter ? "true" : "false"));

    // --- 3. libobs 起链 + 加载插件（属性页和源都要插件已注册） ---
    QString root;
    bool started = false;
    if (prepareRoot(s, &root)) {
        stagePlugins(s, root);
        base_set_log_handler(libobsLog, nullptr);
        started = obs_startup("en-US", nullptr, nullptr);
        if (!started)
            s.fail(QStringLiteral("obs_startup 返回 false（细节看 logcat -s OBS-libobs）"));
    }
    if (!started) {
        s.add(QStringLiteral("=== 结论：A3 未通过（startup 就没过）==="));
        for (const QString &l : s.lines)
            SMK("%s", l.toUtf8().constData());
        return s.lines.join('\n');
    }

    obs_load_all_modules();
    obs_post_load_modules();

    const bool registered = enumTypeIds(obs_enum_source_types).contains("android_video_input");
    s.add(QStringLiteral("android_video_input 已注册 = %1").arg(registered ? "true" : "false"));
    if (!registered)
        s.fail(QStringLiteral("插件没注册上，后面的属性页与设备名取 fd 用例都无意义"));

    struct obs_video_info ovi = {};
    ovi.graphics_module = "libobs-opengl";
    ovi.fps_num = 30;
    ovi.fps_den = 1;
    ovi.base_width = 320;
    ovi.base_height = 180;
    ovi.output_width = 320;
    ovi.output_height = 180;
    ovi.output_format = VIDEO_FORMAT_BGRA;
    ovi.colorspace = VIDEO_CS_DEFAULT;
    ovi.range = VIDEO_RANGE_DEFAULT;
    ovi.scale_type = OBS_SCALE_BICUBIC;
    ovi.gpu_conversion = false;

    const int vrc = obs_reset_video(&ovi);
    s.add(QStringLiteral("obs_reset_video(320x180@30) → %1").arg(videoErrName(vrc)));
    if (vrc != OBS_VIDEO_SUCCESS)
        s.fail(QStringLiteral("没有视频线程就没有 activate，“设备名取 fd”这条用例测不了"));

    // --- 4. Java 侧宿主状态（receiver 装没装上、连接表、计数） ---
    ObsUsbHostStatus hs;
    const bool q = obsUsbHostQuery(&hs, &detail);
    s.add(QStringLiteral("obsUsbHostQuery → %1：Java installed=%2 设备=%3 连接=%4 热插拔计数=%5 "
                         "lastAction=%6 lastPermission=%7（%8）")
              .arg(q ? "ok" : "FAIL")
              .arg(hs.javaInstalled ? "true" : "false")
              .arg(hs.devices)
              .arg(hs.connections)
              .arg(hs.hotplugEvents)
              .arg(hs.lastAction.isEmpty() ? QStringLiteral("<空>") : hs.lastAction)
              .arg(hs.lastPermissionResult.isEmpty() ? QStringLiteral("<空>") : hs.lastPermissionResult)
              .arg(detail));
    if (!q)
        s.fail(QStringLiteral("Java 侧状态读不上来 —— ObsUsbHost 的静态方法没调到或 install 没成"));

    const int n = obs_android_usb_enum_devices(devs, 8);
    s.add(QStringLiteral("总线枚举 obs_android_usb_enum_devices → %1").arg(n));
    for (int i = 0; i < n; i++)
        s.add(QStringLiteral("  [%1] '%2' / '%3' / 权限=%4")
                  .arg(i)
                  .arg(QString::fromUtf8(devs[i].name))
                  .arg(QString::fromUtf8(devs[i].label))
                  .arg(devs[i].has_permission ? QStringLiteral("有") : QStringLiteral("无")));
    if (n != hs.devices)
        s.fail(QStringLiteral("同一个 Java 源，两次枚举条数不一致：%1 vs %2").arg(n).arg(hs.devices));

    // --- 5. 设备选择属性页（UI 线程直接调，走的就是 obs_get_source_properties） ---
    auto dumpProps = [&s](const char *when, QString *firstValue) -> size_t {
        obs_properties_t *props = obs_get_source_properties("android_video_input");
        obs_property_t *dp = props ? obs_properties_get(props, "usb_device") : nullptr;
        if (!dp) {
            s.fail(QStringLiteral("%1：属性页里没有 usb_device 这一项（get_properties 没返回它）").arg(when));
            if (props)
                obs_properties_destroy(props);
            return 0;
        }
        const size_t cnt = obs_property_list_item_count(dp);
        s.add(QStringLiteral("%1：usb_device 下拉框共 %2 项：").arg(when).arg(cnt));
        for (size_t i = 0; i < cnt; i++)
            s.add(QStringLiteral("    [%1] 值='%2' 文本='%3'")
                      .arg(i)
                      .arg(QString::fromUtf8(obs_property_list_item_string(dp, i)))
                      .arg(QString::fromUtf8(obs_property_list_item_name(dp, i))));
        if (firstValue && cnt > 0)
            *firstValue = QString::fromUtf8(obs_property_list_item_string(dp, 0));
        obs_properties_destroy(props);
        return cnt;
    };

    QString firstValue;
    const size_t itemsOn = dumpProps("装桥后", &firstValue);
    const size_t wantItems = (n > 0) ? (size_t) n + 1 : 1; // 有设备时末尾还有一项“不使用”
    if (itemsOn != wantItems)
        s.fail(QStringLiteral("属性页条目数 %1 ≠ 期望 %2（总线枚举到 %3 个设备）")
                   .arg(itemsOn)
                   .arg(wantItems)
                   .arg(n));
    if (n > 0 && firstValue != QString::fromUtf8(devs[0].name))
        s.fail(QStringLiteral("下拉框第一项的值 '%1' 不是总线第一个设备名 '%2' —— 属性页和设备清单对不上")
                   .arg(firstValue)
                   .arg(QString::fromUtf8(devs[0].name)));

    // --- 6. 热插拔广播（自注入，模拟器和真机走同一段 onReceive） ---
    {
        const int ev0 = hs.hotplugEvents;
        const bool sent = obsUsbHostSendTestHotplug(&detail);
        s.add(QStringLiteral("sendTestHotplug → %1：%2").arg(sent ? "true" : "false").arg(detail));
        ObsUsbHostStatus hs2;
        QThread::msleep(600); // 广播投递是异步的
        const bool q2 = obsUsbHostQuery(&hs2, &detail);
        s.add(QStringLiteral("热插拔用例：计数 %1 → %2，lastAction=%3（query=%4 %5）")
                  .arg(ev0)
                  .arg(q2 ? hs2.hotplugEvents : -1)
                  .arg(q2 ? (hs2.lastAction.isEmpty() ? QStringLiteral("<空>") : hs2.lastAction)
                          : QStringLiteral("<查询失败>"))
                  .arg(q2 ? "ok" : "FAIL")
                  .arg(detail));
        if (!sent)
            s.fail(QStringLiteral("测试广播发不出去：install 里的 Context 没存住？"));
        else if (q2 && hs2.hotplugEvents > ev0)
            s.add(QStringLiteral("  → receiver 已注册且过滤器命中（onReceive 真的跑了，含 dropDisconnected）"));
        else if (onMainLooper)
            s.add(QStringLiteral("  → 计数没涨：冒烟占着主线程 looper，广播排在队列里投递不进来。"
                                 "这是工装限制（记进 plan.md，阶段 3 界面跑起来后复验），不算链路故障"));
        else
            s.fail(QStringLiteral("计数没涨而冒烟又不在主线程 —— registerReceiver/IntentFilter/广播 action 三处之一没接通"));
    }

    // --- 7. 插件按设备名要 fd：三态各一条，全部走真实调用栈 ---
    if (vrc == OBS_VIDEO_SUCCESS) {
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                const UsbBridgeCase c = runUsbDeviceCase(s, "usb-real", QString::fromUtf8(devs[i].name));
                s.add(QStringLiteral("  真设备[%1] '%2'：总线给 fd=%3 空闲(等授权)=%4 清单里没有=%5 帧=%6x%7 进 uvc_wrap=%8")
                          .arg(i)
                          .arg(QString::fromUtf8(devs[i].name))
                          .arg(c.sawBusFd ? "是" : "否")
                          .arg(c.sawIdle ? "是" : "否")
                          .arg(c.sawNotListed ? "是" : "否")
                          .arg(c.w)
                          .arg(c.h)
                          .arg(c.sawWrap ? "是" : "否"));
                if (!c.sawBusFd && !c.sawIdle)
                    s.fail(QStringLiteral("设备 '%1' 既没拿到总线 fd 也没走“等授权”空闲态 —— 桥在插件里断了").arg(QString::fromUtf8(devs[i].name)));
                if (devs[i].has_permission && !c.sawBusFd)
                    s.fail(QStringLiteral("已授权设备却没通过总线拿到 fd —— openFd 链路有问题"));
            }
        } else {
            s.add(QStringLiteral("  没有真设备可试（模拟器 0 个 USB 设备）—— “总线借 fd 成功”这条留待 OTG 真机"));
        }

        const UsbBridgeCase gone = runUsbDeviceCase(s, "usb-gone", QString::fromLatin1(bogus));
        s.add(QStringLiteral("  清单外的设备名：不在清单里=%1 空闲=%2 坏fd=%3 进 uvc_wrap=%4 帧=%5x%6")
                  .arg(gone.sawNotListed ? "是" : "否")
                  .arg(gone.sawIdle ? "是" : "否")
                  .arg(gone.sawBadFd ? "是" : "否")
                  .arg(gone.sawWrap ? "是" : "否")
                  .arg(gone.w)
                  .arg(gone.h));
        if (!gone.sawNotListed)
            s.fail(QStringLiteral("设备名不在清单里却没报“不在当前 USB 清单里” —— 拔掉设备的场景会被藏成“没画面”"));
        if (!gone.sawIdle)
            s.fail(QStringLiteral("清单外的设备名没落到空闲态 —— 失败后源不该继续往下走"));
        if (gone.sawWrap || gone.w != 0)
            s.fail(QStringLiteral("清单外的设备名居然进了 libusb 或出了帧 —— 凭空造了 fd"));

        // 拆桥后同一个用例必须换成“总线不可用”这条说法（和“没设备”是两种故障）
        obsUsbHostUninstall();
        const UsbBridgeCase nobus = runUsbDeviceCase(s, "usb-nobus", QString::fromLatin1(bogus));
        const size_t itemsOff = dumpProps("拆桥后", nullptr);
        s.add(QStringLiteral("  拆桥后：总线不可用=%1 不在清单里=%2 属性页条目=%3")
                  .arg(nobus.sawNoBus ? "是" : "否")
                  .arg(nobus.sawNotListed ? "是" : "否")
                  .arg(itemsOff));
        if (!nobus.sawNoBus)
            s.fail(QStringLiteral("拆了桥插件却没说“USB 总线不可用” —— 宿主接线故障会被误读成没插摄像头"));
        if (nobus.sawNotListed || nobus.sawWrap || nobus.w != 0)
            s.fail(QStringLiteral("拆桥后还走进了清单/libusb 分支"));
        if (itemsOff != 1)
            s.fail(QStringLiteral("拆桥后属性页应该有 1 项说明文字，实际 %1 项").arg(itemsOff));
    }

    obs_shutdown();
    obsUsbHostUninstall(); // 幂等：确保回调表不会活过 libobs
    s.add(QStringLiteral("obs_shutdown + 拆桥完成"));
    base_set_log_handler(nullptr, nullptr);

    s.add(s.ok ? QStringLiteral("=== 结论：A3 通过 —— Java 宿主/libobs 总线/属性页/设备名取 fd 四段链路都可观测 ===")
               : QStringLiteral("=== 结论：A3 未通过 ==="));

    for (const QString &l : s.lines)
        SMK("%s", l.toUtf8().constData());
    return s.lines.join('\n');
}

// ---------------------------------------------------------------------------
// 阶段 2 · A4：多输入设备枚举 + 属性页下拉框 + 低延迟/MMAP 实测
//
// 与 A3 同构的四段链路（Java 宿主 → libobs 总线 → 属性页 → 真实采集），但音频这边有两
// 处实质差别，冒烟的写法跟着变：
//   1) 模拟器上真有几台输入设备可选（A3 那台机器 0 个 USB 设备）。所以"总线确认 →
//      setDeviceId → AAudio 实得 id → 出帧"这条**正向**路径可以整个跑一遍并断言，
//      不再只能测失败分支。
//   2) 音频没有 fd，设备之间无法靠句柄区分，唯一的硬证据是 AAudio 回读的实际 device_id。
//      于是加了一条跨层交叉断言：device_id=0（交给系统选）时 AAudio 实得的 id，必须出现
//      在 Java 数出来、总线解析出来的那份清单里。三个数（Java 台数 / 总线台数 / AAudio
//      实得 id）互相自洽，才算这四段真的通。
//
// MMAP 那组接口是 API 36 才有的（本机实测 API 35），插件按 dlsym 探测。探测结果本身
// 就是这一轮要交付的结论，所以测到什么记什么，不当失败。
// ---------------------------------------------------------------------------

namespace {

// 从日志环里抠 "前缀=十进制数" 的第一个数。插件不会把 AAudio 的实得参数暴露在 obs API
// 上，而 A4 的核心断言恰恰是"请求的 id"和"实得的 id"这两个数对不对得上。
bool logRingInt(const char *prefix, long long *out)
{
    const int head = g_logRingHead.load(std::memory_order_relaxed);
    for (int i = 0; i < (head < LOG_RING_N ? head : LOG_RING_N); i++) {
        const char *p = strstr(g_logRing[i], prefix);
        if (!p)
            continue;
        p += strlen(prefix);
        char *end = nullptr;
        const long long v = strtoll(p, &end, 10);
        if (end != p) {
            *out = v;
            return true;
        }
    }
    return false;
}

// 把第一条含 needle 的日志原文整条取回来贴进结论。"请求 vs 实得"是一整行，
// 两边字段同名（sharing/perf/mmap），拆成字段反而丢信息 —— 整条贴出来最实在。
QString logRingLine(const char *needle)
{
    const int head = g_logRingHead.load(std::memory_order_relaxed);
    for (int i = 0; i < (head < LOG_RING_N ? head : LOG_RING_N); i++) {
        if (strstr(g_logRing[i], needle))
            return QString::fromUtf8(g_logRing[i]);
    }
    return QString();
}

struct AudioCase {
    bool created = false;
    int callbacks = 0;
    quint64 frames = 0;
    float peak = 0.0f;
    bool sawOpened = false;
    bool sawDefault = false;    // "设备选择=系统默认输入"
    bool sawConfirmed = false;  // "已通过总线确认设备"
    bool sawNoBus = false;      // "音频总线不可用"
    bool sawNotListed = false;  // "不在当前音频输入清单里"
    long long actualId = -1;    // AAudio 实得的 device_id
    long long xrun = -1;
    QString openLine;
    QString failLine;
    QString mmapTry; // 这一条用例里 AAudio_setMMapPolicy 的原文：要么是真返回码，要么是"没这个入口"
    QString mmapProbe; // 插件里由 pthread_once 护着；实测一个进程内会出现多次（5 次装载 / 2 次打印，机制未查明），地址应一致
};

// 建一个 android_audio_input 源、塞进主通道、等 libobs 音频回调出帧，然后把插件走到
// 哪个分支读回来。六个参数里后三个是 A4 新加的下拉框设置项，传 "" 走默认。
AudioCase runAudioDeviceCase(Smoke &s, const char *tag, qint64 device_id, const char *sharing, const char *perf,
                             const char *mmap)
{
    AudioCase r;

    obs_data_t *st = obs_data_create();
    obs_data_set_int(st, "device_id", device_id);
    obs_data_set_int(st, "sample_rate", 48000);
    obs_data_set_int(st, "channels", 2);
    obs_data_set_string(st, "input_preset", "generic");
    if (sharing && *sharing)
        obs_data_set_string(st, "sharing_mode", sharing);
    if (perf && *perf)
        obs_data_set_string(st, "performance_mode", perf);
    if (mmap && *mmap)
        obs_data_set_string(st, "mmap_policy", mmap);

    logRingReset();

    obs_source_t *src = obs_source_create("android_audio_input", tag, st, nullptr);
    obs_data_release(st);
    if (!src) {
        s.fail(QStringLiteral("%1: obs_source_create 返回空（device_id=%2）").arg(tag).arg(device_id));
        return r;
    }
    r.created = true;

    g_ap = AudioProbe();
    obs_source_add_audio_capture_callback(src, audioProbeCb, nullptr);
    obs_set_output_source(0, src);

    // activate 是视频线程 tick 里补调的；第一帧最多等 2.5 秒，再多收 300 ms 让统计有代表性
    for (int i = 0; i < 25 && g_ap.frames == 0; i++)
        QThread::msleep(100);
    QThread::msleep(300);

    r.callbacks = g_ap.callbacks;
    r.frames = g_ap.frames;
    r.peak = g_ap.peak;
    r.sawOpened = logRingHas("输入流已打开");
    r.sawDefault = logRingHas("设备选择=系统默认输入");
    r.sawConfirmed = logRingHas("已通过总线确认设备");
    r.sawNoBus = logRingHas("音频总线不可用");
    r.sawNotListed = logRingHas("不在当前音频输入清单里");
    logRingInt("device_id=", &r.actualId);
    logRingInt("xrun=", &r.xrun);
    r.openLine = logRingLine("输入流已打开");
    r.failLine = logRingLine("打开输入流失败");
    r.mmapTry = logRingLine("AAudio_setMMapPolicy");
    r.mmapProbe = logRingLine("MMAP 接口");

    obs_set_output_source(0, nullptr);
    obs_source_remove_audio_capture_callback(src, audioProbeCb, nullptr);
    obs_source_release(src);
    // 源的销毁是延迟到图形线程的。不留这一会儿，下一条 EXCLUSIVE 用例会撞上
    // "设备还被上一条占着"，测出来的就是工装的坑而不是平台行为。
    QThread::msleep(400);
    return r;
}

void reportAudioCase(Smoke &s, const char *tag, const AudioCase &r)
{
    s.add(QStringLiteral("  %1：创建=%2 开流=%3 回调=%4 帧=%5 峰值=%6 实得device_id=%7 xrun=%8 "
                        "[默认=%9 总线确认=%10 总线不可用=%11 不在清单=%12]")
              .arg(tag)
              .arg(r.created ? "ok" : "NULL")
              .arg(r.sawOpened ? "是" : "否")
              .arg(r.callbacks)
              .arg(r.frames)
              .arg((double) r.peak, 0, 'f', 5)
              .arg(r.actualId)
              .arg(r.xrun)
              .arg(r.sawDefault ? "是" : "否")
              .arg(r.sawConfirmed ? "是" : "否")
              .arg(r.sawNoBus ? "是" : "否")
              .arg(r.sawNotListed ? "是" : "否"));
    if (!r.openLine.isEmpty())
        s.add(QStringLiteral("      插件原文：%1").arg(r.openLine));
    if (!r.failLine.isEmpty())
        s.add(QStringLiteral("      失败原文：%1").arg(r.failLine));
    if (!r.mmapTry.isEmpty())
        s.add(QStringLiteral("      MMAP 原文：%1").arg(r.mmapTry));
}

} // namespace

QString runObsAudioHostSmoke()
{
    Smoke s;
    QString detail;
    s.add(QStringLiteral("=== 音频宿主冒烟 A4：Java 宿主 + libobs 音频总线 + 设备下拉框 + 指名采集 + 低延迟/MMAP ==="));

    constexpr int MAXD = 16; // 与 ObsAudioHost.MAX_ENTRIES、插件的 AUDIO_DEV_MAX 同值

    // --- 0. 冒烟跑在哪个线程：AudioDeviceCallback 显式挂在主线程 Handler 上，
    //        自注入的测试广播也在主线程投递；冒烟若占着主线程，下面的计数用例就是
    //        工装测不到而不是链路坏（A3 实测过这件事，判分支必须先知道自己在哪个线程）---
    bool onMainLooper = false;
#ifdef Q_OS_ANDROID
    {
        const QJniObject mainL = QJniObject::callStaticObjectMethod("android/os/Looper", "getMainLooper",
                                                                    "()Landroid/os/Looper;");
        const QJniObject myL = QJniObject::callStaticObjectMethod("android/os/Looper", "myLooper",
                                                                  "()Landroid/os/Looper;");
        onMainLooper = mainL.isValid() && myL.isValid() && myL.object<jobject>() == mainL.object<jobject>();
    }
#endif
    s.add(QStringLiteral("冒烟所在线程：Looper.myLooper() %1 getMainLooper() → %2")
              .arg(onMainLooper ? "==" : "!=")
              .arg(onMainLooper ? "设备回调/广播要等本线程让出 looper 才投递" : "非主线程，回调可与本冒烟并发投递"));

    // --- 1. 未装桥：三个入口必须一律报"不可用"，而且不能崩 ---
    struct obs_android_audio_device devs[MAXD];
    const bool readyBefore = obs_android_audio_ready();
    const int enumBefore = obs_android_audio_enum_devices(devs, MAXD);
    const bool permBefore = obs_android_audio_has_permission();
    const int enumNull = obs_android_audio_enum_devices(nullptr, MAXD);
    const int enumZero = obs_android_audio_enum_devices(devs, 0);
    s.add(QStringLiteral("未装桥：ready=%1 enum=%2 has_permission=%3 enum(NULL)=%4 enum(max=0)=%5"
                         "（期望 false / -1 / false / -1 / -1）")
              .arg(readyBefore ? "true" : "false")
              .arg(enumBefore)
              .arg(permBefore ? "true" : "false")
              .arg(enumNull)
              .arg(enumZero));
    if (readyBefore || enumBefore != -1 || permBefore || enumNull != -1 || enumZero != -1)
        s.fail(QStringLiteral("总线没注册却给出了可用值 —— 包装函数判空不严，插件会把\"没接线\"读成\"没设备\""));

    // --- 2. 装桥，并交叉核对 install 与 ready 一致 ---
    const bool installed = obsAudioHostInstall(&detail);
    const bool readyAfter = obs_android_audio_ready();
    s.add(QStringLiteral("obsAudioHostInstall → %1：%2（装后 ready=%3）")
              .arg(installed ? "true" : "false")
              .arg(detail)
              .arg(readyAfter ? "true" : "false"));
    if (installed != readyAfter)
        s.fail(QStringLiteral("install=%1 但 obs_android_audio_ready=%2 —— 回调表置位与安装结果不一致")
                   .arg(installed ? "true" : "false")
                   .arg(readyAfter ? "true" : "false"));

    // --- 3. libobs 起链 + 加载插件 ---
    QString root;
    bool started = false;
    if (prepareRoot(s, &root)) {
        stagePlugins(s, root);
        base_set_log_handler(libobsLog, nullptr);
        started = obs_startup("en-US", nullptr, nullptr);
        if (!started)
            s.fail(QStringLiteral("obs_startup 返回 false（细节看 logcat -s OBS-libobs）"));
    }
    if (!started) {
        s.add(QStringLiteral("=== 结论：A4 未通过（startup 就没过）==="));
        for (const QString &l : s.lines)
            SMK("%s", l.toUtf8().constData());
        return s.lines.join('\n');
    }

    obs_load_all_modules();
    obs_post_load_modules();

    const bool registered = enumTypeIds(obs_enum_source_types).contains("android_audio_input");
    s.add(QStringLiteral("android_audio_input 已注册 = %1").arg(registered ? "true" : "false"));
    if (!registered)
        s.fail(QStringLiteral("插件没注册上，后面的下拉框与指名采集用例都无意义"));

    struct obs_audio_info ai = {};
    ai.samples_per_sec = 48000;
    ai.speakers = SPEAKERS_STEREO;
    const bool audio_ok = obs_reset_audio(&ai);
    s.add(QStringLiteral("obs_reset_audio(48000, STEREO) → %1").arg(audio_ok ? "true" : "false"));
    if (!audio_ok)
        s.fail(QStringLiteral("libobs 音频线程起不来 —— 没有它就收不到采集回调，帧数用例全废"));

    struct obs_video_info ovi = {};
    ovi.graphics_module = "libobs-opengl";
    ovi.fps_num = 30;
    ovi.fps_den = 1;
    ovi.base_width = 320;
    ovi.base_height = 180;
    ovi.output_width = 320;
    ovi.output_height = 180;
    ovi.output_format = VIDEO_FORMAT_BGRA;
    ovi.colorspace = VIDEO_CS_DEFAULT;
    ovi.range = VIDEO_RANGE_DEFAULT;
    ovi.scale_type = OBS_SCALE_BICUBIC;
    ovi.gpu_conversion = false;

    const int vrc = obs_reset_video(&ovi);
    s.add(QStringLiteral("obs_reset_video(320x180@30) → %1").arg(videoErrName(vrc)));
    if (vrc != OBS_VIDEO_SUCCESS)
        s.fail(QStringLiteral("没有视频线程就没有 tick，activate 不会被调，采集用例测不了"));

    // --- 4. Java 台数 ↔ 总线台数 交叉核对（不等就是桥的拆包出了问题）---
    ObsAudioHostStatus hs;
    const bool q = obsAudioHostQuery(&hs, &detail);
    s.add(QStringLiteral("obsAudioHostQuery → %1：Java installed=%2 RECORD_AUDIO=%3 Java 台数=%4 总线台数=%5 "
                         "设备回调=%6 刷新计数=%7 上次回调在主线程=%8 lastAction=%9（%10）")
              .arg(q ? "ok" : "FAIL")
              .arg(hs.javaInstalled ? "true" : "false")
              .arg(hs.hasPermission ? "true" : "false")
              .arg(hs.devices)
              .arg(hs.busDevices)
              .arg(hs.deviceCallbacks)
              .arg(hs.hotplugEvents)
              .arg(hs.lastCallbackOnMain == 1 ? "是" : hs.lastCallbackOnMain == 0 ? "否" : "没跑过")
              .arg(hs.lastAction.isEmpty() ? QStringLiteral("<空>") : hs.lastAction)
              .arg(detail));
    if (!q)
        s.fail(QStringLiteral("Java 侧状态读不上来 —— ObsAudioHost 的静态方法没调到或 install 没成"));

    const int n = obs_android_audio_enum_devices(devs, MAXD);
    s.add(QStringLiteral("总线枚举 obs_android_audio_enum_devices → %1").arg(n));
    for (int i = 0; i < n; i++)
        s.add(QStringLiteral("  [%1] id=%2 type=%3 is_source=%4 product='%5' label='%6'")
                  .arg(i)
                  .arg(devs[i].id)
                  .arg(devs[i].type)
                  .arg(devs[i].is_source ? "true" : "false")
                  .arg(QString::fromUtf8(devs[i].product))
                  .arg(QString::fromUtf8(devs[i].label)));
    if (q && n != hs.devices)
        s.fail(QStringLiteral("同一个 Java 源，两条路数出来的台数不一致：总线 %1 vs Java %2").arg(n).arg(hs.devices));
    if (n == 0)
        s.add(QStringLiteral("  → 总线枚举到 0 台：这台系统上没有采集侧设备（或未授予 RECORD_AUDIO 而读不到）"));

    // 再走一遍壳侧的 List 口，确认 JNI 那条路与 libobs 那条路给的是同一份清单
    {
        QStringList lst;
        int cnt = 0;
        const bool lq = obsAudioHostList(&lst, &cnt, &detail);
        s.add(QStringLiteral("obsAudioHostList → %1：cnt=%2 行数=%3 %4")
                  .arg(lq ? "ok" : "FAIL")
                  .arg(cnt)
                  .arg(lst.size())
                  .arg(detail));
        if (lq && cnt != n)
            s.fail(QStringLiteral("壳侧 List 数到 %1 台，libobs 总线数到 %2 台 —— 两条路不同源").arg(cnt).arg(n));
    }

    // --- 5. 属性页：四个下拉框的条目、取值、文本都要和总线对得上 ---
    // obs_properties_destroy 之后 property 指针就失效了，所以断言必须在同一个 props
    // 的生命周期里当场做完（见 checkDeviceList），不能"先 dump 再毁再回头用"。
    auto checkDeviceList = [&s, &devs, n](obs_properties_t *props) -> size_t {
        obs_property_t *dp = props ? obs_properties_get(props, "device_id") : nullptr;
        if (!dp) {
            s.fail(QStringLiteral("属性页里没有 device_id 这一项 —— A4 的下拉框没接上"));
            return 0;
        }
        const size_t cnt = obs_property_list_item_count(dp);
        s.add(QStringLiteral("装桥后 device_id 下拉框共 %1 项：").arg(cnt));
        for (size_t i = 0; i < cnt; i++)
            s.add(QStringLiteral("    [%1] 值=%2 文本='%3'")
                      .arg(i)
                      .arg(obs_property_list_item_int(dp, i))
                      .arg(QString::fromUtf8(obs_property_list_item_name(dp, i))));

        if (cnt < 1) {
            s.fail(QStringLiteral("下拉框一项都没有，UI 上会是个空框"));
            return cnt;
        }
        if (obs_property_list_item_int(dp, 0) != 0)
            s.fail(QStringLiteral("下拉框第 0 项的值是 %1，不是 0（0 = 交给系统选）")
                       .arg(obs_property_list_item_int(dp, 0)));

        // 总线上 id>0 且 is_source 的条目，必须按同序原样出现在第 1 项往后
        int visible = 0;
        for (int i = 0; i < n; i++) {
            if (devs[i].id <= 0 || !devs[i].is_source)
                continue;
            if ((size_t)(visible + 1) >= cnt) {
                s.fail(QStringLiteral("总线第 %1 台（id=%2）没进下拉框：条目只有 %3 项").arg(i).arg(devs[i].id).arg(cnt));
                break;
            }
            const long long got = obs_property_list_item_int(dp, visible + 1);
            const QString gotName = QString::fromUtf8(obs_property_list_item_name(dp, visible + 1));
            if (got != devs[i].id)
                s.fail(QStringLiteral("下拉框第 %1 项的值是 %2，期望总线第 %3 台的 id=%4")
                           .arg(visible + 1)
                           .arg(got)
                           .arg(i)
                           .arg(devs[i].id));
            if (gotName != QString::fromUtf8(devs[i].label))
                s.fail(QStringLiteral("下拉框第 %1 项的文本与总线 label 不符：'%2' vs '%3'")
                           .arg(visible + 1)
                           .arg(gotName)
                           .arg(QString::fromUtf8(devs[i].label)));
            visible++;
        }
        if (cnt != (size_t) visible + 1)
            s.fail(QStringLiteral("下拉框 %1 项 ≠ 可见设备 %2 + 哨兵 1 —— 有条目来路不明").arg(cnt).arg(visible));
        return cnt;
    };

    size_t itemsOn = 0;
    {
        obs_properties_t *props = obs_get_source_properties("android_audio_input");
        itemsOn = checkDeviceList(props);

        struct { const char *name; size_t want; const char *firstVal; } lists[] = {
            {"sharing_mode", 2, "shared"},
            {"performance_mode", 3, "none"},
            {"mmap_policy", 4, "unspecified"},
        };
        for (auto &L : lists) {
            obs_property_t *pp = props ? obs_properties_get(props, L.name) : nullptr;
            if (!pp) {
                s.fail(QStringLiteral("属性页里没有 %1 —— A4 的低延迟开关没接上").arg(L.name));
                continue;
            }
            const size_t c = obs_property_list_item_count(pp);
            QStringList vals;
            for (size_t i = 0; i < c; i++)
                vals << QString::fromUtf8(obs_property_list_item_string(pp, i));
            s.add(QStringLiteral("%1 下拉框共 %2 项，值依次为 %3").arg(L.name).arg(c).arg(vals.join(" / ")));
            if (c != L.want)
                s.fail(QStringLiteral("%1 条目数 %2 ≠ 期望 %3").arg(L.name).arg(c).arg(L.want));
            if (c > 0 && vals[0] != QLatin1String(L.firstVal))
                s.fail(QStringLiteral("%1 首项值是 '%2'，不是默认 '%3' —— 默认设置会对不上下拉框")
                           .arg(L.name)
                           .arg(vals[0])
                           .arg(L.firstVal));
        }
        obs_properties_destroy(props);
    }

    // --- 6. 测试刷新广播：验 receiver 真的注册上了、过滤器真命中 ---
    {
        const int ev0 = hs.hotplugEvents;
        const bool sent = obsAudioHostSendTestUpdate(&detail);
        s.add(QStringLiteral("sendTestUpdate → %1：%2").arg(sent ? "true" : "false").arg(detail));
        ObsAudioHostStatus hs2;
        QThread::msleep(600); // 广播投递是异步的
        const bool q2 = obsAudioHostQuery(&hs2, &detail);
        s.add(QStringLiteral("热插拔用例：刷新计数 %1 → %2，lastAction=%3（query=%4 %5）")
                  .arg(ev0)
                  .arg(q2 ? hs2.hotplugEvents : -1)
                  .arg(q2 ? (hs2.lastAction.isEmpty() ? QStringLiteral("<空>") : hs2.lastAction)
                          : QStringLiteral("<查询失败>"))
                  .arg(q2 ? "ok" : "FAIL")
                  .arg(detail));
        if (!sent)
            s.fail(QStringLiteral("测试广播发不出去：install 里的 Context 没存住？"));
        else if (q2 && hs2.hotplugEvents > ev0)
            s.add(QStringLiteral("  → receiver 已注册且过滤器命中（onReceive 真的跑了刷新逻辑）"));
        else if (onMainLooper)
            s.add(QStringLiteral("  → 计数没涨：冒烟占着主线程 looper，广播排在队列里投递不进来。"
                                 "这是工装限制（记进 plan.md，阶段 3 界面跑起来后复验），不算链路故障"));
        else
            s.fail(QStringLiteral("计数没涨而冒烟又不在主线程 —— registerReceiver/IntentFilter/广播 action 三处之一没接通"));
    }

    if (vrc != OBS_VIDEO_SUCCESS || !registered) {
        s.add(QStringLiteral("=== 结论：A4 未通过（视频线程或插件注册就没过，采集用例跳过）==="));
        obs_shutdown();
        obsAudioHostUninstall();
        base_set_log_handler(nullptr, nullptr);
        for (const QString &l : s.lines)
            SMK("%s", l.toUtf8().constData());
        return s.lines.join('\n');
    }

    // --- 7a. 指名采集：清单里每台都真开一次流 ---
    const AudioCase dflt = runAudioDeviceCase(s, "a4-default", 0, "", "", "");
    reportAudioCase(s, "device_id=0（交给系统选）", dflt);
    if (!dflt.sawDefault)
        s.fail(QStringLiteral("device_id=0 却没走\"系统默认\"分支 —— 哨兵值判断有问题"));
    if (dflt.frames == 0)
        s.fail(QStringLiteral("系统默认输入采不到帧 —— A1 那轮是通的，这是回归（看上面的失败原文）"));
    // 跨层断言：AAudio 自己挑的那台，必须就在 Java→总线送来的清单里
    if (dflt.actualId > 0) {
        bool inList = false;
        for (int i = 0; i < n; i++) {
            if (devs[i].id == (int32_t) dflt.actualId)
                inList = true;
        }
        s.add(QStringLiteral("  → AAudio 实得 device_id=%1，%2总线清单里").arg(dflt.actualId).arg(inList ? "在" : "不在"));
        if (!inList)
            s.fail(QStringLiteral("AAudio 实得的 id=%1 不在 Java 枚举的 %2 台清单里 —— "
                                  "要么清单不是同一份，要么快照在拆包时丢了条目")
                       .arg(dflt.actualId)
                       .arg(n));
    } else {
        s.add(QStringLiteral("  → AAudio 没回读到具体 device_id（=%1）：交叉断言跳过，只记现象").arg(dflt.actualId));
    }

    int namedOk = 0;
    for (int i = 0; i < n && i < 4; i++) {
        if (devs[i].id <= 0)
            continue;
        char tag[32];
        snprintf(tag, sizeof(tag), "a4-dev-%d", i);
        const AudioCase c = runAudioDeviceCase(s, tag, devs[i].id, "", "", "");
        reportAudioCase(s, QStringLiteral("指名 id=%1 '%2'").arg(devs[i].id).arg(QString::fromUtf8(devs[i].label)).toUtf8().constData(),
                        c);
        if (!c.sawConfirmed)
            s.fail(QStringLiteral("清单里的 id=%1 插件却说没在总线上确认 —— 总线查询与清单不一致").arg(devs[i].id));
        if (c.sawDefault)
            s.fail(QStringLiteral("指名了 id=%1 却走了\"系统默认\"分支").arg(devs[i].id));
        if (c.frames > 0)
            namedOk++;
        if (c.actualId >= 0 && c.actualId != devs[i].id)
            s.add(QStringLiteral("      注：请求 id=%1，AAudio 实得 id=%2 —— 头文件明说\"可能拿不到你要的那台\"，"
                                 "开流后必须回读校验，这里就是回读的结果")
                      .arg(devs[i].id)
                      .arg(c.actualId));
    }
    if (n == 0)
        s.add(QStringLiteral("  没有指名用例可跑（总线 0 台）—— \"逐台真采\"这条留待有输入设备的环境复验"));
    else if (namedOk == 0)
        s.add(QStringLiteral("  → 清单里的设备一台都没出帧：链路是通的（见上面每条的失败原文），"
                             "但这台模拟器不给这些具体设备开采集流。记进 plan.md，不当作 A4 失败"));

    // --- 7b. 清单外的 id：必须说"不在清单里"并回落到默认，而且不许凭空开流失败 ---
    {
        const AudioCase gone = runAudioDeviceCase(s, "a4-gone", 987654, "", "", "");
        reportAudioCase(s, "清单外的 id=987654", gone);
        if (!gone.sawNotListed)
            s.fail(QStringLiteral("清单外的 id 没报\"不在当前音频输入清单里\" —— 拔掉设备后会被读成\"静音\"，最难查的一类"));
        if (gone.sawConfirmed)
            s.fail(QStringLiteral("清单外的 id 居然被总线确认了"));
        if (gone.frames == 0)
            s.fail(QStringLiteral("清单外的 id 回落后仍采不到帧 —— 回落逻辑没真的交给系统默认"));
    }

    // --- 7c. 拆桥后同一个 id：必须换成"音频总线不可用"这条说法（与"没设备"是两种故障）---
    {
        obsAudioHostUninstall();
        const AudioCase nobus = runAudioDeviceCase(s, "a4-nobus", (n > 0) ? devs[0].id : 7, "", "", "");
        reportAudioCase(s, "拆桥后指名", nobus);
        if (!nobus.sawNoBus)
            s.fail(QStringLiteral("拆了桥插件却没说\"音频总线不可用\" —— 宿主接线故障会被误读成没插麦克风"));
        if (nobus.sawConfirmed || nobus.sawNotListed)
            s.fail(QStringLiteral("拆桥后还走进了\"清单确认/不在清单\"分支"));

        obs_properties_t *props = obs_get_source_properties("android_audio_input");
        obs_property_t *dp = props ? obs_properties_get(props, "device_id") : nullptr;
        // 指针在 destroy 之后就失效了，要用的东西一次取干净（A4 前一处 use-after-free 就是这么埋的）
        const size_t itemsOff = dp ? obs_property_list_item_count(dp) : 0;
        const QString offFirstName = dp ? QString::fromUtf8(obs_property_list_item_name(dp, 0)) : QString();
        if (props)
            obs_properties_destroy(props);

        s.add(QStringLiteral("device_id 下拉框：拆桥前 %1 项 → 拆桥后 %2 项%3")
                  .arg(itemsOn)
                  .arg(itemsOff)
                  .arg(offFirstName.isEmpty() ? QStringLiteral("（没有 device_id 项）")
                                              : QStringLiteral("，拆桥后首项文本='%1'").arg(offFirstName)));
        if (itemsOff != 1)
            s.fail(QStringLiteral("拆桥后下拉框应该是 1 项说明文字，实际 %1 项 —— 未接通时不该继续摆设备").arg(itemsOff));
        else if (!offFirstName.contains("未接通"))
            s.fail(QStringLiteral("拆桥后唯一那一项没提\"未接通\"：'%1' —— 这句话要能和\"没有设备\"区分开").arg(offFirstName));

        if (!obsAudioHostInstall(&detail))
            s.fail(QStringLiteral("重装桥失败，后面的低延迟用例没法做：%1").arg(detail));
    }

    // --- 8. 低延迟/独占/MMAP：这三条要的是"实测数字"，能不能开流都算有效结果 ---
    {
        s.add(QStringLiteral("MMAP 探测原文（dlopen+dlsym 的结果；同一进程里可能打到多次，地址应一致）：%1")
                  .arg(dflt.mmapProbe.isEmpty() ? QStringLiteral("<没抓到：说明这条用例之前没开过流>") : dflt.mmapProbe));

        const AudioCase ex = runAudioDeviceCase(s, "a4-exclusive", 0, "exclusive", "low_latency", "");
        reportAudioCase(s, "exclusive + low_latency", ex);
        if (ex.sawOpened)
            s.add(QStringLiteral("  → 模拟器上独占输入流开起来了（A1 那轮记的是\"EXCLUSIVE 打不开\"，"
                                 "这条把结论刷新成\"能开，实得值见上面原文\"）"));
        else
            s.add(QStringLiteral("  → 独占输入流仍开不起来：与 A1 的结论一致（失败原文见上）"));

        const AudioCase ll = runAudioDeviceCase(s, "a4-lowlatency", 0, "shared", "low_latency", "");
        reportAudioCase(s, "shared + low_latency", ll);

        const AudioCase mm = runAudioDeviceCase(s, "a4-mmap", 0, "shared", "none", "auto");
        reportAudioCase(s, "mmap_policy=auto", mm);
        s.add(QStringLiteral("  → 这次试 MMAP 的原文：%1")
                  .arg(mm.mmapTry.isEmpty() ? QStringLiteral("<插件没提 AAudio_setMMapPolicy，检查 mmapResolve 有没有被走到>")
                                            : mm.mmapTry));
        // 开流成功了还出不了帧，说明问题在数据面而不是控制面，这条区分要留在结论里
        if (mm.sawOpened && mm.frames == 0)
            s.fail(QStringLiteral("mmap_policy=auto 下流开起来了却一帧没有 —— 数据面断了，不是开关问题"));
    }

    obs_shutdown();
    obsAudioHostUninstall(); // 幂等：确保回调表不会活过 libobs
    const int enumAfter = obs_android_audio_enum_devices(devs, MAXD);
    s.add(QStringLiteral("obs_shutdown + 拆桥完成（拆后 enum=%1，ready=%2）")
              .arg(enumAfter)
              .arg(obs_android_audio_ready() ? "true" : "false"));
    if (enumAfter != -1 || obs_android_audio_ready())
        s.fail(QStringLiteral("拆桥后总线没回到\"不可用\"状态 —— 悬空的回调表会让插件回调进已失效的 JNI 环境"));
    base_set_log_handler(nullptr, nullptr);

    s.add(s.ok ? QStringLiteral("=== 结论：A4 通过 —— Java 宿主/libobs 音频总线/设备下拉框/指名采集/低延迟MMAP 探测全部可观测 ===")
               : QStringLiteral("=== 结论：A4 未通过 ==="));

    for (const QString &l : s.lines)
        SMK("%s", l.toUtf8().constData());
    return s.lines.join('\n');
}

/* ---------------------------------------------------------------------------
 * 阶段 3 前置 · S1：swapchain 上屏
 *
 * 这一段验的是 M2 以来从没执行过的分支：gs_swapchain_create 真的拿到 ANativeWindow、
 * eglCreateWindowSurface 建出窗口面、之后每帧 gs_load_swapchain + eglSwapBuffers 把
 * 画面送到 SurfaceView 上（M2/A1~A4 全程 cur_swap==NULL，渲染目标是 1x1 pbuffer）。
 *
 * 两个必须实测的点（plan.md 8.8 第 11 项），以及从源码读出来的答案：
 *   a) display 的创建/销毁只能落在图形线程。obs_display_create 内部只包
 *      gs_enter_context/gs_leave_context（不是 obs_enter_graphics），而 gl-android 的
 *      device_leave_context 是彻底解绑（EGL_NO_SURFACE + EGL_NO_CONTEXT）—— 从冒烟线程
 *      直接调就是两个线程抢一个 EGL current。obs_queue_task(OBS_TASK_GRAPHICS, ..., wait)
 *      的落点是视频线程本身（obs.c 把任务推给 video->tasks，obs-video.c:1165 在该线程
 *      置 is_graphics_thread，帧尾 execute_graphics_tasks 在 render_displays 之后执行），
 *      所以这条队列就是单线程归属的正解。下面用 pthread_self 把"任务落点 == 出帧线程"打出来。
 *   b) device_load_swapchain 只置 cur_swap、不解绑，靠 device_enter_context 每次重绑窗口面。
 *      创建的那一刻 cur_swap 刚从 NULL 变过来、还没进过上下文，所以 gs_is_present_ready
 *      在任务里读到 false、要等到 draw 回调里才 true —— 这个先后是推断，用回调里的读数证实。
 * ------------------------------------------------------------------------- */
namespace {

std::atomic<uint32_t> g_scFrames{0};
std::atomic<uint32_t> g_scCbW{0};
std::atomic<uint32_t> g_scCbH{0};
std::atomic<uint32_t> g_scQuad{0};
std::atomic<uint32_t> g_scNoEffect{0};
std::atomic<int> g_scPresentInCb{-1};
std::atomic<long long> g_scCbTid{0};
std::atomic<long long> g_scTaskTid{0};

// display 自己的 background color 已经由 render_display_begin 清好了，这个回调只负责
// 在左上角画一个 1/4 大小的红方块 —— 故意不对称：viewport 的 Y 翻转（device_set_viewport
// 里 gl_getclientsize 那一步）算错的话方块会跑到左下，单看截图就能分辨。
void swapchainDrawCb(void *, uint32_t cx, uint32_t cy)
{
    g_scFrames.fetch_add(1, std::memory_order_relaxed);
    g_scCbW.store(cx, std::memory_order_relaxed);
    g_scCbH.store(cy, std::memory_order_relaxed);
    g_scCbTid.store((long long) pthread_self(), std::memory_order_relaxed);
    g_scPresentInCb.store(gs_is_present_ready() ? 1 : 0, std::memory_order_relaxed);

    gs_effect_t *eff = obs_get_base_effect(OBS_EFFECT_SOLID);
    gs_eparam_t *color = eff ? gs_effect_get_param_by_name(eff, "color") : nullptr;
    if (!eff || !color) {
        g_scNoEffect.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const struct vec4 red = {0.92f, 0.06f, 0.10f, 1.0f};
    gs_effect_set_vec4(color, &red);
    gs_set_viewport((int) (cx / 8), (int) (cy / 8), (int) (cx / 4), (int) (cy / 4));
    while (gs_effect_loop(eff, "Solid"))
        gs_draw_sprite(nullptr, 0, cx, cy);
    gs_set_viewport(0, 0, (int) cx, (int) cy);
    g_scQuad.fetch_add(1, std::memory_order_relaxed);
}

struct DisplaySetup {
    const ObsDisplaySurface *surf;
    uint32_t background;
    obs_display_t *display;
    /* 这个读值必须包在 obs_enter_graphics 里：gs_is_present_ready 的第一道判据是线程局部的
     * thread_graphics（graphics.c:1972），不在图形上下文里调用只会恒 false 并打一条
     * "called while not in a graphics context" —— 那是在问"我在不在上下文里"，不是在问设备。
     * 上一版就是这么读的，日志里那条 OBS-libobs 告警就是证据。
     *
     * 进了上下文之后期望 false，理由在源码里：device_swapchain_create（gl-subsystem.c:362）
     * 建完 swap 直接 return，并不调 device_load_swapchain；真正把 cur_swap 指过去的是帧循环里
     * 第一次 render_display_begin（obs-display.c:174 gs_load_swapchain）。所以"建好 display"
     * 和"这块面开始被 present"是两件事，回调里再读就该是 true。*/
    int readyAfterCreate; // -1 = 压根没读到
    uint32_t readbackW;
    uint32_t readbackH;
};

void createDisplayTask(void *param)
{
    DisplaySetup *d = (DisplaySetup *) param;
    struct gs_init_data gid = {};

    gid.window.surface = d->surf->window;
    gid.cx = d->surf->javaW ? d->surf->javaW : d->surf->windowW;
    gid.cy = d->surf->javaH ? d->surf->javaH : d->surf->windowH;
    gid.num_backbuffers = 1;
    gid.format = GS_RGBA;
    gid.zsformat = GS_ZS_NONE;

    g_scTaskTid.store((long long) pthread_self(), std::memory_order_relaxed);

    d->display = obs_display_create(&gid, d->background);
    if (d->display) {
        obs_display_add_draw_callback(d->display, swapchainDrawCb, nullptr);

        obs_enter_graphics();
        d->readyAfterCreate = gs_is_present_ready() ? 1 : 0;
        obs_leave_graphics();

        obs_display_size(d->display, &d->readbackW, &d->readbackH);
    }
}

void destroyDisplayTask(void *param)
{
    obs_display_destroy(*(obs_display_t **) param);
}

// 可被 aboutToQuit 打断的等待：这一段有 sleep，退出时不能把线程卡在死等上
bool smkSleep(int ms)
{
    QThread *t = QThread::currentThread();
    for (int elapsed = 0; elapsed < ms; elapsed += 100) {
        if (t && t->isInterruptionRequested())
            return false;
        QThread::msleep(100);
    }
    return true;
}

bool waitForFrames(uint32_t from, uint32_t need, int timeoutMs)
{
    QThread *t = QThread::currentThread();
    for (int elapsed = 0; elapsed < timeoutMs; elapsed += 100) {
        if (t && t->isInterruptionRequested())
            return false;
        if (g_scFrames.load(std::memory_order_relaxed) >= from + need)
            return true;
        QThread::msleep(100);
    }
    return false;
}

/* 一轮"建显示面 → 建 display → 出帧"。返回 false 表示这一轮没成，原因已经写进 s。
 * 结论必须是**本轮局部**的：整个冒烟只有一个 Smoke，s.ok 会被别处（teardown 之类）的失败带成
 * false，拿它当本轮结论就会误判 —— 实测过：teardown 那条误判连带让第二轮"明明跑通"被记成失败，
 * S1-RECT2 因此没输出，脚本连采样坐标都读不到。 */
bool runSwapchainRound(Smoke &s, const char *tag, uint32_t background, ObsDisplaySurface *surf,
                       obs_display_t **display)
{
    bool clean = true;
    auto rfail = [&s, &clean](const QString &msg) {
        clean = false;
        s.fail(msg);
    };

    QString detail;
    if (!obsDisplayHostAttach(360, 200, &detail)) {
        rfail(QStringLiteral("%1: attach 失败 —— %2").arg(tag, detail));
        return false;
    }
    s.add(QStringLiteral("%1: %2").arg(tag, detail));

    if (!obsDisplayHostAcquire(surf, &detail)) {
        rfail(QStringLiteral("%1: 换 ANativeWindow 失败 —— %2").arg(tag, detail));
        return false;
    }
    s.add(QStringLiteral("%1: 尺寸三来源对照 → %2").arg(tag, detail));
    if (surf->javaW == 0 || surf->javaH == 0 || surf->javaW != surf->windowW || surf->javaH != surf->windowH)
        rfail(QStringLiteral("%1: Java 报的尺寸和 ANativeWindow 读到的不一致或为 0 —— 面的实际几何不可信").arg(tag));

    const uint32_t before = g_scFrames.load(std::memory_order_relaxed);
    DisplaySetup setup = {surf, background, nullptr, -1, 0, 0};
    obs_queue_task(OBS_TASK_GRAPHICS, createDisplayTask, &setup, true);
    if (!setup.display) {
        rfail(QStringLiteral("%1: obs_display_create 返回 null（libobs 侧原因看 OBS-libobs）").arg(tag));
        return false;
    }
    *display = setup.display;
    s.add(QStringLiteral("%1: obs_display_create 成功；建好后（图形上下文内）gs_is_present_ready=%2，"
                         "期望 false —— 此时 device_swapchain_create 还没把 cur_swap 指过来；"
                         "obs_display_size 回读=%3x%4（gid 请求 %5x%6）")
              .arg(tag)
              .arg(setup.readyAfterCreate == 1 ? "true" : (setup.readyAfterCreate == 0 ? "false" : "<没读到>"))
              .arg((int) setup.readbackW)
              .arg((int) setup.readbackH)
              .arg((int) surf->javaW)
              .arg((int) surf->javaH));
    if (setup.readyAfterCreate == 1)
        rfail(QStringLiteral("%1: 建 display 后立刻 present-ready 就成了 true —— 说明有别处提前 "
                             "gs_load_swapchain，回调里那个 true 就不再是\"帧循环把面挂上了\"的证据，"
                             "本阶段的判据链要重排")
                  .arg(tag));

    const bool framed = waitForFrames(before, 30, 10000);
    s.add(QStringLiteral("%1: draw 回调跑了 %2 次（本轮起点 %3），回调收到的 cx×cy=%4x%5，"
                         "回调里 gs_is_present_ready=%6，effect 缺失次数=%7")
              .arg(tag)
              .arg((int) g_scFrames.load(std::memory_order_relaxed))
              .arg((int) before)
              .arg((int) g_scCbW.load(std::memory_order_relaxed))
              .arg((int) g_scCbH.load(std::memory_order_relaxed))
              .arg(g_scPresentInCb.load() == 1 ? "true" : (g_scPresentInCb.load() == 0 ? "false" : "<没进回调>"))
              .arg((int) g_scNoEffect.load(std::memory_order_relaxed)));
    if (!framed) {
        rfail(QStringLiteral("%1: 10 秒内没等到 30 帧 —— 窗口面没在出画面").arg(tag));
        return false;
    }
    if (g_scPresentInCb.load() != 1)
        rfail(QStringLiteral("%1: 进了 draw 回调但 gs_is_present_ready 不是 true —— load/present 判据不一致")
                  .arg(tag));
    if ((uint32_t) g_scCbW.load() != surf->javaW || (uint32_t) g_scCbH.load() != surf->javaH)
        rfail(QStringLiteral("%1: 回调收到的尺寸和 ANativeWindow 的尺寸不是一回事").arg(tag));

    s.add(QStringLiteral("%1: 线程归属 图形任务 tid=%2  draw 回调 tid=%3  冒烟线程 tid=%4（前两个必须相同）")
              .arg(tag)
              .arg((long long) g_scTaskTid.load())
              .arg((long long) g_scCbTid.load())
              .arg((long long) pthread_self()));
    if (g_scTaskTid.load() != g_scCbTid.load())
        rfail(QStringLiteral("%1: 创建任务和出帧回调不在同一个线程 —— EGL current 会打架").arg(tag));

    return clean;
}

} // namespace

QString runObsSwapchainSmoke()
{
    Smoke s;
    s.add(QStringLiteral("=== 上屏冒烟 S1：SurfaceView → ANativeWindow → gs_swapchain_create → eglSwapBuffers ==="));

    QString root;
    QString detail;
    bool started = false;

    if (prepareRoot(s, &root)) {
        base_set_log_handler(libobsLog, nullptr);
        started = obs_startup("en-US", nullptr, nullptr);
        if (!started)
            s.fail(QStringLiteral("obs_startup 返回 false（细节看 logcat -s OBS-libobs）"));
    }
    if (!started) {
        s.add(QStringLiteral("=== 结论：S1 未通过（libobs 起不来）==="));
        for (const QString &l : s.lines)
            SMK("%s", l.toUtf8().constData());
        return s.lines.join('\n');
    }

    struct obs_video_info ovi = {};
    ovi.graphics_module = "libobs-opengl";
    ovi.fps_num = 60;
    ovi.fps_den = 1;
    ovi.base_width = 640;
    ovi.base_height = 360;
    ovi.output_width = 640;
    ovi.output_height = 360;
    ovi.output_format = VIDEO_FORMAT_RGBA;
    ovi.colorspace = VIDEO_CS_DEFAULT;
    ovi.range = VIDEO_RANGE_DEFAULT;
    ovi.scale_type = OBS_SCALE_BILINEAR;
    ovi.gpu_conversion = false;

    const int vrc = obs_reset_video(&ovi);
    s.add(QStringLiteral("obs_reset_video(libobs-opengl, 640x360@60 RGBA) → %1").arg(videoErrName(vrc)));
    if (vrc != OBS_VIDEO_SUCCESS) {
        s.fail(QStringLiteral("obs_reset_video 未通过"));
        obs_shutdown();
        s.add(QStringLiteral("=== 结论：S1 未通过（视频线程没起来）==="));
        for (const QString &l : s.lines)
            SMK("%s", l.toUtf8().constData());
        return s.lines.join('\n');
    }

    /* 底色的字节序照 vec4_from_rgba 的读法定：它 memcpy 出 u[4] 再 x=u[0]，小端下就是
     * "uint32 的低字节 = R"。所以 0xFFE02010 → (R,G,B)=(0x10,0x20,0xE0)。
     * 这条不是想当然：截图采样会把实测值和期望值一起打出来。 */
    const uint32_t BG = 0xFFE02010u;

    ObsDisplaySurface surf = {};
    obs_display_t *display = nullptr;
    if (!runSwapchainRound(s, "S1-首轮", BG, &surf, &display)) {
        obs_shutdown();
        s.add(QStringLiteral("=== 结论：S1 未通过（首轮上屏就没成）==="));
        for (const QString &l : s.lines)
            SMK("%s", l.toUtf8().constData());
        return s.lines.join('\n');
    }

    // 稳态：连续两次的帧数差要接近 60fps，证明"每帧真在 present"而不是一帧运气
    const uint32_t t0 = g_scFrames.load(std::memory_order_relaxed);
    const bool held = smkSleep(2000);
    const uint32_t t1 = g_scFrames.load(std::memory_order_relaxed);
    s.add(QStringLiteral("稳态 2 秒里 draw 回调增量=%1（60fps 目标≈120），方块绘制次数累计=%2")
              .arg((int) (t1 - t0))
              .arg((int) g_scQuad.load(std::memory_order_relaxed)));
    if (held && (t1 - t0) < 40)
        s.fail(QStringLiteral("稳态帧数太低 —— 没在持续出帧"));

    s.add(QStringLiteral("S1-RECT %1").arg(surf.rect));
    s.add(QStringLiteral("截图采样点（相对 rect 左上角，rect 宽高记作 W×H）："
                         "方块中心=(W/4,H/4) 期望≈(235,15,26)；"
                         "Y 翻转镜像点=(W/4,3H/4) 与 右下底色点=(7W/8,7H/8) 都期望底色 (16,32,224)"));

    /* --- 生命周期一轮：拆 display（图形线程）→ 摘视图 → 重新 attach 再来一次 ---
     * 基线只能在 obs_queue_task(wait=true) 返回之后取。图形任务是在帧循环尾部的
     * execute_graphics_tasks() 里排空的，位置在 render_displays() 之后 —— 所以从"排队"到
     * "落地"中间必然还有一帧在出图。上一版把基线取在排队之前，测到增量=1 判成失败，
     * 那是量法错了，不是 display 没摘干净（obs_display_destroy 全程持 displays_mutex 摘链，
     * 而 render_displays 也在同一把锁下遍历，二者不可能交错）。 */
    const uint32_t beforeDestroy = g_scFrames.load(std::memory_order_relaxed);
    obs_queue_task(OBS_TASK_GRAPHICS, destroyDisplayTask, &display, true);
    display = nullptr;
    const uint32_t landedDestroy = g_scFrames.load(std::memory_order_relaxed);
    smkSleep(1000);
    const uint32_t afterTeardown = g_scFrames.load(std::memory_order_relaxed);
    s.add(QStringLiteral("teardown: obs_display_destroy 已跑（走 gl_platform_cleanup_swapchain + "
                         "gl_windowinfo_destroy）；排队到落地之间还有 %1 帧（帧尾才排空任务，属正常），"
                         "落地之后再等 1 秒增量=%2（应当是 0，非 0 才是 display 还挂在 "
                         "obs->data.first_display 上）")
              .arg((int) (landedDestroy - beforeDestroy))
              .arg((int) (afterTeardown - landedDestroy)));
    if (afterTeardown != landedDestroy)
        s.fail(QStringLiteral("销毁 display 后还在出帧 —— 显示链表没摘干净"));

    if (obsDisplayHostDetach(&detail))
        s.add(QStringLiteral("teardown: %1（Java 计数：%2）").arg(detail, obsDisplayHostStatus()));
    else
        s.fail(QStringLiteral("teardown: %1（Java 计数：%2）").arg(detail, obsDisplayHostStatus()));

    obsDisplayHostRelease(surf.window);
    surf = ObsDisplaySurface{};

    ObsDisplaySurface surf2 = {};
    obs_display_t *display2 = nullptr;
    const bool second = runSwapchainRound(s, "S1-次轮(重建)", BG, &surf2, &display2);
    if (second) {
        s.add(QStringLiteral("S1-RECT2 %1").arg(surf2.rect));
        s.add(QStringLiteral("重建后可重入：destroyed 计数=%1 —— 阶段 3 的 Activity 暂停/恢复就是这条路")
                  .arg(obsDisplayHostStatus()));
    }

    // 最后一轮故意不 shutdown、不销毁 display：颜色留在屏幕上，脚本才能截图采样
    s.add(s.ok && second ? QStringLiteral("=== 结论：S1 通过 —— 窗口面创建/每帧 present/几何与线程归属/销毁重建 全部可观测，"
                                          "OBS 保持运行中等待截图采样 ===")
                         : QStringLiteral("=== 结论：S1 未通过 ==="));

    for (const QString &l : s.lines)
        SMK("%s", l.toUtf8().constData());
    return s.lines.join('\n');
}
