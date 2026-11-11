/* Native mono presenter for the private labwc spatial extension. */
#define _GNU_SOURCE
#include "air-math.h"
#include "air-event.h"
#include "air-workspace.h"
#include "air-pose.h"
#include "air-ipd.h"
#include "air-watchdog.h"
#include "air-pixels.h"
#include "air-cursor.h"
#include "ext-foreign-toplevel-list-v1-protocol.h"
#include "ext-image-capture-source-v1-protocol.h"
#include "air-widget-v1-protocol.h"
#include "ext-image-copy-capture-v1-protocol.h"
#include "presentation-time-protocol.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include "air-filter.h"
#include <cairo/cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-egl.h>

struct app;
struct capture {
    struct app *app;
    struct ext_foreign_toplevel_handle_v1 *handle;
    struct ext_image_copy_capture_session_v1 *session;
    struct ext_image_copy_capture_frame_v1 *frame;
    struct wl_buffer *buffer;
    void *map;
    size_t bytes;
    uint32_t width, height, format, allocated_width, allocated_height, transform, stride;
    bool format_ok, constraints, released, ready, dirty, closed, wanted;
    uint64_t last_request;
    bool background;
    unsigned upload_deferrals;
    char identifier[128];
    uint64_t widget_id;
    GLuint texture;
    uint32_t upload_width, upload_height;
    unsigned char *rgba;
    size_t rgba_capacity;
    struct air_pixel_damage damage;
    struct air_panel last_capture_panel;
    uint32_t last_capture_transform;
    bool has_capture_panel;
    GLuint title_texture;
    char title_cache[128];
    int title_width;
    uint32_t chrome_flags, chrome_hover;
    struct capture *next;
};
struct output {
    struct app *app;
    struct wl_output *wl;
    uint32_t registry_id;
    char name[128];
    struct output *next;
};
struct frame_feedback {
    struct app *app;
    uint64_t epoch, pose_stamp;
    bool healthy;
    uint32_t overview_revision;
    double overview_basis[9];
    double widget_yaw;
    struct wp_presentation_feedback *feedback;
};
struct app {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *shell;
    struct zwlr_layer_surface_v1 *layer;
    struct wl_surface *surface;
    struct ext_foreign_toplevel_list_v1 *list;
    struct ext_foreign_toplevel_image_capture_source_manager_v1 *sources;
    struct ext_image_copy_capture_manager_v1 *copies;
    struct wp_presentation *presentation;
    struct output *outputs, *output;
    struct capture *captures;
    struct air_widget_manager_v1 *widgets;
    struct air_panel panels[AIR_MAX_PANELS];
    int count;
    uint64_t epoch, generation;
    int ipc, udp;
    uint16_t source_port;
    uint32_t pose_sequence;
    bool have_pose_sequence;
    bool running, configured, frame_due, committed, pose_valid, zero_valid, active;
    int width, height, ready_width, ready_height;
    int render_scale, max_render_size;
    bool stereo, calibration, stereo_received;
    double ipd_mm, ipd_center_mm, ipd_baseline_mm;
    GLuint calibration_texture, ipd_texture, depth_texture[2], eye_texture[2], reference_texture;
    double feedback_ipd;
    uint64_t ipd_feedback_until;
    double calibration_ipd, calibration_center, calibration_baseline;
    uint64_t pose_stamp, last_frame, last_pose_report, last_pose_poll;
    uint64_t perf_start, perf_last, perf_frames, perf_gap, perf_slow;
    uint64_t perf_draw_ns, perf_upload_ns, perf_swap_ns, perf_draws, perf_discards;
    struct air_timing perf_gaps, perf_pose_age;
    double raw[9], zero[9], basis[9];
    struct air_pose_filter pose_filter;
    struct air_vec pointer;
    uint64_t hover_id;
    uint32_t hover_region;
    bool hit, widget_follow, hide_cursor;
    bool overview, overview_dirty, overview_pending, overview_staged;
    uint32_t overview_revision, overview_staged_revision, overview_selected, overview_start, overview_count;
    char overview_query[96];
    struct { char title[128], workspace[48]; bool visible; } overview_rows[AIR_OVERVIEW_ROWS];
    GLuint overview_texture;
    int overview_texture_height;
    double widget_yaw;
    GLuint cursor, frame_texture, frame_focus_texture, frame_hover_texture;
    int cursor_w, cursor_h, cursor_x, cursor_y;
    float cursor_scale;
    EGLDisplay egl;
    EGLContext context;
    EGLSurface egl_surface;
    struct wl_egl_window *egl_window;
    GLuint program;
    GLint attr_pos, attr_uv, uniform_basis, uniform_aspect, uniform_texture, uniform_eye, uniform_zoom;
    double zoom;
};
static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void presenter_event(const char *event, const char *reason) {
    char description[320];
    aevent_quote(description, sizeof(description), reason);
    const char *generation = getenv("AIR_SPATIAL_GENERATION");
    aevent_write("presenter", event, ",\"generation\":%llu,\"reason\":%s",
                 generation ? strtoull(generation, NULL, 10) : 0ULL, description);
}

static void fatal(const char *text) {
    fprintf(stderr, "air presenter: %s\n", text);
    presenter_event("failure", text);
    exit(1);
}
static void packet(struct app *a, struct air_packet *m) {
    if(m->type==AIR_POSE){
        int queued=0;
        if(ioctl(a->ipc,TIOCOUTQ,&queued)==0 && queued>16384)return;
        /* Leave room for reliable viewport/presentation packets after a stall. */
    }
    m->magic = AIR_MAGIC;
    m->version = AIR_VERSION;
    m->size = sizeof(*m);
    m->generation = a->generation;
    if (!m->stamp_ns)
        m->stamp_ns = now_ns();
    if (!m->epoch)
        m->epoch = a->epoch;
    if (send(a->ipc, m, sizeof(*m), MSG_DONTWAIT | MSG_NOSIGNAL) != sizeof(*m)) {
        /* Pose updates are replaceable. A busy compositor must not kill its
         * presenter just because health reports filled the private socket. */
        if(m->type==AIR_POSE && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR))return;
        a->running = false;
    }
}
static GLuint shader(GLenum type, const char *source) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &source, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fatal(log);
    }
    return s;
}
static void egl_init(struct app *a) {
    a->egl = eglGetDisplay((EGLNativeDisplayType)a->display);
    if (a->egl == EGL_NO_DISPLAY || !eglInitialize(a->egl, NULL, NULL))
        fatal("EGL initialization failed");
    EGLConfig config;
    EGLint count = 0, samples = 0;
    const int candidates[] = {8, 4, 2, 0};
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        const EGLint attrs[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 24, EGL_SAMPLE_BUFFERS, candidates[i] ? 1 : 0,
            EGL_SAMPLES, candidates[i], EGL_NONE};
        if (eglChooseConfig(a->egl, attrs, &config, 1, &count) && count)
            break;
    }
    if (!count)
        fatal("no EGL window config");
    eglGetConfigAttrib(a->egl, config, EGL_SAMPLES, &samples);
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ctx[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    a->context = eglCreateContext(a->egl, config, EGL_NO_CONTEXT, ctx);
    a->egl_window = wl_egl_window_create(a->surface, a->width, a->height);
    a->egl_surface =
        eglCreateWindowSurface(a->egl, config, (EGLNativeWindowType)a->egl_window, NULL);
    if (!eglMakeCurrent(a->egl, a->egl_surface, a->egl_surface, a->context))
        fatal("EGL context creation failed");
    eglSwapInterval(a->egl, 0);
    GLint viewport[2], renderbuffer;
    glGetIntegerv(GL_MAX_VIEWPORT_DIMS, viewport);
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &renderbuffer);
    a->max_render_size = viewport[0] < viewport[1] ? viewport[0] : viewport[1];
    if (renderbuffer < a->max_render_size) a->max_render_size = renderbuffer;
    a->render_scale = afilter_scale(a->width, a->height, a->max_render_size);
    wl_surface_set_buffer_scale(a->surface, a->render_scale);
    wl_egl_window_resize(a->egl_window, a->width * a->render_scale,
                         a->height * a->render_scale, 0, 0);
    afilter_init();
    fprintf(stderr, "air presenter: AA %dx supersampling per axis, %d MSAA samples, "
                    "mipmaps %s, anisotropy %.0fx\n", a->render_scale, samples,
                    filter_npot ? "full NPOT" : "power-of-two only", filter_anisotropy);

    GLint range[2], precision;
    glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER, GL_HIGH_FLOAT, range, &precision);
    char vert[1024], frag[1024];
    afilter_shaders(vert, sizeof(vert), frag, sizeof(frag), precision != 0);
    GLuint vs = shader(GL_VERTEX_SHADER, vert), fs = shader(GL_FRAGMENT_SHADER, frag);
    a->program = glCreateProgram();
    glAttachShader(a->program, vs);
    glAttachShader(a->program, fs);
    glLinkProgram(a->program);
    GLint ok;
    glGetProgramiv(a->program, GL_LINK_STATUS, &ok);
    if (!ok)
        fatal("shader link failed");
    glDeleteShader(vs);
    glDeleteShader(fs);
    a->attr_pos = glGetAttribLocation(a->program, "pos");
    a->attr_uv = glGetAttribLocation(a->program, "uv");
    a->uniform_basis = glGetUniformLocation(a->program, "basis");
    a->uniform_aspect = glGetUniformLocation(a->program, "aspect");
    a->uniform_texture = glGetUniformLocation(a->program, "image");
    a->uniform_eye = glGetUniformLocation(a->program, "eye_x");
    a->uniform_zoom = glGetUniformLocation(a->program, "zoom");
    glUseProgram(a->program);
    glUniform1i(a->uniform_texture, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}
