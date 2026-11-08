/* Native mono presenter for the private labwc spatial extension. */
#define _GNU_SOURCE
#include "air-math.h"
#include "ext-foreign-toplevel-list-v1-protocol.h"
#include "ext-image-capture-source-v1-protocol.h"
#include "ext-image-copy-capture-v1-protocol.h"
#include "presentation-time-protocol.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
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
    uint32_t width, height, format, allocated_width, allocated_height, transform;
    bool format_ok, constraints, released, ready, dirty, closed, wanted;
    char identifier[128];
    GLuint texture;
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
    uint64_t epoch;
    bool healthy;
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
    struct air_panel panels[AIR_MAX_PANELS];
    int count;
    uint64_t epoch, generation;
    int ipc, udp;
    uint16_t source_port;
    bool running, configured, frame_due, committed, pose_valid, zero_valid, active;
    int width, height;
    uint64_t pose_stamp, last_frame, last_pose_report;
    double raw[9], zero[9], basis[9];
    struct air_vec pointer;
    bool hit;
    char status[192];
    bool hud_dirty;
    GLuint hud, cursor;
    int cursor_w, cursor_h, cursor_x, cursor_y;
    float cursor_scale;
    EGLDisplay egl;
    EGLContext context;
    EGLSurface egl_surface;
    struct wl_egl_window *egl_window;
    GLuint program;
    GLint attr_pos, attr_uv, uniform_basis, uniform_aspect, uniform_texture;
};
static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void fatal(const char *text) {
    fprintf(stderr, "air presenter: %s\n", text);
    exit(1);
}
static void packet(struct app *a, struct air_packet *m) {
    m->magic = AIR_MAGIC;
    m->version = AIR_VERSION;
    m->size = sizeof(*m);
    m->generation = a->generation;
    if (!m->stamp_ns)
        m->stamp_ns = now_ns();
    if (!m->epoch)
        m->epoch = a->epoch;
    if (send(a->ipc, m, sizeof(*m), MSG_DONTWAIT | MSG_NOSIGNAL) != sizeof(*m))
        a->running = false;
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
    const EGLint attrs[] = {EGL_SURFACE_TYPE,
                            EGL_WINDOW_BIT,
                            EGL_RENDERABLE_TYPE,
                            EGL_OPENGL_ES2_BIT,
                            EGL_RED_SIZE,
                            8,
                            EGL_GREEN_SIZE,
                            8,
                            EGL_BLUE_SIZE,
                            8,
                            EGL_ALPHA_SIZE,
                            8,
                            EGL_DEPTH_SIZE,
                            16,
                            EGL_NONE};
    EGLConfig config;
    EGLint count;
    if (!eglChooseConfig(a->egl, attrs, &config, 1, &count) || !count)
        fatal("no EGL window config");
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ctx[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    a->context = eglCreateContext(a->egl, config, EGL_NO_CONTEXT, ctx);
    a->egl_window = wl_egl_window_create(a->surface, a->width, a->height);
    a->egl_surface =
        eglCreateWindowSurface(a->egl, config, (EGLNativeWindowType)a->egl_window, NULL);
    if (!eglMakeCurrent(a->egl, a->egl_surface, a->egl_surface, a->context))
        fatal("EGL context creation failed");
    eglSwapInterval(a->egl, 0);
    const char *vert =
        "attribute vec3 pos;attribute vec2 uv;varying vec2 tex;uniform mat3 basis;uniform float "
        "aspect;void main(){vec3 q=basis*pos;float "
        "t=0.363970234;gl_Position=vec4(q.x/t,q.y*aspect/t,1.002002*q.z-0.02002002,q.z);tex=uv;}";
    const char *frag = "precision mediump float;varying vec2 tex;uniform sampler2D image;void "
                       "main(){vec4 c=texture2D(image,tex);if(c.a<0.01)discard;gl_FragColor=c;}";
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
    glUseProgram(a->program);
    glUniform1i(a->uniform_texture, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}
static void texture(GLuint *id, const void *pixels, uint32_t w, uint32_t h, uint32_t format) {
    if (!*id) {
        glGenTextures(1, id);
        glBindTexture(GL_TEXTURE_2D, *id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else
        glBindTexture(GL_TEXTURE_2D, *id);
    size_t bytes = (size_t)w * h * 4;
    unsigned char *rgba = malloc(bytes);
    if (!rgba)
        fatal("texture allocation failed");
    const unsigned char *src = pixels;
    bool bgra = format == WL_SHM_FORMAT_ARGB8888 || format == WL_SHM_FORMAT_XRGB8888;
    bool opaque = format == WL_SHM_FORMAT_XRGB8888 || format == WL_SHM_FORMAT_XBGR8888;
    for (size_t i = 0; i < bytes; i += 4) {
        rgba[i] = src[i + (bgra ? 2 : 0)];
        rgba[i + 1] = src[i + 1];
        rgba[i + 2] = src[i + (bgra ? 0 : 2)];
        rgba[i + 3] = opaque ? 255 : src[i + 3];
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    free(rgba);
}
static void request_capture(struct capture *c);
static void buffer_release(void *data, struct wl_buffer *buffer) {
    (void)buffer;
    struct capture *c = data;
    c->released = true;
    request_capture(c);
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
}
static void capture_transform(void *d, struct ext_image_copy_capture_frame_v1 *f, uint32_t t) {
    (void)f;
    ((struct capture *)d)->transform = t;
}
static void capture_damage(void *d, struct ext_image_copy_capture_frame_v1 *f, int32_t x, int32_t y,
                           int32_t w, int32_t h) {
    (void)d;
    (void)f;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
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
    c->ready = true;
    c->dirty = true; /* Upload before recycling SHM, so head turns reuse an unchanged texture. */
}
static void capture_failed(void *d, struct ext_image_copy_capture_frame_v1 *f, uint32_t reason) {
    struct capture *c = d;
    ext_image_copy_capture_frame_v1_destroy(f);
    c->frame = NULL;
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
    if (format == WL_SHM_FORMAT_ARGB8888 ||
        (!c->format_ok && (format == WL_SHM_FORMAT_XRGB8888 || format == WL_SHM_FORMAT_ABGR8888 ||
                           format == WL_SHM_FORMAT_XBGR8888))) {
        c->format = format;
        c->format_ok = true;
    }
}
static void session_device(void *d, struct ext_image_copy_capture_session_v1 *s,
                           struct wl_array *device) {
    (void)d;
    (void)s;
    (void)device;
}
static void session_dmabuf(void *d, struct ext_image_copy_capture_session_v1 *s, uint32_t f,
                           struct wl_array *mods) {
    (void)d;
    (void)s;
    (void)f;
    (void)mods;
}
static void session_done(void *d, struct ext_image_copy_capture_session_v1 *s) {
    (void)s;
    struct capture *c = d;
    c->constraints = true;
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
    if (!c->width || !c->height || c->width > 8192 || c->height > 8192)
        fatal("capture dimensions unsupported");
    if (!c->buffer || c->allocated_width != c->width || c->allocated_height != c->height) {
        free_buffer(c);
        c->ready = false;
        c->bytes = (size_t)c->width * c->height * 4;
        int fd = memfd_create("air-window-capture", MFD_CLOEXEC);
        if (fd < 0 || ftruncate(fd, c->bytes) < 0)
            fatal("SHM allocation failed");
        c->map = mmap(NULL, c->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (c->map == MAP_FAILED)
            fatal("SHM mapping failed");
        struct wl_shm_pool *pool = wl_shm_create_pool(c->app->shm, fd, c->bytes);
        c->buffer =
            wl_shm_pool_create_buffer(pool, 0, c->width, c->height, c->width * 4, c->format);
        wl_shm_pool_destroy(pool);
        close(fd);
        wl_buffer_add_listener(c->buffer, &buffer_listener, c);
        c->allocated_width = c->width;
        c->allocated_height = c->height;
    }
    c->frame = ext_image_copy_capture_session_v1_create_frame(c->session);
    ext_image_copy_capture_frame_v1_add_listener(c->frame, &capture_listener, c);
    ext_image_copy_capture_frame_v1_attach_buffer(c->frame, c->buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(c->frame, 0, 0, c->width, c->height);
    ext_image_copy_capture_frame_v1_capture(c->frame);
    c->released = false;
}
static void start_capture(struct capture *c) {
    if (c->session || c->closed)
        return;
    struct ext_image_capture_source_v1 *source =
        ext_foreign_toplevel_image_capture_source_manager_v1_create_source(c->app->sources,
                                                                           c->handle);
    c->session = ext_image_copy_capture_manager_v1_create_session(c->app->copies, source, 0);
    ext_image_capture_source_v1_destroy(source);
    ext_image_copy_capture_session_v1_add_listener(c->session, &session_listener, c);
    c->released = true;
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
    if (a->egl_window)
        wl_egl_window_resize(a->egl_window, a->width, a->height, 0, 0);
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
    if (p->healthy) {
        struct air_packet m = {.type = AIR_PRESENTED,
                               .epoch = p->epoch,
                               .stamp_ns = (((uint64_t)hi << 32) | lo) * 1000000000ULL + ns};
        packet(p->app, &m);
    }
    wp_presentation_feedback_destroy(f);
    free(p);
}
static void feedback_discarded(void *d, struct wp_presentation_feedback *f) {
    wp_presentation_feedback_destroy(f);
    free(d);
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
static void update_hud(struct app *a) {
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1600, 64);
    cairo_t *cr = cairo_create(surface);
    cairo_set_source_rgba(cr, 0, 0, 0, .85);
    cairo_paint(cr);
    cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 22);
    cairo_set_source_rgb(cr, .7, 1, .9);
    cairo_move_to(cr, 16, 40);
    cairo_show_text(cr, a->status);
    cairo_surface_flush(surface);
    texture(&a->hud, cairo_image_surface_get_data(surface), 1600, 64, WL_SHM_FORMAT_ARGB8888);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    a->hud_dirty = false;
}
static void draw(struct app *a) {
    if (!a->configured || !a->frame_due)
        return;
    if (!a->egl_window)
        egl_init(a);
    a->frame_due = false;
    glViewport(0, 0, a->width, a->height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(a->program);
    float matrix[9]; /* GL uniforms are column-major; basis vectors are rows. */
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            matrix[j * 3 + i] = a->basis[i * 3 + j];
    glUniformMatrix3fv(a->uniform_basis, 1, GL_FALSE, matrix);
    double aspect = (double)a->width / a->height;
    glUniform1f(a->uniform_aspect, aspect);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    bool healthy = a->committed && a->pose_valid && now_ns() - a->pose_stamp < 250000000;
    for (int i = 0; i < a->count; i++) {
        struct air_panel *p = &a->panels[i];
        struct capture *c = get_capture(a, p->identifier);
        if (!c) {
            healthy = false;
            continue;
        }
        c->wanted = true;
        start_capture(c);
        if (c->dirty) {
            texture(&c->texture, c->map, c->allocated_width, c->allocated_height, c->format);
            c->dirty = false;
            request_capture(c);
        }
        bool rotated = (c->transform % 2) != 0;
        uint32_t width = rotated ? c->allocated_height : c->allocated_width,
                 height = rotated ? c->allocated_width : c->allocated_height;
        if (!c->ready || !c->texture || width != (uint32_t)p->bw || height != (uint32_t)p->bh) {
            healthy = false;
            continue;
        }
        struct air_vec corners[4] = {apoint(p, p->bx, p->by), apoint(p, p->bx + p->bw, p->by),
                                     apoint(p, p->bx, p->by + p->bh),
                                     apoint(p, p->bx + p->bw, p->by + p->bh)};
        draw_quad(a, c->texture, corners, c->transform);
    }
    glDisable(GL_DEPTH_TEST);
    if (a->cursor && a->cursor_w > 0) {
        double scale = .0009 / fmax(1, a->cursor_scale);
        struct air_vec right = abaxis(a->basis, 0), up = abaxis(a->basis, 1);
        struct air_vec tl = aadd(
            a->pointer, aadd(ascale(right, -a->cursor_x * scale), ascale(up, a->cursor_y * scale)));
        struct air_vec corners[4] = {
            tl, aadd(tl, ascale(right, a->cursor_w * scale)),
            aadd(tl, ascale(up, -a->cursor_h * scale)),
            aadd(aadd(tl, ascale(right, a->cursor_w * scale)), ascale(up, -a->cursor_h * scale))};
        draw_quad(a, a->cursor, corners, 0);
    }
    if (a->hud_dirty)
        update_hud(a);
    struct air_vec f = abaxis(a->basis, 2), right = abaxis(a->basis, 0), up = abaxis(a->basis, 1);
    double t = tan(40 * AIR_PI / 360);
    struct air_vec center = aadd(f, ascale(up, -.92 * t / aspect));
    double hw = t * .95, hh = t / aspect * .035;
    struct air_vec hud[4] = {aadd(center, aadd(ascale(right, -hw), ascale(up, hh))),
                             aadd(center, aadd(ascale(right, hw), ascale(up, hh))),
                             aadd(center, aadd(ascale(right, -hw), ascale(up, -hh))),
                             aadd(center, aadd(ascale(right, hw), ascale(up, -hh)))};
    draw_quad(a, a->hud, hud, 0);
    struct frame_feedback *fb = calloc(1, sizeof(*fb));
    fb->app = a;
    fb->epoch = a->epoch;
    fb->healthy = healthy;
    fb->feedback = wp_presentation_feedback(a->presentation, a->surface);
    wp_presentation_feedback_add_listener(fb->feedback, &feedback_listener, fb);
    struct wl_callback *cb = wl_surface_frame(a->surface);
    wl_callback_add_listener(cb, &frame_listener, a);
    if (!eglSwapBuffers(a->egl, a->egl_surface)) {
        a->running = false;
        return;
    }
    if (!a->last_frame) {
        struct air_packet m = {.type = AIR_READY};
        m.data.ready.width = a->width;
        m.data.ready.height = a->height;
        packet(a, &m);
    }
    a->last_frame = now_ns();
    if (a->pose_valid) {
        struct air_packet m = {.type = AIR_POSE};
        memcpy(m.data.pose.basis, a->basis, sizeof(a->basis));
        m.data.pose.age = (double)(now_ns() - a->pose_stamp) / 1e9;
        packet(a, &m);
    }
}
static void update_basis(struct app *a) {
    if (!a->zero_valid) {
        memcpy(a->zero, a->raw, sizeof(a->zero));
        a->zero_valid = true;
    }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            a->basis[i * 3 + j] = adot(abaxis(a->raw, i), abaxis(a->zero, j));
}
static void receive_pose(struct app *a) {
    unsigned char buf[1024];
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
        abasis(-values[3] * AIR_PI / 180, -values[4] * AIR_PI / 180, values[5] * AIR_PI / 180,
               a->raw);
        a->pose_stamp = now_ns();
        a->pose_valid = true;
        update_basis(a);
    }
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
                a->panels[a->count++] = m.data.panel;
            }
        } else if (m.type == AIR_SCENE_COMMIT && m.epoch == a->epoch)
            a->committed = true;
        else if (m.type == AIR_POINTER) {
            a->pointer = av(m.data.pointer.xyz[0], m.data.pointer.xyz[1], m.data.pointer.xyz[2]);
            a->hit = m.data.pointer.hit;
            a->active = m.data.pointer.active;
        } else if (m.type == AIR_STATUS) {
            m.data.text[191] = 0;
            snprintf(a->status, sizeof(a->status), "%s", m.data.text);
            a->hud_dirty = true;
        } else if (m.type == AIR_RECENTER) {
            a->zero_valid = false;
            if (a->pose_valid)
                update_basis(a);
        } else if (m.type == AIR_SHUTDOWN)
            a->running = false;
        else if (m.type == AIR_CURSOR) {
            uint32_t w = m.data.cursor.width, h = m.data.cursor.height;
            if (!w || !h)
                a->cursor_w = 0;
            else if (fd >= 0 && w <= 256 && h <= 256) {
                size_t bytes = (size_t)w * h * 4;
                void *map = mmap(NULL, bytes, PROT_READ, MAP_SHARED, fd, 0);
                if (map != MAP_FAILED) {
                    if (!a->egl_window)
                        egl_init(a);
                    texture(&a->cursor, map, w, h, m.data.cursor.format);
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
            fatal("usage: air-presenter --output NAME (requires inherited private socket)");
    }
    const char *ipc = getenv("AIR_SPATIAL_FD"), *gen = getenv("AIR_SPATIAL_GENERATION");
    if (!name || !ipc || !gen)
        fatal("private socket and output are required");
    struct app a = {.ipc = atoi(ipc),
                    .generation = strtoull(gen, NULL, 10),
                    .running = true,
                    .cursor_scale = 1,
                    .hud_dirty = true,
                    .width = 1920,
                    .height = 1080};
    abasis(0, 0, 0, a.basis);
    snprintf(a.status, sizeof(a.status), "ARMING | waiting for compositor scene and tracking");
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
        fatal("missing required Wayland capture, layer-shell or presentation protocol");
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
    fprintf(stderr, "air presenter: native mono output %s, PNG-free SHM capture\n", name);
    while (a.running) {
        wl_display_dispatch_pending(a.display);
        receive_ipc(&a);
        receive_pose(&a);
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
    /* Destroy the connection and process: no captured pixels are persisted. */
    wl_display_disconnect(a.display);
    close(a.udp);
    close(a.ipc);
    return 0;
}
