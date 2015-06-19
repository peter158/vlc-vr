
#include <iostream>
#include <cstdio>

#include <unistd.h> // getopt

#include <GL/glew.h>
#include <GL/glx.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <SDL2/SDL_mutex.h>

#define OVR_ENABLED 1
//#define USE_RV16 1
#define MEMCPY_PIXEL_LINES

#ifdef WIN32
# define OVR_OS_WIN32
#else
# define OVR_OS_LINUX
#endif
#include <OVR_CAPI.h>
#include <OVR_CAPI_GL.h>

#include <vlc/vlc.h>

#include "shaders/cylinder_distort_frag.glsl.h"
#include "shaders/cylinder_distort_vert.glsl.h"
#include "shaders/dome_distort_frag.glsl.h"
#include "shaders/dome_distort_vert.glsl.h"
#include "shaders/passthrough_frag.glsl.h"
#include "shaders/passthrough_vert.glsl.h"
#include "shaders/fxaa_frag.glsl.h"
#include "shaders/fxaa_vert.glsl.h"

using namespace std;

bool quit;

// SDL 
SDL_Window *sdlWindow;
SDL_GLContext glContext;

// Oculus
bool hmd_is_debug;
ovrHmd hmd;
ovrSizei eyeres[2];
ovrEyeRenderDesc eye_rdesc[2];
ovrGLTexture fb_ovr_tex[2];
union ovrGLConfig glcfg;
unsigned int distort_caps;
unsigned int hmd_caps;

unsigned int frame_index;
ovrPosef eyePose[2];
ovrTrackingState trackingState;

// jdt: reverse projection for "look-at" GUI selection.
//bool lookAtValid;
//TVector3 lookAtPrevPos[2];
//TVector3 lookAtPos[2];
//GLfloat lookAtDepth[2];

// VLC
libvlc_instance_t*       vlc;
libvlc_media_player_t*   vlc_media_player;
libvlc_media_t*          vlc_media;
libvlc_event_manager_t*  vlc_event_manager;

// OpenGL
unsigned int fbo, fb_tex[2], fb_depth;
unsigned int fb_width, fb_height;
int fb_tex_width, fb_tex_height;
//unsigned int stereo_gl_list;
GLuint fxaa_prog;
GLuint dome_distort_prog;
GLuint cylinder_distort_prog;
GLuint passthrough_prog;

typedef enum {
    STEREO_NONE,
    STEREO_SBS,
    STEREO_OVER_UNDER,
    MAX_STEREO_MODE
} stereo_mode_t;

typedef enum {
    DISTORTION_NONE, // planar
    DISTORTION_DOME,
    DISTORTION_CYLINDER,
    MAX_DISTORTION
} distortion_t;

struct _param {
    bool    console_dump;
    bool    fullscreen;
    bool    use_fxaa;
    float   fov;
    bool    no_prediction;
    bool    no_vsync;
    bool    no_timewarp;
    bool    no_hq_distortion;
    bool    no_restore;
    bool    no_timewarp_spinwaits;
    bool    no_compute_shader;
    bool    no_tracking;
    stereo_mode_t stereo_mode; // true if sbs else under/over
    float   ipd_multiplier;
    float   tv_size;
    float   tv_zoffset;
    float   mesh_radius;
    distortion_t distortion;
    bool    view_locked;
} param;

typedef enum {
    ASPECT_AUTO,
    ASPECT_4_BY_3,
    ASPECT_16_BY_9,
    MAX_ASPECT_MODE
} aspect_ratio_mode_t;

struct _video {
    Uint32 width;
    Uint32 height;
    Uint32 pitch;
    bool updateFrame;
    SDL_mutex *sdlMutex;
    SDL_Surface *sdlSurface;
    GLuint glTexture[2];
    Uint8 *glVideo[2];
    GLuint glVideoWidth;
    GLuint glVideoHeight;
    GLuint glVideoPitch;
    unsigned int bpp;
    float aspect_ratio; // auto-detected aspect ratio.
    aspect_ratio_mode_t aspect_ratio_mode;
} video;

void setDefaults() {
    param.console_dump = true;
    param.use_fxaa = true;
    param.fov = 90;
    param.no_prediction = false;
    param.no_vsync = false;
    param.no_timewarp = false;
    param.no_hq_distortion = false;
    param.no_restore = false; // TODO
    param.no_timewarp_spinwaits = false;
    param.no_compute_shader = false;
    param.no_tracking = false;
    param.ipd_multiplier = 1.0;
    param.tv_size = 1;
    param.tv_zoffset = -1;
    param.mesh_radius = param.tv_size / 2;

    switch(param.distortion) {
    case DISTORTION_DOME:
        param.tv_size = 1.8;
        param.tv_zoffset = -3.5;
        param.ipd_multiplier = 4.0;
        param.mesh_radius = param.tv_size * sqrt(2);
        break;

    case DISTORTION_CYLINDER:
        param.tv_size = 4.6;
        param.tv_zoffset = -1.2;
        break;
    }

#ifdef USE_RV16
    video.bpp = 16;
#else
    video.bpp = 32;
#endif
    video.width = 0;
    video.height = 0;
    video.updateFrame = false;
    video.sdlMutex = 0;
    video.sdlSurface = 0;
    video.glTexture[0] = 0;
    video.glTexture[1] = 0;
    video.glVideo[0] = 0;
    video.glVideo[1] = 0;
    video.glVideoWidth = 0;
    video.glVideoHeight = 0;
    video.glVideoPitch = 0;
    video.aspect_ratio = 0;
    video.aspect_ratio_mode = ASPECT_AUTO;
}

#define NEAR_CLIP_DIST 0.1

// Ripped from:
// Author: John Tsiombikas <nuclear@member.fsf.org>
// LICENSE: This code is in the public domain. Do whatever you like with it.
// DOC: 1 << (log(x-1, 2) + 1) next higher power of two for ogl texture sizing.
unsigned int next_pow2(unsigned int x)
{
    x -= 1;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    return x + 1;
}