static void texture(GLuint *id, const void *pixels, uint32_t w, uint32_t h, uint32_t stride,
                    uint32_t format) {
    if (!*id) {
        glGenTextures(1, id);
        glBindTexture(GL_TEXTURE_2D, *id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else
        glBindTexture(GL_TEXTURE_2D, *id);
    size_t bytes = (size_t)w * h * 4;
    unsigned char *rgba = malloc(bytes);
    if (!rgba)
        fatal("texture allocation failed");
    air_pixels_rgba(rgba, pixels, w, h, stride, format);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    free(rgba);
    afilter_texture(w, h);
}
static void request_capture(struct capture *c);
static void buffer_release(void *data, struct wl_buffer *buffer) {
    (void)data;
    (void)buffer;
    /* Image-copy capture explicitly does not use wl_buffer.release. Its
     * ready/failed events complete ownership of each requested frame. */
}
static const struct wl_buffer_listener buffer_listener = {.release = buffer_release};
static void free_buffer(struct capture *c) {
    if (c->buffer)
        wl_buffer_destroy(c->buffer);
    if (c->map && c->map != MAP_FAILED)
        munmap(c->map, c->bytes);
    c->buffer = NULL;
    c->map = NULL;
    c->bytes = 0;
    c->upload_width=c->upload_height=0;
}
static void capture_transform(void *d, struct ext_image_copy_capture_frame_v1 *f, uint32_t t) {
    (void)f;
    ((struct capture *)d)->transform = t;
}
static void capture_damage(void *d, struct ext_image_copy_capture_frame_v1 *f, int32_t x, int32_t y,
                           int32_t w, int32_t h) {
    (void)f;
    struct capture *c=d;
    air_pixel_damage_add(&c->damage,x,y,w,h,c->allocated_width,c->allocated_height);
}
static void capture_time(void *d, struct ext_image_copy_capture_frame_v1 *f, uint32_t hi,
                         uint32_t lo, uint32_t ns) {
    (void)d;
    (void)f;
    (void)hi;
    (void)lo;
    (void)ns;
}
static void capture_ready(void *d, struct ext_image_copy_capture_frame_v1 *f) {
    struct capture *c = d;
    ext_image_copy_capture_frame_v1_destroy(f);
    c->frame = NULL;
    c->released = true;
    c->ready = true;
    c->dirty = true; /* Upload before recycling SHM, so head turns reuse an
                        unchanged texture. */
}
static void capture_failed(void *d, struct ext_image_copy_capture_frame_v1 *f, uint32_t reason) {
    struct capture *c = d;
    ext_image_copy_capture_frame_v1_destroy(f);
    c->frame = NULL;
    c->released = true;
    c->ready = false;
    if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED)
        c->closed = true;
    /* Constraints may already have arrived; retry after reallocation/release. */
    if (c->released && !c->closed) {
        free_buffer(c);
        request_capture(c);
    }
    fprintf(stderr, "air presenter: capture failed (%u), retrying if source is live\n", reason);
}
static const struct ext_image_copy_capture_frame_v1_listener capture_listener = {
    .transform = capture_transform,
    .damage = capture_damage,
    .presentation_time = capture_time,
    .ready = capture_ready,
    .failed = capture_failed};
static void session_size(void *d, struct ext_image_copy_capture_session_v1 *s, uint32_t w,
                         uint32_t h) {
    (void)s;
    struct capture *c = d;
    c->width = w;
    c->height = h;
    c->constraints = false;
}
static void session_format(void *d, struct ext_image_copy_capture_session_v1 *s, uint32_t format) {
    (void)s;
    struct capture *c = d;
    if (air_pixel_bytes(format) && (!c->format_ok || format == WL_SHM_FORMAT_ARGB8888)) {
        c->format = format;
        c->format_ok = true;
    }
}
static void session_device(void *d, struct ext_image_copy_capture_session_v1 *s,
                           struct wl_array *device) {
    (void)d;(void)s;(void)device;
}
static void session_dmabuf(void *d, struct ext_image_copy_capture_session_v1 *s, uint32_t f,
                           struct wl_array *mods) {
    (void)d;(void)s;(void)f;(void)mods;
}
static void session_done(void *d, struct ext_image_copy_capture_session_v1 *s) {
    (void)s;
    struct capture *c = d;
    c->constraints = true;
    if (!c->format_ok)
        fatal("capture source offers no supported SHM pixel format");
    request_capture(c);
}
static void session_stopped(void *d, struct ext_image_copy_capture_session_v1 *s) {
    (void)s;
    struct capture *c = d;
    c->closed = true;
    c->ready = false;
}
static const struct ext_image_copy_capture_session_v1_listener session_listener = {
    .buffer_size = session_size,
    .shm_format = session_format,
    .dmabuf_device = session_device,
    .dmabuf_format = session_dmabuf,
    .done = session_done,
    .stopped = session_stopped};
static void request_capture(struct capture *c) {
    if (c->closed || !c->wanted || !c->session || !c->constraints || !c->format_ok || c->frame ||
        c->dirty || !c->released)
        return;
    uint64_t now = now_ns();
    if (c->background && c->texture &&
        c->allocated_width == c->width && c->allocated_height == c->height &&
        now - c->last_request < 200000000ULL)
        return;
    if (!c->width || !c->height || c->width > AIR_MAX_CAPTURE_DIMENSION ||
        c->height > AIR_MAX_CAPTURE_DIMENSION)
        fatal("capture dimensions unsupported");
    if (!c->buffer || c->allocated_width != c->width || c->allocated_height != c->height) {
        free_buffer(c);
        c->ready = false;
        c->stride = air_pixel_stride(c->format, c->width);
        c->bytes = (size_t)c->stride * c->height;
        int fd = memfd_create("air-window-capture", MFD_CLOEXEC);
        if (fd < 0 || ftruncate(fd, c->bytes) < 0)
            fatal("SHM allocation failed");
        c->map = mmap(NULL, c->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (c->map == MAP_FAILED)
            fatal("SHM mapping failed");
        struct wl_shm_pool *pool = wl_shm_create_pool(c->app->shm, fd, c->bytes);
        c->buffer = wl_shm_pool_create_buffer(pool, 0, c->width, c->height, c->stride, c->format);
        wl_shm_pool_destroy(pool);
        close(fd);
        wl_buffer_add_listener(c->buffer, &buffer_listener, c);
        c->allocated_width = c->width;
        c->allocated_height = c->height;
    }
    c->damage=(struct air_pixel_damage){0};
    c->frame = ext_image_copy_capture_session_v1_create_frame(c->session);
    ext_image_copy_capture_frame_v1_add_listener(c->frame, &capture_listener, c);
    ext_image_copy_capture_frame_v1_attach_buffer(c->frame, c->buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(c->frame, 0, 0, c->width, c->height);
    ext_image_copy_capture_frame_v1_capture(c->frame);
    c->released = false;
    c->last_request = now;
}
static void start_capture(struct capture *c) {
    if (c->session || c->closed)
        return;
    struct ext_image_capture_source_v1 *source = c->widget_id
        ? air_widget_manager_v1_get_source(c->app->widgets, c->widget_id >> 32, (uint32_t)c->widget_id)
        : ext_foreign_toplevel_image_capture_source_manager_v1_create_source(c->app->sources, c->handle);
    c->session = ext_image_copy_capture_manager_v1_create_session(c->app->copies, source, 0);
    ext_image_capture_source_v1_destroy(source);
    ext_image_copy_capture_session_v1_add_listener(c->session, &session_listener, c);
    c->released = true;
}
/* Reuse GL storage and CPU staging. The ready event reports source damage;
 * damage_buffer is separate and describes the destination buffer's contents. */
static void capture_upload(struct capture *c) {
    c->has_capture_panel = false;
    uint32_t w=c->allocated_width,h=c->allocated_height;
    bool full=!c->texture || c->upload_width!=w || c->upload_height!=h;
    struct air_pixel_damage d=c->damage;
    if(full || !d.width || !d.height)d=(struct air_pixel_damage){0,0,w,h};
    size_t bytes=(size_t)d.width*d.height*4;
    if(bytes>c->rgba_capacity){
        unsigned char *rgba=realloc(c->rgba,bytes);
        if(!rgba)fatal("capture conversion allocation failed");
        c->rgba=rgba;c->rgba_capacity=bytes;
    }
    const unsigned char *src=(const unsigned char *)c->map+(size_t)d.y*c->stride+
                              (size_t)d.x*air_pixel_bytes(c->format);
    air_pixels_rgba(c->rgba,src,d.width,d.height,c->stride,c->format);
    if(!c->texture){
        glGenTextures(1,&c->texture);
        glBindTexture(GL_TEXTURE_2D,c->texture);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    }else glBindTexture(GL_TEXTURE_2D,c->texture);
    if(full)glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,c->rgba);
    else glTexSubImage2D(GL_TEXTURE_2D,0,d.x,d.y,d.width,d.height,GL_RGBA,GL_UNSIGNED_BYTE,c->rgba);
    c->upload_width=w;c->upload_height=h;
    afilter_texture(w,h);
}
/* A failure/constraints event may arrive between SCENE_BEGIN and the next draw,
 * while wanted is false. Re-evaluate capture on every draw, even without a dirty
 * texture, so a deferred retry cannot strand a resized static widget. */
static void prepare_capture(struct capture *c) {
    c->wanted = true;
    start_capture(c);
    if (c->dirty) {
        uint64_t started=now_ns();
        capture_upload(c);
        c->app->perf_upload_ns+=now_ns()-started;
        c->dirty = false;
    }
    request_capture(c);
}
static void toplevel_closed(void *d, struct ext_foreign_toplevel_handle_v1 *h) {
    (void)h;
    struct capture *c = d;
    c->closed = true;
    c->ready = false;
}
static void toplevel_done(void *d, struct ext_foreign_toplevel_handle_v1 *h) {
    (void)d;
    (void)h;
}
static void toplevel_title(void *d, struct ext_foreign_toplevel_handle_v1 *h, const char *s) {
    (void)d;
    (void)h;
    (void)s;
}
static void toplevel_id(void *d, struct ext_foreign_toplevel_handle_v1 *h, const char *s) {
    (void)h;
    snprintf(((struct capture *)d)->identifier, 128, "%s", s);
}
static const struct ext_foreign_toplevel_handle_v1_listener toplevel_listener = {
    .closed = toplevel_closed,
    .done = toplevel_done,
    .title = toplevel_title,
    .app_id = toplevel_title,
    .identifier = toplevel_id};
static void list_toplevel(void *d, struct ext_foreign_toplevel_list_v1 *l,
                          struct ext_foreign_toplevel_handle_v1 *h) {
    (void)l;
    struct app *a = d;
    struct capture *c = calloc(1, sizeof(*c));
    if (!c)
        fatal("out of memory");
    c->app = a;
    c->handle = h;
    c->next = a->captures;
    a->captures = c;
    ext_foreign_toplevel_handle_v1_add_listener(h, &toplevel_listener, c);
}
static void list_finished(void *d, struct ext_foreign_toplevel_list_v1 *l) {
    (void)l;
    ((struct app *)d)->running = false;
}
static const struct ext_foreign_toplevel_list_v1_listener list_listener = {
    .toplevel = list_toplevel, .finished = list_finished};
static struct capture *get_capture(struct app *a, const char *id) {
    for (struct capture *c = a->captures; c; c = c->next)
        if (!c->closed && !strcmp(c->identifier, id))
            return c;
    return NULL;
}
static void output_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw,
                            int32_t ph, int32_t sub, const char *make, const char *model,
                            int32_t transform) {
    (void)d;
    (void)o;
    (void)x;
    (void)y;
    (void)pw;
    (void)ph;
    (void)sub;
    (void)make;
    (void)model;
    (void)transform;
}
static void output_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w, int32_t h,
                        int32_t rate) {
    (void)d;
    (void)o;
    (void)flags;
    (void)w;
    (void)h;
    (void)rate;
}
static void output_done(void *d, struct wl_output *o) {
    (void)d;
    (void)o;
}
static void output_scale(void *d, struct wl_output *o, int32_t scale) {
    (void)d;
    (void)o;
    (void)scale;
}
static void output_name(void *d, struct wl_output *o, const char *name) {
    (void)o;
    snprintf(((struct output *)d)->name, 128, "%s", name);
}
static void output_description(void *d, struct wl_output *o, const char *name) {
    (void)d;
    (void)o;
    (void)name;
}
static const struct wl_output_listener output_listener = {.geometry = output_geometry,
                                                          .mode = output_mode,
                                                          .done = output_done,
                                                          .scale = output_scale,
                                                          .name = output_name,
                                                          .description = output_description};
