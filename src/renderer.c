/*
 * renderer.c - OpenGL 3.3 Core Profile renderer
 */
#include "renderer.h"
#include "weapon.h"  /* for g_weapon_defs in HUD ammo display */
#include "font5x7.h" /* embedded font for on-screen messages */
#include <GL/glew.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* -------------------------------------------------------------------------
 * Shader sources (embedded GLSL 3.30)
 * ---------------------------------------------------------------------- */

static const char *WORLD_VERT_SRC =
"#version 330 core\n"
"layout(location=0) in vec3 aPos;\n"
"layout(location=1) in vec2 aUV;\n"
"uniform vec2 uUVOffset;\n"
"layout(location=2) in float aLight;\n"
"out vec2 vUV;\n"
"out float vLight;\n"
"uniform mat4 uMVP;\n"
"void main() {\n"
"    gl_Position = uMVP * vec4(aPos, 1.0);\n"
"    vUV = aUV + uUVOffset;\n"
"    vLight = aLight;\n"
"}\n";

static const char *WORLD_FRAG_SRC =
"#version 330 core\n"
"in vec2 vUV;\n"
"in float vLight;\n"
"out vec4 fragColor;\n"
"uniform sampler2D uTex;\n"
"uniform float uAmbient;\n"
"void main() {\n"
"    vec4 col = texture(uTex, vUV);\n"
"    if (col.a < 0.1) discard;\n"
"    float light = clamp(uAmbient + vLight, 0.0, 1.0);\n"
"    fragColor = vec4(col.rgb * light, col.a);\n"
"}\n";

static const char *SPRITE_VERT_SRC =
"#version 330 core\n"
"layout(location=0) in vec3 aPos;\n"
"layout(location=1) in vec2 aUV;\n"
"layout(location=2) in vec4 aColor;\n"
"out vec2 vUV;\n"
"out vec4 vColor;\n"
"uniform mat4 uMVP;\n"
"void main() {\n"
"    gl_Position = uMVP * vec4(aPos, 1.0);\n"
"    vUV = aUV;\n"
"    vColor = aColor;\n"
"}\n";

static const char *SPRITE_FRAG_SRC =
"#version 330 core\n"
"in vec2 vUV;\n"
"in vec4 vColor;\n"
"out vec4 fragColor;\n"
"uniform sampler2D uTex;\n"
"uniform bool uHasTex;\n"
"void main() {\n"
"    if (uHasTex) {\n"
"        vec4 col = texture(uTex, vUV);\n"
"        if (col.a < 0.1) discard;\n"
"        fragColor = col * vColor;\n"
"    } else {\n"
"        fragColor = vColor;\n"
"    }\n"
"}\n";

/* 2D HUD shader */
static const char *HUD_VERT_SRC =
"#version 330 core\n"
"layout(location=0) in vec2 aPos;\n"
"layout(location=1) in vec2 aUV;\n"
"layout(location=2) in vec4 aColor;\n"
"out vec2 vUV;\n"
"out vec4 vColor;\n"
"uniform mat4 uOrtho;\n"
"void main() {\n"
"    gl_Position = uOrtho * vec4(aPos, 0.0, 1.0);\n"
"    vUV = aUV;\n"
"    vColor = aColor;\n"
"}\n";

static const char *HUD_FRAG_SRC =
"#version 330 core\n"
"in vec2 vUV;\n"
"in vec4 vColor;\n"
"out vec4 fragColor;\n"
"uniform sampler2D uTex;\n"
"uniform bool uHasTex;\n"
"void main() {\n"
"    if (uHasTex) {\n"
"        vec4 t = texture(uTex, vUV);\n"
"        if (t.a < 0.1) discard;\n"
"        fragColor = t * vColor;\n"
"    } else {\n"
"        fragColor = vColor;\n"
"    }\n"
"}\n";

/* Sky shader — exact Outlaws/Jedi parallax sky projection, per pixel.
 * Ghidra olwin.exe: sky_BuildColumnAngleTable@0x4b09b0 (texel U per column =
 * atan((x-centerX)/focal)/turn * parallaxX), sky_UpdateScrollOffsets@0x4b0920
 * (U scroll = yawTurns*parallaxX; V scroll = -pitchTurns*parallaxY), and the
 * span setup in sky_DrawCeiling@0x4b6190: V(y) = (texH-1) -
 * ((y - viewH/2)*vScale + 100 - pitchTexels), vScale = 640/viewH texels/pixel
 * (view init @0x4a68d9). V is a bottom-up texel index (texture columns are
 * stored bottom-up); level textures are uploaded row-flipped so GL v = V/texH.
 * The software renderer runs with parallax fixed at 1024x1024 (setter
 * @0x4b0990, sole call passes 1024.0,1024.0). */
static const char *SKY_VERT_SRC =
"#version 330 core\n"
"layout(location=0) in vec2 aPos;\n"      /* fullscreen quad in NDC */
"void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *SKY_FRAG_SRC =
"#version 330 core\n"
"out vec4 fragColor;\n"
"uniform sampler2D uTex;\n"
"uniform vec2  uScreen;    /* viewport W,H in pixels */\n"
"uniform vec2  uTexSize;   /* sky texture W,H in texels */\n"
"uniform vec2  uParallax;  /* texels per full turn (1024,1024) */\n"
"uniform float uYawTurns;  /* camera yaw, Outlaws convention, in turns */\n"
"uniform float uPitchTurns;/* camera pitch in turns, positive = up */\n"
"uniform float uFocalPx;   /* horizontal focal length in pixels */\n"
"#define PI 3.14159265358979\n"
"void main() {\n"
"    float dx  = gl_FragCoord.x - 0.5 * uScreen.x;\n"
"    float ang = atan(dx / uFocalPx);\n"
"    float U = (uYawTurns + ang / (2.0*PI)) * uParallax.x;\n"
"    /* gl_FragCoord.y grows upward; software y grows downward */\n"
"    float yDown  = uScreen.y - gl_FragCoord.y;\n"
"    float vScale = 640.0 / uScreen.y;\n"
"    float V = (uTexSize.y - 1.0)\n"
"            - ((yDown - 0.5*uScreen.y) * vScale + 100.0\n"
"               - uPitchTurns * uParallax.y);\n"
"    vec2 uv = vec2(U / uTexSize.x, V / uTexSize.y);\n"
"    fragColor = texture(uTex, uv);\n"   /* wraps via GL_REPEAT */
"}\n";

/* ---- Post-processing: fullscreen quad + multi-effect resolve ---- */
static const char *POST_VERT_SRC =
"#version 330 core\n"
"layout(location=0) in vec2 aPos;\n"
"out vec2 vUV;\n"
"void main(){ vUV = aPos*0.5+0.5; gl_Position = vec4(aPos,0.0,1.0); }\n";

static const char *POST_FRAG_SRC =
"#version 330 core\n"
"in vec2 vUV;\n"
"out vec4 frag;\n"
"uniform sampler2D uScene;\n"
"uniform vec2  uRes;\n"
"uniform float uTime;\n"
"uniform int   uCRT;\n"
"uniform int   uBloom;\n"
"uniform int   uChroma;\n"
"uniform int   uVig;\n"
"uniform int   uGrain;\n"
"uniform int   uGrade;\n"
"uniform float uCurv;\n"
"uniform float uScan;\n"
"uniform float uMask;\n"
"uniform float uBloomAmt;\n"
"uniform float uBloomThr;\n"
"uniform float uChromaAmt;\n"
"uniform float uVigAmt;\n"
"uniform float uGrainAmt;\n"
"uniform float uSat;\n"
"uniform float uCon;\n"
"uniform float uGam;\n"
"#define PI 3.14159265\n"
"float luma(vec3 c){ return dot(c, vec3(0.299,0.587,0.114)); }\n"
"float hash(vec2 p){ return fract(sin(dot(p, vec2(12.9898,78.233))) * 43758.5453); }\n"
"vec2 crtCurve(vec2 uv){\n"
"    uv = uv*2.0-1.0;\n"
"    vec2 off = uv.yx*uv.yx*uCurv;\n"
"    uv += uv*off;\n"
"    return uv*0.5+0.5;\n"
"}\n"
"void main(){\n"
"    vec2 uv = vUV;\n"
"    if(uCRT==1) uv = crtCurve(uv);\n"
"    bool outside = (uv.x<0.0||uv.x>1.0||uv.y<0.0||uv.y>1.0);\n"
"    vec3 col;\n"
"    if(uChroma==1){\n"
"        vec2 dir = uv-0.5;\n"
"        vec2 o = dir * (uChromaAmt/uRes);\n"
"        col.r = texture(uScene, uv+o).r;\n"
"        col.g = texture(uScene, uv).g;\n"
"        col.b = texture(uScene, uv-o).b;\n"
"    } else {\n"
"        col = texture(uScene, uv).rgb;\n"
"    }\n"
"    if(uBloom==1){\n"
"        vec3 b = vec3(0.0); float tot=0.0;\n"
"        vec2 px = 1.0/uRes;\n"
"        for(int x=-3;x<=3;x++){\n"
"            for(int y=-3;y<=3;y++){\n"
"                vec2 so = vec2(float(x),float(y))*px*1.5;\n"
"                vec3 s = texture(uScene, uv+so).rgb;\n"
"                float br = max(0.0, luma(s)-uBloomThr);\n"
"                float w = exp(-float(x*x+y*y)/8.0);\n"
"                b += s*br*w; tot += w;\n"
"            }\n"
"        }\n"
"        b /= max(tot,0.001);\n"
"        col += b*uBloomAmt;\n"
"    }\n"
"    if(uGrade==1){\n"
"        col = mix(vec3(luma(col)), col, uSat);\n"          /* saturation */
"        col = (col-0.5)*uCon+0.5;\n"                        /* contrast */
"        col = pow(max(col,0.0), vec3(1.0/uGam));\n"         /* gamma */
"    }\n"
"    if(uCRT==1){\n"
"        float sl = 0.5+0.5*sin(uv.y*uRes.y*PI);\n"
"        col *= 1.0 - uScan*(1.0-sl);\n"
"        float m = mod(gl_FragCoord.x, 3.0);\n"
"        vec3 mask = (m<1.0)?vec3(1.0,0.7,0.7):(m<2.0)?vec3(0.7,1.0,0.7):vec3(0.7,0.7,1.0);\n"
"        col *= mix(vec3(1.0), mask, uMask);\n"
"    }\n"
"    if(uVig==1){\n"
"        float d = distance(uv, vec2(0.5));\n"
"        col *= 1.0 - uVigAmt*smoothstep(0.35,0.85,d);\n"
"    }\n"
"    if(uGrain==1){\n"
"        float g = hash(gl_FragCoord.xy + fract(uTime))-0.5;\n"
"        col += g*uGrainAmt;\n"
"    }\n"
"    if(outside) col = vec3(0.0);\n"                         /* CRT overscan border */
"    frag = vec4(clamp(col,0.0,1.0), 1.0);\n"
"}\n";

/* -------------------------------------------------------------------------
 * Shader compilation helpers
 * ---------------------------------------------------------------------- */

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        OL_ERR("Shader compile error: %s\n", log);
        glDeleteShader(sh); return 0;
    }
    return sh;
}

static GLuint link_program(const char *vert_src, const char *frag_src) {
    GLuint vert = compile_shader(GL_VERTEX_SHADER,   vert_src);
    GLuint frag = compile_shader(GL_FRAGMENT_SHADER, frag_src);
    if (!vert || !frag) {
        glDeleteShader(vert); glDeleteShader(frag); return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vert); glAttachShader(prog, frag);
    glLinkProgram(prog);
    glDeleteShader(vert); glDeleteShader(frag);
    GLint ok; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024]; glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        OL_ERR("Shader link error: %s\n", log);
        glDeleteProgram(prog); return 0;
    }
    return prog;
}

/* -------------------------------------------------------------------------
 * Renderer init / shutdown
 * ---------------------------------------------------------------------- */

static void post_init(Renderer *r);          /* post-processing setup (below) */