// Ripped from:
// Author: John Tsiombikas <nuclear@member.fsf.org>
// LICENSE: This code is in the public domain. Do whatever you like with it.
// DOC: convert a quaternion to a rotation matrix
void quat_to_matrix(const float *quat, float *mat)
{
    mat[0] = 1.0 - 2.0 * quat[1] * quat[1] - 2.0 * quat[2] * quat[2];
    mat[4] = 2.0 * quat[0] * quat[1] + 2.0 * quat[3] * quat[2];
    mat[8] = 2.0 * quat[2] * quat[0] - 2.0 * quat[3] * quat[1];
    mat[12] = 0.0f;

    mat[1] = 2.0 * quat[0] * quat[1] - 2.0 * quat[3] * quat[2];
    mat[5] = 1.0 - 2.0 * quat[0]*quat[0] - 2.0 * quat[2]*quat[2];
    mat[9] = 2.0 * quat[1] * quat[2] + 2.0 * quat[3] * quat[0];
    mat[13] = 0.0f;

    mat[2] = 2.0 * quat[2] * quat[0] + 2.0 * quat[3] * quat[1];
    mat[6] = 2.0 * quat[1] * quat[2] - 2.0 * quat[3] * quat[0];
    mat[10] = 1.0 - 2.0 * quat[0]*quat[0] - 2.0 * quat[1]*quat[1];
    mat[14] = 0.0f;

    mat[3] = mat[7] = mat[11] = 0.0f;
    mat[15] = 1.0f;
}


void UpdateVideoTarget(unsigned int width, unsigned int height)
{
    glGenTextures(1, video.glTexture);

    if (!video.glVideo[0] || width != video.width || height != video.height) {
        delete video.glVideo[0];
        video.glVideoWidth = next_pow2(width);
        video.glVideoHeight = next_pow2(height);
        video.glVideo[0] = new Uint8[video.glVideoWidth * video.glVideoHeight * 4];
        memset(video.glVideo[0], 0, sizeof(video.glVideo[0]));
        video.glVideoPitch = video.glVideoWidth * 4;
        video.width = width;
        video.height = height;

        cerr << "Changed video res to: " << width << "x" << height << endl;
        cerr << "changed glVideo res to: " << video.glVideoWidth << "x" << video.glVideoHeight << endl;
    }

    // sdl target
    {
        Uint32 rmask, gmask, bmask, amask;

        // SDL interprets each pixel as a 32-bit number, so our masks must depend
        // on the endianness (byte order) of the machine.
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
        rmask = 0xff000000;
        gmask = 0x00ff0000;
        bmask = 0x0000ff00;
        amask = 0x000000ff;
#else
        rmask = 0x000000ff;
        gmask = 0x0000ff00;
        bmask = 0x00ff0000;
        amask = 0xff000000;
#endif

#ifdef USE_RV16
        video.sdlSurface = SDL_CreateRGBSurface(SDL_SWSURFACE, video.width, video.height, 16,
                0xf800, 0x07e0, 0x001f, 0); // 5, 6, 5
#else
        video.sdlSurface = SDL_CreateRGBSurface(SDL_SWSURFACE, video.width, video.height, 32, rmask, gmask, bmask, amask);
#endif
        video.sdlMutex = SDL_CreateMutex();
    }
}