static void presentation_clock(void *d, struct wp_presentation *p, uint32_t clock) {
    (void)d;
    (void)p;
    if (clock != CLOCK_MONOTONIC)
        fatal("presentation clock is not monotonic");
}
static const struct wp_presentation_listener presentation_listener = {.clock_id =
                                                                          presentation_clock};
static void registry_global(void *d, struct wl_registry *r, uint32_t id, const char *name,
                            uint32_t version) {
    struct app *a = d;
#define BIND(n, field, iface, v)                                                                   \
    if (!strcmp(name, n))                                                                          \
    a->field = wl_registry_bind(r, id, &iface, v)
    BIND("wl_compositor", compositor, wl_compositor_interface, 4);
    else BIND("wl_shm", shm, wl_shm_interface, 1);
    else BIND("zwlr_layer_shell_v1", shell, zwlr_layer_shell_v1_interface, 4);
    else BIND("air_widget_manager_v1", widgets, air_widget_manager_v1_interface, 1);
    else BIND("ext_foreign_toplevel_list_v1", list, ext_foreign_toplevel_list_v1_interface, 1);
    else BIND("ext_foreign_toplevel_image_capture_source_manager_v1", sources,
              ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
    else BIND("ext_image_copy_capture_manager_v1", copies,
              ext_image_copy_capture_manager_v1_interface, 1);
    else BIND("wp_presentation", presentation, wp_presentation_interface, 1);
    else if (!strcmp(name, "wl_output") && version >= 4) {
        struct output *o = calloc(1, sizeof(*o));
        o->app = a;
        o->registry_id = id;
        o->wl = wl_registry_bind(r, id, &wl_output_interface, 4);
        o->next = a->outputs;
        a->outputs = o;
        wl_output_add_listener(o->wl, &output_listener, o);
    }
#undef BIND
}
static void registry_remove(void *d, struct wl_registry *r, uint32_t id) {
    (void)r;
    struct app *a = d;
    if (a->output && a->output->registry_id == id)
        a->running = false;
}
static const struct wl_registry_listener registry_listener = {.global = registry_global,
                                                              .global_remove = registry_remove};
static void layer_configure(void *d, struct zwlr_layer_surface_v1 *s, uint32_t serial, uint32_t w,
                            uint32_t h) {
    struct app *a = d;
    zwlr_layer_surface_v1_ack_configure(s, serial);
    a->width = w ? w : 1920;
    a->height = h ? h : 1080;
    if (a->egl_window) {
        a->render_scale = afilter_scale(a->width, a->height, a->max_render_size);
        wl_surface_set_buffer_scale(a->surface, a->render_scale);
        wl_egl_window_resize(a->egl_window, a->width * a->render_scale,
                             a->height * a->render_scale, 0, 0);
    }
    a->configured = true;
    a->frame_due = true;
}
static void layer_closed(void *d, struct zwlr_layer_surface_v1 *s) {
    (void)s;
    ((struct app *)d)->running = false;
}
static const struct zwlr_layer_surface_v1_listener layer_listener = {.configure = layer_configure,
                                                                     .closed = layer_closed};
static void feedback_sync(void *d, struct wp_presentation_feedback *f, struct wl_output *o) {
    (void)d;
    (void)f;
    (void)o;
}
static void feedback_presented(void *d, struct wp_presentation_feedback *f, uint32_t hi,
                               uint32_t lo, uint32_t ns, uint32_t refresh, uint32_t shi,
                               uint32_t slo, uint32_t flags) {
    (void)hi;
    (void)lo;
    (void)ns;
    (void)refresh;
    (void)shi;
    (void)slo;
    (void)flags;
    struct frame_feedback *p = d;
    struct app *a=p->app;
    uint64_t stamp=(((uint64_t)hi << 32) | lo)*1000000000ULL+ns;
    if(!a->perf_start)a->perf_start=stamp;
    if(a->perf_last && stamp>a->perf_last){
        uint64_t gap=stamp-a->perf_last;
        if(gap>a->perf_gap)a->perf_gap=gap;
        atiming_record(&a->perf_gaps, gap);
        if(refresh && gap>(uint64_t)refresh*3/2)a->perf_slow++;
    }
    a->perf_last=stamp;a->perf_frames++;
    if (p->pose_stamp && stamp >= p->pose_stamp)
        atiming_record(&a->perf_pose_age, stamp - p->pose_stamp);
    if(stamp>a->perf_start && stamp-a->perf_start>=5000000000ULL){
        double seconds=(stamp-a->perf_start)/1e9;
        double draws=a->perf_draws ? a->perf_draws : 1;
        fprintf(stderr,"air presenter: perf %.1f FPS, max gap %.1f ms, %llu missed-refresh intervals; "
                       "gap p95/p99 %u/%u ms, pose age at scanout p95/p99 %u/%u ms, "
                       "%llu discarded, CPU frame %.2f ms (upload %.2f, swap %.2f), %dx%d, %s\n",
                (a->perf_frames-1)/seconds,a->perf_gap/1e6,(unsigned long long)a->perf_slow,
                atiming_percentile(&a->perf_gaps, 95), atiming_percentile(&a->perf_gaps, 99),
                atiming_percentile(&a->perf_pose_age, 95), atiming_percentile(&a->perf_pose_age, 99),
                (unsigned long long)a->perf_discards, a->perf_draw_ns/draws/1e6,a->perf_upload_ns/draws/1e6,a->perf_swap_ns/draws/1e6,
                a->width,a->height,a->stereo?"stereo":"mono");
        a->perf_start=stamp;a->perf_frames=1;a->perf_gap=a->perf_slow=0;
        a->perf_draw_ns=a->perf_upload_ns=a->perf_swap_ns=a->perf_draws=a->perf_discards=0;
        a->perf_gaps = (struct air_timing){0};
        a->perf_pose_age = (struct air_timing){0};
    }
    struct air_packet frame = {.type = AIR_FRAME_PRESENTED,
                               .epoch = p->epoch,
                               .stamp_ns = (((uint64_t)hi << 32) | lo) * 1000000000ULL + ns};
    bool complete=p->healthy && awatchdog_age(stamp,p->pose_stamp)<250000000ULL;
    frame.data.frame.complete = complete;
    frame.data.frame.widget_yaw = p->widget_yaw;
    frame.data.frame.overview_revision = p->overview_revision;
    memcpy(frame.data.frame.overview_basis, p->overview_basis, sizeof(p->overview_basis));
    packet(p->app, &frame);
    if (complete) {
        struct air_packet m = {.type = AIR_PRESENTED,
                               .epoch = p->epoch,
                               .stamp_ns = (((uint64_t)hi << 32) | lo) * 1000000000ULL + ns};
        packet(p->app, &m);
    }
    wp_presentation_feedback_destroy(f);
    free(p);
}
static void feedback_discarded(void *d, struct wp_presentation_feedback *f) {
    struct frame_feedback *p = d;
    p->app->perf_discards++;
    wp_presentation_feedback_destroy(f);
    free(p);
}
static const struct wp_presentation_feedback_listener feedback_listener = {
    .sync_output = feedback_sync, .presented = feedback_presented, .discarded = feedback_discarded};
static void frame_done(void *d, struct wl_callback *cb, uint32_t time) {
    (void)time;
    struct app *a = d;
    wl_callback_destroy(cb);
    a->frame_due = true;
}
static const struct wl_callback_listener frame_listener = {.done = frame_done};
static void draw_quad(struct app *a, GLuint tex, const struct air_vec q[4], uint32_t transform) {
    float uv[8] = {0, 0, 1, 0, 0, 1, 1, 1};
    for (int i = 0; i < 4; i++) {
        float u = uv[2 * i], v = uv[2 * i + 1];
        if (transform >= 4)
            u = 1 - u;
        switch (transform % 4) {
        case 1:
            uv[2 * i] = 1 - v;
            uv[2 * i + 1] = u;
            break;
        case 2:
            uv[2 * i] = 1 - u;
            uv[2 * i + 1] = 1 - v;
            break;
        case 3:
            uv[2 * i] = v;
            uv[2 * i + 1] = 1 - u;
            break;
        default:
            uv[2 * i] = u;
            uv[2 * i + 1] = v;
        }
    }
    float pos[12];
    for (int i = 0; i < 4; i++) {
        pos[3 * i] = q[i].x;
        pos[3 * i + 1] = q[i].y;
        pos[3 * i + 2] = q[i].z;
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glEnableVertexAttribArray(a->attr_pos);
    glEnableVertexAttribArray(a->attr_uv);
    glVertexAttribPointer(a->attr_pos, 3, GL_FLOAT, GL_FALSE, 0, pos);
    glVertexAttribPointer(a->attr_uv, 2, GL_FLOAT, GL_FALSE, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}
static void frame_rect(struct app *a, GLuint texture_id, const struct air_panel *p, double x,
                       double y, double width, double height) {
    struct air_vec corners[4] = {apoint(p, x, y), apoint(p, x + width, y), apoint(p, x, y + height),
                                 apoint(p, x + width, y + height)};
    draw_quad(a, texture_id, corners, 0);
}
static void fitted_text(cairo_t *cr, const char *text, double x, double y, double width);

static void draw_frame(struct app *a, struct capture *c, const struct air_panel *p) {
    if (!a->frame_texture) {
        const unsigned char dark[4] = {31, 44, 51, 255}, focused[4] = {65, 118, 125, 255},
                            bright[4] = {107, 228, 206, 255};
        texture(&a->frame_texture, dark, 1, 1, 4, WL_SHM_FORMAT_ABGR8888);
        texture(&a->frame_focus_texture, focused, 1, 1, 4, WL_SHM_FORMAT_ABGR8888);
        texture(&a->frame_hover_texture, bright, 1, 1, 4, WL_SHM_FORMAT_ABGR8888);
    }
    double left, top, right, bottom;
    aframe_bounds(p, &left, &top, &right, &bottom);
    int width = (int)(right - left) + 2 * AIR_FRAME_BORDER;
    uint32_t hover = a->hover_id == p->id ? a->hover_region : 0;
    uint32_t chrome_flags = p->flags & (AIR_PANEL_FOCUSED | AIR_PANEL_PINNED);
    uint32_t chrome_hover = hover >= AIR_CENTER ? hover : 0;
    bool controls = (chrome_flags & AIR_PANEL_FOCUSED) && right - left >= AIR_FRAME_ACTION_MIN_WIDTH;
    if (!c->title_texture || strcmp(c->title_cache, p->title) || c->title_width != width ||
        c->chrome_flags != chrome_flags || c->chrome_hover != chrome_hover) {
        cairo_surface_t *surface =
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width * 2, AIR_FRAME_TITLE * 2);
        cairo_t *cr = cairo_create(surface);
        cairo_scale(cr, 2, 2);
        cairo_font_options_t *options = cairo_font_options_create();
        cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
        cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_OFF);
        cairo_set_font_options(cr, options);
        cairo_font_options_destroy(options);
        cairo_set_source_rgb(cr, .10, .17, .21);
        cairo_paint(cr);
        cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 22);
        cairo_set_source_rgb(cr, .85, .96, .95);
        double actions = controls ? AIR_FRAME_ACTION_COUNT * AIR_FRAME_ACTION_WIDTH : 0;
        fitted_text(cr, p->title, 17, 28, width - actions - 38);
        if (controls) {
            const uint32_t regions[] = {AIR_CENTER, AIR_PIN, AIR_MINIMIZE};
            const char *labels[] = {"◎", (chrome_flags & AIR_PANEL_PINNED) ? "◆" : "◇", "−"};
            for (int i = 0; i < AIR_FRAME_ACTION_COUNT; i++) {
                double x = width - AIR_FRAME_BORDER - actions + i * AIR_FRAME_ACTION_WIDTH;
                if (chrome_hover == regions[i]) {
                    cairo_set_source_rgb(cr, .21, .40, .43);
                    cairo_rectangle(cr, x, 2, AIR_FRAME_ACTION_WIDTH, AIR_FRAME_TITLE - 4);
                    cairo_fill(cr);
                }
                cairo_set_source_rgb(cr, .85, .96, .95);
                cairo_move_to(cr, x + 9, 28);
                cairo_show_text(cr, labels[i]);
            }
        }
        cairo_surface_flush(surface);
        texture(&c->title_texture, cairo_image_surface_get_data(surface), width * 2, AIR_FRAME_TITLE * 2,
                cairo_image_surface_get_stride(surface), WL_SHM_FORMAT_ARGB8888);
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        snprintf(c->title_cache, sizeof(c->title_cache), "%s", p->title);
        c->title_width = width;
        c->chrome_flags = chrome_flags;
        c->chrome_hover = chrome_hover;
    }
    double b = AIR_FRAME_BORDER, title = AIR_FRAME_TITLE;
    GLuint border = p->flags & AIR_PANEL_FOCUSED ? a->frame_focus_texture : a->frame_texture;
    GLuint bright = a->frame_hover_texture;
    frame_rect(a, hover & AIR_EDGE_TOP ? bright : border, p, left - b, top - title - b, width, b);
    frame_rect(a, hover & AIR_EDGE_BOTTOM ? bright : border, p, left - b, bottom, width, b);
    frame_rect(a, hover & AIR_EDGE_LEFT ? bright : border, p, left - b, top - title, b, bottom - top + title);
    frame_rect(a, hover & AIR_EDGE_RIGHT ? bright : border, p, right, top - title, b, bottom - top + title);
    if (p->flags & AIR_PANEL_SNAP) {
        struct air_panel target = *p;
        target.azimuth = p->snap_azimuth;
        frame_rect(a, bright, &target, left - b, top - title - b, width, 2);
        frame_rect(a, bright, &target, left - b, bottom + b, width, 2);
    }
    frame_rect(a, c->title_texture, p, left - b, top - title, width, title);
}
/* Settings are applied once per packet; draw() reads the same value for both eyes. */
static void apply_stereo(struct app *a, const struct air_packet *m) {
    double ipd = m->data.stereo.ipd_mm;
    if (!aipd_valid((struct air_ipd){ipd,m->data.stereo.center_mm,m->data.stereo.baseline_mm})) {
        a->running = false;
        return;
    }
    if (a->stereo_received && (ipd != a->ipd_mm || m->data.stereo.center_mm != a->ipd_center_mm))
        a->ipd_feedback_until = now_ns() + 2500000000ULL;
    a->stereo = m->data.stereo.enabled != 0;
    a->ipd_mm = ipd;
    a->ipd_center_mm = m->data.stereo.center_mm;
    a->ipd_baseline_mm = m->data.stereo.baseline_mm;
    a->calibration = m->data.stereo.calibration != 0;
    a->stereo_received = true;
    fprintf(stderr, "air presenter: applied %s IPD %.1f mm, guide %s\n",
            a->stereo ? "stereo" : "mono", ipd, a->calibration ? "on" : "off");
}
static void camera_quad(struct app *a, GLuint tex, double x, double y,
                        double depth, double width, double height) {
    struct air_vec f=abaxis(a->basis,2),r=abaxis(a->basis,0),u=abaxis(a->basis,1);
    struct air_vec c=aadd(ascale(f,depth),aadd(ascale(r,x),ascale(u,y)));
    struct air_vec q[4]={aadd(c,aadd(ascale(r,-width/2),ascale(u,height/2))),
                        aadd(c,aadd(ascale(r,width/2),ascale(u,height/2))),
                        aadd(c,aadd(ascale(r,-width/2),ascale(u,-height/2))),
                        aadd(c,aadd(ascale(r,width/2),ascale(u,-height/2)))};
    draw_quad(a,tex,q,0);
}
static void fitted_text(cairo_t *cr, const char *text, double x, double y, double width) {
    char fit[192];
    snprintf(fit, sizeof(fit), "%s", text);
    size_t length = strlen(fit);
    bool truncated = strlen(text) > length;
    while (length) {
        cairo_text_extents_t extents;
        cairo_text_extents(cr, fit, &extents);
        if (cairo_status(cr) != CAIRO_STATUS_SUCCESS)
            return;
        if (extents.x_advance <= width - (truncated ? 24 : 0))
            break;
        do {
            length--;
        } while (length && ((unsigned char)fit[length] & 0xc0) == 0x80);
        fit[length] = 0;
        truncated = true;
    }
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, fit);
    if (truncated)
        cairo_show_text(cr, "…");
}