bool renderer_init(Renderer *r, const RenderConfig *cfg, const char *title) {
    memset(r, 0, sizeof(*r));
    r->cfg = *cfg;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) {
        OL_ERR("SDL_Init failed: %s\n", SDL_GetError()); return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    r->window = SDL_CreateWindow(title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        cfg->width, cfg->height,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!r->window) {
        OL_ERR("SDL_CreateWindow failed: %s\n", SDL_GetError()); return false;
    }

    r->gl_ctx = SDL_GL_CreateContext(r->window);
    if (!r->gl_ctx) {
        OL_ERR("SDL_GL_CreateContext failed: %s\n", SDL_GetError()); return false;
    }
    SDL_GL_SetSwapInterval(1);

    glewExperimental = GL_TRUE;
    GLenum glew_err = glewInit();
    if (glew_err != GLEW_OK) {
        OL_ERR("glewInit failed: %s\n", glewGetErrorString(glew_err)); return false;
    }

    OL_LOG("OpenGL: %s\n", glGetString(GL_VERSION));
    OL_LOG("Renderer: %s\n", glGetString(GL_RENDERER));

    /* Compile shaders */
    r->prog_world  = link_program(WORLD_VERT_SRC,  WORLD_FRAG_SRC);
    r->prog_sprite = link_program(SPRITE_VERT_SRC, SPRITE_FRAG_SRC);
    r->prog_hud    = link_program(HUD_VERT_SRC,    HUD_FRAG_SRC);
    r->prog_sky    = link_program(SKY_VERT_SRC,    SKY_FRAG_SRC);
    if (!r->prog_world || !r->prog_sprite || !r->prog_hud || !r->prog_sky)
        return false;

    /* Fullscreen quad for the sky pass (NDC positions only) */
    {
        static const f32 quad[] = { -1,-1,  1,-1,  1,1,  -1,-1,  1,1,  -1,1 };
        glGenVertexArrays(1, &r->sky_vao);
        glGenBuffers(1, &r->sky_vbo);
        glBindVertexArray(r->sky_vao);
        glBindBuffer(GL_ARRAY_BUFFER, r->sky_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2*sizeof(f32), (void*)0);
        glEnableVertexAttribArray(0);
        glBindVertexArray(0);
    }

    /* GL state defaults */
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL); /* LEQUAL: floor polygons at same depth as wall base edges pass */
    /* Back-face culling: winding is interior-facing after Z-negation correction */
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    /* Sky blue: visible through outdoor sectors with high ceilings */
    glClearColor(0.38f, 0.60f, 0.88f, 1.0f);

    /* Projection matrix.
     * cfg->fov is HORIZONTAL FOV in degrees (Outlaws used 90° horizontal).
     * mat4_perspective takes vertical FOV, so convert: vfov = 2*atan(tan(hfov/2)/aspect). */
    f32 hfov_rad = cfg->fov * OL_DEG2RAD;
    f32 aspect   = (f32)cfg->width / (f32)cfg->height;
    f32 fov_rad  = 2.0f * atanf(tanf(hfov_rad * 0.5f) / aspect);
    r->proj = mat4_perspective(fov_rad, aspect, cfg->near_plane, cfg->far_plane);
    r->cam_fov_rad = fov_rad;

    /* Sprite dynamic VBO (updated each frame, enough for 2048 sprites * 4 verts) */
    glGenVertexArrays(1, &r->sprite_vao);
    glGenBuffers(1, &r->sprite_vbo);
    glBindVertexArray(r->sprite_vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->sprite_vbo);
    glBufferData(GL_ARRAY_BUFFER, 2048 * 6 * (3+2+4) * sizeof(f32), NULL, GL_DYNAMIC_DRAW);
    /* Attrib 0: pos (3 f32) */
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, (3+2+4)*sizeof(f32), (void*)0);
    glEnableVertexAttribArray(0);
    /* Attrib 1: uv (2 f32) */
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, (3+2+4)*sizeof(f32), (void*)(3*sizeof(f32)));
    glEnableVertexAttribArray(1);
    /* Attrib 2: color (4 f32) */
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, (3+2+4)*sizeof(f32), (void*)(5*sizeof(f32)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);

    /* HUD dynamic VBO */
    glGenVertexArrays(1, &r->hud_vao);
    glGenBuffers(1, &r->hud_vbo);
    glBindVertexArray(r->hud_vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->hud_vbo);
    glBufferData(GL_ARRAY_BUFFER, 1024 * 6 * (2+2+4) * sizeof(f32), NULL, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, (2+2+4)*sizeof(f32), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, (2+2+4)*sizeof(f32), (void*)(2*sizeof(f32)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, (2+2+4)*sizeof(f32), (void*)(4*sizeof(f32)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);

    /* Missing texture: 2x2 magenta checkerboard */
    {
        u8 data[4*4] = { 255,0,255,255, 0,0,0,255, 0,0,0,255, 255,0,255,255 };
        glGenTextures(1, &r->missing_tex);
        glBindTexture(GL_TEXTURE_2D, r->missing_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    /* Post-processing pipeline (disabled by default; toggled via debug menu). */
    post_init(r);

    OL_LOG("Renderer initialized (%dx%d)\n", cfg->width, cfg->height);
    return true;
}

void renderer_shutdown(Renderer *r) {
    /* Free level meshes */
    if (r->level_meshes) {
        for (u32 i = 0; i < r->level_mesh_count; i++) {
            glDeleteVertexArrays(1, &r->level_meshes[i].vao);
            glDeleteBuffers(1, &r->level_meshes[i].vbo);
            glDeleteBuffers(1, &r->level_meshes[i].ibo);
        }
        free(r->level_meshes);
    }

    /* Delete textures */
    for (u32 i = 0; i < r->texture_count; i++) {
        if (r->textures[i].handle) glDeleteTextures(1, &r->textures[i].handle);
    }

    if (r->prog_world)  glDeleteProgram(r->prog_world);
    if (r->prog_sprite) glDeleteProgram(r->prog_sprite);
    if (r->prog_hud)    glDeleteProgram(r->prog_hud);
    if (r->prog_sky)    glDeleteProgram(r->prog_sky);

    if (r->sky_vao)    { glDeleteVertexArrays(1, &r->sky_vao);    glDeleteBuffers(1, &r->sky_vbo); }
    if (r->sprite_vao) { glDeleteVertexArrays(1, &r->sprite_vao); glDeleteBuffers(1, &r->sprite_vbo); }
    if (r->hud_vao)    { glDeleteVertexArrays(1, &r->hud_vao);    glDeleteBuffers(1, &r->hud_vbo); }
    if (r->missing_tex) glDeleteTextures(1, &r->missing_tex);

    if (r->prog_post)   glDeleteProgram(r->prog_post);
    if (r->post_vao)    { glDeleteVertexArrays(1, &r->post_vao); glDeleteBuffers(1, &r->post_vbo); }
    if (r->post_color)  glDeleteTextures(1, &r->post_color);
    if (r->post_depth)  glDeleteRenderbuffers(1, &r->post_depth);
    if (r->post_fbo)    glDeleteFramebuffers(1, &r->post_fbo);

    if (r->gl_ctx)  SDL_GL_DeleteContext(r->gl_ctx);
    if (r->window)  SDL_DestroyWindow(r->window);
    SDL_Quit();
}

/* -------------------------------------------------------------------------
 * Frame management
 * ---------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * Post-processing (optional pretty shaders)
 * ---------------------------------------------------------------------- */

/* (Re)allocate the offscreen color texture + depth buffer to (w,h). */
static void post_ensure_size(Renderer *r, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (r->post_color && r->post_w == w && r->post_h == h) return;
    r->post_w = w; r->post_h = h;

    if (!r->post_fbo) glGenFramebuffers(1, &r->post_fbo);
    if (!r->post_color) glGenTextures(1, &r->post_color);
    if (!r->post_depth) glGenRenderbuffers(1, &r->post_depth);

    glBindTexture(GL_TEXTURE_2D, r->post_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glBindRenderbuffer(GL_RENDERBUFFER, r->post_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, r->post_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->post_color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, r->post_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        OL_ERR("Post FBO incomplete (%dx%d)\n", w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* Compile the post program + fullscreen quad. Called from renderer_init. */
static void post_init(Renderer *r) {
    r->prog_post = link_program(POST_VERT_SRC, POST_FRAG_SRC);
    if (!r->prog_post) { OL_ERR("Post shader failed to link\n"); return; }
    static const f32 quad[] = { -1,-1,  1,-1,  -1,1,   -1,1,  1,-1,  1,1 };
    glGenVertexArrays(1, &r->post_vao);
    glGenBuffers(1, &r->post_vbo);
    glBindVertexArray(r->post_vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->post_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2*sizeof(f32), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
    post_ensure_size(r, r->cfg.width, r->cfg.height);
}

void renderer_post_resolve(Renderer *r) {
    if (!r->post.enabled || !r->prog_post || r->post_resolved) return;
    r->post_resolved = true;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, r->cfg.width, r->cfg.height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(r->prog_post);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, r->post_color);
    const PostFX *p = &r->post;
    glUniform1i(glGetUniformLocation(r->prog_post, "uScene"), 0);
    glUniform2f(glGetUniformLocation(r->prog_post, "uRes"),
                (f32)r->post_w, (f32)r->post_h);
    glUniform1f(glGetUniformLocation(r->prog_post, "uTime"),
                (f32)(SDL_GetTicks64() % 100000) / 1000.0f);
    glUniform1i(glGetUniformLocation(r->prog_post, "uCRT"),    p->crt);
    glUniform1i(glGetUniformLocation(r->prog_post, "uBloom"),  p->bloom);
    glUniform1i(glGetUniformLocation(r->prog_post, "uChroma"), p->chromatic);
    glUniform1i(glGetUniformLocation(r->prog_post, "uVig"),    p->vignette);
    glUniform1i(glGetUniformLocation(r->prog_post, "uGrain"),  p->grain);
    glUniform1i(glGetUniformLocation(r->prog_post, "uGrade"),  p->grade);
    glUniform1f(glGetUniformLocation(r->prog_post, "uCurv"),      p->curvature);
    glUniform1f(glGetUniformLocation(r->prog_post, "uScan"),      p->scanline);
    glUniform1f(glGetUniformLocation(r->prog_post, "uMask"),      p->mask);
    glUniform1f(glGetUniformLocation(r->prog_post, "uBloomAmt"),  p->bloom_amt);
    glUniform1f(glGetUniformLocation(r->prog_post, "uBloomThr"),  p->bloom_thresh);
    glUniform1f(glGetUniformLocation(r->prog_post, "uChromaAmt"), p->chroma_amt);
    glUniform1f(glGetUniformLocation(r->prog_post, "uVigAmt"),    p->vignette_amt);
    glUniform1f(glGetUniformLocation(r->prog_post, "uGrainAmt"),  p->grain_amt);
    glUniform1f(glGetUniformLocation(r->prog_post, "uSat"),       p->saturation);
    glUniform1f(glGetUniformLocation(r->prog_post, "uCon"),       p->contrast);
    glUniform1f(glGetUniformLocation(r->prog_post, "uGam"),       p->gamma);

    glBindVertexArray(r->post_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glEnable(GL_DEPTH_TEST);
}

void renderer_begin_frame(Renderer *r) {
    r->post_resolved = false;
    if (r->post.enabled && r->prog_post) {
        post_ensure_size(r, r->cfg.width, r->cfg.height);
        glBindFramebuffer(GL_FRAMEBUFFER, r->post_fbo);
        glViewport(0, 0, r->post_w, r->post_h);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void renderer_end_frame(Renderer *r) {
    /* If nothing resolved the offscreen buffer yet (paths without a UI pass),
     * resolve it now so the frame is actually shown. */
    if (r->post.enabled && !r->post_resolved) renderer_post_resolve(r);
    SDL_GL_SwapWindow(r->window);
}

/* -------------------------------------------------------------------------
 * Texture management
 * ---------------------------------------------------------------------- */

u32 renderer_upload_texture(Renderer *r, const char *name,
                            const u8 *rgba, u32 w, u32 h) {
    /* Check for existing */
    u32 existing = renderer_find_texture(r, name);
    if (existing) return existing;

    if (r->texture_count >= R_MAX_TEXTURES) {
        OL_WARN("Texture cache full! Cannot upload '%s'\n", name); return 0;
    }

    GLuint handle;
    glGenTextures(1, &handle);
    glBindTexture(GL_TEXTURE_2D, handle);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);

    u32 idx = r->texture_count++;
    r->textures[idx].handle = handle;
    r->textures[idx].width  = w;
    r->textures[idx].height = h;
    snprintf(r->textures[idx].name, sizeof(r->textures[idx].name), "%s", name);
    return idx + 1; /* 1-based index, 0 = "no texture" */
}

void renderer_upload_video(Renderer *r, u32 *slot, const u8 *rgba, int w, int h) {
    if (!slot || !rgba) return;
    if (*slot == 0) {
        if (r->texture_count >= R_MAX_TEXTURES) return;
        GLuint handle;
        glGenTextures(1, &handle);
        glBindTexture(GL_TEXTURE_2D, handle);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        u32 idx = r->texture_count++;
        r->textures[idx].handle = handle;
        r->textures[idx].width  = (u32)w;
        r->textures[idx].height = (u32)h;
        snprintf(r->textures[idx].name, sizeof(r->textures[idx].name), "$video%u", idx);
        *slot = idx + 1;
    } else if (*slot <= r->texture_count) {
        glBindTexture(GL_TEXTURE_2D, r->textures[*slot - 1].handle);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

u32 renderer_find_texture(const Renderer *r, const char *name) {
    char lower[64]; snprintf(lower, sizeof(lower), "%s", name);
    for (char *p = lower; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    for (u32 i = 0; i < r->texture_count; i++) {
        if (strcmp(r->textures[i].name, lower) == 0) return i + 1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Animated texture management
 * ---------------------------------------------------------------------- */

void renderer_add_anim_texture(Renderer *r, u32 base_tex,
                               const GLuint *frame_handles, u32 count, f32 fps,
                               bool loop, u32 loop_start) {
    /* A single-frame static ATX (STOP after frame 0) still needs to pin its
     * base texture to frame 0, but has nothing to animate. */
    if (!base_tex || count < 1 || fps <= 0.0f) return;
    if (r->anim_texture_count >= R_MAX_ANIM_TEXTURES) {
        OL_WARN("AnimTexture table full\n"); return;
    }
    AnimTexture *at = &r->anim_textures[r->anim_texture_count++];
    memset(at, 0, sizeof(*at));
    at->base_tex = base_tex;
    u32 n = (count < R_MAX_ANIM_FRAMES) ? count : R_MAX_ANIM_FRAMES;
    for (u32 i = 0; i < n; i++) at->frame_handles[i] = frame_handles[i];
    at->frame_count   = n;
    at->fps           = fps;
    at->current_frame = 0;
    at->loop          = loop && n >= 2;
    at->loop_start    = (loop_start < n) ? loop_start : 0;
}

void renderer_register_window_break(Renderer *r, u32 base_tex,
                                     const u32 *frame_tex, u32 count, f32 fps) {
    if (!base_tex || count < 2) return;   /* need at least intact + 1 break frame */
    if (r->window_break_count >= 16) return;
    /* Dedup by base_tex (all windows of a type share one ATX). */
    for (u32 i = 0; i < r->window_break_count; i++)
        if (r->window_breaks[i].base_tex == base_tex) return;
    u32 n = count < 8 ? count : 8;
    u32 k = r->window_break_count++;
    r->window_breaks[k].base_tex = base_tex;
    r->window_breaks[k].frame_count = n;
    r->window_breaks[k].fps = (fps > 0.0f) ? fps : 12.0f;
    for (u32 i = 0; i < n; i++) r->window_breaks[k].frame_tex[i] = frame_tex[i];
}

u32 renderer_window_frame_tex(const Renderer *r, u32 base_tex,
                              bool broken, f32 break_time) {
    for (u32 i = 0; i < r->window_break_count; i++) {
        if (r->window_breaks[i].base_tex != base_tex) continue;
        if (!broken) return r->window_breaks[i].frame_tex[0];   /* intact */
        /* Break frames are indices 1..count-1; play then hold the last. */
        u32 fi = 1u + (u32)(break_time * r->window_breaks[i].fps);
        if (fi >= r->window_breaks[i].frame_count) fi = r->window_breaks[i].frame_count - 1;
        return r->window_breaks[i].frame_tex[fi];
    }
    return base_tex;   /* not a registered window */
}

void renderer_update_anim_textures(Renderer *r, f32 dt) {
    for (u32 i = 0; i < r->anim_texture_count; i++) {
        AnimTexture *at = &r->anim_textures[i];
        if (at->frame_count < 2 || at->fps <= 0.0f) continue;

        /* Static (STOP) textures hold frame 0; only looping ones advance. */
        if (at->loop) {
            at->timer += dt;
            f32 frame_time = 1.0f / at->fps;
            u32 span = at->frame_count - at->loop_start;
            if (span < 1) span = 1;
            while (at->timer >= frame_time) {
                at->timer -= frame_time;
                at->current_frame++;
                if (at->current_frame >= at->frame_count)
                    at->current_frame = at->loop_start;
            }
        } else {
            at->current_frame = 0;
        }

        /* Swap the GL handle in the base texture slot to the current frame */
        u32 bt = at->base_tex;
        if (bt > 0 && bt <= r->texture_count)
            r->textures[bt - 1].handle = at->frame_handles[at->current_frame];
    }
}

/* -------------------------------------------------------------------------
 * Level geometry building
 *
 * For each sector we generate:
 *   - Floor polygon (triangle fan)
 *   - Ceiling polygon (triangle fan, reversed winding)
 *   - Wall quads (one per wall segment, or multiple for portal walls)
 *
 * Texture coordinates:
 *   Walls: u = distance along wall / texture_width, v = height / texture_height
 *   Floor/Ceiling: u = x / 64, v = z / 64 (world units per tile)
 *
 * Coordinate system conversion (LVT → OpenGL):
 *   LVT: X right, Z forward, Y up
 *   GL:  X right, Y up, Z out of screen
 *   We map: LVT(X, Y, Z) → GL(X, Y, -Z)
 * ---------------------------------------------------------------------- */

/* Texture repeat scale: 1 world unit = 1/64 texture tiles */
/* Jedi Engine: 1 world unit = 8 texels.
 * UV = world_pos * 8 / texture_pixel_dimension.
 * For 64px texture: UV = pos * 8/64 = pos/8 → repeats every 8 wu.
 * For 128px texture: UV = pos * 8/128 = pos/16 → repeats every 16 wu.
 * WU_TO_TEXEL converts world units to texels (multiply by 8). */
#define WU_TO_TEXEL 8.0f

typedef struct {
    WorldVertex *verts;
    u32         *indices;
    u32          vert_count, vert_cap;
    u32          idx_count,  idx_cap;
    u32          tex_id;
} MeshBuilder;

static void mb_init(MeshBuilder *mb, u32 tex_id) {
    memset(mb, 0, sizeof(*mb));
    mb->tex_id = tex_id;
}

static void mb_push_vert(MeshBuilder *mb, WorldVertex v) {
    if (mb->vert_count >= mb->vert_cap) {
        mb->vert_cap = mb->vert_cap ? mb->vert_cap * 2 : 64;
        mb->verts = realloc(mb->verts, mb->vert_cap * sizeof(WorldVertex));
    }
    mb->verts[mb->vert_count++] = v;
}

static void mb_push_idx(MeshBuilder *mb, u32 idx) {
    if (mb->idx_count >= mb->idx_cap) {
        mb->idx_cap = mb->idx_cap ? mb->idx_cap * 2 : 64;
        mb->indices = realloc(mb->indices, mb->idx_cap * sizeof(u32));
    }
    mb->indices[mb->idx_count++] = idx;
}

/* Upload a builder to GPU. If `reuse` has live GL objects (from the previous
 * build), re-upload into them (buffer orphaning) instead of creating new VAO/VBO/
 * IBO — creating/destroying hundreds of GL objects each rebuild stalls the driver
 * (~36ms on civlwar1); reuse makes a dynamic-INF rebuild cheap. The builder set is
 * stable across rebuilds (same geometry, only vertex data changes), so reusing the
 * k-th mesh's buffers is valid; the VAO attrib pointers stay bound to the VBO. */
static RenderMesh mb_upload(MeshBuilder *mb, const RenderMesh *reuse) {
    RenderMesh mesh = {0};
    if (!mb->vert_count || !mb->idx_count) return mesh;

    bool reused = reuse && reuse->vao;
    if (reused) { mesh.vao = reuse->vao; mesh.vbo = reuse->vbo; mesh.ibo = reuse->ibo; }
    else { glGenVertexArrays(1, &mesh.vao); glGenBuffers(1, &mesh.vbo); glGenBuffers(1, &mesh.ibo); }

    glBindVertexArray(mesh.vao);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER, mb->vert_count * sizeof(WorldVertex),
                 mb->verts, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, mb->idx_count * sizeof(u32),
                 mb->indices, GL_DYNAMIC_DRAW);

    if (!reused) {   /* attrib layout only needs setting up once per VAO */
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(WorldVertex),
                              (void*)offsetof(WorldVertex, x));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(WorldVertex),
                              (void*)offsetof(WorldVertex, u));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(WorldVertex),
                              (void*)offsetof(WorldVertex, light));
        glEnableVertexAttribArray(2);
    }
    glBindVertexArray(0);

    mesh.index_count = mb->idx_count;
    mesh.tex_id      = mb->tex_id;
    free(mb->verts); free(mb->indices);
    return mesh;
}

/*
 * Build a textured wall quad between two vertices at given floor/ceiling Y.
 * Adds 4 vertices and 6 indices (2 triangles) to the builder.
 */
/*
 * Emit one wall strip as a TRAPEZOID: the bottom edge runs y_bot0..y_bot1 and
 * the top edge y_top0..y_top1, so the wall follows SLOPED floors/ceilings at
 * each endpoint and meets the sloped flats with no gap (the flat rectangle this
 * replaced left "sky-bleed" triangles on slope-heavy maps like CANYON).
 * V is anchored per-column to the strip's top (top-pegged) or bottom
 * (bottom-pegged) so the texture flows correctly down the slope.
 */
static void build_wall_quad(MeshBuilder *mb,
                             f32 x0, f32 z0, f32 x1, f32 z1,
                             f32 y_bot0, f32 y_top0, f32 y_bot1, f32 y_top1,
                             f32 u_off, f32 v_off,
                             f32 ambient_norm, u32 flags,
                             u32 tex_w, u32 tex_h) {
    f32 dx = x1 - x0, dz = z1 - z0;
    f32 wall_len = sqrtf(dx*dx + dz*dz);

    /* UV scale: 1 world unit = 8 texels. UV = world * 8 / tex_dim. */
    f32 u_scale = (tex_w > 0) ? (WU_TO_TEXEL / (f32)tex_w) : (WU_TO_TEXEL / 64.0f);
    f32 v_scale = (tex_h > 0) ? (WU_TO_TEXEL / (f32)tex_h) : (WU_TO_TEXEL / 64.0f);

    /* U runs along the wall from v1 (u_off) to v2 (u_off+len); v1 is the
     * screen-left endpoint seen from the sector interior (TFE rwallFloat
     * wall_process: uCoord0 anchors at w0/LEFT, backface test guarantees
     * w0 projects left of w1). WF1_FLIP_HORIZ (0x04) mirrors in TEXTURE space:
     * texelU -> texW-1-texelU (rwallFloat.cpp:838-840), i.e. u -> 1-u in
     * normalized coords — NOT an endpoint swap (a swap mirrors around the
     * quad's own span and shifts the texture phase). */
    f32 u0 = u_off * u_scale;
    f32 u1 = (u_off + wall_len) * u_scale;
    if (flags & 0x04u) { u0 = 1.0f - u0; u1 = 1.0f - u1; }

    /* Jedi/Outlaws walls are BOTTOM-anchored with V running UPWARD from the
     * strip's bottom edge: V = ((worldY - stripBottomY) + v_off) * 8/texH.
     * (Verified: TFE rwallFloat.cpp anchors at the floor pixel; Outlaws
     * Wall_ComputeOpenings@0x4e4600 uses (top-bottom)*8 texel spans.) The raw
     * LVT v_offset is used as-is — the engine never subtracts a sector height,
     * so all the old `- floor_y` / `- adj_ceil` call-site hacks are gone. */
    f32 v_b0 = v_off * v_scale;
    f32 v_t0 = ((y_top0 - y_bot0) + v_off) * v_scale;
    f32 v_b1 = v_off * v_scale;
    f32 v_t1 = ((y_top1 - y_bot1) + v_off) * v_scale;

    u32 base = mb->vert_count;
    WorldVertex verts[4] = {
        { x0, y_bot0, -z0, u0, v_b0, ambient_norm },
        { x1, y_bot1, -z1, u1, v_b1, ambient_norm },
        { x1, y_top1, -z1, u1, v_t1, ambient_norm },
        { x0, y_top0, -z0, u0, v_t0, ambient_norm },
    };
    for (int i = 0; i < 4; i++) mb_push_vert(mb, verts[i]);

    /* CW triangles (interior-facing after LVT→GL Z-negation flips winding) */
    mb_push_idx(mb, base+0); mb_push_idx(mb, base+2); mb_push_idx(mb, base+1);
    mb_push_idx(mb, base+0); mb_push_idx(mb, base+3); mb_push_idx(mb, base+2);
}

/*
 * Wall SIGN overlay (LVT OVERLAY slot) — Ghidra wall_FillZBuffer_Textured
 * @0x4c71c0 + TFE setupSignTexture (rwallFloat.cpp:711-740):
 *   - Drawn where the host wall part's texel U (incl. the part's own u
 *     offset, measured from v1) lies in [signU0 .. signU0+signW], with
 *     signU0 = overlay.u*8. The sign is NEVER mirrored by WF1_FLIP_HORIZ.
 *   - V anchors at the part's bottom edge; positive overlay.v moves the sign
 *     DOWN by overlay.v world units; the sign spans texH/8 wu upward and is
 *     clipped strictly to the part (no tiling).
 *   - Fullbright when wall flag 0x2 (WF1_ILLUM_SIGN) is set.
 * The quad is emitted into a dedicated sign builder and drawn with polygon
 * offset over the coplanar wall part (the software renderer overdraws the
 * same columns; depth-offset is the GL equivalent).
 *
 * sec_u_off: the HOST PART's texture u offset (mid/top/bot slot).
 * yb0/yb1, yt0/yt1: the host part's bottom/top edges at the wall endpoints.
 */
static void build_sign_quad(MeshBuilder *mb,
                            f32 x0, f32 z0, f32 x1, f32 z1,
                            f32 sec_u_off, const WallTex *ov,
                            f32 yb0, f32 yt0, f32 yb1, f32 yt1,
                            f32 light, u32 tex_w, u32 tex_h) {
    f32 dx = x1 - x0, dz = z1 - z0;
    f32 wall_len = sqrtf(dx*dx + dz*dz);
    if (tex_w == 0 || tex_h == 0 || wall_len <= 0.0f) return;

    f32 sw = (f32)tex_w / WU_TO_TEXEL;   /* sign extent in world units */
    f32 sh = (f32)tex_h / WU_TO_TEXEL;

    /* Horizontal window along the wall (distance from v1). */
    f32 d0 = ov->offset.u - sec_u_off;
    f32 d1 = d0 + sw;
    f32 u0 = 0.0f, u1 = 1.0f;
    if (d0 < 0.0f)      { u0 = -d0 / sw;                  d0 = 0.0f; }
    if (d1 > wall_len)  { u1 = 1.0f - (d1 - wall_len)/sw; d1 = wall_len; }
    if (d1 <= d0) return;

    f32 t0 = d0 / wall_len, t1 = d1 / wall_len;
    f32 ax = x0 + dx*t0, az = z0 + dz*t0;
    f32 bx = x0 + dx*t1, bz = z0 + dz*t1;

    /* Vertical extent per endpoint: part bottom edge (lerped) - overlay.v,
     * clipped to the part span, v texels adjusted proportionally.
     * GL v=0 = image bottom row (level textures are uploaded row-flipped). */
    f32 partB0 = yb0 + (yb1 - yb0)*t0, partB1 = yb0 + (yb1 - yb0)*t1;
    f32 partT0 = yt0 + (yt1 - yt0)*t0, partT1 = yt0 + (yt1 - yt0)*t1;
    f32 b0 = partB0 - ov->offset.v, b1 = partB1 - ov->offset.v;
    f32 tp0 = b0 + sh,              tp1 = b1 + sh;
    f32 vb0 = 0.0f, vb1 = 0.0f, vt0 = 1.0f, vt1 = 1.0f;
    if (b0 < partB0)  { vb0 = (partB0 - b0)/sh;        b0 = partB0; }
    if (b1 < partB1)  { vb1 = (partB1 - b1)/sh;        b1 = partB1; }
    if (tp0 > partT0) { vt0 = 1.0f - (tp0 - partT0)/sh; tp0 = partT0; }
    if (tp1 > partT1) { vt1 = 1.0f - (tp1 - partT1)/sh; tp1 = partT1; }
    if (tp0 <= b0 || tp1 <= b1) return;

    u32 base = mb->vert_count;
    WorldVertex verts[4] = {
        { ax, b0,  -az, u0, vb0, light },
        { bx, b1,  -bz, u1, vb1, light },
        { bx, tp1, -bz, u1, vt1, light },
        { ax, tp0, -az, u0, vt0, light },
    };
    for (int i = 0; i < 4; i++) mb_push_vert(mb, verts[i]);
    mb_push_idx(mb, base+0); mb_push_idx(mb, base+2); mb_push_idx(mb, base+1);
    mb_push_idx(mb, base+0); mb_push_idx(mb, base+3); mb_push_idx(mb, base+2);
}

/*
 * Compute per-vertex Y for a sloped floor/ceiling.
 * The slope is defined by a pivot wall edge and an angle.
 * Angle is in 14-bit fixed point: 16384 = 90 degrees.
 */
static f32 slope_y_at(const LvtSector *sec, f32 base_y,
                       i32 wall_idx, i32 angle_fixed,
                       f32 vx, f32 vz) {
    /* Exact Outlaws/TFE slope plane — shared with collision (lvt.c). */
    return lvt_slope_height(sec, base_y, wall_idx, angle_fixed, vx, vz);
}

/*
 * Build a textured floor/ceiling polygon (triangle fan).
 * verts_xz: array of Vec2 (X,Z) in sector order
 * y: base height of the plane
 * flip_winding: true for ceiling (reverse face direction)
 * sec: sector pointer (for slope computation, may be NULL)
 * is_floor: true for floor, false for ceiling
 */
static void build_floor_poly(MeshBuilder *mb,
                              const Vec2 *verts_xz, u32 count,
                              f32 y, f32 u_off, f32 v_off,
                              bool flip_winding, f32 ambient_norm,
                              const LvtSector *sec, bool is_floor,
                              u32 tex_w, u32 tex_h) {
    if (count < 3) return;

    /* Check for slope */
    bool has_slope = false;
    i32 slope_wall = 0, slope_angle = 0;
    if (sec) {
        if (is_floor && sec->has_slope_floor) {
            has_slope = true;
            slope_wall  = sec->slope_floor_wall;
            slope_angle = sec->slope_floor_angle;
        } else if (!is_floor && sec->has_slope_ceil) {
            has_slope = true;
            slope_wall  = sec->slope_ceil_wall;
            slope_angle = sec->slope_ceil_angle;
        }
    }

    /* Outlaws flat texel mapping (Ghidra flat_FillZBuffer_Floor@0x4b4bb0 /
     * _Ceiling@0x4b42a0, identical formulas for both):
     *   U_texel =  8*((z-offZ)*cos(rot) - (x-offX)*sin(rot))
     *   V_texel = -8*((x-offX)*cos(rot) + (z-offZ)*sin(rot))
     * where U indexes WITHIN a stored column (bottom-up rows after the
     * row-flipped upload → GL v) and V indexes ACROSS columns (image x →
     * GL u). At rot=0 this is GLu = (offX-x)*8/texW, GLv = (z-offZ)*8/texH
     * — matching Dark Forces/TFE; the rotation is an Outlaws addition. */
    f32 rot_deg  = sec ? (is_floor ? sec->floor_rot_deg : sec->ceil_rot_deg) : 0.0f;
    f32 cr = 1.0f, sr = 0.0f;
    if (rot_deg != 0.0f) {
        f32 a = rot_deg * OL_DEG2RAD;
        cr = cosf(a); sr = sinf(a);
    }
    f32 fu_scale = (tex_w > 0) ? (WU_TO_TEXEL / (f32)tex_w) : (WU_TO_TEXEL / 64.0f);
    f32 fv_scale = (tex_h > 0) ? (WU_TO_TEXEL / (f32)tex_h) : (WU_TO_TEXEL / 64.0f);

    u32 base = mb->vert_count;
    for (u32 i = 0; i < count; i++) {
        WorldVertex v;
        v.x = verts_xz[i].x;
        v.y = has_slope
              ? slope_y_at(sec, y, slope_wall, slope_angle,
                           verts_xz[i].x, verts_xz[i].y)
              : y;
        v.z = -verts_xz[i].y;  /* LVT Z → GL -Z */
        f32 dx = verts_xz[i].x - u_off;   /* world X - offX */
        f32 dz = verts_xz[i].y - v_off;   /* world Z - offZ */
        v.u = -(dx*cr + dz*sr) * fu_scale;
        v.v =  (dz*cr - dx*sr) * fv_scale;
        v.light = ambient_norm;
        mb_push_vert(mb, v);
    }

    /* Ear-clipping tessellation for concave polygon support.
     * Falls back to triangle fan if ear-clipping stalls. */
    {
        u32 idx[LVT_MAX_VERTICES];
        u32 n = count;
        if (n > LVT_MAX_VERTICES) n = LVT_MAX_VERTICES;
        for (u32 i = 0; i < n; i++) idx[i] = i;

        /* Signed area to determine winding */
        f32 area2 = 0;
        for (u32 i = 0; i < n; i++) {
            u32 j = (i + 1) % n;
            area2 += verts_xz[i].x * verts_xz[j].y
                   - verts_xz[j].x * verts_xz[i].y;
        }
        bool ccw = (area2 > 0);

        u32 fail_count = 0;
        while (n > 2) {
            bool found = false;
            for (u32 i = 0; i < n; i++) {
                u32 p = (i + n - 1) % n;
                u32 nx = (i + 1) % n;
                f32 ax = verts_xz[idx[p]].x,  ay = verts_xz[idx[p]].y;
                f32 bx = verts_xz[idx[i]].x,  by = verts_xz[idx[i]].y;
                f32 cx = verts_xz[idx[nx]].x, cy = verts_xz[idx[nx]].y;

                f32 cross = (bx-ax)*(cy-ay) - (by-ay)*(cx-ax);
                /* Convex check with small epsilon to handle collinear */
                bool convex = ccw ? (cross > -1e-6f) : (cross < 1e-6f);
                if (!convex) continue;

                /* Skip near-degenerate triangles */
                if (fabsf(cross) < 1e-8f) continue;

                /* Check no other vertex strictly inside this triangle */
                bool ear = true;
                for (u32 k = 0; k < n && ear; k++) {
                    if (k == p || k == i || k == nx) continue;
                    f32 px = verts_xz[idx[k]].x, py = verts_xz[idx[k]].y;
                    f32 d1 = (px-ax)*(by-ay) - (bx-ax)*(py-ay);
                    f32 d2 = (px-bx)*(cy-by) - (cx-bx)*(py-by);
                    f32 d3 = (px-cx)*(ay-cy) - (ax-cx)*(py-cy);
                    bool all_neg = (d1 <= 0) && (d2 <= 0) && (d3 <= 0);
                    bool all_pos = (d1 >= 0) && (d2 >= 0) && (d3 >= 0);
                    if (all_neg || all_pos) ear = false;
                }
                if (!ear) continue;

                if (flip_winding) {
                    mb_push_idx(mb, base + idx[p]);
                    mb_push_idx(mb, base + idx[nx]);
                    mb_push_idx(mb, base + idx[i]);
                } else {
                    mb_push_idx(mb, base + idx[p]);
                    mb_push_idx(mb, base + idx[i]);
                    mb_push_idx(mb, base + idx[nx]);
                }
                for (u32 j = i; j + 1 < n; j++) idx[j] = idx[j + 1];
                n--;
                found = true;
                fail_count = 0;
                break;
            }
            if (!found) {
                fail_count++;
                if (fail_count > n) {
                    /* Fallback: fan remaining vertices to avoid holes */
                    for (u32 i = 1; i + 1 < n; i++) {
                        if (flip_winding) {
                            mb_push_idx(mb, base + idx[0]);
                            mb_push_idx(mb, base + idx[i + 1]);
                            mb_push_idx(mb, base + idx[i]);
                        } else {
                            mb_push_idx(mb, base + idx[0]);
                            mb_push_idx(mb, base + idx[i]);
                            mb_push_idx(mb, base + idx[i + 1]);
                        }
                    }
                    break;
                }
            }
        }
    }

}

/* Resolve texture id (1-based, 0 = no texture). Clamps to valid range. */
static u32 resolve_tex(const Renderer *r, const LvtLevel *level, i32 tex_idx) {
    if (tex_idx < 0 || (u32)tex_idx >= level->texture_count) return 0;
    u32 id = (tex_idx < LVT_MAX_TEXTURES) ? r->texmap[tex_idx]   /* O(1) cached */
                                          : renderer_find_texture(r, level->textures[tex_idx]);
    return (id < R_MAX_TEXTURES) ? id : 0;
}

/* Get texture pixel dimensions (0,0 if not found). */
static void tex_dims(const Renderer *r, u32 tex_id, u32 *w, u32 *h) {
    if (tex_id > 0 && tex_id <= r->texture_count) {
        *w = r->textures[tex_id - 1].width;
        *h = r->textures[tex_id - 1].height;
    } else {
        *w = 64; *h = 64;
    }
}

bool renderer_build_level(Renderer *r, const LvtLevel *level, const InfSystem *inf) {
    /* Keep the previous meshes' GL objects so this rebuild can re-upload into them
     * (buffer reuse) instead of churning VAOs/VBOs — see mb_upload(reuse). */
    RenderMesh *old_meshes = r->level_meshes;
    u32 old_count = r->level_mesh_count;
    r->level_meshes = NULL;
    r->level_mesh_count = 0;

    if (!level || level->sector_count == 0) {
        for (u32 i = 0; i < old_count; i++) {
            glDeleteVertexArrays(1, &old_meshes[i].vao);
            glDeleteBuffers(1, &old_meshes[i].vbo);
            glDeleteBuffers(1, &old_meshes[i].ibo);
        }
        free(old_meshes);
        return false;
    }

    /* DEFAULT.PCX now renders like any texture (the original engine draws the
     * slot's handle regardless). Only the MORPH-door leaf workaround still uses
     * this id, to prefer a real panel texture for the swinging apron. */
    u32 default_pcx_tex = renderer_find_texture(r, "default.pcx");

    /* Resolve each level texture index → GL id ONCE (not per wall/flat). */
    for (u32 i = 0; i < level->texture_count && i < LVT_MAX_TEXTURES; i++)
        r->texmap[i] = renderer_find_texture(r, level->textures[i]);

    /*
     * Texture-batched builders (one per texture slot).
     * Scroll-floor sectors get their own per-sector builders so UV offset
     * can be applied independently each frame.
     */
    MeshBuilder *builders = calloc(R_MAX_TEXTURES + 1, sizeof(MeshBuilder));
    if (!builders) return false;

    /* Builder 0 = no texture (debug purple) */
    mb_init(&builders[0], 0);
    for (u32 i = 1; i <= r->texture_count; i++) mb_init(&builders[i], i);

    /* Wall sign overlays get their own builders: they are coplanar with the
     * wall parts they decorate and are drawn after them with polygon offset. */
    MeshBuilder *sign_builders = calloc(R_MAX_TEXTURES + 1, sizeof(MeshBuilder));
    if (!sign_builders) { free(builders); return false; }
    for (u32 i = 0; i <= r->texture_count; i++) mb_init(&sign_builders[i], i);

    /* Per-sector scroll floor builders (one per scroll-floor sector).
     * Indexed as scroll_si[i] = sector index, scroll_mb[i] = its builder. */
    u32        scroll_si[INF_MAX_ELEVS];
    MeshBuilder scroll_mb[INF_MAX_ELEVS];
    u32        scroll_count = 0;

    u32 dbg_floors = 0;

    for (u32 si = 0; si < level->sector_count; si++) {
        const LvtSector *sec = &level->sectors[si];
        f32 ambient = OL_CLAMP((f32)sec->ambient / 31.0f, 0.0f, 1.0f);

        bool is_scroll = inf && inf_is_scroll_floor(inf, si);

        /* Floor and ceiling polygons.
         * ALWAYS trace wall chains to find individual polygon loops.
         * Multi-loop sectors (99 in TOWN) have disconnected wall loops
         * sharing the same vertex array — using raw vertex order creates
         * self-intersecting triangle fans that cover adjacent sectors' floors.
         * Rendering each loop separately avoids this. */
        if (sec->vertex_count >= 3 && sec->wall_count >= 3) {
            /* Trace wall chains to find ALL loops */
            Vec2 traced[LVT_MAX_VERTICES];
            u32  loff[64], lsz[64], lcnt = 0;
            u32  toff = 0;
            bool wused[LVT_MAX_WALLS];
            memset(wused, 0, sec->wall_count * sizeof(bool));

            for (u32 st = 0; st < sec->wall_count && lcnt < 64; st++) {
                if (wused[st]) continue;
                u32 lv = 0, cur = st;
                for (u32 s = 0; s < sec->wall_count; s++) {
                    if (wused[cur]) break;
                    wused[cur] = true;
                    i32 vi = sec->walls[cur].v1;
                    if (vi >= 0 && vi < (i32)sec->vertex_count && toff + lv < LVT_MAX_VERTICES)
                        traced[toff + lv++] = sec->vertices[vi];
                    i32 v2 = sec->walls[cur].v2;
                    bool found = false;
                    for (u32 nx = 0; nx < sec->wall_count; nx++) {
                        if (!wused[nx] && sec->walls[nx].v1 == v2) {
                            cur = nx; found = true; break;
                        }
                    }
                    if (!found) break;
                }
                if (lv >= 3) {
                    loff[lcnt] = toff;
                    lsz[lcnt] = lv;
                    lcnt++;
                    toff += lv;
                }
            }

            /* Render floor for EVERY loop — except SKY floors (flag bit 1,
             * Ghidra sky_DrawFloor@0x4b6410: bottomless pit showing sky). */
            u32 ftex = (sec->flags & LVT_SEC_FLAG_SKY_FLOOR) ? 0
                       : resolve_tex(r, level, sec->floor_tex);
            if (ftex > 0) {
                u32 ftw, fth; tex_dims(r, ftex, &ftw, &fth);
                for (u32 li = 0; li < lcnt; li++) {
                    dbg_floors++;
                    if (is_scroll && scroll_count < INF_MAX_ELEVS) {
                        mb_init(&scroll_mb[scroll_count], ftex);
                        scroll_si[scroll_count] = si;
                        build_floor_poly(&scroll_mb[scroll_count],
                                         &traced[loff[li]], lsz[li],
                                         sec->floor_y, sec->floor_offset.u, sec->floor_offset.v,
                                         true, ambient, sec, true, ftw, fth);
                        scroll_count++;
                    } else {
                        build_floor_poly(&builders[ftex],
                                         &traced[loff[li]], lsz[li],
                                         sec->floor_y, sec->floor_offset.u, sec->floor_offset.v,
                                         true, ambient, sec, true, ftw, fth);
                    }
                }
            }

            /* Render ceiling for EVERY loop — except SKY ceilings (flag bit 0,
             * Ghidra sky_DrawCeiling@0x4b6190): the parallax sky pass shows
             * through instead of a textured plane. */
            if (!(sec->flags & LVT_SEC_FLAG_SKY_CEIL)) {
                u32 ctex = resolve_tex(r, level, sec->ceil_tex);
                if (ctex > 0) {
                    u32 ctw, cth; tex_dims(r, ctex, &ctw, &cth);
                    for (u32 li = 0; li < lcnt; li++) {
                        build_floor_poly(&builders[ctex],
                                         &traced[loff[li]], lsz[li],
                                         sec->ceil_y, sec->ceil_offset.u, sec->ceil_offset.v,
                                         false, ambient, sec, false, ctw, cth);
                    }
                }
            }
        }

        /* Walls */
        for (u32 wi = 0; wi < sec->wall_count; wi++) {
            const LvtWall *wall = &sec->walls[wi];
            if (wall->v1 < 0 || wall->v1 >= (i32)sec->vertex_count ||
                wall->v2 < 0 || wall->v2 >= (i32)sec->vertex_count) continue;

            f32 x0 = sec->vertices[wall->v1].x;
            f32 z0 = sec->vertices[wall->v1].y;
            f32 x1 = sec->vertices[wall->v2].x;
            f32 z1 = sec->vertices[wall->v2].y;

            /* NOTE: 0x20000 was previously treated as "SKY_BOUNDARY: skip this
             * wall" — a GUESS (RENDER_ANALYSIS flagged it unconfirmed). But
             * 12% of walls carry it, almost all adjoin portals with real
             * textures; skipping them left huge floor-line sky-bleed (HIDEOUT
             * house/courtyard). They render as normal geometry-driven portal
             * walls; sky ceilings/floors are handled by the sky_both height
             * rule below, not by dropping whole walls. */

            /* This sector's floor/ceiling at each wall endpoint (slope-aware) so
             * wall strips are trapezoids that meet the sloped flats seamlessly. */
            f32 sF0 = lvt_floor_at(sec, x0, z0), sF1 = lvt_floor_at(sec, x1, z1);
            f32 sC0 = lvt_ceil_at (sec, x0, z0), sC1 = lvt_ceil_at (sec, x1, z1);

            u32 mid_tex = resolve_tex(r, level, wall->mid.tex_id);
            u32 top_tex = resolve_tex(r, level, wall->top.tex_id);
            u32 bot_tex = resolve_tex(r, level, wall->bot.tex_id);

            f32 wall_light = OL_CLAMP(ambient + wall->light / 31.0f, 0.0f, 1.0f);

            /* Skip degenerate zero-length walls */
            { f32 wdx = x1-x0, wdz = z1-z0;
              if (wdx*wdx + wdz*wdz < 0.001f) continue; }

            /* Wall sign overlay (OVERLAY slot). Fullbright when flag 0x2
             * (WF1_ILLUM_SIGN; Ghidra: colormap-less blitter @0x4d0e10). */
            u32 ov_tex = (wall->overlay.tex_id >= 0)
                         ? resolve_tex(r, level, wall->overlay.tex_id) : 0;
            u32 ov_w = 0, ov_h = 0;
            if (ov_tex > 0) tex_dims(r, ov_tex, &ov_w, &ov_h);
            f32 sign_light = (wall->flags & 0x2u) ? 1.0f : wall_light;
            /* Emits the sign onto one host wall part (per-part anchor). */
            #define EMIT_SIGN(part_u_off, b0, t0, b1, t1)                     \
                do { if (ov_tex > 0)                                          \
                    build_sign_quad(&sign_builders[ov_tex], x0, z0, x1, z1,   \
                                    (part_u_off), &wall->overlay,             \
                                    (b0), (t0), (b1), (t1),                   \
                                    sign_light, ov_w, ov_h); } while (0)

            if (wall->adjoin < 0) {
                /* Solid wall: one MID quad, floor to ceiling, raw LVT offsets. */
                if (mid_tex > 0) {
                    u32 mw, mh; tex_dims(r, mid_tex, &mw, &mh);
                    build_wall_quad(&builders[mid_tex],
                                    x0, z0, x1, z1,
                                    sF0, sC0, sF1, sC1,
                                    wall->mid.offset.u, wall->mid.offset.v,
                                    wall_light, wall->flags, mw, mh);
                    EMIT_SIGN(wall->mid.offset.u, sF0, sC0, sF1, sC1);
                }
            } else {
                /* Portal wall: render TOP and BOT strips */
                const LvtSector *adj = (wall->adjoin < (i32)level->sector_count)
                                       ? &level->sectors[wall->adjoin] : NULL;
                f32 adj_floor = adj ? adj->floor_y : sec->floor_y;
                f32 adj_ceil  = adj ? adj->ceil_y  : sec->ceil_y;
                /* Adjoin floor/ceil at each endpoint (slope-aware) for the strips. */
                f32 aF0 = adj ? lvt_floor_at(adj, x0, z0) : sF0;
                f32 aF1 = adj ? lvt_floor_at(adj, x1, z1) : sF1;
                f32 aC0 = adj ? lvt_ceil_at (adj, x0, z0) : sC0;
                f32 aC1 = adj ? lvt_ceil_at (adj, x1, z1) : sC1;

                if (wall->dadjoin >= 0 &&
                    wall->dadjoin < (i32)level->sector_count) {
                    /* DADJOIN portal: 3-sector vertical split.
                     * Ghidra RE of Wall_ComputeOpenings: the wall has TWO
                     * portal openings and up to 3 solid strips:
                     *   TOP:  adj_ceil  → sec_ceil   (above upper portal)
                     *   MID:  dadj_ceil → adj_floor   (between the two portals)
                     *   BOT:  sec_floor → dadj_floor  (below lower portal)
                     * Upper opening: adj_floor → adj_ceil  (into adjoin sector)
                     * Lower opening: dadj_floor → dadj_ceil (into dadjoin sector)
                     *
                     * BANK example: sec 603 (floor=0,ceil=24) → adj 38 (floor=7.5)
                     *   + dadj 44 (floor=0,ceil=3.5)
                     *   Lower opening: 0→3.5 (into sec 44, ground level)
                     *   MID strip: 3.5→7.5 (door frame, MID texture)
                     *   Upper opening: 7.5→24 (into sec 38, upper level) */
                    const LvtSector *dadj = &level->sectors[wall->dadjoin];
                    f32 dadj_floor = dadj->floor_y;
                    f32 dadj_ceil  = dadj->ceil_y;
                    f32 dF0 = lvt_floor_at(dadj, x0, z0), dF1 = lvt_floor_at(dadj, x1, z1);
                    f32 dC0 = lvt_ceil_at (dadj, x0, z0), dC1 = lvt_ceil_at (dadj, x1, z1);

                    /* TOP strip: adj_ceil → sec_ceil (above the upper opening) */
                    if (adj_ceil < sec->ceil_y && top_tex > 0 && top_tex != default_pcx_tex &&
                        !(sec->flags & adj->flags & LVT_SEC_FLAG_SKY_CEIL)) {
                        u32 tw, th; tex_dims(r, top_tex, &tw, &th);
                        build_wall_quad(&builders[top_tex], x0, z0, x1, z1,
                                        aC0, sC0, aC1, sC1,
                                        wall->top.offset.u, wall->top.offset.v,
                                        wall_light, wall->flags, tw, th);
                        EMIT_SIGN(wall->top.offset.u, aC0, sC0, aC1, sC1);
                    }
                    /* MID strip: dadj_ceil → adj_floor (between the two openings) */
                    if (dadj_ceil < adj_floor && mid_tex > 0 && mid_tex != default_pcx_tex) {
                        u32 mw, mh; tex_dims(r, mid_tex, &mw, &mh);
                        build_wall_quad(&builders[mid_tex], x0, z0, x1, z1,
                                        dC0, aF0, dC1, aF1,
                                        wall->mid.offset.u, wall->mid.offset.v,
                                        wall_light, wall->flags, mw, mh);
                        EMIT_SIGN(wall->mid.offset.u, dC0, aF0, dC1, aF1);
                    }
                    /* BOT strip: sec_floor → dadj_floor (below the lower opening) */
                    if (dadj_floor > sec->floor_y && bot_tex > 0 && bot_tex != default_pcx_tex) {
                        u32 tw, th; tex_dims(r, bot_tex, &tw, &th);
                        build_wall_quad(&builders[bot_tex], x0, z0, x1, z1,
                                        sF0, dF0, sF1, dF1,
                                        wall->bot.offset.u, wall->bot.offset.v,
                                        wall_light, wall->flags, tw, th);
                        EMIT_SIGN(wall->bot.offset.u, sF0, dF0, sF1, dF1);
                    }
                } else {
                    /* Regular portal (no DADJOIN): TOP/BOT strips.
                     * Jedi sky rule (TFE wall_drawTop/Bottom): when BOTH sectors
                     * have a sky ceiling (resp. sky floor), the step strip is
                     * not textured — the sky continues through it. */
                    bool sky_both_c = (sec->flags & adj->flags & LVT_SEC_FLAG_SKY_CEIL);
                    bool sky_both_f = (sec->flags & adj->flags & LVT_SEC_FLAG_SKY_FLOOR);
                    bool has_top = (adj_ceil < sec->ceil_y) && !sky_both_c;
                    bool has_bot = (adj_floor > sec->floor_y) && !sky_both_f;

                    /* Swinging door panels.
                     * A MORPH door leaf sector (e.g. HOTELDOOR01) sits at the top
                     * of the doorway (its floor is ABOVE the room floor); the
                     * visible door leaf is the BOT "apron" hanging from the leaf
                     * floor down to the room floor. It must be drawn from the LEAF
                     * (moving vertices) so it swings; and the same panel drawn
                     * from the static neighbour side must be skipped, else a fixed
                     * copy hides the animation. */
                    bool this_is_door = inf && inf_is_morph_door(inf, si);
                    bool adj_is_door  = inf && wall->adjoin >= 0 &&
                                        inf_is_morph_door(inf, (u32)wall->adjoin);
                    if (adj_is_door) {
                        /* Neighbour side of a door: the leaf draws its own panel. */
                        has_bot = false;
                    }
                    if (this_is_door && adj_floor < sec->floor_y) {
                        /* Draw the leaf apron from the room floor up to the leaf
                         * floor, using this (moving) sector's vertices. */
                        u32 t; f32 tu, tv;
                        if (bot_tex > 0 && bot_tex != default_pcx_tex) {
                            t = bot_tex; tu = wall->bot.offset.u; tv = wall->bot.offset.v;
                        } else if (mid_tex > 0 && mid_tex != default_pcx_tex) {
                            t = mid_tex; tu = wall->mid.offset.u; tv = wall->mid.offset.v;
                        } else { t = 0; tu = tv = 0; }
                        if (t > 0) {
                            u32 tw, th; tex_dims(r, t, &tw, &th);
                            build_wall_quad(&builders[t], x0, z0, x1, z1,
                                            aF0, sF0, aF1, sF1,
                                            tu, tv, wall_light, wall->flags, tw, th);
                        }
                    }

                    /* TOP strip: adj_ceil → sec_ceil */
                    if (has_top && top_tex > 0 && top_tex != default_pcx_tex) {
                        u32 tw, th; tex_dims(r, top_tex, &tw, &th);
                        build_wall_quad(&builders[top_tex], x0, z0, x1, z1,
                                        aC0, sC0, aC1, sC1,
                                        wall->top.offset.u, wall->top.offset.v,
                                        wall_light, wall->flags, tw, th);
                        EMIT_SIGN(wall->top.offset.u, aC0, sC0, aC1, sC1);
                    }
                    /* BOT strip: sec_floor → adj_floor. This lower strip IS the
                     * flag-door leaf when the adjoining sector is an auto door
                     * (its BOT texture is the door panel, e.g. HIDEOUT tex 12):
                     * as the door opens its bottom edge rises (door_slide 0→1) so
                     * it slides up out of the doorway, then vanishes. */
                    if (has_bot && bot_tex > 0 && bot_tex != default_pcx_tex) {
                        f32 dx0 = x0, dz0 = z0, dx1 = x1, dz1 = z1;
                        const LvtSector *da = &level->sectors[wall->adjoin];
                        if (da->is_flag_door && da->door_slide > 0.0f &&
                            da->vertex_count > 0) {
                            /* Hinged (swinging) door: this BOT strip is a face of
                             * the door leaf. Revolve the WHOLE leaf as one rigid
                             * slab about a single hinge corner (a fixed vertex of
                             * the door sector) so both faces swing together the
                             * same way — like the building's lower swing doors —
                             * instead of each face hinging on its own edge (which
                             * splayed them in opposite directions). Render-only;
                             * sector geometry/adjoins untouched. Flag 0x200 flips
                             * the swing direction. */
                            f32 ang = da->door_slide * 1.4835f;   /* 85° in radians */
                            if (sec->flags & LVT_SEC_FLAG_DOOR_DOWN) ang = -ang;
                            f32 ca = cosf(ang), sa = sinf(ang);
                            f32 hx = da->vertices[0].x, hz = da->vertices[0].y;
                            f32 e0x = x0 - hx, e0z = z0 - hz;
                            f32 e1x = x1 - hx, e1z = z1 - hz;
                            dx0 = hx + e0x*ca - e0z*sa; dz0 = hz + e0x*sa + e0z*ca;
                            dx1 = hx + e1x*ca - e1z*sa; dz1 = hz + e1x*sa + e1z*ca;
                        }
                        u32 tw, th; tex_dims(r, bot_tex, &tw, &th);
                        build_wall_quad(&builders[bot_tex], dx0, dz0, dx1, dz1,
                                        sF0, aF0, sF1, aF1,
                                        wall->bot.offset.u, wall->bot.offset.v,
                                        wall_light, wall->flags, tw, th);
                        EMIT_SIGN(wall->bot.offset.u, sF0, aF0, sF1, aF1);
                    }
                    /* MID mask overlay in the portal opening (maskwall: glass
                     * windows, fences, grilles). The ONLY flag that controls
                     * this is WF1_ADJ_MID_TEX = bit 0 (TFE rwall.h:25,
                     * rsectorFloat.cpp:488-492) — portal structure itself is
                     * pure geometry. A glass window renders its break-frame:
                     * intact frame 0, or (once shot) the shatter frame for its
                     * break_time (held on the final shattered frame) — it does
                     * NOT vanish. main.c rebuilds the mesh as break_time advances. */
                    u32 draw_mid = mid_tex;
                    if (wall->is_window)
                        draw_mid = renderer_window_frame_tex(r, mid_tex,
                                       wall->window_broken, wall->break_time);
                    if ((wall->flags & 0x01u) &&
                        draw_mid > 0 && draw_mid != default_pcx_tex) {
                        f32 open_bot = adj_floor > sec->floor_y ? adj_floor : sec->floor_y;
                        f32 open_top = adj_ceil  < sec->ceil_y  ? adj_ceil  : sec->ceil_y;
                        f32 ob0 = aF0 > sF0 ? aF0 : sF0, ob1 = aF1 > sF1 ? aF1 : sF1;
                        f32 ot0 = aC0 < sC0 ? aC0 : sC0, ot1 = aC1 < sC1 ? aC1 : sC1;
                        if (open_top > open_bot) {
                            u32 mw, mh; tex_dims(r, draw_mid, &mw, &mh);
                            build_wall_quad(&builders[draw_mid], x0, z0, x1, z1,
                                            ob0, ot0, ob1, ot1,
                                            wall->mid.offset.u, wall->mid.offset.v,
                                            wall_light, wall->flags, mw, mh);
                        }
                    }

                }
            }
        }
    }

    #undef EMIT_SIGN

    /* Count non-empty builders and upload */
    u32 mesh_count = 0;
    for (u32 i = 0; i <= r->texture_count; i++)
        if (builders[i].vert_count > 0) mesh_count++;
    for (u32 i = 0; i <= r->texture_count; i++)
        if (sign_builders[i].vert_count > 0) mesh_count++;
    mesh_count += scroll_count;

    r->level_meshes = calloc(mesh_count, sizeof(RenderMesh));
    if (!r->level_meshes) { free(builders); free(sign_builders); return false; }
    r->level_mesh_count = 0;

    #define REUSE_MESH ((r->level_mesh_count < old_count) ? &old_meshes[r->level_mesh_count] : NULL)
    u32 dbg_total_verts = 0, dbg_total_idx = 0;
    for (u32 i = 0; i <= r->texture_count; i++) {
        if (builders[i].vert_count > 0) {
            dbg_total_verts += builders[i].vert_count;
            dbg_total_idx += builders[i].idx_count;
            RenderMesh m = mb_upload(&builders[i], REUSE_MESH);
            m.sector_idx = 0xFFFFFFFF;
            m.is_scroll_floor = false;
            r->level_meshes[r->level_mesh_count++] = m;
        }
    }

    /* Upload dedicated scroll-floor sector meshes */
    for (u32 i = 0; i < scroll_count; i++) {
        RenderMesh m = mb_upload(&scroll_mb[i], REUSE_MESH);
        m.sector_idx = scroll_si[i];
        m.is_scroll_floor = true;
        r->level_meshes[r->level_mesh_count++] = m;
    }

    /* Upload wall sign meshes last: they are drawn after the coplanar wall
     * parts, with polygon offset (see renderer_draw_level). */
    for (u32 i = 0; i <= r->texture_count; i++) {
        if (sign_builders[i].vert_count > 0) {
            RenderMesh m = mb_upload(&sign_builders[i], REUSE_MESH);
            m.sector_idx = 0xFFFFFFFF;
            m.is_sign = true;
            r->level_meshes[r->level_mesh_count++] = m;
        }
    }
    #undef REUSE_MESH

    /* Delete any leftover old meshes not reused this build (build had fewer). */
    for (u32 i = r->level_mesh_count; i < old_count; i++) {
        glDeleteVertexArrays(1, &old_meshes[i].vao);
        glDeleteBuffers(1, &old_meshes[i].vbo);
        glDeleteBuffers(1, &old_meshes[i].ibo);
    }
    free(old_meshes);
    if (getenv("OL_MESHLOG"))
        OL_LOG("Level mesh totals: %u verts, %u indices (%u tris)\n",
               dbg_total_verts, dbg_total_idx, dbg_total_idx / 3);

    free(builders);
    free(sign_builders);
    OL_LOG("Level geom: %u floors built\n", dbg_floors);
    OL_LOG("Level built: %u draw calls (%u scroll floors)\n",
           r->level_mesh_count, scroll_count);
    return true;
}

/* -------------------------------------------------------------------------
 * Camera and rendering
 * ---------------------------------------------------------------------- */

void renderer_set_camera(Renderer *r, Vec3 pos, f32 yaw, f32 pitch) {
    f32 cp = cosf(pitch), sp = sinf(pitch);
    f32 cy = cosf(yaw),   sy = sinf(yaw);
    Vec3 forward = { cy * cp, sp, -sy * cp };
    Vec3 center  = vec3_add(pos, forward);
    Vec3 up      = { 0, 1, 0 };
    r->view = mat4_look_at(pos, center, up);

    /* Extract right/up for billboard rendering (column-major: m[col][row]) */
    r->cam_right = (Vec3){ r->view.m[0][0], r->view.m[1][0], r->view.m[2][0] };
    r->cam_up    = (Vec3){ r->view.m[0][1], r->view.m[1][1], r->view.m[2][1] };

    /* Cache for sky rendering */
    r->cam_yaw   = yaw;
    r->cam_pitch = pitch;
}

void renderer_draw_level(Renderer *r) {
    if (!r->level_meshes) return;

    glUseProgram(r->prog_world);

    /* Disable backface culling for level geometry.  The Jedi Engine is a
     * portal renderer that only draws walls visible from the correct side;
     * our GL renderer draws ALL geometry, and some wall quads have winding
     * that faces away from the sector interior.  Disabling culling ensures
     * no walls become invisible. */
    glDisable(GL_CULL_FACE);

    /* Enable alpha blending so transparent textures (window bars, fences)
     * render correctly. The shader discards alpha < 0.1 for hard cutoff,
     * and blending handles the remaining alpha compositing. */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* MVP uniform */
    Mat4 mvp = mat4_mul(r->proj, r->view);
    GLint mvp_loc = glGetUniformLocation(r->prog_world, "uMVP");
    glUniformMatrix4fv(mvp_loc, 1, GL_FALSE, &mvp.m[0][0]);

    GLint tex_loc   = glGetUniformLocation(r->prog_world, "uTex");
    GLint amb_loc   = glGetUniformLocation(r->prog_world, "uAmbient");
    GLint uvoff_loc = glGetUniformLocation(r->prog_world, "uUVOffset");
    glUniform1f(amb_loc, 0.1f); /* Minimum ambient */
    glUniform2f(uvoff_loc, 0.0f, 0.0f); /* Default: no scroll */

    for (u32 i = 0; i < r->level_mesh_count; i++) {
        const RenderMesh *mesh = &r->level_meshes[i];
        if (!mesh->vao || !mesh->index_count) continue;

        /* Bind texture — skip meshes with no valid texture (tex_id 0 = none) */
        if (mesh->tex_id == 0) continue;
        glActiveTexture(GL_TEXTURE0);
        if (mesh->tex_id > 0 && mesh->tex_id <= r->texture_count) {
            glBindTexture(GL_TEXTURE_2D, r->textures[mesh->tex_id - 1].handle);
        } else {
            glBindTexture(GL_TEXTURE_2D, r->missing_tex);
        }
        glUniform1i(tex_loc, 0);

        /* Apply UV scroll offset for scroll-floor sector meshes */
        if (mesh->is_scroll_floor && mesh->sector_idx < 4096) {
            glUniform2f(uvoff_loc,
                        r->sector_scroll_u[mesh->sector_idx],
                        r->sector_scroll_v[mesh->sector_idx]);
        } else {
            glUniform2f(uvoff_loc, 0.0f, 0.0f);
        }

        /* Wall signs are coplanar overdraws of their host wall part (the
         * software renderer draws them over the same columns); pull them
         * toward the viewer so they win the depth test. */
        if (mesh->is_sign) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(-1.0f, -2.0f);
        }

        glBindVertexArray(mesh->vao);
        glDrawElements(GL_TRIANGLES, (GLsizei)mesh->index_count, GL_UNSIGNED_INT, 0);

        if (mesh->is_sign) glDisable(GL_POLYGON_OFFSET_FILL);
    }
    glBindVertexArray(0);
    /* (polygon offset was disabled above) */
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE); /* Re-enable for sprites/HUD */
    glUseProgram(0);
}

void renderer_sync_scroll(Renderer *r, const InfSystem *inf) {
    if (!inf) return;
    for (u32 i = 0; i < inf->count; i++) {
        const Elevator *el = &inf->elevs[i];
        if (!el->active) continue;
        if (el->type != ELEV_TYPE_SCROLL_FLOOR) continue;
        if (el->sector_idx >= 4096) continue;
        r->sector_scroll_u[el->sector_idx] = el->scroll_u;
        r->sector_scroll_v[el->sector_idx] = el->scroll_v;
    }
}

/* -------------------------------------------------------------------------
 * Sky panorama rendering
 *
 * Renders the sky as a screen-space textured background quad before world
 * geometry (depth writes disabled). UV u scrolls with camera yaw; v maps
 * camera pitch. The panorama wraps at u=1.
 * ---------------------------------------------------------------------- */

void renderer_set_sky(Renderer *r, u32 sky_tex, f32 parallax_x, f32 parallax_y) {
    r->sky_tex        = sky_tex;
    /* The original software renderer runs the sky with parallax fixed at
     * 1024x1024 texels per revolution (Ghidra: setter @0x4b0990, sole caller
     * passes 1024.0,1024.0). Shipped LVTs all say PARALLAX 1024 1024 anyway;
     * honor the LVT value when present. */
    r->sky_parallax_x = (parallax_x > 0.0f) ? parallax_x : 1024.0f;
    r->sky_parallax_y = (parallax_y > 0.0f) ? parallax_y : 1024.0f;
}

void renderer_draw_sky(Renderer *r) {
    if (!r->sky_tex || !r->prog_sky || !r->sky_vao) return;
    if (r->sky_tex > r->texture_count) return;

    f32 W  = (f32)r->cfg.width;
    f32 H  = (f32)r->cfg.height;
    f32 tw = (f32)r->textures[r->sky_tex - 1].width;
    f32 th = (f32)r->textures[r->sky_tex - 1].height;

    /* Horizontal focal length in pixels from the horizontal FOV. */
    f32 aspect = W / H;
    f32 hfov   = 2.0f * atanf(tanf(r->cam_fov_rad * 0.5f) * aspect);
    f32 focal  = (W * 0.5f) / tanf(hfov * 0.5f);

    /* Our yaw convention: 0 = +X, pi/2 = +Z. Outlaws (Render_SetCamera
     * @0x4a66e0): 0 = +Z, increasing toward +X. yaw_outlaws = pi/2 - yaw. */
    f32 yaw_turns   = (0.5f * OL_PI - r->cam_yaw) / (2.0f * OL_PI);
    f32 pitch_turns = r->cam_pitch / (2.0f * OL_PI);

    glUseProgram(r->prog_sky);
    glUniform1i(glGetUniformLocation(r->prog_sky, "uTex"), 0);
    glUniform2f(glGetUniformLocation(r->prog_sky, "uScreen"), W, H);
    glUniform2f(glGetUniformLocation(r->prog_sky, "uTexSize"), tw, th);
    glUniform2f(glGetUniformLocation(r->prog_sky, "uParallax"),
                r->sky_parallax_x, r->sky_parallax_y);
    glUniform1f(glGetUniformLocation(r->prog_sky, "uYawTurns"), yaw_turns);
    glUniform1f(glGetUniformLocation(r->prog_sky, "uPitchTurns"), pitch_turns);
    glUniform1f(glGetUniformLocation(r->prog_sky, "uFocalPx"), focal);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, r->textures[r->sky_tex - 1].handle);
    /* Both axes wrap (the original masks U and V with pow2 masks). */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glBindVertexArray(r->sky_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glBindVertexArray(0);
    glUseProgram(0);
}

/* -------------------------------------------------------------------------
 * Sprite billboard rendering
 *
 * Per-sprite vertex layout: x,y,z, u,v, r,g,b,a  (9 floats)
 * Each sprite = 6 vertices (two triangles, no index buffer).
 * ---------------------------------------------------------------------- */
#define SPRITE_STRIDE 9  /* floats per vertex */
#define SPRITE_VERTS  6  /* vertices per sprite (2 triangles) */

void renderer_draw_sprites(Renderer *r, const EntityList *entities) {
    if (!r->prog_sprite || !r->sprite_vao || !entities) return;

    static f32 vbuf[2048 * SPRITE_VERTS * SPRITE_STRIDE];
    u32 nv = 0;

    Vec3 cr  = r->cam_right;
    Vec3 cu  = r->cam_up;
    Mat4 mvp = mat4_mul(r->proj, r->view);

    /* Camera forward vector (into the scene, GL space: -Z) */
    Vec3 cam_fwd = {
        -(r->view.m[0][2]),
        -(r->view.m[1][2]),
        -(r->view.m[2][2])
    };

    glUseProgram(r->prog_sprite);
    GLint mvp_loc    = glGetUniformLocation(r->prog_sprite, "uMVP");
    GLint tex_loc    = glGetUniformLocation(r->prog_sprite, "uTex");
    GLint hastex_loc = glGetUniformLocation(r->prog_sprite, "uHasTex");
    glUniformMatrix4fv(mvp_loc, 1, GL_FALSE, &mvp.m[0][0]);
    glUniform1i(tex_loc, 0);
    glBindVertexArray(r->sprite_vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->sprite_vbo);

    for (u32 i = 0; i < entities->count; i++) {
        const Entity *e = &entities->entities[i];
        if (!e->active || e->kind == ENTITY_TRIGGER) continue;
        /* Skip invisible: no sprite dimensions in entity def or NWX */
        if (e->sprite_w == 0.0f && e->sprite_h == 0.0f &&
            e->render_w == 0.0f && e->render_h == 0.0f) continue;
        if (nv + SPRITE_VERTS > 2048 * SPRITE_VERTS) break;

        /* Use NWX-derived render dimensions when available, else entity def */
        f32 rw = (e->render_w > 0) ? e->render_w : e->sprite_w;
        f32 rh = (e->render_h > 0) ? e->render_h : e->sprite_h;
        f32 hw = rw * 0.5f;
        f32 h  = rh;

        /* Convert LVT position to GL space (negate Z) */
        Vec3 base = { e->pos.x, e->pos.y, -e->pos.z };

        /* No color tint — sprites render at their authored colors.
         * (Enemy state was previously debug-tinted red/pink; removed.) */
        f32 tr=1,tg=1,tb=1,ta=1;

        /*
         * 8-directional sprite selection.
         * dir=0: player sees enemy front (enemy faces player).
         * dir=4: player sees enemy back.  dir=2/6: side views.
         *
         * The entity-to-camera direction = -cam_fwd (camera looks INTO scene,
         * so entity→camera is the opposite direction).
         * In LVT space: entity→cam = (-cam_fwd.x, cam_fwd.z).
         */
        u32 dir = 0;
        {
            /* Entity-to-camera direction in LVT space */
            f32 dx = -cam_fwd.x;
            f32 dz =  cam_fwd.z;   /* cam_fwd.z is already GL -Z; negate → LVT +Z */
            f32 ey = e->yaw;       /* entity facing direction (LVT yaw in radians) */
            /* Relative angle: 0 when entity faces the camera (front view) */
            f32 rel = atan2f(dz, dx) - ey;
            /* Normalize to [0, 2π) */
            while (rel <  0.0f)        rel += 2.0f * OL_PI;
            while (rel >= 2.0f*OL_PI) rel -= 2.0f * OL_PI;
            /* Map to 0-7 (each slice = π/4 = 45°) */
            dir = (u32)(rel / (OL_PI / 4.0f) + 0.5f) % 8;
        }

        /* Select texture: scenery chor frame, per-AI-state animation, cyclic
         * decoration anim, or per-direction sprite_dir_tex[]. */
        u32 tex_id;
        if (e->is_scenery && e->scn_count > 0 &&
            e->scn_state < e->scn_count &&
            e->scn_frame < e->scn[e->scn_state].nframes) {
            const ScnChor *ch = &e->scn[e->scn_state];
            tex_id = ch->tex[e->scn_frame];
            if (tex_id && ch->fh[e->scn_frame] > 0.0f) {
                rw = ch->fw[e->scn_frame];
                rh = ch->fh[e->scn_frame];
                hw = rw * 0.5f;
                h  = rh;
            }
            if (!tex_id) tex_id = e->sprite_tex;
        } else if (e->has_anim_seqs && e->anim_seqs[e->cur_anim].frame_count > 0) {
            /* Enemy with per-AI-state animation sequences */
            const EntityAnimSeq *seq = &e->anim_seqs[e->cur_anim];
            u32 frame = e->cur_anim_frame;
            if (frame >= seq->frame_count) frame = seq->frame_count - 1;
            tex_id = seq->dir_frames[dir][frame];
            /* Size the billboard to the CURRENT frame's cell (a lying corpse
             * cell is wide and short, a standing pose tall) instead of the idle
             * size — otherwise the corpse gets stretched into a tall smear. */
            if (tex_id && seq->fh[dir][frame] > 0.0f) {
                rw = seq->fw[dir][frame];
                rh = seq->fh[dir][frame];
                hw = rw * 0.5f;
                h  = rh;
            }
            if (!tex_id) tex_id = e->sprite_dir_tex[dir];
            if (!tex_id) tex_id = e->sprite_tex;
        } else if (e->anim_count > 1 && e->anim_frame < e->anim_count && e->anim_tex[e->anim_frame]) {
            tex_id = e->anim_tex[e->anim_frame];
        } else {
            tex_id = e->sprite_dir_tex[dir];
            if (!tex_id) tex_id = e->sprite_tex; /* fallback to primary */
        }

        /* Skip entities with no texture — no NWX sprite loaded.
         * Don't render large placeholder quads for model-only entities. */
        if (!tex_id) continue;

        /* Flat (ground-lying) sprites: horizontal quad at floor level.
         * X axis = world right (+X), Z axis = world forward (-Z in GL = +Z in LVT).
         * sprite_w = width, sprite_h = depth on the floor. */
        Vec3 bl, br, tr_, tl_;
        if (e->flat) {
            f32 hd = rh * 0.5f; /* half-depth along Z */
            bl  = (Vec3){ base.x - hw, base.y, base.z - hd };
            br  = (Vec3){ base.x + hw, base.y, base.z - hd };
            tr_ = (Vec3){ base.x + hw, base.y, base.z + hd };
            tl_ = (Vec3){ base.x - hw, base.y, base.z + hd };
        } else {
            /* Billboard corners: bottom-left, bottom-right, top-right, top-left */
            bl  = vec3_sub(base, vec3_scale(cr, hw));
            br  = vec3_add(base, vec3_scale(cr, hw));
            tr_ = vec3_add(vec3_add(base, vec3_scale(cr, hw)), vec3_scale(cu, h));
            tl_ = vec3_add(vec3_sub(base, vec3_scale(cr, hw)), vec3_scale(cu, h));
        }

#define PUSH_V(vx,vy,vz,uu,vv) \
    vbuf[nv*SPRITE_STRIDE+0]=(vx); vbuf[nv*SPRITE_STRIDE+1]=(vy); vbuf[nv*SPRITE_STRIDE+2]=(vz); \
    vbuf[nv*SPRITE_STRIDE+3]=(uu); vbuf[nv*SPRITE_STRIDE+4]=(vv); \
    vbuf[nv*SPRITE_STRIDE+5]=tr;   vbuf[nv*SPRITE_STRIDE+6]=tg; \
    vbuf[nv*SPRITE_STRIDE+7]=tb;   vbuf[nv*SPRITE_STRIDE+8]=ta; nv++

        PUSH_V(bl.x,bl.y,bl.z,   0,0);
        PUSH_V(br.x,br.y,br.z,   1,0);
        PUSH_V(tr_.x,tr_.y,tr_.z, 1,1);
        PUSH_V(bl.x,bl.y,bl.z,   0,0);
        PUSH_V(tr_.x,tr_.y,tr_.z, 1,1);
        PUSH_V(tl_.x,tl_.y,tl_.z, 0,1);
#undef PUSH_V

        /* Upload and draw this sprite */
        glBufferSubData(GL_ARRAY_BUFFER, 0, nv * SPRITE_STRIDE * sizeof(f32), vbuf);

        GLuint tex_handle;
        bool   has_tex;
        if (tex_id > 0 && tex_id <= r->texture_count) {
            tex_handle = r->textures[tex_id - 1].handle;
            has_tex    = true;
        } else {
            tex_handle = r->missing_tex;
            has_tex    = false;
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex_handle);
        glUniform1i(hastex_loc, has_tex ? 1 : 0);
        glDrawArrays(GL_TRIANGLES, (GLint)(nv - SPRITE_VERTS), SPRITE_VERTS);
        nv = 0;
    }

    glBindVertexArray(0);
    glUseProgram(0);
}

void renderer_draw_billboards(Renderer *r, const BillboardDraw *list, u32 count) {
    if (!r->prog_sprite || !r->sprite_vao || !count) return;

    Vec3 cr = r->cam_right;
    Vec3 cu = r->cam_up;
    Mat4 mvp = mat4_mul(r->proj, r->view);

    glUseProgram(r->prog_sprite);
    glUniformMatrix4fv(glGetUniformLocation(r->prog_sprite, "uMVP"),
                       1, GL_FALSE, &mvp.m[0][0]);
    glUniform1i(glGetUniformLocation(r->prog_sprite, "uTex"), 0);
    GLint hastex_loc = glGetUniformLocation(r->prog_sprite, "uHasTex");
    glBindVertexArray(r->sprite_vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->sprite_vbo);

    for (u32 i = 0; i < count; i++) {
        const BillboardDraw *b = &list[i];
        if (!b->tex || b->tex > r->texture_count) continue;
        Vec3 base = { b->pos.x, b->pos.y, -b->pos.z };  /* LVT → GL */
        f32 hw = b->w * 0.5f, h = b->h;
        Vec3 bl  = vec3_sub(base, vec3_scale(cr, hw));
        Vec3 br  = vec3_add(base, vec3_scale(cr, hw));
        Vec3 tr_ = vec3_add(br, vec3_scale(cu, h));
        Vec3 tl_ = vec3_add(bl, vec3_scale(cu, h));
        f32 vb[6 * SPRITE_STRIDE];
        u32 nv = 0;
#define PUSH_B(vx,vy,vz,uu,vv) do { \
        vb[nv*SPRITE_STRIDE+0]=(vx); vb[nv*SPRITE_STRIDE+1]=(vy); vb[nv*SPRITE_STRIDE+2]=(vz); \
        vb[nv*SPRITE_STRIDE+3]=(uu); vb[nv*SPRITE_STRIDE+4]=(vv); \
        vb[nv*SPRITE_STRIDE+5]=1; vb[nv*SPRITE_STRIDE+6]=1; vb[nv*SPRITE_STRIDE+7]=1; vb[nv*SPRITE_STRIDE+8]=1; \
        nv++; } while (0)
        PUSH_B(bl.x,bl.y,bl.z,   0,0);
        PUSH_B(br.x,br.y,br.z,   1,0);
        PUSH_B(tr_.x,tr_.y,tr_.z,1,1);
        PUSH_B(bl.x,bl.y,bl.z,   0,0);
        PUSH_B(tr_.x,tr_.y,tr_.z,1,1);
        PUSH_B(tl_.x,tl_.y,tl_.z,0,1);
#undef PUSH_B
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vb), vb);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, r->textures[b->tex - 1].handle);
        glUniform1i(hastex_loc, 1);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    glBindVertexArray(0);
    glUseProgram(0);
}

/* -------------------------------------------------------------------------
 * Flat-shaded 3DO model instances (untextured palette-colored triangles;
 * matches the original's rendering of gknife/gdynam ground objects).
 * ---------------------------------------------------------------------- */
#include "tdo.h"

int renderer_upload_tdo(Renderer *r, const struct TdoModel *m,
                        const u8 pal[256][3]) {
    if (r->tdo_count >= R_MAX_TDO) return -1;
    u32 total = 0;
    for (u32 o = 0; o < m->object_count; o++)
        total += m->objects[o].tri_count * 3;
    if (!total) return -1;
    f32 *data = malloc((size_t)total * SPRITE_STRIDE * sizeof(f32));
    if (!data) return -1;

    u32 nv = 0;
    for (u32 o = 0; o < m->object_count; o++) {
        const TdoObject *ob = &m->objects[o];
        for (u32 t = 0; t < ob->tri_count; t++) {
            const TdoTriangle *tr = &ob->tris[t];
            if (tr->a < 0 || tr->b < 0 || tr->c < 0) continue;
            if ((u32)tr->a >= ob->vert_count || (u32)tr->b >= ob->vert_count ||
                (u32)tr->c >= ob->vert_count) continue;
            u8 ci = (u8)(tr->color & 0xFF);
            f32 cr = pal ? pal[ci][0] / 255.0f : 0.8f;
            f32 cg = pal ? pal[ci][1] / 255.0f : 0.2f;
            f32 cb = pal ? pal[ci][2] / 255.0f : 0.2f;
            const i32 idx[3] = { tr->a, tr->b, tr->c };
            for (int k = 0; k < 3; k++) {
                const Vec3 *v = &ob->verts[idx[k]].pos;
                f32 *out = &data[(size_t)nv * SPRITE_STRIDE];
                out[0]=v->x; out[1]=v->y; out[2]=v->z;
                out[3]=0; out[4]=0;
                out[5]=cr; out[6]=cg; out[7]=cb; out[8]=1.0f;
                nv++;
            }
        }
    }
    if (!nv) { free(data); return -1; }
    int id = (int)r->tdo_count++;
    r->tdo_data[id]   = data;
    r->tdo_vcount[id] = nv;
    return id;
}

void renderer_draw_tdos(Renderer *r, const TdoDraw *list, u32 count) {
    if (!r->prog_sprite || !r->sprite_vao || !count) return;

    Mat4 mvp = mat4_mul(r->proj, r->view);
    glUseProgram(r->prog_sprite);
    glUniformMatrix4fv(glGetUniformLocation(r->prog_sprite, "uMVP"),
                       1, GL_FALSE, &mvp.m[0][0]);
    glUniform1i(glGetUniformLocation(r->prog_sprite, "uHasTex"), 0);
    glBindVertexArray(r->sprite_vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->sprite_vbo);

    static f32 xbuf[4096 * SPRITE_STRIDE];
    for (u32 i = 0; i < count; i++) {
        const TdoDraw *d = &list[i];
        if (d->id < 0 || d->id >= (int)r->tdo_count) continue;
        const f32 *src = r->tdo_data[d->id];
        u32 nv = r->tdo_vcount[d->id];
        if (nv > 4096) nv = 4096;
        f32 cy = cosf(d->yaw),    sy = sinf(d->yaw);
        f32 ct = cosf(d->tumble), st = sinf(d->tumble);
        for (u32 v = 0; v < nv; v++) {
            const f32 *in = &src[(size_t)v * SPRITE_STRIDE];
            /* tumble (rotate around model X), then yaw, then translate */
            f32 x0 = in[0], y0 = in[1] * ct - in[2] * st,
                z0 = in[1] * st + in[2] * ct;
            f32 wx = d->pos.x + x0 * cy - z0 * sy;
            f32 wz = d->pos.z + x0 * sy + z0 * cy;
            f32 wy = d->pos.y + y0;
            f32 *out = &xbuf[(size_t)v * SPRITE_STRIDE];
            out[0] = wx; out[1] = wy; out[2] = -wz;   /* LVT → GL */
            out[3] = in[3]; out[4] = in[4];
            out[5] = in[5]; out[6] = in[6]; out[7] = in[7]; out[8] = in[8];
        }
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        (GLsizeiptr)nv * SPRITE_STRIDE * sizeof(f32), xbuf);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)nv);
    }
    glBindVertexArray(0);
    glUseProgram(0);
}

/* -------------------------------------------------------------------------
 * HUD rendering (2D orthographic overlay)
 *
 * Per-vertex: x,y, u,v, r,g,b,a  (8 floats)
 * ---------------------------------------------------------------------- */
#define HUD_STRIDE 8

static void hud_quad(f32 *buf, u32 *n,
                     f32 x0, f32 y0, f32 x1, f32 y1,
                     f32 u0, f32 v0, f32 u1, f32 v1,
                     f32 r, f32 g, f32 b, f32 a) {
    /* Two triangles from rect */
    f32 verts[6][HUD_STRIDE] = {
        {x0,y0,u0,v0,r,g,b,a}, {x1,y0,u1,v0,r,g,b,a}, {x1,y1,u1,v1,r,g,b,a},
        {x0,y0,u0,v0,r,g,b,a}, {x1,y1,u1,v1,r,g,b,a}, {x0,y1,u0,v1,r,g,b,a},
    };
    for (int i = 0; i < 6; i++) {
        for (int j = 0; j < HUD_STRIDE; j++)
            buf[(*n)*HUD_STRIDE + j] = verts[i][j];
        (*n)++;
    }
}

/* Thick 2D line (rotated quad) for the automap. */
static void hud_line(f32 *buf, u32 *n, f32 x0, f32 y0, f32 x1, f32 y1,
                     f32 t, f32 r, f32 g, f32 b, f32 a) {
    f32 dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx*dx + dy*dy);
    if (len < 1e-3f) return;
    f32 nx = -dy / len * t * 0.5f, ny = dx / len * t * 0.5f;
    f32 v[6][HUD_STRIDE] = {
        {x0+nx,y0+ny,0,0,r,g,b,a}, {x1+nx,y1+ny,0,0,r,g,b,a}, {x1-nx,y1-ny,0,0,r,g,b,a},
        {x0+nx,y0+ny,0,0,r,g,b,a}, {x1-nx,y1-ny,0,0,r,g,b,a}, {x0-nx,y0-ny,0,0,r,g,b,a},
    };
    for (int i = 0; i < 6; i++) {
        for (int j = 0; j < HUD_STRIDE; j++) buf[(*n)*HUD_STRIDE+j] = v[i][j];
        (*n)++;
    }
}

/*
 * Automap overlay (TAB). Player-oriented (the player faces "up" and the map
 * rotates around them), centered on the player, drawn over the 3D view like the
 * original's "mapoverlay maporiented" mode. Walls are coloured by kind: solid
 * walls bright, portals/adjoins dim, breakable windows cyan; the player is a
 * white arrow at the centre.
 */
void renderer_draw_minimap(Renderer *r, const LvtLevel *level,
                           const InfSystem *inf, Vec3 ppos, f32 pyaw) {
    if (!r->prog_hud || !r->hud_vao || !level) return;
    f32 W = (f32)r->cfg.width, H = (f32)r->cfg.height;
    Mat4 ortho = mat4_ortho(0, W, H, 0, -1, 1);

    /* Must fit the shared hud_vbo (allocated for 1024*6 verts). */
    static f32 vbuf[1024 * 6 * HUD_STRIDE];
    u32 nv = 0;

    glUseProgram(r->prog_hud);
    GLint ortho_loc  = glGetUniformLocation(r->prog_hud, "uOrtho");
    GLint hastex_loc = glGetUniformLocation(r->prog_hud, "uHasTex");
    glUniformMatrix4fv(ortho_loc, 1, GL_FALSE, &ortho.m[0][0]);
    glUniform1i(hastex_loc, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(r->hud_vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->hud_vbo);

    #define MM_FLUSH() do { if (nv > 0) { \
        glBufferSubData(GL_ARRAY_BUFFER, 0, nv*HUD_STRIDE*sizeof(f32), vbuf); \
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)nv); nv = 0; } } while(0)

    /* Dark backdrop so the wall lines read over the bright scene. */
    hud_quad(vbuf, &nv, 0, 0, W, H, 0,0,0,0, 0.0f, 0.03f, 0.0f, 0.80f);
    MM_FLUSH();

    f32 cx = W * 0.5f, cy = H * 0.5f;
    f32 scale = H / 380.0f;              /* world units shown vertically ≈ 380 */
    f32 cf = cosf(pyaw), sf = sinf(pyaw);
    f32 thick = OL_MAX(2.0f, H / 260.0f);

    for (u32 si = 0; si < level->sector_count; si++) {
        const LvtSector *sec = &level->sectors[si];
        /* Is this sector a morph-door leaf? colour it yellow. */
        bool is_door = inf && inf_is_morph_door(inf, si);
        for (u32 wi = 0; wi < sec->wall_count; wi++) {
            const LvtWall *w = &sec->walls[wi];
            if (w->v1 < 0 || w->v2 < 0 ||
                w->v1 >= (i32)sec->vertex_count || w->v2 >= (i32)sec->vertex_count) continue;
            f32 ax = sec->vertices[w->v1].x - ppos.x, az = sec->vertices[w->v1].y - ppos.z;
            f32 bx = sec->vertices[w->v2].x - ppos.x, bz = sec->vertices[w->v2].y - ppos.z;
            /* player-oriented: forward → screen up, right → screen right */
            f32 a_fwd = ax*cf + az*sf, a_rgt = ax*sf - az*cf;
            f32 b_fwd = bx*cf + bz*sf, b_rgt = bx*sf - bz*cf;
            f32 sx0 = cx + a_rgt*scale, sy0 = cy - a_fwd*scale;
            f32 sx1 = cx + b_rgt*scale, sy1 = cy - b_fwd*scale;
            /* cull segments fully outside the view */
            if ((sx0 < 0 && sx1 < 0) || (sx0 > W && sx1 > W) ||
                (sy0 < 0 && sy1 < 0) || (sy0 > H && sy1 > H)) continue;
            f32 cr, cg, cb, ca = 1.0f;
            if (w->is_window)      { cr=0.40f; cg=0.95f; cb=1.00f; }   /* window: cyan */
            else if (is_door)      { cr=1.00f; cg=0.85f; cb=0.15f; }   /* door: yellow */
            else if (w->adjoin < 0){ cr=0.35f; cg=1.00f; cb=0.35f; }   /* solid: bright green */
            else                   { cr=0.35f; cg=0.55f; cb=0.40f; ca=0.85f; } /* portal: dim green */
            hud_line(vbuf, &nv, sx0, sy0, sx1, sy1, thick, cr, cg, cb, ca);
            if (nv + 6 > 1024*6) MM_FLUSH();
        }
    }
    MM_FLUSH();

    /* Player arrow (points up), white. */
    f32 as = OL_MAX(6.0f, H / 90.0f);
    hud_line(vbuf, &nv, cx, cy - as, cx - as*0.6f, cy + as*0.6f, thick*1.4f, 1,1,1,1);
    hud_line(vbuf, &nv, cx, cy - as, cx + as*0.6f, cy + as*0.6f, thick*1.4f, 1,1,1,1);
    hud_line(vbuf, &nv, cx - as*0.6f, cy + as*0.6f, cx + as*0.6f, cy + as*0.6f, thick*1.4f, 1,1,1,1);
    MM_FLUSH();

    #undef MM_FLUSH
    glBindVertexArray(0);
    glUseProgram(0);
    glDisable(GL_BLEND);
}

