/*
 * engine.h - Core types and definitions for Outlaws engine
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* -------------------------------------------------------------------------
 * Integer types
 * ---------------------------------------------------------------------- */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;
typedef float    f32;
typedef double   f64;

/* -------------------------------------------------------------------------
 * Math
 * ---------------------------------------------------------------------- */
#define OL_PI       3.14159265358979323846f
#define OL_TWO_PI   (2.0f * OL_PI)
#define OL_HALF_PI  (0.5f * OL_PI)
#define OL_DEG2RAD  (OL_PI / 180.0f)
#define OL_RAD2DEG  (180.0f / OL_PI)

#define OL_MAX(a, b)  ((a) > (b) ? (a) : (b))
#define OL_MIN(a, b)  ((a) < (b) ? (a) : (b))
#define OL_CLAMP(v, lo, hi) OL_MAX((lo), OL_MIN((hi), (v)))

typedef struct { f32 x, y; }          Vec2;
typedef struct { f32 x, y, z; }       Vec3;
typedef struct { f32 x, y, z, w; }    Vec4;
typedef struct { f32 m[4][4]; }       Mat4;

/* Vec3 operations */
static inline Vec3 vec3(f32 x, f32 y, f32 z) { return (Vec3){x, y, z}; }
static inline Vec3 vec3_add(Vec3 a, Vec3 b)  { return (Vec3){a.x+b.x, a.y+b.y, a.z+b.z}; }
static inline Vec3 vec3_sub(Vec3 a, Vec3 b)  { return (Vec3){a.x-b.x, a.y-b.y, a.z-b.z}; }
static inline Vec3 vec3_scale(Vec3 v, f32 s) { return (Vec3){v.x*s, v.y*s, v.z*s}; }
static inline f32  vec3_dot(Vec3 a, Vec3 b)  { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline f32  vec3_len(Vec3 v)          { return sqrtf(vec3_dot(v, v)); }
static inline Vec3 vec3_norm(Vec3 v)         { f32 l = vec3_len(v); return l > 1e-6f ? vec3_scale(v, 1.0f/l) : (Vec3){0,0,0}; }
static inline Vec3 vec3_cross(Vec3 a, Vec3 b) {
    return (Vec3){ a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}

/* Mat4 operations */
static inline Mat4 mat4_identity(void) {
    Mat4 m = {0};
    m.m[0][0] = m.m[1][1] = m.m[2][2] = m.m[3][3] = 1.0f;
    return m;
}

/* Column-major mat4 multiply: m[col][row].
 * For C = A*B: C[col=i][row=j] = sum_k A[col=k][row=j] * B[col=i][row=k]
 *                               = sum_k a.m[k][j]       * b.m[i][k]       */
static inline Mat4 mat4_mul(Mat4 a, Mat4 b) {
    Mat4 r = {0};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++)
                r.m[i][j] += a.m[k][j] * b.m[i][k];
    return r;
}

static inline Mat4 mat4_perspective(f32 fov_y, f32 aspect, f32 znear, f32 zfar) {
    Mat4 m = {0};
    f32 f = 1.0f / tanf(fov_y * 0.5f);
    m.m[0][0] = f / aspect;
    m.m[1][1] = f;
    m.m[2][2] = (zfar + znear) / (znear - zfar);
    m.m[2][3] = -1.0f;
    m.m[3][2] = (2.0f * zfar * znear) / (znear - zfar);
    return m;
}

static inline Mat4 mat4_look_at(Vec3 eye, Vec3 center, Vec3 up) {
    Vec3 f = vec3_norm(vec3_sub(center, eye));
    Vec3 r = vec3_norm(vec3_cross(f, up));
    Vec3 u = vec3_cross(r, f);
    Mat4 m = mat4_identity();
    m.m[0][0] = r.x;  m.m[1][0] = r.y;  m.m[2][0] = r.z;
    m.m[0][1] = u.x;  m.m[1][1] = u.y;  m.m[2][1] = u.z;
    m.m[0][2] = -f.x; m.m[1][2] = -f.y; m.m[2][2] = -f.z;
    m.m[3][0] = -vec3_dot(r, eye);
    m.m[3][1] = -vec3_dot(u, eye);
    m.m[3][2] =  vec3_dot(f, eye);
    return m;
}

static inline Mat4 mat4_translate(Vec3 t) {
    Mat4 m = mat4_identity();
    m.m[3][0] = t.x; m.m[3][1] = t.y; m.m[3][2] = t.z;
    return m;
}

static inline Mat4 mat4_rotate_y(f32 angle) {
    Mat4 m = mat4_identity();
    f32 c = cosf(angle), s = sinf(angle);
    m.m[0][0] =  c; m.m[2][0] = s;
    m.m[0][2] = -s; m.m[2][2] = c;
    return m;
}

static inline Mat4 mat4_scale(Vec3 s) {
    Mat4 m = mat4_identity();
    m.m[0][0] = s.x; m.m[1][1] = s.y; m.m[2][2] = s.z;
    return m;
}

/* Orthographic projection (column-major, for HUD/2D) */
static inline Mat4 mat4_ortho(f32 left, f32 right, f32 bottom, f32 top,
                               f32 znear, f32 zfar) {
    Mat4 m = {0};
    m.m[0][0] =  2.0f / (right - left);
    m.m[1][1] =  2.0f / (top - bottom);
    m.m[2][2] = -2.0f / (zfar - znear);
    m.m[3][0] = -(right + left)  / (right - left);
    m.m[3][1] = -(top   + bottom) / (top - bottom);
    m.m[3][2] = -(zfar  + znear)  / (zfar - znear);
    m.m[3][3] = 1.0f;
    return m;
}

static inline Vec3 vec3_neg(Vec3 v) { return (Vec3){-v.x, -v.y, -v.z}; }
static inline f32  vec3_dist(Vec3 a, Vec3 b) { return vec3_len(vec3_sub(a, b)); }

/* -------------------------------------------------------------------------
 * Logging
 * ---------------------------------------------------------------------- */
#define OL_LOG(fmt, ...)  fprintf(stdout, "[OL] " fmt, ##__VA_ARGS__)
#define OL_WARN(fmt, ...) fprintf(stderr, "[WARN] " fmt, ##__VA_ARGS__)
#define OL_ERR(fmt, ...)  fprintf(stderr, "[ERR] %s:%d: " fmt, __FILE__, __LINE__, ##__VA_ARGS__)

/* -------------------------------------------------------------------------
 * String helpers
 * ---------------------------------------------------------------------- */
static inline void str_to_lower(char *s) {
    for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32;
}

static inline const char *str_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot ? dot + 1 : "";
}

/* -------------------------------------------------------------------------
 * Dynamic array
 * ---------------------------------------------------------------------- */
#define DA_INIT_CAP 16

#define DA_DECL(T, name) \
    struct { T *data; u32 len; u32 cap; } name

#define DA_PUSH(arr, val) do { \
    if ((arr).len >= (arr).cap) { \
        (arr).cap = (arr).cap ? (arr).cap * 2 : DA_INIT_CAP; \
        (arr).data = realloc((arr).data, (arr).cap * sizeof(*(arr).data)); \
    } \
    (arr).data[(arr).len++] = (val); \
} while(0)

#define DA_FREE(arr) do { free((arr).data); (arr).data = NULL; (arr).len = (arr).cap = 0; } while(0)