/* On-wire strings may have been clipped at a byte limit. Drop an incomplete tail. */
static void finish_utf8(char *text, size_t size) {
    text[size - 1] = 0;
    size_t length = strlen(text);
    size_t i = 0;
    while (i < length) {
        unsigned char ch = text[i];
        unsigned bytes = ch < 0x80 ? 1 : ch >= 0xc2 && ch <= 0xdf ? 2 :
                         ch >= 0xe0 && ch <= 0xef ? 3 : ch >= 0xf0 && ch <= 0xf4 ? 4 : 0;
        if (!bytes || i + bytes > length) {
            text[i] = 0;
            break;
        }
        for (unsigned j = 1; j < bytes; j++) {
            if (((unsigned char)text[i + j] & 0xc0) != 0x80) {
                text[i] = 0;
                return;
            }
        }
        i += bytes;
    }
}

static void draw_overview(struct app *a, float eye_offset) {
    if (!a->overview)
        return;
    if (a->overview_dirty && !a->overview_pending) {
        unsigned rows = a->overview_count > a->overview_start ?
            a->overview_count - a->overview_start : 0;
        if (rows > AIR_OVERVIEW_ROWS)
            rows = AIR_OVERVIEW_ROWS;
        int height = aoverview_texture_height(rows);
        cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
            AIR_OVERVIEW_TEXTURE_W, height);
        cairo_t *cr = cairo_create(surface);
        cairo_set_source_rgb(cr, .06, .10, .13);
        cairo_paint(cr);
        cairo_set_source_rgb(cr, .88, .96, .98);
        cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 42);
        cairo_move_to(cr, 37, 65);
        cairo_show_text(cr, "Windows");
        cairo_set_source_rgb(cr, .12, .20, .23);
        cairo_rectangle(cr, 36, 84, 1068, 58);
        cairo_fill(cr);
        cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_source_rgb(cr, .83, .92, .94);
        cairo_set_font_size(cr, 29);
        fitted_text(cr, a->overview_query[0] ? a->overview_query : "Search windows or workspaces",
                    55, 123, 1026);
        for (unsigned i = 0; i < AIR_OVERVIEW_ROWS && i + a->overview_start < a->overview_count; i++) {
            double y = AIR_OVERVIEW_ROW_TOP + i * AIR_OVERVIEW_ROW_HEIGHT;
            if (i + a->overview_start == a->overview_selected) {
                cairo_set_source_rgb(cr, .15, .30, .34);
                cairo_rectangle(cr, 36, y, 1068, AIR_OVERVIEW_ROW_HEIGHT - 2);
                cairo_fill(cr);
            }
            cairo_set_source_rgb(cr, .92, .97, .98);
            cairo_set_font_size(cr, 29);
            fitted_text(cr, a->overview_rows[i].title, 55, y + 37, 690);
            cairo_set_source_rgb(cr, .69, .82, .84);
            cairo_set_font_size(cr, 23);
            fitted_text(cr, a->overview_rows[i].workspace, 766, y + 37, 200);
            cairo_move_to(cr, 976, y + 37);
            cairo_show_text(cr, a->overview_rows[i].visible ? "In view" : "Outside");
        }
        cairo_set_source_rgb(cr, .69, .82, .84);
        cairo_set_font_size(cr, 24);
        if (!a->overview_count) {
            cairo_move_to(cr, 55, 204);
            cairo_show_text(cr, "No matching windows");
        }
        cairo_move_to(cr, 36, height - 34);
        cairo_show_text(cr, "↑ ↓ Select     Enter Center     Esc Close");
        char count[48];
        snprintf(count, sizeof(count), "%u / %u",
                 a->overview_count ? a->overview_selected + 1 : 0, a->overview_count);
        cairo_move_to(cr, 986, height - 34);
        cairo_show_text(cr, count);
        cairo_surface_flush(surface);
        texture(&a->overview_texture, cairo_image_surface_get_data(surface),
                AIR_OVERVIEW_TEXTURE_W, height,
                cairo_image_surface_get_stride(surface), WL_SHM_FORMAT_ARGB8888);
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        a->overview_dirty = false;
        a->overview_texture_height = height;
    }
    glUniform1f(a->uniform_eye, 0);
    glUniform1f(a->uniform_zoom, 1);
    camera_quad(a, a->overview_texture, 0, 0, 1, AIR_OVERVIEW_WIDTH, a->overview_texture_height / 2000.);
    glUniform1f(a->uniform_eye, eye_offset);
    glUniform1f(a->uniform_zoom, a->zoom);
}