/* --- Generic 2D helpers for menus (self-contained GL setup per call) --- */
static void r2d_begin(Renderer *r, Mat4 *ortho) {
    *ortho = mat4_ortho(0, (f32)r->cfg.width, (f32)r->cfg.height, 0, -1, 1);
    glUseProgram(r->prog_hud);
    glUniformMatrix4fv(glGetUniformLocation(r->prog_hud, "uOrtho"), 1, GL_FALSE, &ortho->m[0][0]);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(r->hud_vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->hud_vbo);
}

/* Draw a textured image quad at (x,y) size (w,h). tex = renderer texture id. */
void renderer_draw_image(Renderer *r, u32 tex, f32 x, f32 y, f32 w, f32 h,
                         f32 tint, f32 alpha) {
    if (!r->prog_hud || !r->hud_vao) return;
    Mat4 ortho; r2d_begin(r, &ortho);
    static f32 vb[6 * HUD_STRIDE]; u32 nv = 0;
    hud_quad(vb, &nv, x, y, x+w, y+h, 0,0,1,1, tint, tint, tint, alpha);
    glUniform1i(glGetUniformLocation(r->prog_hud, "uHasTex"), tex ? 1 : 0);
    glUniform1i(glGetUniformLocation(r->prog_hud, "uTex"), 0);
    glActiveTexture(GL_TEXTURE0);
    if (tex && tex <= r->texture_count) glBindTexture(GL_TEXTURE_2D, r->textures[tex-1].handle);
    else glBindTexture(GL_TEXTURE_2D, 0);
    glBufferSubData(GL_ARRAY_BUFFER, 0, nv*HUD_STRIDE*sizeof(f32), vb);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)nv);
    glBindVertexArray(0); glUseProgram(0); glDisable(GL_BLEND);
}