void UpdateRenderTarget(unsigned int width, unsigned int height)
{
    // save to globals for the heck of it.  
    fb_width = width;
    fb_height = height;

    if(!fbo) {
        // if fbo does not exist, then nothing does. create opengl objects
        glGenFramebuffers(1, &fbo);
        glGenTextures(2, fb_tex);
        glGenRenderbuffers(1, &fb_depth);

        glBindTexture(GL_TEXTURE_2D, fb_tex[1]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        glBindTexture(GL_TEXTURE_2D, fb_tex[0]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    // calculate the next power of two in both dimensions and use that as a texture size 
    fb_tex_width = next_pow2(width);
    fb_tex_height = next_pow2(height);

    // create and attach the texture that will be used as a color buffer
    glBindTexture(GL_TEXTURE_2D, fb_tex[1]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fb_tex_width, fb_tex_height, 0,
            GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glBindTexture(GL_TEXTURE_2D, fb_tex[0]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fb_tex_width, fb_tex_height, 0,
            GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb_tex[0], 0);

    // create and attach the renderbuffer that will serve as our z-buffer
    glBindRenderbuffer(GL_RENDERBUFFER, fb_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, fb_tex_width, fb_tex_height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, fb_depth);

    if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "Failed to create Complete Framebuffer!\n");
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    printf("created render target: %dx%d (texture size: %dx%d)\n", width, height, fb_tex_width, fb_tex_height);

    // jdt: cache geometry for second eye.
    //if (glIsList(stereo_gl_list)) {
    //    glDeleteLists(stereo_gl_list, 1);
    //}
    //stereo_gl_list = glGenLists(1);
}


void SetupDisplay (ovrEyeType eye, bool skybox) {
    // we'll just have to use the projection matrix supplied by the oculus SDK for this eye
    // note that libovr matrices are the transpose of what OpenGL expects, so we have
    // to use glLoadTransposeMatrixf instead of glLoadMatrixf to load it.
    //
    //double far_clip = currentMode == GUI ? 2000.0f : param.forward_clip_distance + FAR_CLIP_FUDGE_AMOUNT;
    double far_clip = 2000.0f; // jdt: trying to lessen the difference between GUI and 3D modes.
    // jdt: increase near_clip to get better depth buffer resolution if we turn that on.
    ovrMatrix4f proj = ovrMatrix4f_Projection(hmd->DefaultEyeFov[eye], NEAR_CLIP_DIST, far_clip, 1);
    glMatrixMode(GL_PROJECTION);
    glLoadTransposeMatrixf(proj.M[0]);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    if (!param.view_locked)
    {
        // retrieve the orientation quaternion and convert it to a rotation matrix 
        float rot_mat[16];
        quat_to_matrix(&eyePose[eye].Orientation.x, rot_mat);

        // jdt: trick to render skybox without stereo.. up close to avoid fog.
        /*
           if (skybox) {
           glMultMatrixf(rot_mat);

           CControl *ctrl = Players.GetCtrl (g_game.player_id);
           if (ctrl && !State::manager.isGuiState()) {
           TMatrix view_mat;
           TransposeMatrix (ctrl->env_view_mat, view_mat);
           view_mat[0][3] = 0;  // remove translation
           view_mat[1][3] = 0;
           view_mat[2][3] = 0;
           view_mat[3][0] = 0;
           view_mat[3][1] = 0;
           view_mat[3][2] = 0;
           glMultMatrixd ((double*)view_mat);
           }

           Env.DrawSkybox (TVector3(0,0,0), true);
           glLoadIdentity();
           }
         */

        glTranslatef(eye_rdesc[eye].HmdToEyeViewOffset.x * param.ipd_multiplier,
                eye_rdesc[eye].HmdToEyeViewOffset.y * param.ipd_multiplier,
                eye_rdesc[eye].HmdToEyeViewOffset.z * param.ipd_multiplier);

        glMultMatrixf(rot_mat);

        // translate the view matrix with the positional tracking
        glTranslatef(-eyePose[eye].Position.x * param.ipd_multiplier,
                -eyePose[eye].Position.y * param.ipd_multiplier,
                -eyePose[eye].Position.z * param.ipd_multiplier);
        // move the camera to the eye level of the user
        //glTranslate(0, -ovrHmd_GetFloat(hmd, OVR_KEY_EYE_HEIGHT, 1.65), 0);
    }

    glColor4f (1.0, 1.0, 1.0, 1.0);
}

void ClearDisplay () {
    glDepthMask (GL_TRUE);
    glClearColor (0, 0, 0, 0);
    glClearStencil (0);
    glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}


void InitOpenglExtensions () {
    if (!GLEW_EXT_framebuffer_object) {
        cout << "Oculus Rift support currently requires high-end cards w/ frame buffer object support." << endl;
        cout << "This system configuration has been determined not to have this.  Aborting" << endl;
        // jdt TODO: Don't quit if oculus isn't the configured video mode.  give helpful hint otherwise.
        SDL_Quit();
    }

    if (!GLEW_EXT_compiled_vertex_array) {
        cout << "Failed to find OpenGL extension support for GL_EXT_compiled_vertex_array" << endl;
        SDL_Quit();
    }
}

#define MAX_SHADER_SIZE (1 << 15)
#if 0 // switching to static shader includes
int load_shader(GLenum type, const char* filename)
{
    GLuint shader;
    GLint chars_read;
    SDL_RWops* file;
    GLint compiled;

    file=SDL_RWFromFile(filename, "r");

    if (file==NULL)
    {
        printf("shader %s wasn't opened\n", filename);
        exit(1);
    }

    shader=glCreateShader(type);

    char* shader_source = new char[MAX_SHADER_SIZE];
    chars_read = (GLint)SDL_RWread(file, shader_source, 1, MAX_SHADER_SIZE-1);

    //const GLchar* sources[] = {shader_source, 0};
    glShaderSource(shader, 1, (const GLchar**)&shader_source, &chars_read);

    glCompileShader(shader);

    delete[] shader_source;

    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled==GL_FALSE) {
        printf("shader %s failed to compile\n", filename);

        GLsizei info_len;
        glGetShaderInfoLog(shader, MAX_SHADER_SIZE, &info_len, shader_source);
        cout << shader_source << std::endl;
        exit(1);
    }

    return shader;
}

void init_shader_program(GLuint* program, const char* vertfile, const char* fragfile)
{
    GLint linked;

    *program=glCreateProgram();

    if (vertfile) glAttachShader(*program, load_shader(GL_VERTEX_SHADER, vertfile));
    if (fragfile) glAttachShader(*program, load_shader(GL_FRAGMENT_SHADER, fragfile));

    glLinkProgram(*program);

    glGetProgramiv(*program, GL_LINK_STATUS, &linked);
    if (linked==GL_FALSE)
    {
        printf("shader failed to link\n");
        exit(1);
    }
}
#endif

int load_shader(GLenum type, const GLchar** shader_text)
{
    GLuint shader;
    GLint compiled;

    shader=glCreateShader(type);

    GLsizei length = strlen(*shader_text);
    cout << "Compiling shader with " << length << " chars." << endl;
    glShaderSource(shader, 1, shader_text, NULL);

    glCompileShader(shader);

    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled==GL_FALSE) {
        GLsizei info_len;
        GLchar shader_err[MAX_SHADER_SIZE];
        glGetShaderInfoLog(shader, MAX_SHADER_SIZE, &info_len, shader_err);
        cout << "Shader failed to compiled: " << shader_err << std::endl;
        exit(1);
    }

    return shader;
}

void init_shader_program(GLuint* program, const GLchar** vertshader, const GLchar** fragshader)
{
    GLint linked;

    *program=glCreateProgram();

    if (vertshader) glAttachShader(*program, load_shader(GL_VERTEX_SHADER, vertshader));
    if (fragshader) glAttachShader(*program, load_shader(GL_FRAGMENT_SHADER, fragshader));

    glLinkProgram(*program);

    glGetProgramiv(*program, GL_LINK_STATUS, &linked);
    if (linked==GL_FALSE) {
        printf("shader failed to link\n");
        exit(1);
    }
}

// Load a texture
void LoadVideoTexture() {
    SDL_LockMutex(video.sdlMutex);

    glBindTexture(GL_TEXTURE_2D, video.glTexture[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, video.glVideoWidth, video.glVideoHeight, 0,
            GL_BGRA, GL_UNSIGNED_BYTE, video.glVideo[0]);
    video.updateFrame = false;

    SDL_UnlockMutex(video.sdlMutex);
}

// VLC Callback Functions
void* lock(void *data, void **p_pixels) 
{
    SDL_LockSurface(video.sdlSurface);
    *p_pixels = video.sdlSurface->pixels;
    return NULL;
}

void unlock(void *data, void *id, void *const *p_pixels)
{
    Uint8 * pixelSource;
    Uint8 * pixelDestination;
#ifdef USE_RV16
    Uint16 pix;
#else
    Uint32 pix;
#endif

    SDL_LockMutex(video.sdlMutex);

    Uint8 pixelDepth = video.sdlSurface->format->BytesPerPixel;

    // TODO: openmp
    for (unsigned int i = video.height; i > 0; i--) {
        pixelDestination = video.glVideo[0] + (video.height-i) * video.glVideoPitch;
        pixelSource = (Uint8*)video.sdlSurface->pixels + (i-1) * video.sdlSurface->pitch;
#ifdef MEMCPY_PIXEL_LINES
        // requires same pixelDepth for both sdlsurface and opengl
        memcpy(pixelDestination, pixelSource, video.sdlSurface->pitch);
        pixelDestination += video.glVideoPitch;
#else
        for (unsigned int j = 0; j < video.width; j++) {
            pixelSource += j*pixelDepth;
#ifdef USE_RV16
            pix = *(Uint16 *) pixelSource;
#else
            pix = *(Uint32 *) pixelSource;
#endif
            SDL_GetRGBA(pix, video.sdlSurface->format,
                    &(pixelDestination[0]),
                    &(pixelDestination[1]),
                    &(pixelDestination[2]),
                    &(pixelDestination[3]));
            pixelDestination += 4;
        }
#endif
    }

    SDL_UnlockSurface(video.sdlSurface);
    SDL_UnlockMutex(video.sdlMutex);
}

void display(void *data, void *id) 
{
    video.updateFrame = true;
}

void ToggleHmdFullscreen()
{
    static int fullscr, prev_x, prev_y;

    // Using F9 in direct-to-rift mode causes hard-to-debug performance issues.
    if(hmd->HmdCaps & ovrHmdCap_ExtendDesktop == 0) {
        printf("Not switching to fullscreen in Direct-to-rift mode\n");
        return;
    }

    fullscr = !fullscr;

    if(fullscr) {
        //
        // going fullscreen on the rift. save current window position, and move it
        // to the rift's part of the desktop before going fullscreen
        //
        SDL_GetWindowPosition(sdlWindow, &prev_x, &prev_y);
        printf("Going fullscreen to Rift:\n");
        printf("\tprev window position: %d,%d\n", prev_x, prev_y);
        printf("\thmd window position: %d,%d\n", hmd->WindowsPos.x, hmd->WindowsPos.y);
        SDL_SetWindowPosition(sdlWindow, hmd->WindowsPos.x, hmd->WindowsPos.y);
        SDL_SetWindowFullscreen(sdlWindow, SDL_WINDOW_FULLSCREEN_DESKTOP);

#ifdef OVR_OS_LINUX
        // on linux for now we have to deal with screen rotation during rendering. The docs are promoting
        // not rotating the DK2 screen globally
        //
        glcfg.OGL.Header.BackBufferSize.w = hmd->Resolution.h; // >= 0.4.4
        glcfg.OGL.Header.BackBufferSize.h = hmd->Resolution.w;
        printf("\tSwapping window resolution to: %dx%d\n", hmd->Resolution.h, hmd->Resolution.w);

        distort_caps |= ovrDistortionCap_LinuxDevFullscreen;
        ovrHmd_ConfigureRendering(hmd, &glcfg.Config, distort_caps, hmd->DefaultEyeFov, eye_rdesc);
#endif
    } else {
        // return to windowed mode and move the window back to its original position
        SDL_SetWindowFullscreen(sdlWindow, 0);
        SDL_SetWindowPosition(sdlWindow, prev_x, prev_y);

#ifdef OVR_OS_LINUX
        glcfg.OGL.Header.BackBufferSize = hmd->Resolution;

        distort_caps &= ~ovrDistortionCap_LinuxDevFullscreen;
        ovrHmd_ConfigureRendering(hmd, &glcfg.Config, distort_caps, hmd->DefaultEyeFov, eye_rdesc);
#endif
    }
}

void OvrConfigureTracking()
{
    if (!param.no_tracking) {
        // enable position and rotation tracking
        ovrHmd_ConfigureTracking(hmd, ovrTrackingCap_Orientation | ovrTrackingCap_MagYawCorrection | ovrTrackingCap_Position, 0);
    }
}

void OvrFindResolution()
{
    // retrieve the optimal render target resolution for each eye
    // jdt: I tried reducing pixelsPerDisplayPixel from 1.0 for performance but 
    // we seem to be CPU bound on the quadtree and actual trees.. setting back to 1
    eyeres[0] = ovrHmd_GetFovTextureSize(hmd, ovrEye_Left, hmd->DefaultEyeFov[0], 1.0);
    eyeres[1] = ovrHmd_GetFovTextureSize(hmd, ovrEye_Right, hmd->DefaultEyeFov[1], 1.0);

    // Set etr fov to that of the Oculus Rift
    float fovTan0 = max(hmd->DefaultEyeFov[0].LeftTan, hmd->DefaultEyeFov[0].RightTan);
    float fovTan1 = max(hmd->DefaultEyeFov[1].LeftTan, hmd->DefaultEyeFov[1].RightTan);
    // TODO: ... used?
    //param.fov = floor(std::abs(2.0f * atan(max(fovTan0, fovTan1)) * (180.0f / M_PI)));
    //jdt TODO param.fov -= 10; // account for overlap in fov of both eyes.  fudge..
    // I'm seeing 95 degrees here.. doesn't look good to me.
    printf("TODO!!!! Detected HMD FOV of %f.  Overriding etr fov with it.\n", param.fov);

    // and create a single render target texture to encompass both eyes 
    // jdt: fb_width, etc declared in ogl.h
    fb_width = eyeres[0].w + eyeres[1].w;
    fb_height = eyeres[0].h > eyeres[1].h ? eyeres[0].h : eyeres[1].h;
}

void OvrConfigureRendering()
{
    // fill in the ovrGLTexture structures that describe our render target texture
    for(int i=0; i<2; i++) {
        fb_ovr_tex[i].OGL.Header.API = ovrRenderAPI_OpenGL;
        fb_ovr_tex[i].OGL.Header.TextureSize.w = fb_tex_width;
        fb_ovr_tex[i].OGL.Header.TextureSize.h = fb_tex_height;
        // this next field is the only one that differs between the two eyes
        fb_ovr_tex[i].OGL.Header.RenderViewport.Pos.x = i == 0 ? 0 : fb_width / 2.0;
        fb_ovr_tex[i].OGL.Header.RenderViewport.Pos.y = 0;
        fb_ovr_tex[i].OGL.Header.RenderViewport.Size.w = fb_width / 2.0;
        fb_ovr_tex[i].OGL.Header.RenderViewport.Size.h = fb_height;
        fb_ovr_tex[i].OGL.TexId = fb_tex[1];	// both eyes will use the same texture id 
    }

    // fill in the ovrGLConfig structure needed by the SDK to draw our stereo pair
    // to the actual HMD display (SDK-distortion mode)
    memset(&glcfg, 0, sizeof glcfg);
    glcfg.OGL.Header.API = ovrRenderAPI_OpenGL;
    glcfg.OGL.Header.BackBufferSize.w = hmd->Resolution.w;
    glcfg.OGL.Header.BackBufferSize.h = hmd->Resolution.h;
    glcfg.OGL.Header.Multisample = 1;

#ifdef WIN32
    glcfg.OGL.Window = GetActiveWindow();
    glcfg.OGL.DC = wglGetCurrentDC();
#else
    glcfg.OGL.Disp = glXGetCurrentDisplay();
#endif

    if(hmd->HmdCaps & ovrHmdCap_ExtendDesktop) {
        printf("Running in \"extended desktop\" mode\n");
    } else {
        // to sucessfully draw to the HMD display in "direct-hmd" mode, we have to
        // call ovrHmd_AttachToWindow
        // XXX: this doesn't work properly yet due to bugs in the oculus 0.4.1 sdk/driver
        //
#ifdef WIN32
        ovrHmd_AttachToWindow(hmd, glcfg.OGL.Window, 0, 0);
#else
        ovrHmd_AttachToWindow(hmd, (void*)glXGetCurrentDrawable(), 0, 0); // 0.4.4
#endif
        printf("Running in \"direct-hmd\" mode.  Or maybe oculusd isn't running..?\n");
#ifndef WIN32
        printf("RUN oculusd FIRST!! Take this out if Oculus fixes direct mode on Linux\n");
        SDL_Quit();
#endif
    }

    // enable low-persistence display and dynamic prediction for latency compensation
    hmd_caps = ovrHmdCap_LowPersistence;
    hmd_caps |= param.no_prediction ? 0 : ovrHmdCap_DynamicPrediction;
    if (param.no_vsync) {
        hmd_caps |= ovrHmdCap_NoVSync; // This.. for whatever reason "solves" the low 37.5 fps problem.
        // well.. now I don't have a 37.5 fps problem after updating mesa,xorg-server and restarting X...
        printf("VSYNC Disabled.\n");
    } else {
        printf("VSYNC Enabled.\n");
    }
    ovrHmd_SetEnabledCaps(hmd, hmd_caps);

    // configure SDK-rendering and enable chromatic aberation correction, vignetting, and
    // timewarp, which shifts the image before drawing to counter any latency between the call
    // to ovrHmd_GetEyePose and ovrHmd_EndFrame.

    // Overdrive brightness transitions to reduce artifacts on DK2+ displays.
    distort_caps = ovrDistortionCap_Overdrive | ovrDistortionCap_Vignette;
    distort_caps |= param.no_timewarp ? 0 : ovrDistortionCap_TimeWarp;
    distort_caps |= param.no_hq_distortion ? 0 : ovrDistortionCap_HqDistortion;
    distort_caps |= param.no_restore ? ovrDistortionCap_NoRestore : 0;

#if OVR_MAJOR_VERSION < 5
    distort_caps |= ovrDistortionCap_Chromatic; // can't turn it off in >0.5
    distort_caps |= param.no_timewarp_spinwaits ? ovrDistortionCap_ProfileNoTimewarpSpinWaits : 0;
#endif

#ifdef WIN32
    distort_caps |= param.no_compute_shader ? 0 : ovrDistortionCap_ComputeShader; // #ifdef'd out in the sdk for linux
#endif

    if(!ovrHmd_ConfigureRendering(hmd, &glcfg.Config, distort_caps, hmd->DefaultEyeFov, eye_rdesc)) {
        fprintf(stderr, "failed to configure renderer for Oculus SDK\n");
    }

    ovrHmd_DismissHSWDisplay(hmd);
}

void Init () {

#ifdef OVR_ENABLED
    // jdt: oculus init needs better home
    if (!ovr_Initialize()) {
        cout << "ovr_Initialize failed.  Likely can't find OVR Runtime dll/so. Aborting." << endl;
        exit(1);
    }
#endif // OVR_ENABLED

    Uint32 sdl_flags = SDL_INIT_VIDEO | SDL_INIT_NOPARACHUTE | SDL_INIT_TIMER;
    if (SDL_Init (sdl_flags) < 0) cout << "Could not initialize SDL" << endl;

    // requiring anything higher than OpenGL 3.0 causes deprecation of 
    // GL_LIGHTING GL_LIGHT0 GL_NORMALIZE, etc.. need replacements.
    // also deprecates immediate mode, which would be a complete overhaul.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    /*
       SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
       SDL_GL_SetAttribute(SDL_GL_FRAMEBUFFER_SRGB_CAPABLE, 1); // enables TrueColor
       SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
       SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
       SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
       SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
       SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
     */
    //SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);
    //SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
    //		SDL_GL_CONTEXT_PROFILE_CORE        // deprecated functions are disabled
    //SDL_GL_CONTEXT_PROFILE_COMPATIBILITY // deprecated functions are allowed
    //SDL_GL_CONTEXT_PROFILE_ES // subset of the base OpenGL functionality
    //);
#if defined (USE_STENCIL_BUFFER)
    SDL_GL_SetAttribute (SDL_GL_STENCIL_SIZE, 8);
#endif
#if 0
    SDL_GL_SetAttribute (SDL_GL_MULTISAMPLEBUFFERS, 1); // enable msaa
    SDL_GL_SetAttribute (SDL_GL_MULTISAMPLESAMPLES, 2);
#endif

    //resolution = GetResolution (param.res_type);
    Uint32 window_width = 1920; //resolution.width;
    Uint32 window_height = 1080; //resolution.height;
    Uint32 window_flags = SDL_WINDOW_OPENGL;
    if (param.fullscreen) {
        // jdt: fullscreen option now controls whether to automatically send 
        // window fullscreen to the rift in extended mode.
        //window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        //window_width = window_height = 0; // don't switch display mode.
    }

    sdlWindow = SDL_CreateWindow("vlc-vr",
            SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
            window_width, window_height, window_flags);
    if (sdlWindow == NULL) {
        cout << "Failed to create window: " << SDL_GetError() << endl;
        SDL_Quit();
    }

    // Create an opengl context instead of an sdl renderer.
    glContext = SDL_GL_CreateContext(sdlWindow);
    if (!glContext) {
        cout << "Couldn't initialize OpenGL context: " << SDL_GetError() << endl;
        SDL_Quit();
    }
    SDL_GL_MakeCurrent(sdlWindow, glContext); // jdt: probably not necessary

    // Initialize opengl extension wrangling lib for Frame Buffer Object support (rift)
    glewExperimental = GL_TRUE; // jdt: probably not necessary
    GLenum err = glewInit();
    if (err != GLEW_OK) {
        cout << "Failed to initialize GLEW library: " << (char*)glewGetErrorString(err) << endl;
        SDL_Quit();
    }
    cout << "Status: Using GLEW: " << (char*)glewGetString(GLEW_VERSION) << endl;
    printf("Setting up video mode with res: %ux%u\n", window_width, window_height);

#ifdef OVR_ENABLED
    if (!(hmd = ovrHmd_Create(0))) {
        cout << "No Oculus Rift device found.  Creating fake DK2." << endl;
        if(!(hmd = ovrHmd_CreateDebug(ovrHmd_DK2))) {
            cout << "failed to create virtual debug HMD" << endl;
            SDL_Quit();
        }
        hmd_is_debug = true;
    }
    else hmd_is_debug = false;

    printf("initialized HMD: %s - %s\n", hmd->Manufacturer, hmd->ProductName);
    printf("\tdisplay resolution: %dx%d\n", hmd->Resolution.w, hmd->Resolution.h);
    printf("\tdisplay position: %d,%d\n", hmd->WindowsPos.x, hmd->WindowsPos.y);

    OvrConfigureTracking();
    OvrFindResolution();

    fbo = fb_tex[0] = fb_tex[1] = fb_depth = 0;
    UpdateRenderTarget(fb_width, fb_height);

    OvrConfigureRendering();
#else
    fb_width = window_width;
    fb_height = window_height;
#endif // OVR_ENABLED

#if 0 // dynamically load shaders via relative path:
    init_shader_program(&fxaa_prog, "shaders/fxaa.vert", "shaders/fxaa.frag");
    init_shader_program(&passthrough_prog, "shaders/passthrough.vert", "shaders/passthrough.frag");
    init_shader_program(&dome_distort_prog, "shaders/dome_distort.vert", "shaders/dome_distort.frag");
    init_shader_program(&cylinder_distort_prog, "shaders/cylinder_distort.vert", "shaders/cylinder_distort.frag");
#else
    cout << "loading fxaa shader" << endl;
    init_shader_program(&fxaa_prog, fxaa_vertShaderSource, fxaa_fragShaderSource);
    cout << "loading passthrough shader" << endl;
    init_shader_program(&passthrough_prog, passthrough_vertShaderSource, passthrough_fragShaderSource);
    cout << "loading dome shader" << endl;
    init_shader_program(&dome_distort_prog, dome_distort_vertShaderSource, dome_distort_fragShaderSource);
    cout << "loading cylinder shader" << endl;
    init_shader_program(&cylinder_distort_prog, cylinder_distort_vertShaderSource, cylinder_distort_fragShaderSource);
#endif

#ifdef OVR_ENABLED
    if (param.fullscreen && (hmd->HmdCaps & ovrHmdCap_ExtendDesktop))
        ToggleHmdFullscreen ();
#endif

}


const unsigned int maxFrames = 50;
static unsigned int numFrames = 0;
static float averagefps = 0;
static float prevTime = 0;
static unsigned int numDumps = 0;

void dump_fps()
{
    numFrames++;

    if (numFrames % maxFrames == 0) {
        float curTime = SDL_GetTicks() / 1000.0f;
        if (prevTime && curTime > prevTime) {
            averagefps = 1 / (curTime - prevTime) * maxFrames;
        }
        numFrames = 0;
        prevTime = curTime;
        printf("%u fps:%.3f\n", numDumps*maxFrames, averagefps);
        numDumps++;
    }
}

/* TODO
   void LookAtSelection(ovrEyeType eye)
   {
   int idx = eye == ovrEye_Left ? 0 : 1;

   SetupDisplay (eye, false); // false to not render skybox
   SetupGuiDisplay (false); // false to not render the background frame again.

   GLdouble modelview[16];
   GLdouble projection[16];
   GLint viewport[4];

   glGetDoublev (GL_PROJECTION_MATRIX, projection);
   glGetDoublev (GL_MODELVIEW_MATRIX, modelview);
   glGetIntegerv (GL_VIEWPORT, viewport);

// viewport is that of the framebuffer size.. not 1920x1080
TVector3 center(viewport[2]/2.f + viewport[0], viewport[3]/2.f + viewport[1], 0);

GLfloat depth;
glReadPixels((int)center.x, (int)center.y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth); 
lookAtDepth[idx] = depth;


//GLfloat rgb[3];
//glReadPixels((int)center.x, (int)center.y, 1, 1, GL_RGB, GL_FLOAT, &rgb); 
//lookAtRgb[eye][0] = rgb[0];
//lookAtRgb[eye][1] = rgb[1];
//lookAtRgb[eye][2] = rgb[2];

GLdouble fx, fy, fz; 
//if (depth == 1.0 ||  
if (gluUnProject (center.x, center.y, (GLdouble)depth, modelview, projection, viewport, &fx, &fy, &fz) != GL_TRUE)
{
fx = fy = fz = 0.0;
}

lookAtPrevPos[idx] = lookAtPos[idx];
lookAtPos[idx] = TVector3(fx, fy, fz);
}
 */

// TODO: convert to vertex buffer object
void draw_mesh(float d, int nx, int ny, float texLeft, float texRight, float texUp, float texDown)
{
    float dx = d / (nx-1);
    float dy = d / (ny-1);
    float dtx = (texRight - texLeft) / (nx-1);
    float dty = (texUp - texDown) / (ny-1);

    float offx = -d/2;
    float offy = -d/2;

    for(int y = 1; y < ny; y++)
    {
        float fy = (y-1) * dy + offy;
        float ty = (y-1) * dty + texDown;

        glBegin (GL_QUAD_STRIP);

        for(int x = 0; x < nx; x++)
        {
            float fx = x * dx + offx;
            float tx = x * dtx + texLeft;

            glTexCoord2f(tx, ty + dty);
            glVertex2f(fx, fy + dy); 
            glTexCoord2f(tx, ty);
            glVertex2f(fx, fy); 
        }

        glEnd();
    }
}


void RenderFrame()
{
#ifdef OVR_ENABLED
    ovrHmd_BeginFrame(hmd, frame_index);

    ovrVector3f eye_view_offsets[2] = {
        eye_rdesc[0].HmdToEyeViewOffset,
        eye_rdesc[1].HmdToEyeViewOffset
    };
    ovrHmd_GetEyePoses(hmd, frame_index, eye_view_offsets, eyePose, &trackingState);
    frame_index++;
#endif

#ifdef OVR_ENABLED
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
#else
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
#endif

    // TODO:
    // Set modelview to "cyclops" mode for non-opengl geometry culling (UpdateCourse).
    // jdt: we just use the left eye for now. (might want to average values for both)
#ifndef OVR_ENABLED
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    //glOrtho(-video.width, video.width, -video.height, video.height, 0.001, 100.0);
    glOrtho(-1, 1, -1, 1, 0.001, 100.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
#endif
    //glNewList(stereo_gl_list, GL_COMPILE);
    //current->Loop(g_game.time_step);
    //glEndList();

    ClearDisplay();

    glEnable(GL_TEXTURE_2D);
    glColor3f(1,1,1);

    GLuint distort_prog = 0;
    GLuint nmesh = 20;
    GLuint mesh_nx = 2;
    GLuint mesh_ny = 2;

    switch(param.distortion) {
    case DISTORTION_DOME:
        mesh_nx = nmesh + 1;
        mesh_ny = nmesh + 1;
        distort_prog = dome_distort_prog;
        break;
    case DISTORTION_CYLINDER:
        mesh_nx = nmesh * 2 + 1;
        mesh_ny = 2;
        distort_prog = cylinder_distort_prog;
        break;
    default: break;
    };

    if (distort_prog) {
        glUseProgram (distort_prog);
        glBindTexture(GL_TEXTURE_2D, video.glTexture[0]);
        glUniform1i(glGetUniformLocation(distort_prog, "fbo_texture"), 0);
        glUniform3f(glGetUniformLocation(distort_prog, "mesh_focus"), 0, 0, 0); // TODO
        glUniform1f(glGetUniformLocation(distort_prog, "mesh_radius"), param.mesh_radius); //param.tv_size * sqrt(2));
    } else {
        glBindTexture(GL_TEXTURE_2D, video.glTexture[0]);
    }

#ifdef OVR_ENABLED
    for (int i = 0; i < 2; ++i)
    {
        ovrEyeType eye = hmd->EyeRenderOrder[i];

        if (eye == ovrEye_Left) {
            glViewport(0, 0, fb_width/2, fb_height);
        } else {
            glViewport(fb_width/2, 0, fb_width/2, fb_height);
        }

        SetupDisplay (eye, true);

        float texLeft = 0;
        float texRight =(float)video.width / video.glVideoWidth;
        float texDown = 0;
        float texUp = (float)video.height / video.glVideoHeight;

        if (param.stereo_mode == STEREO_SBS) {
            texLeft = eye == ovrEye_Left ? 0.0f : texRight/2;
            texRight = eye == ovrEye_Left ? texRight/2 : texRight;
        } else if (param.stereo_mode == STEREO_OVER_UNDER) {
            texDown = eye == ovrEye_Left ? 0.0f : texUp/2;
            texUp = eye == ovrEye_Left ? texUp/2 : texUp;
        } 

        glTranslatef(0, 0, param.tv_zoffset);

        float d = param.tv_size;

        glScalef(video.aspect_ratio, 1, 1);
        draw_mesh(d, mesh_nx, mesh_ny, texLeft, texRight, texUp, texDown);

        // TODO;
        //glCallList(stereo_gl_list);

        /*
           if (!hmd_is_debug && current != &Racing && current != &Intro) 
           {
        // Optimization: only read from left viewport.. pixel reads are expensive.
        if (eye == ovrEye_Left)
        LookAtSelection (eye);
        }
         */
    }
#else
    {
        glColor3f(1, 1, 1);
        glTranslatef(0, 0, param.tv_zoffset);
        float texLeft = 0;
        float texRight = 1;
        float d = param.tv_size;
        glBegin (GL_QUADS);
        glTexCoord2f(texLeft, 0);
        glVertex2f(-d, -d); 
        glTexCoord2f(texRight, 0);
        glVertex2f( d, -d);
        glTexCoord2f(texRight, 1);
        glVertex2f( d,  d);
        glTexCoord2f(texLeft, 1);
        glVertex2f(-d,  d);
        glEnd();
    }

#endif

    /* TODO
    // Read pixels from framebuffer to get 3D position user is looking.
    lookAtValid = false;
    // jdt: TODO: need a better way to signify that a state is GUI enabled.
    if (!hmd_is_debug && current != &Racing && current != &Intro) // && current_render_mode() == GUI)
    {
        float eps = 100.f;
        if (abs(lookAtPos[0].x) > 0 && abs(lookAtPos[0].y) > 0) {
            // jdt: only using one eye for now... for performance
            //if (abs(lookAtPos[0].x - lookAtPos[1].x) < eps &&
            //    abs(lookAtPos[0].y - lookAtPos[1].y) < eps) {
                lookAtValid = true;
            //}
        }
    }
    */

#ifdef OVR_ENABLED
    // Full-frame post processing before handing off to oculus sdk. Bind to 
    // previous framebuffer texture and write to the one Oculus is configured with.
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb_tex[1], 0);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glViewport(0, 0, fb_tex_width, fb_tex_height);
    glLoadIdentity();
    if (param.use_fxaa) {
        // jdt: TODO uniforms don't need to be set every frame
        glUseProgram (fxaa_prog);
        glBindTexture(GL_TEXTURE_2D, fb_tex[0]);
        glUniform1i(glGetUniformLocation(fxaa_prog, "u_texture0"), 0);
        glUniform2f(glGetUniformLocation(fxaa_prog, "resolution"), fb_tex_width, fb_tex_height);
        glUniform1i(glGetUniformLocation(fxaa_prog, "enabled"), 1);
    } else {
        glUseProgram (passthrough_prog);
        glBindTexture(GL_TEXTURE_2D, fb_tex[0]);
        glUniform1i(glGetUniformLocation(passthrough_prog, "fbo_texture"), 0);
    }
    glBegin (GL_QUADS);
    glVertex2f(-1, -1); 
    glVertex2f( 1, -1);
    glVertex2f(1, 1);
    glVertex2f(-1, 1);
    glEnd();
    glUseProgram(0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb_tex[0], 0);

    // After drawing both eyes and post processing, revert to drawing directly to the
    // display and call ovrHmd_EndFrame to let the Oculus SDK compensate for lens distortion
    // and chromatic aberation and double buffering.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    ovrHmd_EndFrame(hmd, eyePose, &fb_ovr_tex[0].Texture);
#else
    SDL_GL_SwapWindow(sdlWindow);
#endif

    if (param.console_dump) dump_fps();
}

void PollEvent()
{
    SDL_Event event;
    unsigned int key;
    int x, y;

    unsigned int seekspeed[] = {5000, 30000, 240000};
    libvlc_time_t curtime = libvlc_media_player_get_time(vlc_media_player);

    while (SDL_PollEvent (&event)) {
        switch (event.type) {
        case SDL_KEYDOWN:
            SDL_GetMouseState(&x, &y);
            key = event.key.keysym.sym;

            switch(key) {
            case SDLK_F2:
            case SDLK_F9: ToggleHmdFullscreen(); break;
            case SDLK_x: param.use_fxaa = !param.use_fxaa; break;
            case SDLK_LSHIFT:
            case SDLK_RSHIFT: ovrHmd_RecenterPose(hmd); break;
            case SDLK_SPACE: libvlc_media_player_pause (vlc_media_player); break;
            case SDLK_ESCAPE: quit = true; break;
            case SDLK_a: param.tv_size -= 0.1f; break;
            case SDLK_d: param.tv_size += 0.1f; break;
            case SDLK_w: param.tv_zoffset += 0.1; break;
            case SDLK_s: param.tv_zoffset -= 0.1; break;
            case SDLK_v: param.view_locked = !param.view_locked; break;
            case SDLK_m: libvlc_audio_toggle_mute(vlc_media_player); break;
            case SDLK_h: param.ipd_multiplier--; break;
            case SDLK_l: param.ipd_multiplier++; break;
            case SDLK_j: param.mesh_radius -= 0.1; break;
            case SDLK_k: param.mesh_radius += 0.1; break;
            case SDLK_1:
            case SDLK_2:
            case SDLK_3: {
                param.distortion = (distortion_t)(key - SDLK_1);
                float half_mesh = param.tv_size / 2;
                switch(param.distortion) {
                case DISTORTION_NONE:
                    param.mesh_radius = half_mesh;
                    break;
                case DISTORTION_DOME:
                    param.mesh_radius = sqrt(2 * half_mesh * half_mesh);
                    break;
                case DISTORTION_CYLINDER:
                    param.mesh_radius = half_mesh;
                    break;
                default: break;
                }
                break;
            }
            case SDLK_r: {
                param.stereo_mode = (stereo_mode_t)(((int)param.stereo_mode + 1) % MAX_STEREO_MODE);
                break;
            }
            case SDLK_t: {
                video.aspect_ratio_mode = (aspect_ratio_mode_t)(((int)video.aspect_ratio_mode+1) % MAX_ASPECT_MODE);
                switch(video.aspect_ratio_mode) {
                    case ASPECT_4_BY_3: video.aspect_ratio = 4.f / 3.f; break;
                    case ASPECT_16_BY_9: video.aspect_ratio = 16.f / 9.f; break;
                    default:
                    case ASPECT_AUTO: video.aspect_ratio = video.width / video.height; break;
                }
                break;
            }
            case SDLK_UP: libvlc_media_player_set_time(vlc_media_player, curtime + seekspeed[0]); break;
            case SDLK_DOWN: libvlc_media_player_set_time(vlc_media_player, curtime - seekspeed[0]); break;
            case SDLK_LEFT: libvlc_media_player_set_time(vlc_media_player, curtime - seekspeed[1]); break;
            case SDLK_RIGHT: libvlc_media_player_set_time(vlc_media_player, curtime + seekspeed[1]); break;
            case SDLK_PAGEUP: libvlc_media_player_set_time(vlc_media_player, curtime + seekspeed[2]); break;
            case SDLK_PAGEDOWN: libvlc_media_player_set_time(vlc_media_player, curtime - seekspeed[2]); break;
            default: break;
            }
            cout << "ipd:" << param.ipd_multiplier << " tsize:" << param.tv_size << "  zoffset:" << param.tv_zoffset << "  mesh_radius:" << param.mesh_radius << endl;

#ifdef OVR_ENABLED
            // jdt: grr this damn oculus safety screen won't go away.
            ovrHmd_DismissHSWDisplay(hmd);
#endif
            break;

        case SDL_QUIT:
            quit = true;
            break;
        }
    }
}

void printUsage(int argc, char *argv[])
{
    cerr << "Usage: " << argv[0] << " [options] <video-filename>" << endl;
    cerr << "options:" << endl;
    cerr << "\t-d[1-3] Sets distortion (1=None,2=Dome,3=Cylinder)" << endl;
    cerr << "\t\tChange during playback with numeric keys 1-3." << endl;
    cerr << "\t-s[1-3] Sets stereo mode (1=None,2=SBS,3=Over/Under)" << endl;
    cerr << "\t\tCycle modes during playback with the 'r' key." << endl;
    cerr << "\t-f Startup fullscreen on Oculus Rift (only valid in extended mode)." << endl;
    cerr << "\t\tUse F2 or F9 to toggle video to rift during playback." << endl;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printUsage(argc, argv);
        return 1;
    }
    string basename; // filename of input

    quit = false;
    frame_index = 0;
    param.stereo_mode = STEREO_NONE;
    param.distortion = DISTORTION_NONE;
    param.fullscreen = false;
    param.view_locked = false;

    int c;
    opterr = 0;
    while ((c = getopt(argc, argv, "fvd:s:")) != -1) {
        switch(c) {
        case 'f': param.fullscreen = true; break;
        case 'd': {
            int idistortion = atoi(optarg);
            if (idistortion < 1 || idistortion > (int)MAX_DISTORTION)
                param.distortion = DISTORTION_NONE;
            else
                param.distortion = (distortion_t)(idistortion-1);
        } break;
        case 's': {
            int istereo = atoi(optarg);
            if (istereo < 1 || istereo > (int)MAX_STEREO_MODE)
                param.stereo_mode = STEREO_NONE;
            else
                param.stereo_mode = (stereo_mode_t)(istereo-1);
        } break;
        case 'v': param.view_locked = true; break;
        case '?':
            if (optopt == 'd' || optopt == 'c')
                fprintf(stderr, "Option -%c requires an argument.\n", optopt);
            else if (isprint (optopt))
                fprintf(stderr, "Unknown option `-%c'.\n", optopt);
            else
                fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
            printUsage(argc, argv);
            return 1;
        default:
            abort();
        }
    }
    if(argc - optind != 1) {
        printUsage(argc, argv);
        return -1;
    }
    basename = argv[argc-1];
    cout << "Reading video from: " << basename << endl;

    setDefaults();
    Init();

    const char * const vlc_args[] = {
        "-I", "dummy", "--ignore-config"
    };
    vlc = libvlc_new(sizeof(vlc_args) / sizeof(*vlc_args), vlc_args);
    if (!vlc) {
        cerr << "Failed to Create VLC Instance" << endl;
        return -1;
    }

    vlc_media_player = libvlc_media_player_new(vlc);
    vlc_event_manager = libvlc_media_player_event_manager(vlc_media_player);
    vlc_media = libvlc_media_new_path (vlc, basename.c_str());
    libvlc_media_player_set_media (vlc_media_player, vlc_media);

    libvlc_media_player_play (vlc_media_player);

    while(!quit && libvlc_media_player_get_state(vlc_media_player) < libvlc_Playing) {
        PollEvent();
    }

    libvlc_video_get_size(vlc_media_player, 0, &video.width, &video.height);
    UpdateVideoTarget(video.width, video.height);

#ifdef USE_RV16
    libvlc_video_set_format (vlc_media_player, "RV16", video.width, video.height, video.width*(video.bpp/8));
#else
    libvlc_video_set_format (vlc_media_player, "RV32", video.width, video.height, video.width*(video.bpp/8));
#endif
    libvlc_video_set_callbacks (vlc_media_player, lock, unlock, display, NULL);

    video.aspect_ratio = video.width / video.height;

    while(!quit && libvlc_media_player_get_state(vlc_media_player) != libvlc_Ended) {
        PollEvent();
        if (video.updateFrame)
            LoadVideoTexture();
        RenderFrame();
    }

#ifdef OVR_ENABLED
    ovrHmd_Destroy(hmd);
#endif

    return 0;
}