static void draw_ipd_feedback(struct app *a) {
    if (!a->stereo || a->calibration || now_ns() >= a->ipd_feedback_until) return;
    if (!a->ipd_texture || a->feedback_ipd != a->ipd_mm) {
        cairo_surface_t *s=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,900,100);
        cairo_t *cr=cairo_create(s);
        cairo_set_source_rgba(cr,.02,.05,.08,.95);cairo_paint(cr);
        cairo_set_source_rgb(cr,.85,1,.95);cairo_set_font_size(cr,38);
        char text[128];snprintf(text,sizeof(text),"Stereo IPD: %.1f mm   |   Super+Alt+I: guide",a->ipd_mm);
        cairo_move_to(cr,24,64);cairo_show_text(cr,text);cairo_surface_flush(s);
        texture(&a->ipd_texture,cairo_image_surface_get_data(s),900,100,
                cairo_image_surface_get_stride(s),WL_SHM_FORMAT_ARGB8888);
        cairo_destroy(cr);cairo_surface_destroy(s);a->feedback_ipd=a->ipd_mm;
    }
    camera_quad(a,a->ipd_texture,0,.23,1.4,.70,.078);
}
static void draw_calibration(struct app *a, int eye) {
    if (!a->calibration) return;
    if (!a->calibration_texture || a->calibration_ipd != a->ipd_mm ||
        a->calibration_center != a->ipd_center_mm || a->calibration_baseline != a->ipd_baseline_mm) {
        cairo_surface_t *s=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,1400,360);
        cairo_t *cr=cairo_create(s);
        cairo_set_source_rgba(cr,.02,.05,.08,.95);cairo_paint(cr);
        cairo_select_font_face(cr,"sans",CAIRO_FONT_SLANT_NORMAL,CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_source_rgb(cr,.85,1,.95);cairo_set_font_size(cr,44);
        char text[128];snprintf(text,sizeof(text),"Stereo depth check | Software IPD: %.1f mm",a->ipd_mm);
        cairo_move_to(cr,40,70);cairo_show_text(cr,text);
        cairo_set_font_size(cr,30);
        snprintf(text,sizeof(text),"Super+Alt+, / .: adjust    \\: reset %.2f mm",a->ipd_baseline_mm);
        cairo_move_to(cr,40,130);cairo_show_text(cr,text);
        cairo_move_to(cr,40,185);cairo_show_text(cr,"Blue marks stay fixed; white targets test image movement.");
        cairo_move_to(cr,40,240);cairo_show_text(cr,"Do not align them. This test does not measure your IPD.");
        snprintf(text,sizeof(text),"Left %.2f | Right %.2f mm    Super+Alt+I: close",
                 a->ipd_mm/2-a->ipd_center_mm,a->ipd_mm/2+a->ipd_center_mm);
        cairo_move_to(cr,40,300);cairo_show_text(cr,text);
        cairo_surface_flush(s);
        texture(&a->calibration_texture,cairo_image_surface_get_data(s),1400,360,
                cairo_image_surface_get_stride(s),WL_SHM_FORMAT_ARGB8888);
        cairo_destroy(cr);cairo_surface_destroy(s);a->calibration_ipd=a->ipd_mm;
        a->calibration_center=a->ipd_center_mm;a->calibration_baseline=a->ipd_baseline_mm;
    }
    camera_quad(a,a->calibration_texture,0,.08,1.4,.80,.206);
    if (!a->eye_texture[eye]) {
        cairo_surface_t *s=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,400,100);
        cairo_t *cr=cairo_create(s);
        cairo_set_source_rgb(cr,.4,.8,1);cairo_set_font_size(cr,48);
        cairo_move_to(cr,25,65);cairo_show_text(cr,eye ? "RIGHT EYE" : "LEFT EYE");
        cairo_surface_flush(s);
        texture(&a->eye_texture[eye],cairo_image_surface_get_data(s),400,100,
                cairo_image_surface_get_stride(s),WL_SHM_FORMAT_ARGB8888);
        cairo_destroy(cr);cairo_surface_destroy(s);
    }
    if (!a->reference_texture) {
        cairo_surface_t *s=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,256,256);
        cairo_t *cr=cairo_create(s);
        cairo_set_source_rgb(cr,.2,.65,1);cairo_set_line_width(cr,7);
        cairo_move_to(cr,128,25);cairo_line_to(cr,128,195);
        cairo_move_to(cr,103,110);cairo_line_to(cr,153,110);cairo_stroke(cr);
        cairo_surface_flush(s);
        texture(&a->reference_texture,cairo_image_surface_get_data(s),256,256,
                cairo_image_surface_get_stride(s),WL_SHM_FORMAT_ARGB8888);
        cairo_destroy(cr);cairo_surface_destroy(s);
    }
    /* Screen-fixed references bypass eye translation only. Restore the live
     * offset before any scene content; these are guide marks, not world poses. */
    glUniform1f(a->uniform_eye,0);
    camera_quad(a,a->eye_texture[eye],eye ? .23 : -.23,.25,1.4,.22,.055);
    for (int i=0;i<2;i++) {
        double z=i ? 3 : .6;
        camera_quad(a,a->reference_texture,(i ? .12 : -.12)*z,-.10*z,z,.12*z,.12*z);
    }
    glUniform1f(a->uniform_eye,aeye_offset_center(eye,a->ipd_mm,a->ipd_center_mm));
    for (int i=0;i<2;i++) {
        if (!a->depth_texture[i]) {
            cairo_surface_t *s=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,256,256);
            cairo_t *cr=cairo_create(s);
            cairo_set_source_rgb(cr,.85,1,.95);cairo_set_line_width(cr,5);
            cairo_arc(cr,128,110,70,0,2*AIR_PI);cairo_stroke(cr);
            cairo_move_to(cr,128,25);cairo_line_to(cr,128,195);
            cairo_move_to(cr,43,110);cairo_line_to(cr,213,110);cairo_stroke(cr);
            cairo_set_font_size(cr,28);cairo_move_to(cr,40,240);
            cairo_show_text(cr,i ? "Far: 3 m" : "Near: 0.6 m");cairo_surface_flush(s);
            texture(&a->depth_texture[i],cairo_image_surface_get_data(s),256,256,
                    cairo_image_surface_get_stride(s),WL_SHM_FORMAT_ARGB8888);
            cairo_destroy(cr);cairo_surface_destroy(s);
        }
        double z=i ? 3 : .6;
        camera_quad(a,a->depth_texture[i],(i ? .12 : -.12)*z,-.10*z,z,.12*z,.12*z);
    }
}
static void receive_pose(struct app *a);
static void report_pose(struct app *a);