/* Textured quad with explicit UV sub-rect + colour tint (font atlases). */
void renderer_draw_image_uv(Renderer *r, u32 tex, f32 x, f32 y, f32 w, f32 h,
                            f32 u0, f32 v0, f32 u1, f32 v1,
                            f32 cr, f32 cg, f32 cb, f32 ca) {
    if (!r->prog_hud || !r->hud_vao) return;
    Mat4 ortho; r2d_begin(r, &ortho);
    static f32 vb[6 * HUD_STRIDE]; u32 nv = 0;
    hud_quad(vb, &nv, x, y, x+w, y+h, u0, v0, u1, v1, cr, cg, cb, ca);
    glUniform1i(glGetUniformLocation(r->prog_hud, "uHasTex"), tex ? 1 : 0);
    glUniform1i(glGetUniformLocation(r->prog_hud, "uTex"), 0);
    glActiveTexture(GL_TEXTURE0);
    if (tex && tex <= r->texture_count) glBindTexture(GL_TEXTURE_2D, r->textures[tex-1].handle);
    else glBindTexture(GL_TEXTURE_2D, 0);
    glBufferSubData(GL_ARRAY_BUFFER, 0, nv*HUD_STRIDE*sizeof(f32), vb);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)nv);
    glBindVertexArray(0); glUseProgram(0); glDisable(GL_BLEND);
}

