/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AIR_FILTER_H
#define AIR_FILTER_H
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static bool filter_npot;
static GLfloat filter_anisotropy = 1;
static inline bool afilter_extension(const char *all, const char *name) {
    if (!all) return false;
    size_t n = strlen(name);
    const char *p = all;
    while ((p = strstr(p, name))) {
        if ((p == all || p[-1] == ' ') && (!p[n] || p[n] == ' ')) return true;
        p += n;
    }
    return false;
}
static inline int afilter_scale(int width, int height, int limit) {
    return width > 0 && height > 0 && width <= limit / 2 && height <= limit / 2 ? 2 : 1;
}
static inline void afilter_init(void) {
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    const char *version = (const char *)glGetString(GL_VERSION);
    int major = 0;
    if (version) sscanf(version, "OpenGL ES %d", &major);
    filter_npot = major >= 3 || afilter_extension(extensions, "GL_OES_texture_npot");
    if (afilter_extension(extensions, "GL_EXT_texture_filter_anisotropic"))
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &filter_anisotropy);
}
static inline void afilter_texture(unsigned width, unsigned height) {
    bool mipmaps = filter_npot || (width && height && !(width & (width - 1)) && !(height & (height - 1)));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    if (filter_anisotropy > 1)
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, filter_anisotropy);
    if (mipmaps)
        glGenerateMipmap(GL_TEXTURE_2D);
}
/* Shared by the presenter and the invisible EGL rendering regression. */
static inline void afilter_shaders(char *vert, size_t vert_size, char *frag, size_t frag_size,
                                  bool high_precision) {
    const char *qualifier = high_precision ? "highp" : "mediump";
    snprintf(vert, vert_size,
        "attribute vec3 pos;attribute vec2 uv;varying %s vec2 tex;"
        "uniform mat3 basis;uniform float aspect;uniform float eye_x;uniform float zoom;void main(){vec3 q=basis*pos; q.x-=eye_x;"
        "float t=0.363970234/max(1.0,zoom);gl_Position=vec4(q.x/t,q.y*aspect/t,"
        "1.002002*q.z-0.02002002,q.z);tex=uv;}", qualifier);
    snprintf(frag, frag_size,
        "precision %s float;varying %s vec2 tex;uniform sampler2D image;"
        "void main(){vec4 c=texture2D(image,tex);if(c.a<0.01)discard;gl_FragColor=c;}",
        qualifier, qualifier);
}
#endif