static struct capture *prepare_panel(struct app *a, const struct air_panel *p, double aspect,
                                     uint64_t upload_base) {
    struct capture *c = get_capture(a, p->identifier);
    if (!c && (p->flags & AIR_PANEL_WIDGET) && a->widgets) {
        c = calloc(1, sizeof(*c));
        if (c) {
            c->app = a;
            c->widget_id = p->id;
            c->released = true;
            memcpy(c->identifier, p->identifier, sizeof(c->identifier));
            c->next = a->captures;
            a->captures = c;
        }
    }
    if (!c)
        return NULL;
    c->background = a->active && a->pose_valid && !(p->flags & AIR_PANEL_WIDGET) &&
                    !apanel_maybe_visible(p, a->basis, aspect, a->zoom);
    /* Leave an offscreen update in SHM until it is relevant. Initial textures
     * and replacement sizes still complete promptly for scene acknowledgement. */
    bool reuse = c->dirty && c->texture && c->upload_width == c->allocated_width &&
                 c->upload_height == c->allocated_height;
    bool budget_exhausted = a->perf_upload_ns - upload_base >= 3500000ULL;
    if (reuse && (c->background || (budget_exhausted && c->upload_deferrals < 2))) {
        c->wanted = true;
        if (!c->background)
            c->upload_deferrals++;
    } else {
        prepare_capture(c);
        c->upload_deferrals = 0;
    }
    return c;
}

static void draw(struct app *a) {
    if (!a->configured || !a->frame_due)
        return;
    if (!a->egl_window)
        egl_init(a);
    a->frame_due = false;
    uint64_t draw_started=now_ns();
    glViewport(0, 0, a->width * a->render_scale, a->height * a->render_scale);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(a->program);
    int eye_width = a->stereo ? a->width / 2 : a->width;
    double aspect = (double)eye_width / a->height;
    struct capture *captures[AIR_MAX_PANELS];
    uint64_t upload_base = a->perf_upload_ns;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < a->count; i++) {
            bool priority = a->panels[i].flags & (AIR_PANEL_FOCUSED | AIR_PANEL_WIDGET);
            if (priority == (pass == 0))
                captures[i] = prepare_panel(a, &a->panels[i], aspect, upload_base);
        }
    }

    /* Capture uploads can block. Sample again after them, once for both eyes. */
    receive_pose(a);
    if (!a->running)
        return;
    report_pose(a);
    float matrix[9]; /* GL uniforms are column-major; basis vectors are rows. */
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            matrix[j * 3 + i] = a->basis[i * 3 + j];
    glUniformMatrix3fv(a->uniform_basis, 1, GL_FALSE, matrix);
    glUniform1f(a->uniform_aspect, aspect);
    glUniform1f(a->uniform_zoom, a->zoom);
    if (a->stereo && (a->width != 3840 || a->height != 1080)) {
        a->running=false; /* Never send an SBS image to a mono transport. */
        return;
    }
    double frame_widget_yaw=awidget_frame_yaw(a->basis,a->widget_follow,a->widget_yaw);
    bool healthy = a->committed && a->pose_valid && now_ns() - a->pose_stamp < 250000000;
    for (int eye=0; eye<(a->stereo?2:1);eye++) {
        glViewport(eye * eye_width * a->render_scale,0,
                   eye_width * a->render_scale,a->height * a->render_scale);
        glEnable(GL_SCISSOR_TEST);
        glScissor(eye * eye_width * a->render_scale,0,
                  eye_width * a->render_scale,a->height * a->render_scale);
        glUniform1f(a->uniform_eye,a->stereo?aeye_offset_center(eye,a->ipd_mm,a->ipd_center_mm):0);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    for (int pass = 0; pass < 2; pass++) {
      if (pass) glDisable(GL_DEPTH_TEST);
      for (int i = 0; i < a->count; i++) {
        struct air_panel frame_panel=a->panels[i];
        if(frame_panel.flags & AIR_PANEL_WIDGET)frame_panel.azimuth=frame_widget_yaw;
        struct air_panel *p = &frame_panel;
        if (!!(p->flags & AIR_PANEL_POPUP) != pass) continue;
        struct capture *c = captures[i];
        if (!c) {
            healthy = false;
            continue;
        }
        uint32_t transform = c->transform;
        bool rotated = (c->transform % 2) != 0;
        uint32_t width = rotated ? c->allocated_height : c->allocated_width,
                 height = rotated ? c->allocated_width : c->allocated_height;
        if (!c->ready || !c->texture || c->upload_width != c->allocated_width ||
            c->upload_height != c->allocated_height ||
            width != (uint32_t)p->bw || height != (uint32_t)p->bh) {
            healthy = false;
            if (c->closed || !c->has_capture_panel || (p->flags & AIR_PANEL_WIDGET))
                continue;
            /* A resize has committed, but the replacement capture is pending.
             * Keep the previous texture at its original logical dimensions. */
            frame_panel.width = c->last_capture_panel.width;
            frame_panel.height = c->last_capture_panel.height;
            frame_panel.bx = c->last_capture_panel.bx;
            frame_panel.by = c->last_capture_panel.by;
            frame_panel.bw = c->last_capture_panel.bw;
            frame_panel.bh = c->last_capture_panel.bh;
            transform = c->last_capture_transform;
        } else {
            c->last_capture_panel = *p;
            c->last_capture_transform = transform;
            c->has_capture_panel = true;
        }
        if (a->active && !(p->flags & AIR_PANEL_WIDGET) &&
            !apanel_maybe_visible(p, a->basis, aspect, a->zoom))
            continue;
        if (!(p->flags & AIR_PANEL_WIDGET))
            draw_frame(a, c, p);
        struct air_vec corners[4] = {apoint(p, p->bx, p->by), apoint(p, p->bx + p->bw, p->by),
                                     apoint(p, p->bx, p->by + p->bh),
                                     apoint(p, p->bx + p->bw, p->by + p->bh)};
        draw_quad(a, c->texture, corners, transform);
    }
    }
    glDisable(GL_DEPTH_TEST);
    draw_calibration(a,eye);
    draw_ipd_feedback(a);
    draw_overview(a, a->stereo ? aeye_offset_center(eye,a->ipd_mm,a->ipd_center_mm) : 0);
    if (a->cursor && a->cursor_w > 0 && !a->hide_cursor) {
        struct air_vec point = a->pointer;
        double depth = adot(point, abaxis(a->basis, 2));
        double zoom = a->zoom;
        if (a->overview && depth > .001) {
            point = ascale(point, 1 / depth);
            depth = zoom = 1;
            glUniform1f(a->uniform_eye, 0);
            glUniform1f(a->uniform_zoom, 1);
        }
        double scale = acursor_pixel_scale(depth, eye_width, a->cursor_scale) / fmax(1, zoom);
        struct air_vec right = abaxis(a->basis, 0), up = abaxis(a->basis, 1);
        struct air_vec tl = aadd(
            point, aadd(ascale(right, -a->cursor_x * scale), ascale(up, a->cursor_y * scale)));
        struct air_vec corners[4] = {
            tl, aadd(tl, ascale(right, a->cursor_w * scale)),
            aadd(tl, ascale(up, -a->cursor_h * scale)),
            aadd(aadd(tl, ascale(right, a->cursor_w * scale)), ascale(up, -a->cursor_h * scale))};
        draw_quad(a, a->cursor, corners, 0);
    }
    } /* Both eyes complete before presentation/scene acknowledgement. */
    glDisable(GL_SCISSOR_TEST);
    /* Closed/replaced Waybar instances must not accumulate SHM/GL resources. */
    struct capture **link = &a->captures;
    while (*link) {
        struct capture *c = *link;
        if (!c->widget_id || c->wanted) { link = &c->next; continue; }
        *link = c->next;
        if (c->frame) ext_image_copy_capture_frame_v1_destroy(c->frame);
        if (c->session) ext_image_copy_capture_session_v1_destroy(c->session);
        free_buffer(c);
        if (c->texture) glDeleteTextures(1, &c->texture);
        free(c->rgba);
        free(c);
    }
    struct frame_feedback *fb = calloc(1, sizeof(*fb));
    fb->app = a;
    fb->epoch = a->epoch;
    fb->healthy = healthy;
    fb->pose_stamp = a->pose_stamp;
    fb->widget_yaw = frame_widget_yaw;
    fb->overview_revision = a->overview_revision;
    if (a->overview)
        memcpy(fb->overview_basis, a->basis, sizeof(fb->overview_basis));
    fb->feedback = wp_presentation_feedback(a->presentation, a->surface);
    wp_presentation_feedback_add_listener(fb->feedback, &feedback_listener, fb);
    struct wl_callback *cb = wl_surface_frame(a->surface);
    wl_callback_add_listener(cb, &frame_listener, a);
    uint64_t swap_started=now_ns();
    if (!eglSwapBuffers(a->egl, a->egl_surface)) {
        aevent_write("presenter", "egl_swap_failed",
                     ",\"generation\":%llu,\"egl_error\":%d",
                     (unsigned long long)a->generation, eglGetError());
        a->running = false;
        return;
    }
    a->perf_swap_ns+=now_ns()-swap_started;
    a->perf_draw_ns+=now_ns()-draw_started;
    a->perf_draws++;
    if (eye_width != a->ready_width || a->height != a->ready_height) {
        struct air_packet m = {.type = AIR_READY};
        m.data.ready.width = eye_width;
        m.data.ready.height = a->height;
        packet(a, &m);
        a->ready_width = eye_width;
        a->ready_height = a->height;
    }
    a->last_frame = now_ns();
}
/* Pose liveness is independent of frame callbacks. Report the actual latest
 * owned UDP sample before GPU work, not an aged pose after a stalled upload. */