/* Draw a solid colour rectangle (tex=0). */
void renderer_draw_rect(Renderer *r, f32 x, f32 y, f32 w, f32 h,
                        f32 cr, f32 cg, f32 cb, f32 ca) {
    if (!r->prog_hud || !r->hud_vao) return;
    Mat4 ortho; r2d_begin(r, &ortho);
    static f32 vb[6 * HUD_STRIDE]; u32 nv = 0;
    hud_quad(vb, &nv, x, y, x+w, y+h, 0,0,0,0, cr, cg, cb, ca);
    glUniform1i(glGetUniformLocation(r->prog_hud, "uHasTex"), 0);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, 0);
    glBufferSubData(GL_ARRAY_BUFFER, 0, nv*HUD_STRIDE*sizeof(f32), vb);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)nv);
    glBindVertexArray(0); glUseProgram(0); glDisable(GL_BLEND);
}

/* Draw text via the embedded 5x7 font at (x,y), pixel-size px, with a shadow. */
void renderer_draw_text(Renderer *r, const char *s, f32 x, f32 y, f32 px,
                        f32 cr, f32 cg, f32 cb) {
    if (!r->prog_hud || !r->hud_vao || !s) return;
    Mat4 ortho; r2d_begin(r, &ortho);
    static f32 vb[4096 * HUD_STRIDE]; u32 nv = 0;
    glUniform1i(glGetUniformLocation(r->prog_hud, "uHasTex"), 0);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, 0);
    f32 char_w = 6.0f * px;
    for (int pass = 0; pass < 2; pass++) {
        f32 ox = (pass==0) ? px : 0.0f, oy = (pass==0) ? px : 0.0f;
        f32 pr = (pass==0) ? 0.0f : cr, pg = (pass==0) ? 0.0f : cg, pb = (pass==0) ? 0.0f : cb;
        f32 cx = x;
        for (int i = 0; s[i]; i++) {
            if (s[i] == '\n') { cx = x; y += 9.0f*px; continue; }
            int gi = font5x7_index(s[i]);
            const unsigned char *gl = FONT5X7[gi];
            for (int col = 0; col < 5; col++)
                for (int row = 0; row < 7; row++) {
                    if (!(gl[col] & (1 << row))) continue;
                    f32 qx = cx + col*px + ox, qy = y + row*px + oy;
                    if (nv + 6 <= 4096*6)
                        hud_quad(vb, &nv, qx, qy, qx+px, qy+px, 0,0,0,0, pr,pg,pb,1.0f);
                }
            cx += char_w;
        }
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0, nv*HUD_STRIDE*sizeof(f32), vb);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)nv);
    glBindVertexArray(0); glUseProgram(0); glDisable(GL_BLEND);
}

