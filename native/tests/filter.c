/* Invisible EGL test: no Wayland connection, window, input or sensor access. */
#define _GNU_SOURCE
#include "air-filter.h"
#include "air-math.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <assert.h>
#include <stdlib.h>

static GLuint compile(GLenum type, const char *source) {
    GLuint s=glCreateShader(type);glShaderSource(s,1,&source,NULL);glCompileShader(s);
    GLint ok;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){char log[2048];glGetShaderInfoLog(s,sizeof(log),NULL,log);fprintf(stderr,"%s\n",log);}
    assert(ok);return s;
}
int main(void) {
    assert(afilter_scale(1920,1080,16384)==2);
    assert(afilter_scale(1920,1080,3000)==1);
    assert(afilter_extension("GL_X GL_OES_texture_npot GL_Y","GL_OES_texture_npot"));
    assert(!afilter_extension("GL_OES_texture_npot_extra","GL_OES_texture_npot"));
    PFNEGLQUERYDEVICESEXTPROC devices=(void *)eglGetProcAddress("eglQueryDevicesEXT");
    PFNEGLGETPLATFORMDISPLAYEXTPROC platform=(void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay display=EGL_NO_DISPLAY;
    if(devices && platform){
        EGLDeviceEXT list[8];EGLint n=0;
        if(devices(8,list,&n))for(int i=0;i<n;i++){
            display=platform(EGL_PLATFORM_DEVICE_EXT,list[i],NULL);
            if(display!=EGL_NO_DISPLAY && eglInitialize(display,NULL,NULL))break;
            display=EGL_NO_DISPLAY;
        }
    }
    if(display==EGL_NO_DISPLAY && platform){
        display=platform(EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,NULL);
        if(display==EGL_NO_DISPLAY || !eglInitialize(display,NULL,NULL))return 77;
    }
    if(display==EGL_NO_DISPLAY)return 77;
    assert(eglBindAPI(EGL_OPENGL_ES_API));
    EGLConfig config;EGLint n=0,samples=0;
    int candidates[]={8,4,2,0};
    for(int i=0;i<4;i++){
        EGLint attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,
            EGL_SAMPLE_BUFFERS,candidates[i]?1:0,EGL_SAMPLES,candidates[i],EGL_NONE};
        if(eglChooseConfig(display,attrs,&config,1,&n)&&n)break;
    }
    assert(n);eglGetConfigAttrib(display,config,EGL_SAMPLES,&samples);
    EGLint ctx[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
    EGLContext context=eglCreateContext(display,config,EGL_NO_CONTEXT,ctx);assert(context!=EGL_NO_CONTEXT);
    EGLint dims[]={EGL_WIDTH,256,EGL_HEIGHT,256,EGL_NONE};
    EGLSurface surface=eglCreatePbufferSurface(display,config,dims);assert(surface!=EGL_NO_SURFACE);
    assert(eglMakeCurrent(display,surface,surface,context));
    afilter_init();assert(filter_npot);
    char vert[1024],frag[1024];afilter_shaders(vert,sizeof(vert),frag,sizeof(frag),true);
    GLuint program=glCreateProgram(),vs=compile(GL_VERTEX_SHADER,vert),fs=compile(GL_FRAGMENT_SHADER,frag);
    glAttachShader(program,vs);glAttachShader(program,fs);glLinkProgram(program);
    GLint ok;glGetProgramiv(program,GL_LINK_STATUS,&ok);assert(ok);glUseProgram(program);
    float identity[]={1,0,0,0,1,0,0,0,1};
    glUniformMatrix3fv(glGetUniformLocation(program,"basis"),1,GL_FALSE,identity);
    glUniform1f(glGetUniformLocation(program,"aspect"),1);
    glUniform1i(glGetUniformLocation(program,"image"),0);
    const float t=.363970234f;
    float pos[]={-t,t,1,t,t,1,-t,-t,1,t,-t,1},uv[]={0,0,1,0,0,1,1,1};
    GLint ap=glGetAttribLocation(program,"pos"),au=glGetAttribLocation(program,"uv");
    glEnableVertexAttribArray(ap);glEnableVertexAttribArray(au);
    glVertexAttribPointer(ap,3,GL_FLOAT,0,0,pos);glVertexAttribPointer(au,2,GL_FLOAT,0,0,uv);
    GLuint texture;glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    unsigned char pixels[64*64*4];
    for(int y=0;y<64;y++)for(int x=0;x<64;x++){
        int i=(y*64+x)*4;pixels[i]=pixels[i+1]=pixels[i+2]=((x+y)%2)*255;pixels[i+3]=255;
    }
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,64,64,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    afilter_texture(64,64);glViewport(0,0,4,4);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    unsigned char out[4*4*4];glReadPixels(0,0,4,4,GL_RGBA,GL_UNSIGNED_BYTE,out);
    for(int i=0;i<16;i++){assert(out[i*4]>=125&&out[i*4]<=130);assert(out[i*4+3]==255);}
    /* A resized NPOT capture must remain complete and keep solid colours. */
    unsigned char npot[73*47*4];
    for(int i=0;i<73*47;i++){npot[4*i]=20;npot[4*i+1]=80;npot[4*i+2]=200;npot[4*i+3]=255;}
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,73,47,0,GL_RGBA,GL_UNSIGNED_BYTE,npot);
    afilter_texture(73,47);glDrawArrays(GL_TRIANGLE_STRIP,0,4);glReadPixels(0,0,4,4,GL_RGBA,GL_UNSIGNED_BYTE,out);
    for(int i=0;i<16;i++)for(int c=0;c<4;c++)assert(abs((int)out[4*i+c]-npot[c])<=1);
    /* Multisampling must resolve fractional coverage on a diagonal edge. */
    unsigned char white[]={255,255,255,255};
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,white);
    afilter_texture(1,1);glViewport(0,0,16,16);glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES,0,3);
    unsigned char edge[16*16*4];glReadPixels(0,0,16,16,GL_RGBA,GL_UNSIGNED_BYTE,edge);
    int fractional=0;
    for(int i=0;i<16*16;i++)if(edge[i*4]>0&&edge[i*4]<255)fractional++;
    if(samples>1)assert(fractional>0);
    /* Render through the real eye-offset shader and measure disparity, without
     * an HMD/Wayland window. Near content must have larger binocular disparity. */
    GLint eye_uniform=glGetUniformLocation(program,"eye_x");assert(eye_uniform>=0);
    double disparities[4][2];
    double ipds[4]={45,64,80,63.5};
    double centers_mm[4]={0,0,0,-.25};
    for(int setting=0;setting<4;setting++)
    for(int distance=0;distance<2;distance++) {
        double centers[2];float z=distance?3.f:.6f;
        for(int eye=0;eye<2;eye++) {
            for(int v=0;v<4;v++){pos[v*3]=(v%2?1:-1)*.03f;pos[v*3+1]=(v<2?1:-1)*.03f;pos[v*3+2]=z;}
            glUniform1f(eye_uniform,aeye_offset_center(eye,ipds[setting],centers_mm[setting]));
            glViewport(0,0,256,256);glClear(GL_COLOR_BUFFER_BIT);glDrawArrays(GL_TRIANGLE_STRIP,0,4);
            unsigned char stereo_pixels[256*256*4];glReadPixels(0,0,256,256,GL_RGBA,GL_UNSIGNED_BYTE,stereo_pixels);
            double sum=0,weighted=0;
            for(int y=0;y<256;y++)for(int x=0;x<256;x++){
                unsigned value=stereo_pixels[(y*256+x)*4];sum+=value;weighted+=(x+.5)*value;
            }
            assert(sum>0);centers[eye]=weighted/sum;
        }
        assert(centers[0]>centers[1]);disparities[setting][distance]=centers[0]-centers[1];
        double scale=256/(2*t*z);
        assert(fabs(disparities[setting][distance]-ipds[setting]/1000*scale)<.6);
        assert(fabs((centers[0]+centers[1])/2-(128-centers_mm[setting]/1000*scale))<.3);
    }
    for(int setting=0;setting<3;setting++)
        assert(disparities[setting][0]>disparities[setting][1]*3);
    for(int distance=0;distance<2;distance++) {
        assert(disparities[0][distance]<disparities[1][distance]);
        assert(disparities[1][distance]<disparities[2][distance]);
    }
    glUniform1f(eye_uniform,0);
    /* Actual shader magnification in mono and stereo, including snap-back. */
    GLint zoom_uniform=glGetUniformLocation(program,"zoom");assert(zoom_uniform>=0);
    for(int eye=-1;eye<2;eye++) {
        double areas[3],centers[3];
        double offset=eye<0?0:aeye_offset_center(eye,64,0);
        glUniform1f(eye_uniform,offset);
        for(int step=0;step<3;step++) {
            double zoom=step==1?2:1;
            glUniform1f(zoom_uniform,zoom);
            for(int v=0;v<4;v++) {
                pos[v*3]=(v%2?1:-1)*.04f;
                pos[v*3+1]=(v<2?1:-1)*.04f;pos[v*3+2]=1;
            }
            glClear(GL_COLOR_BUFFER_BIT);glDrawArrays(GL_TRIANGLE_STRIP,0,4);
            unsigned char pixels[256*256*4];glReadPixels(0,0,256,256,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
            double sum=0,weighted=0;
            for(int y=0;y<256;y++)for(int x=0;x<256;x++) {
                unsigned value=pixels[(y*256+x)*4];sum+=value;weighted+=(x+.5)*value;
            }
            assert(sum>0);areas[step]=sum;centers[step]=weighted/sum;
            assert(fabs(centers[step]-(128-offset*zoom*256/(2*t)))<.4);
        }
        assert(fabs(areas[1]/areas[0]-4)<.2);
        assert(fabs(areas[2]/areas[0]-1)<.001);
        assert(fabs(centers[2]-centers[0])<.001);
    }
    glUniform1f(zoom_uniform,1);
    glUniform1f(eye_uniform,0);
    assert(glGetError()==GL_NO_ERROR);
    printf("PASS: invisible %s rendering, %d MSAA samples, %.0fx anisotropy; checker/NPOT/edge coverage and stereo disparity pass\n",glGetString(GL_RENDERER),samples,filter_anisotropy);
    eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    eglDestroySurface(display,surface);eglDestroyContext(display,context);eglTerminate(display);
}