static void report_pose(struct app *a) {
    uint64_t now=now_ns();
    if(!a->pose_valid || now<a->pose_stamp || now-a->pose_stamp>=250000000ULL ||
       now-a->last_pose_report<8000000ULL)return;
    struct air_packet m={.type=AIR_POSE};
    memcpy(m.data.pose.basis,a->basis,sizeof(a->basis));
    m.data.pose.age=(double)(now-a->pose_stamp)/1e9;
    packet(a,&m);a->last_pose_report=now;
}
static void update_basis(struct app *a) {
    bool recentered = !a->zero_valid;
    if (!a->zero_valid) {
        memcpy(a->zero, a->raw, sizeof(a->zero));
        a->zero_valid = true;
    }
    bool head_roll =
        getenv("AIR_SPATIAL_HEAD_ROLL") && !strcmp(getenv("AIR_SPATIAL_HEAD_ROLL"), "1");
    double filtered[9];
    apose_update(&a->pose_filter, a->raw, a->pose_stamp, recentered, filtered);
    arelative_basis(filtered, a->zero, head_roll, a->basis);
}
static void receive_pose(struct app *a) {
    uint64_t polled=now_ns();
    bool discard=a->last_pose_poll && awatchdog_age(polled,a->last_pose_poll)>=250000000ULL;
    a->last_pose_poll=polled;
    unsigned char buf[1024];
    bool received = false;
    ssize_t n;
    struct sockaddr_in peer;
    socklen_t peer_len;
    while ((peer_len = sizeof(peer), n = recvfrom(a->udp, buf, sizeof(buf), MSG_DONTWAIT,
                                                  (struct sockaddr *)&peer, &peer_len)) > 0) {
        if (peer.sin_family != AF_INET || peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK) ||
            ntohs(peer.sin_port) != a->source_port)
            continue;
        if (n != 48 && n != 52)
            continue;
        double values[6];
        memcpy(values, buf, 48);
        bool valid = true;
        for (int i = 3; i < 6; i++)
            valid = valid && isfinite(values[i]) && fabs(values[i]) < 10000;
        if (!valid)
            continue;
        if (n == 52) {
            uint32_t sequence;
            memcpy(&sequence, buf + 48, sizeof(sequence));
            /* The driver resets its counter with its orientation reference.
             * Never keep rendering against an obsolete neutral frame after a
             * reconnect: it can suddenly turn the entire workspace upside down.
             * Ordinary uint32 wrap is continuous, not a new tracker stream. */
            if (a->have_pose_sequence && sequence == 0 && a->pose_sequence > 10 &&
                a->pose_sequence < UINT32_MAX - 10) {
                fprintf(stderr, "air presenter: tracker reference reset; re-enter after warmup\n");
                presenter_event("tracker_reference_reset", "pose sequence restarted");
                a->running = false;
                return;
            }
            a->pose_sequence = sequence;
            a->have_pose_sequence = true;
        }
        /* A datagram queued during a long GPU stall has no trustworthy sample
         * timestamp. Drain the backlog, then wait for a new live packet. */
        if(discard)continue;
        abasis(-values[3] * AIR_PI / 180, -values[4] * AIR_PI / 180, values[5] * AIR_PI / 180,
               a->raw);
        a->pose_stamp = now_ns();
        a->pose_valid = true;
        received = true;
    }
    /* Sample the newest pose once per receive pass. Packets drained together
     * have no individual arrival timestamps; treating their microsecond read
     * spacing as sensor time would amplify derivative noise. */
    if (received) update_basis(a);
}
static void receive_ipc(struct app *a) {
    struct air_packet m;
    char ancillary[CMSG_SPACE(sizeof(int) * 4)];
    struct iovec iov = {&m, sizeof(m)};
    struct msghdr hdr = {.msg_iov = &iov,
                         .msg_iovlen = 1,
                         .msg_control = ancillary,
                         .msg_controllen = sizeof(ancillary)};
    ssize_t n;
    while ((hdr.msg_controllen = sizeof(ancillary),
            n = recvmsg(a->ipc, &hdr, MSG_DONTWAIT | MSG_CMSG_CLOEXEC)) > 0) {
        int fd = -1;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&hdr); c; c = CMSG_NXTHDR(&hdr, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                int *fds = (int *)CMSG_DATA(c);
                int count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                for (int i = 0; i < count; i++) {
                    if (fd < 0)
                        fd = fds[i];
                    else
                        close(fds[i]);
                }
            }
        if (n != sizeof(m) || (hdr.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || m.magic != AIR_MAGIC ||
            m.version != AIR_VERSION || m.size != sizeof(m) || m.generation != a->generation) {
            if (fd >= 0)
                close(fd);
            a->running = false;
            return;
        }
        if (m.type == AIR_SCENE_BEGIN) {
            a->count = 0;
            a->committed = false;
            a->epoch = m.epoch;
            for (struct capture *c = a->captures; c; c = c->next)
                c->wanted = false;
        } else if (m.type == AIR_PANEL && !a->committed && m.epoch == a->epoch) {
            if (a->count == AIR_MAX_PANELS) {
                a->running = false;
            } else {
                m.data.panel.identifier[127] = 0;
                finish_utf8(m.data.panel.title, sizeof(m.data.panel.title));
                a->panels[a->count++] = m.data.panel;
            }
        } else if (m.type == AIR_SCENE_COMMIT && m.epoch == a->epoch)
            a->committed = true;
        else if (m.type == AIR_POINTER) {
            a->pointer = av(m.data.pointer.xyz[0], m.data.pointer.xyz[1], m.data.pointer.xyz[2]);
            a->hit = m.data.pointer.hit;
            a->active = m.data.pointer.active;
            a->widget_follow=m.data.pointer.widget_follow!=0;
            if(isfinite(m.data.pointer.widget_yaw))a->widget_yaw=m.data.pointer.widget_yaw;
            a->hover_id = m.data.pointer.view_id;
            a->hide_cursor = m.data.pointer.hide_cursor;
            a->hover_region = m.data.pointer.region;
        } else if (m.type == AIR_OVERVIEW) {
            a->overview_pending = true;
            a->overview_staged = m.data.overview.visible;
            a->overview_staged_revision = m.data.overview.revision;
            a->overview_selected = m.data.overview.selected;
            a->overview_start = m.data.overview.start;
            a->overview_count = m.data.overview.count;
            memcpy(a->overview_query, m.data.overview.query, sizeof(a->overview_query));
            finish_utf8(a->overview_query, sizeof(a->overview_query));
            memset(a->overview_rows, 0, sizeof(a->overview_rows));
            a->overview_dirty = true;
        } else if (m.type == AIR_OVERVIEW_ROW && m.data.row.index < AIR_OVERVIEW_ROWS) {
            unsigned i = m.data.row.index;
            memcpy(a->overview_rows[i].title, m.data.row.title, sizeof(a->overview_rows[i].title));
            memcpy(a->overview_rows[i].workspace, m.data.row.workspace, sizeof(a->overview_rows[i].workspace));
            finish_utf8(a->overview_rows[i].title, sizeof(a->overview_rows[i].title));
            finish_utf8(a->overview_rows[i].workspace, sizeof(a->overview_rows[i].workspace));
            a->overview_rows[i].visible = m.data.row.visible;
            a->overview_dirty = true;
        } else if (m.type == AIR_OVERVIEW_COMMIT && a->overview_pending) {
            a->overview_pending = false;
            a->overview = a->overview_staged;
            a->overview_revision = a->overview_staged_revision;
        } else if (m.type == AIR_STATUS) {
            m.data.text[191] = 0;
            fprintf(stderr, "air presenter: %s\n", m.data.text);
        } else if (m.type == AIR_ZOOM) {
            if (!isfinite(m.data.zoom.factor) || m.data.zoom.factor < 1 || m.data.zoom.factor > 3) {
                a->running = false;
                return;
            }
            a->zoom = m.data.zoom.factor;
        } else if (m.type == AIR_STEREO) {
            apply_stereo(a, &m);
        } else if (m.type == AIR_RECENTER) {
            a->zero_valid = false;
            if (a->pose_valid)
                update_basis(a);
        } else if (m.type == AIR_SHUTDOWN)
            a->running = false;
        else if (m.type == AIR_CURSOR) {
            uint32_t w = m.data.cursor.width, h = m.data.cursor.height;
            /* Hide/blank requests must not erase the spatial interaction point. */
            if (w && h && fd >= 0 && w <= 256 && h <= 256) {
                size_t bytes = (size_t)w * h * 4;
                void *map = mmap(NULL, bytes, PROT_READ, MAP_SHARED, fd, 0);
                if (map != MAP_FAILED) {
                    if (!a->egl_window)
                        egl_init(a);
                    if (!acursor_visible(map, w, h, w * 4,
                            m.data.cursor.format == WL_SHM_FORMAT_XRGB8888 ||
                            m.data.cursor.format == WL_SHM_FORMAT_XBGR8888)) {
                        munmap(map, bytes);
                        close(fd);
                        continue;
                    }
                    texture(&a->cursor, map, w, h, w * 4, m.data.cursor.format);
                    munmap(map, bytes);
                    a->cursor_w = w;
                    a->cursor_h = h;
                    a->cursor_x = m.data.cursor.hot_x;
                    a->cursor_y = m.data.cursor.hot_y;
                    a->cursor_scale = m.data.cursor.scale;
                }
            }
        }
        if (fd >= 0)
            close(fd);
    }
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
        a->running = false;
}
int main(int argc, char **argv) {
    const char *name = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--output") && i + 1 < argc)
            name = argv[++i];
        else
            fatal("usage: air-presenter --output NAME (requires inherited private "
                  "socket)");
    }
    const char *ipc = getenv("AIR_SPATIAL_FD"), *gen = getenv("AIR_SPATIAL_GENERATION");
    if (!name || !ipc || !gen)
        fatal("private socket and output are required");
    struct app a = {.ipc = atoi(ipc),
                    .generation = strtoull(gen, NULL, 10),
                    .running = true,
                    .cursor_scale = 1,
                    .stereo = getenv("AIR_SPATIAL_STEREO") && !strcmp(getenv("AIR_SPATIAL_STEREO"),"1"),
                    .ipd_mm = 64,
                    .ipd_baseline_mm = 64,
                    .width = 1920,
                    .height = 1080};
    a.zoom = 1;
    abasis(0, 0, 0, a.basis);
    a.pointer = av(0, 0, AIR_RADIUS);
    signal(SIGPIPE, SIG_IGN);
    a.display = wl_display_connect(NULL);
    if (!a.display)
        fatal("Wayland connection failed");
    a.registry = wl_display_get_registry(a.display);
    wl_registry_add_listener(a.registry, &registry_listener, &a);
    if (wl_display_roundtrip(a.display) < 0)
        fatal("Wayland registry failed");
    if (!a.compositor || !a.shm || !a.shell || !a.list || !a.sources || !a.copies ||
        !a.presentation)
        fatal("missing required Wayland capture, layer-shell or presentation "
              "protocol");
    ext_foreign_toplevel_list_v1_add_listener(a.list, &list_listener, &a);
    wp_presentation_add_listener(a.presentation, &presentation_listener, &a);
    if (wl_display_roundtrip(a.display) < 0)
        fatal("Wayland toplevel enumeration failed");
    for (struct output *o = a.outputs; o; o = o->next)
        if (!strcmp(o->name, name))
            a.output = o;
    if (!a.output)
        fatal("selected output unavailable");
    a.surface = wl_compositor_create_surface(a.compositor);
    a.layer = zwlr_layer_shell_v1_get_layer_surface(
        a.shell, a.surface, a.output->wl, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, AIR_NAMESPACE);
    zwlr_layer_surface_v1_add_listener(a.layer, &layer_listener, &a);
    zwlr_layer_surface_v1_set_size(a.layer, 0, 0);
    zwlr_layer_surface_v1_set_anchor(a.layer, 15);
    zwlr_layer_surface_v1_set_exclusive_zone(a.layer, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        a.layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    struct wl_region *empty = wl_compositor_create_region(a.compositor);
    wl_surface_set_input_region(a.surface, empty);
    wl_region_destroy(empty);
    wl_surface_commit(a.surface);
    if (wl_display_roundtrip(a.display) < 0)
        fatal("presentation surface configure failed");
    a.udp = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (a.udp < 0)
        fatal("UDP socket failed");
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port =
            htons(getenv("AIR_SPATIAL_UDP_PORT") ? atoi(getenv("AIR_SPATIAL_UDP_PORT")) : 4243),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (bind(a.udp, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        fatal("loopback pose port is busy");
    const char *source_port = getenv("AIR_SPATIAL_SOURCE_PORT");
    long authorized = source_port ? strtol(source_port, NULL, 10) : 0;
    if (authorized < 1 || authorized > 65535)
        fatal("authorized tracker source port is required");
    a.source_port = (uint16_t)authorized;
    struct sockaddr_in source = {.sin_family = AF_INET,
                                 .sin_port = htons(a.source_port),
                                 .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (connect(a.udp, (struct sockaddr *)&source, sizeof(source)) < 0)
        fatal("tracker source pin failed");
    fprintf(stderr, "air presenter: native spatial output %s, damage-aware packed SHM capture\n", name);
    presenter_event("ready", "Wayland and tracker sockets connected");
    while (a.running) {
        wl_display_dispatch_pending(a.display);
        receive_ipc(&a);
        receive_pose(&a);
        report_pose(&a);
        draw(&a);
        while (wl_display_prepare_read(a.display) != 0)
            if (wl_display_dispatch_pending(a.display) < 0)
                goto done;
        wl_display_flush(a.display);
        struct pollfd fds[3] = {
            {wl_display_get_fd(a.display), POLLIN, 0}, {a.ipc, POLLIN, 0}, {a.udp, POLLIN, 0}};
        int ret = poll(fds, 3, 16);
        if (ret < 0 && errno != EINTR) {
            wl_display_cancel_read(a.display);
            break;
        }
        if (fds[0].revents & POLLIN) {
            if (wl_display_read_events(a.display) < 0)
                break;
        } else
            wl_display_cancel_read(a.display);
        if (fds[0].revents & (POLLERR | POLLHUP))
            break;
        if (fds[1].revents & (POLLERR | POLLHUP))
            break;
    }
done:
    presenter_event("loop_exit", "presenter event loop ended");
    /* Destroy the connection and process: no captured pixels are persisted. */
    wl_display_disconnect(a.display);
    close(a.udp);
    close(a.ipc);
    return 0;
}