void renderer_draw_hud(Renderer *r, const HudParams *hud) {
    if (!r->prog_hud || !r->hud_vao) return;

    f32 W = (f32)r->cfg.width;
    f32 H = (f32)r->cfg.height;
    Mat4 ortho = mat4_ortho(0, W, H, 0, -1, 1);

    static f32 vbuf[1024 * 6 * HUD_STRIDE];
    u32 nv = 0;

    glUseProgram(r->prog_hud);
    GLint ortho_loc  = glGetUniformLocation(r->prog_hud, "uOrtho");
    GLint hastex_loc = glGetUniformLocation(r->prog_hud, "uHasTex");
    GLint tex_loc    = glGetUniformLocation(r->prog_hud, "uTex");
    glUniformMatrix4fv(ortho_loc, 1, GL_FALSE, &ortho.m[0][0]);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(r->hud_vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->hud_vbo);

    /* Helper: flush and draw current batch with given texture */
    #define HUD_FLUSH(tex_id, has_tex) do { \
        if (nv > 0) { \
            glUniform1i(hastex_loc, (has_tex) ? 1 : 0); \
            glUniform1i(tex_loc, 0); \
            glActiveTexture(GL_TEXTURE0); \
            { u32 _hf_tid = (u32)(tex_id); \
              if (has_tex && _hf_tid != 0 && _hf_tid <= r->texture_count) \
                glBindTexture(GL_TEXTURE_2D, r->textures[_hf_tid-1].handle); \
              else \
                glBindTexture(GL_TEXTURE_2D, 0); \
            } \
            glBufferSubData(GL_ARRAY_BUFFER, 0, nv * HUD_STRIDE * sizeof(f32), vbuf); \
            glDrawArrays(GL_TRIANGLES, 0, (GLsizei)nv); \
            nv = 0; \
        } \
    } while(0)

    /* Enable blending for alpha (death vignette, semi-transparent elements) */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* ---- Death screen vignette ---- */
    if (hud->dead) {
        hud_quad(vbuf, &nv, 0, 0, W, H, 0,0,0,0, 0.6f,0,0, 0.4f);
        HUD_FLUSH(0, false);
    }

    /* The first-person weapon hand renders BEHIND the status bar
     * (the original draws the view model in the 3D view, then the
     * HUD panel over it) — so the weapon block comes first. */
    /* ---- First-person weapon sprite (multi-frame fire/reload animation) ----
     *
     * Each animation frame's cell carries its own FRMT anchor offset (off_x,
     * off_y) and pixel size. All frames of a weapon share one sprite origin
     * placed at the bottom-center of a native 640x480 canvas, so the hand stays
     * put while the gun recoils. We map native coords to the window with a
     * uniform vertical scale (H/480) and center horizontally, preserving the
     * sprite's aspect ratio on wide displays. */
    if (!hud->dead) {
        int wi = hud->weapon_idx;
        if (wi >= 0 && wi < WEAPON_COUNT && r->weapon_hud_tex[wi]) {
            /* Selected frame texture + its native anchor geometry. */
            u32 tid = r->weapon_hud_tex[wi];
            i32 fox = r->weapon_idle_ox[wi], foy = r->weapon_idle_oy[wi];
            u32 fw  = r->weapon_idle_w[wi],  fh  = r->weapon_idle_h[wi];
            bool ftrans = false;

            bool cook_alt = hud->cooking && hud->cooking_alt &&
                            r->weapon_fire2_frame_count[wi] > 0 &&
                            r->weapon_fire2_frames[wi][0];
            bool cook_pri = hud->cooking && !hud->cooking_alt &&
                            r->weapon_fire_frame_count[wi] > 0 &&
                            r->weapon_fire_frames[wi][0];
            if (cook_alt || cook_pri) {
                /* Wind-up hold: the throw chor's first frame (arm back),
                 * frozen while the fire button is held (0xFFF9 pause). */
                if (cook_alt) {
                    tid = r->weapon_fire2_frames[wi][0];
                    fox = r->weapon_fire2_ox[wi][0];
                    foy = r->weapon_fire2_oy[wi][0];
                    fw  = r->weapon_fire2_w[wi][0];
                    fh  = r->weapon_fire2_h[wi][0];
                    ftrans = r->weapon_fire2_trans[wi][0] != 0;
                } else {
                    tid = r->weapon_fire_frames[wi][0];
                    fox = r->weapon_fire_ox[wi][0];
                    foy = r->weapon_fire_oy[wi][0];
                    fw  = r->weapon_fire_w[wi][0];
                    fh  = r->weapon_fire_h[wi][0];
                    ftrans = r->weapon_fire_trans[wi][0] != 0;
                }
            } else if (!hud->firing && hud->holding_lit &&
                       r->weapon_fire_frame_count[wi] > 0) {
                /* Lit stick in hand: hold the light chor's last frame */
                u32 lf = r->weapon_fire_frame_count[wi] - 1;
                if (r->weapon_fire_frames[wi][lf]) {
                    tid = r->weapon_fire_frames[wi][lf];
                    fox = r->weapon_fire_ox[wi][lf];
                    foy = r->weapon_fire_oy[wi][lf];
                    fw  = r->weapon_fire_w[wi][lf];
                    fh  = r->weapon_fire_h[wi][lf];
                    ftrans = r->weapon_fire_trans[wi][lf] != 0;
                }
            } else if (hud->firing) {
                u32 *frames = hud->fire_alt ? r->weapon_fire2_frames[wi]
                                            : r->weapon_fire_frames[wi];
                u32 *dts    = hud->fire_alt ? r->weapon_fire2_dt[wi]
                                            : r->weapon_fire_dt[wi];
                u32 nframes = hud->fire_alt ? r->weapon_fire2_frame_count[wi]
                                            : r->weapon_fire_frame_count[wi];
                i32 *oxs    = hud->fire_alt ? r->weapon_fire2_ox[wi]
                                            : r->weapon_fire_ox[wi];
                i32 *oys    = hud->fire_alt ? r->weapon_fire2_oy[wi]
                                            : r->weapon_fire_oy[wi];
                u32 *ws     = hud->fire_alt ? r->weapon_fire2_w[wi]
                                            : r->weapon_fire_w[wi];
                u32 *hs     = hud->fire_alt ? r->weapon_fire2_h[wi]
                                            : r->weapon_fire_h[wi];
                u8  *trs    = hud->fire_alt ? r->weapon_fire2_trans[wi]
                                            : r->weapon_fire_trans[wi];
                if (nframes > 0) {
                    u32 total_ms = 0;
                    for (u32 fi = 0; fi < nframes; fi++) total_ms += dts[fi];
                    f32 elapsed_ms = hud->fire_timer * (f32)total_ms;
                    u32 accum = 0, frame_idx = nframes - 1;
                    for (u32 fi = 0; fi < nframes; fi++) {
                        accum += dts[fi];
                        if (elapsed_ms < (f32)accum) { frame_idx = fi; break; }
                    }
                    if (frames[frame_idx]) {
                        tid = frames[frame_idx];
                        fox = oxs[frame_idx]; foy = oys[frame_idx];
                        fw  = ws[frame_idx];  fh  = hs[frame_idx];
                        ftrans = trs[frame_idx] != 0;
                    }
                }
            } else if (hud->reloading && r->weapon_reload_frame_count[wi] > 0) {
                /* Cycle reload frames on a free-running timer. */
                u32 nframes = r->weapon_reload_frame_count[wi];
                u32 total_ms = 0;
                for (u32 fi = 0; fi < nframes; fi++) total_ms += r->weapon_reload_dt[wi][fi];
                if (total_ms == 0) total_ms = nframes * 100u;
                u32 elapsed = (u32)SDL_GetTicks() % total_ms;
                u32 accum = 0, frame_idx = nframes - 1;
                for (u32 fi = 0; fi < nframes; fi++) {
                    accum += r->weapon_reload_dt[wi][fi];
                    if (elapsed < accum) { frame_idx = fi; break; }
                }
                if (r->weapon_reload_frames[wi][frame_idx]) {
                    tid = r->weapon_reload_frames[wi][frame_idx];
                    fox = r->weapon_reload_ox[wi][frame_idx];
                    foy = r->weapon_reload_oy[wi][frame_idx];
                    fw  = r->weapon_reload_w[wi][frame_idx];
                    fh  = r->weapon_reload_h[wi][frame_idx];
                }
            }

            if (tid > 0 && tid <= r->texture_count && fw > 0 && fh > 0) {
                /* Native origin at bottom-center (320, 480). */
                f32 s  = H / 480.0f;
                f32 x0 = W * 0.5f + (f32)fox * s;
                f32 y0 = (480.0f + (f32)foy) * s;
                f32 x1 = x0 + (f32)fw * s;
                f32 y1 = y0 + (f32)fh * s;
                hud_quad(vbuf, &nv, x0, y0, x1, y1, 0,0,1,1, 1,1,1,1);
                (void)ftrans; /* Glow is baked as per-pixel alpha at decode time. */
                HUD_FLUSH(tid, true);
            }
        }
    }

    /* ---- Status bar layout (Outlaws-faithful) ----
     *
     * Scale factors derived from original 640×480 Jedi Engine reference (Ghidra confirmed):
     *   sx = W/640, sy = H/480
     *   Panel (INTERFAC.NWX): 640×43 at 480 reference → bar_h = 43*sy ≈ 64.5px at 720p
     *   Ammo digits x:  0xC4 * sx = 196 * sx
     *   Hearts x start: 0x222 * sx = 546 * sx, step = heart_w/4, 10 hearts right-to-left
     *
     * Rendering order:
     *   1. Solid dark background (fills bar area — prevents 3D world showing through
     *      transparent holes in INTERFAC.NWX on levels like CANYON)
     *   2. INTERFAC.NWX panel texture on top (transparent pixels reveal dark background)
     *   3. HUD elements (ammo, face portrait, hearts)
     */
    {
        f32 sx    = W / 640.0f;
        f32 sy    = H / 480.0f;

        /* Bar height from INTERFAC.NWX FRMT off_y=-42 → 43px at 480 reference */
        f32 bar_h = 43.0f * sy;
        f32 by    = H - bar_h;

        /* Step 1: solid dark background behind everything */
        hud_quad(vbuf, &nv, 0, by, W, H, 0,0,0,0, 0.07f,0.05f,0.03f,1.0f);
        HUD_FLUSH(0, false);

        /* ---- Digit rendering helper ----
         * Left-aligns number starting at pixel x=lx, top y=dy, digit height=dh.
         */
        #define DRAW_DIGITS(value, lx, dy, dh, cr, cg, cb) do { \
            char _buf[8]; \
            int _n = snprintf(_buf, sizeof(_buf), "%d", (value)); \
            f32 _dh = (dh); \
            f32 _dw; { \
                u32 _d0 = r->digit_tex[0]; \
                if (_d0 && _d0 <= r->texture_count && r->textures[_d0-1].height > 0) \
                    _dw = _dh * (f32)r->textures[_d0-1].width / (f32)r->textures[_d0-1].height; \
                else \
                    _dw = _dh * 34.0f / 66.0f; \
            } \
            f32 _spacing = _dw + 1.0f; \
            f32 _x = (lx); \
            for (int _i = 0; _i < _n; _i++, _x += _spacing) { \
                int _d = _buf[_i] - '0'; \
                if (_d < 0 || _d > 9) continue; \
                u32 _tid = r->digit_tex[_d]; \
                if (_tid && _tid <= r->texture_count) { \
                    hud_quad(vbuf, &nv, _x, (dy), _x+_dw, (dy)+_dh, \
                             0,0,1,1, (cr),(cg),(cb),1.0f); \
                    HUD_FLUSH(_tid, true); \
                } else { \
                    hud_quad(vbuf, &nv, _x+1, (dy)+2, _x+_dw-1, (dy)+_dh-2, \
                             0,0,0,0, (cr),(cg),(cb),0.85f); \
                    HUD_FLUSH(0, false); \
                } \
            } \
        } while(0)

        /* Ghidra RE of HUD layout. Cartridges and hearts have their BOTTOM
         * behind the bar and their TOP protruding above it. The bar covers
         * the lower portion of these sprites (like a shelf). */

        /* ---- Clip ammo cartridges (left, above "AMMO") ----
         * Ghidra: each weapon's NWX seq 11 = ammo sprite.
         * clip_size < 13 → 1 icon per round. >= 13 → 1 icon per 10.
         * Positioned right-to-left, x_base ≈ 0x4A/640 * W. */
        {
            int wi = hud->weapon_idx;
            u32 ammo_tex = (wi >= 0 && wi < WEAPON_COUNT) ? r->weapon_ammo_tex[wi] : 0;
            if (hud->clip_size > 0 && hud->ammo > 0 && ammo_tex && ammo_tex <= r->texture_count) {
                const GpuTexture *at = &r->textures[ammo_tex - 1];
                /* Ghidra: ammo_per_icon = (clip_size < 13) ? 1 : 10 */
                i32 ammo_per_icon = (hud->clip_size < 13) ? 1 : 10;
                i32 n_icons = hud->ammo / ammo_per_icon;
                if (n_icons > 12) n_icons = 12;
                if (n_icons < 0) n_icons = 0;

                f32 shell_h = bar_h * 1.5f;
                f32 shell_w = (at->width > 0 && at->height > 0)
                              ? shell_h * (f32)at->width / (f32)at->height : shell_h * 0.3f;
                /* Ghidra spacing: min(shell_w*0.75, available_space/(n-1)) */
                f32 shell_step = shell_w * 0.75f;
                /* Ghidra: x_base ≈ 74/640 * W, rightmost = x_base + total_span/2 */
                f32 x_base = 74.0f * sx;
                f32 x_start = x_base + (shell_w * 0.5f + shell_step * (f32)(n_icons - 1)) * 0.5f;
                /* Shell top protrudes well above bar, bottom hidden behind */
                f32 shell_y = by - shell_h * 0.6f;
                for (i32 ci = 0; ci < n_icons; ci++) {
                    f32 shx = x_start - (f32)ci * shell_step;
                    hud_quad(vbuf, &nv, shx, shell_y, shx + shell_w, shell_y + shell_h,
                             0, 0, 1, 1, 1, 1, 1, 1);
                    HUD_FLUSH(ammo_tex, true);
                }
            }
        }

        /* ---- Reserve ammo digits (above "RESERVE", ~196/640) ---- */
        {
            f32 dh     = bar_h * 1.4f;
            f32 dy     = by - dh * 0.7f;
            f32 ammo_x = 200.0f * sx;
            DRAW_DIGITS(OL_CLAMP(hud->reserve, 0, 999), ammo_x, dy, dh,
                        1, 1, 1);
        }

        /* ---- Center: thermometer/energy (INTNERGY.NWX) ---- */
        if (r->hud_energy_cell_count > 0) {
            f32 hp_frac = OL_CLAMP((f32)hud->health / (f32)hud->max_health, 0.0f, 1.0f);
            u32 ecell_idx = (u32)(hp_frac * (f32)(r->hud_energy_cell_count - 1) + 0.5f);
            if (ecell_idx >= r->hud_energy_cell_count)
                ecell_idx = r->hud_energy_cell_count - 1;
            u32 etex = r->hud_energy_cells[ecell_idx];
            if (etex && etex <= r->texture_count) {
                const GpuTexture *et = &r->textures[etex - 1];
                f32 eh = bar_h * 0.7f;
                f32 ew = (et->width > 0 && et->height > 0)
                         ? eh * (f32)et->width / (f32)et->height : eh * 3.0f;
                f32 ex = W * 0.5f - ew * 0.5f;
                f32 ey = by + (bar_h - eh) * 0.5f;
                hud_quad(vbuf, &nv, ex, ey, ex + ew, ey + eh, 0, 0, 1, 1, 1, 1, 1, 1);
                HUD_FLUSH(etex, true);
            }
        }

        /* ---- Right: 10 hearts (above "HEALTH") ----
         * Ghidra RE: heart_step = heart_w / 4 (25% of width = tight overlap).
         * x_base = 0x222 * W/640 = 546/640 * W.
         * x_start = x_base + (heart_w/2 + step*9) / 2.
         * Bottom of hearts at screen bottom, top protrudes above bar. */
        if (r->hud_heart_tex && r->hud_heart_tex <= r->texture_count) {
            f32 hp_frac = OL_CLAMP((f32)hud->health / (f32)hud->max_health, 0.0f, 1.0f);
            int full_hearts = (int)(hp_frac * 10.0f + 0.5f);
            if (full_hearts > 10) full_hearts = 10;

            const GpuTexture *ht = &r->textures[r->hud_heart_tex - 1];
            f32 heart_h = bar_h * 1.5f;
            f32 heart_w = (ht->width > 0 && ht->height > 0)
                          ? heart_h * (f32)ht->width / (f32)ht->height : heart_h * 0.5f;
            /* Ghidra: step = heart_w * 0x4000 >> 16 = heart_w / 4 */
            f32 heart_step = heart_w * 0.25f;

            /* Ghidra: x_base = 0x222 * scaleX >> 16 */
            f32 x_base = 546.0f / 640.0f * W;
            f32 x_start = x_base + (heart_w * 0.5f + heart_step * 9.0f) * 0.5f;
            /* Hearts top protrudes well above bar */
            f32 heart_y = by - heart_h * 0.6f;

            for (int hi = 0; hi < 10; hi++) {
                f32 hx = x_start - (f32)hi * heart_step;
                if (hi < full_hearts) {
                    hud_quad(vbuf, &nv, hx, heart_y, hx + heart_w, heart_y + heart_h,
                             0, 0, 1, 1, 1, 1, 1, 1);
                    HUD_FLUSH(r->hud_heart_tex, true);
                }
            }
        }

        /* Step LAST: INTERFAC.NWX panel bar rendered ON TOP of everything.
         * The bar covers the bottom portion of cartridges/hearts, creating
         * the "shelf" effect where only the tops protrude above. */
        if (r->hud_panel_tex && r->hud_panel_tex <= r->texture_count) {
            hud_quad(vbuf, &nv, 0, by, W, H, 0,0,1,1, 1,1,1,1);
            HUD_FLUSH(r->hud_panel_tex, true);
        }

        #undef DRAW_DIGITS
    }

    /* ---- On-screen message (USER_MSG / level events), centered near top ---- */
    if (hud->message && hud->message[0]) {
        const char *msg = hud->message;
        int len = (int)strlen(msg);
        f32 px = OL_MAX(2.0f, H / 260.0f);  /* pixel size, scales with resolution */
        f32 char_w = 6.0f * px;             /* 5 cols + 1 spacing */
        f32 total_w = len * char_w;
        f32 sx = (W - total_w) * 0.5f;
        f32 sy = H * 0.12f;
        /* Shadow + text: draw each lit font pixel as a small quad. */
        for (int pass = 0; pass < 2; pass++) {
            f32 ox = (pass == 0) ? px : 0.0f, oy = (pass == 0) ? px : 0.0f;
            f32 cr = (pass == 0) ? 0.0f : 1.0f;
            f32 cg = (pass == 0) ? 0.0f : 0.92f;
            f32 cb = (pass == 0) ? 0.0f : 0.55f;
            for (int ci = 0; ci < len; ci++) {
                int gi = font5x7_index(msg[ci]);
                const unsigned char *gl = FONT5X7[gi];
                f32 gx = sx + ci * char_w + ox;
                for (int col = 0; col < 5; col++) {
                    for (int row = 0; row < 7; row++) {
                        if (!(gl[col] & (1 << row))) continue;
                        f32 qx = gx + col * px, qy = sy + row * px + oy;
                        hud_quad(vbuf, &nv, qx, qy, qx + px, qy + px,
                                 0,0,0,0, cr, cg, cb, 1.0f);
                    }
                }
            }
            HUD_FLUSH(0, false);
        }
    }

    /* ---- Inventory readout (held keys/tools), top-left, one item per line ---- */
    if (hud->inventory && hud->inventory[0]) {
        f32 px = OL_MAX(1.0f, H / 400.0f);
        f32 char_w = 6.0f * px, line_h = 9.0f * px;
        f32 sx = 8.0f * px, sy = H * 0.14f;
        int line = 0, col0 = 0;
        const char *s = hud->inventory;
        for (int i = 0; ; i++) {
            char c = s[i];
            if (c == '\n' || c == '\0') {
                HUD_FLUSH(0, false);  /* flush per line — keeps vbuf under cap */
                line++; col0 = i + 1;
                if (c == '\0') break;
                continue;
            }
            int gi = font5x7_index(c);
            const unsigned char *gl = FONT5X7[gi];
            f32 gx = sx + (i - col0) * char_w;
            f32 gy = sy + line * line_h;
            for (int pass = 0; pass < 2; pass++) {
                f32 ox = (pass == 0) ? px : 0.0f, oy = (pass == 0) ? px : 0.0f;
                f32 cr = (pass == 0) ? 0.0f : 1.0f;
                f32 cg = (pass == 0) ? 0.0f : 0.85f;
                f32 cb = (pass == 0) ? 0.0f : 0.30f;
                for (int col = 0; col < 5; col++)
                    for (int row = 0; row < 7; row++) {
                        if (!(gl[col] & (1 << row))) continue;
                        f32 qx = gx + col * px + ox, qy = gy + row * px + oy;
                        hud_quad(vbuf, &nv, qx, qy, qx + px, qy + px,
                                 0,0,0,0, cr, cg, cb, 1.0f);
                    }
            }
        }
    }

    /* ---- Crosshair ---- */
    if (hud->show_crosshair) {
        f32 cx = W*0.5f, cy = H*0.5f;
        f32 cs = 10, ct = 2;
        /* Gap in center for classic FPS crosshair look */
        hud_quad(vbuf, &nv, cx-cs, cy-ct*0.5f, cx-4, cy+ct*0.5f, 0,0,0,0, 1,1,1,0.85f);
        hud_quad(vbuf, &nv, cx+4,  cy-ct*0.5f, cx+cs, cy+ct*0.5f, 0,0,0,0, 1,1,1,0.85f);
        hud_quad(vbuf, &nv, cx-ct*0.5f, cy-cs, cx+ct*0.5f, cy-4, 0,0,0,0, 1,1,1,0.85f);
        hud_quad(vbuf, &nv, cx-ct*0.5f, cy+4,  cx+ct*0.5f, cy+cs, 0,0,0,0, 1,1,1,0.85f);
        HUD_FLUSH(0, false);
    }

    #undef HUD_FLUSH

    glBindVertexArray(0);
    glUseProgram(0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
}

/* -------------------------------------------------------------------------
 * Level loading screen (faithful to olwin.exe FUN_0042d050):
 *   background = MM220.PCX (640x480), a thin GREEN progress bar at
 *   x=80 y=420 w=320 h=5 (in the 640x480 background space; RGB≈1,254,96),
 *   plus the level name. Rendered by the game loop between load stages.
 * ---------------------------------------------------------------------- */
void renderer_draw_loading(Renderer *r, u32 bg_tex, f32 progress,
                           const char *label) {
    if (!r->prog_hud || !r->hud_vao) return;
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    f32 W = (f32)r->cfg.width, H = (f32)r->cfg.height;
    f32 sx = W / 640.0f, sy = H / 480.0f;
    Mat4 ortho = mat4_ortho(0, W, H, 0, -1, 1);

    glUseProgram(r->prog_hud);
    GLint ortho_loc  = glGetUniformLocation(r->prog_hud, "uOrtho");
    GLint hastex_loc = glGetUniformLocation(r->prog_hud, "uHasTex");
    GLint tex_loc    = glGetUniformLocation(r->prog_hud, "uTex");
    glUniformMatrix4fv(ortho_loc, 1, GL_FALSE, &ortho.m[0][0]);
    glUniform1i(tex_loc, 0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glBindVertexArray(r->hud_vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->hud_vbo);

    static f32 vb[1024 * 6 * HUD_STRIDE];
    u32 nv;
    #define LD_FLUSH(tex,has) do { if (nv){ \
        glUniform1i(hastex_loc,(has)?1:0); glActiveTexture(GL_TEXTURE0); \
        if((has)&&(tex)>0&&(tex)<=r->texture_count) glBindTexture(GL_TEXTURE_2D,r->textures[(tex)-1].handle); \
        else glBindTexture(GL_TEXTURE_2D,0); \
        glBufferSubData(GL_ARRAY_BUFFER,0,nv*HUD_STRIDE*sizeof(f32),vb); \
        glDrawArrays(GL_TRIANGLES,0,(GLsizei)nv); nv=0; } } while(0)

    /* Opaque black backdrop (so any index-0/discarded bg texels stay black,
     * not the sky clear-color), then the MM220 background on top. */
    nv = 0;
    hud_quad(vb,&nv, 0,0, W,H, 0,0,0,0, 0,0,0,1);
    LD_FLUSH(0, false);
    if (bg_tex > 0 && bg_tex <= r->texture_count) {
        hud_quad(vb,&nv, 0,0, W,H, 0,0,1,1, 1,1,1,1);
        LD_FLUSH(bg_tex, true);
    }

    /* Progress bar: dark trough + green fill (RGB 1/254/96). */
    f32 bx = 80.0f * sx, by = 420.0f * sy, bw = 320.0f * sx, bh = 5.0f * sy;
    if (bh < 3.0f) bh = 3.0f;
    hud_quad(vb,&nv, bx-1, by-1, bx+bw+1, by+bh+1, 0,0,0,0, 0,0,0,0.6f);
    LD_FLUSH(0, false);
    hud_quad(vb,&nv, bx, by, bx + bw*progress, by+bh, 0,0,0,0,
             1.0f/255.0f, 254.0f/255.0f, 96.0f/255.0f, 1.0f);
    LD_FLUSH(0, false);

    /* Level name centred just above the bar. */
    if (label && label[0]) {
        int len = 0; while (label[len] && len < 32) len++;
        f32 px = OL_MAX(2.0f, sy * 3.0f);
        f32 cw = 6.0f * px;
        f32 tx = (W - len * cw) * 0.5f, ty = by - 14.0f * px;
        for (int pass = 0; pass < 2; pass++) {
            f32 ox = (pass==0)?px:0, oy = (pass==0)?px:0;
            f32 cr = (pass==0)?0:1, cg = (pass==0)?0:0.92f, cb = (pass==0)?0:0.55f;
            for (int ci = 0; ci < len; ci++) {
                int gi = font5x7_index(label[ci]);
                const unsigned char *gl = FONT5X7[gi];
                f32 gx = tx + ci*cw + ox;
                for (int col = 0; col < 5; col++)
                    for (int row = 0; row < 7; row++) {
                        if (!(gl[col] & (1<<row))) continue;
                        f32 qx = gx+col*px, qy = ty+row*px+oy;
                        hud_quad(vb,&nv, qx,qy, qx+px,qy+px, 0,0,0,0, cr,cg,cb,1);
                    }
            }
            LD_FLUSH(0, false);
        }
    }
    #undef LD_FLUSH

    glBindVertexArray(0);
    glUseProgram(0);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
}

void renderer_resize(Renderer *r, int width, int height) {
    r->cfg.width  = width;
    r->cfg.height = height;
    glViewport(0, 0, width, height);
    f32 zoom = (r->view_zoom > 1.0f) ? r->view_zoom : 1.0f;
    f32 hfov_rad = (r->cfg.fov / zoom) * OL_DEG2RAD;
    f32 aspect   = (f32)width / (f32)height;
    f32 fov_rad  = 2.0f * atanf(tanf(hfov_rad * 0.5f) / aspect);
    r->proj = mat4_perspective(fov_rad, aspect, r->cfg.near_plane, r->cfg.far_plane);
    r->cam_fov_rad = fov_rad;
}

/* View zoom (rifle scope): narrows the FOV by `zoom` (1.0 = none). */
void renderer_set_zoom(Renderer *r, f32 zoom) {
    r->view_zoom = zoom;
    renderer_resize(r, r->cfg.width, r->cfg.height);
}
