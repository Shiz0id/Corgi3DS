/*
 * pica/matrix.h - minimal row-major 4x4 matrix helpers.
 *
 * m[row * 4 + col].  A vertex shader computing `dp4 out.x, M[0], v` etc.
 * evaluates out = M * v with these matrices.
 *
 * The PICA200 clips z to [-w, 0] (not [-w, w]) and the LCDs are rotated by
 * 90 degrees, so the *_tilt projections bake in both adjustments.  With the
 * driver's default depth map (scale -1, offset 0) and PICA_GREATER depth
 * test, nearer fragments win and depth should be cleared to 0.
 */
#ifndef PICA_MATRIX_H
#define PICA_MATRIX_H

#include <math.h>
#include <string.h>

static inline void pica_mtx_identity(float m[16])
{
    memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* out = a * b (out may alias a or b) */
static inline void pica_mtx_multiply(float out[16], const float a[16], const float b[16])
{
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] +
                           a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
    memcpy(out, r, sizeof(r));
}

/* m = m * T(x, y, z) */
static inline void pica_mtx_translate(float m[16], float x, float y, float z)
{
    for (int i = 0; i < 4; i++)
        m[i * 4 + 3] += m[i * 4 + 0] * x + m[i * 4 + 1] * y + m[i * 4 + 2] * z;
}

/* m = m * S(x, y, z) */
static inline void pica_mtx_scale(float m[16], float x, float y, float z)
{
    for (int i = 0; i < 4; i++)
    {
        m[i * 4 + 0] *= x;
        m[i * 4 + 1] *= y;
        m[i * 4 + 2] *= z;
    }
}

/* m = m * R(axis, angle) for a unit axis 0 = X, 1 = Y, 2 = Z */
static inline void pica_mtx_rotate_axis(float m[16], int axis, float radians)
{
    float c = cosf(radians), s = sinf(radians), r[16];
    pica_mtx_identity(r);
    int a = (axis + 1) % 3, b = (axis + 2) % 3;
    r[a * 4 + a] = c;
    r[a * 4 + b] = -s;
    r[b * 4 + a] = s;
    r[b * 4 + b] = c;
    pica_mtx_multiply(m, m, r);
}

/* Orthographic projection for a rotated 3DS screen.  left/right/bottom/top
 * are in the *unrotated* (landscape) orientation, e.g. 0, 400, 0, 240 for
 * the top screen. Maps z = -near to depth -1 and z = -far to 0. */
static inline void pica_mtx_ortho_tilt(float m[16], float left, float right, float bottom, float top,
                                       float near_z, float far_z)
{
    memset(m, 0, sizeof(float) * 16);
    m[0 * 4 + 1] = 2.0f / (top - bottom);
    m[0 * 4 + 3] = (bottom + top) / (bottom - top);
    m[1 * 4 + 0] = 2.0f / (left - right);
    m[1 * 4 + 3] = (left + right) / (right - left);
    m[2 * 4 + 2] = 1.0f / (near_z - far_z);
    m[2 * 4 + 3] = 0.5f * (near_z + far_z) / (near_z - far_z) - 0.5f;
    m[3 * 4 + 3] = 1.0f;
}

/* Perspective projection for a rotated 3DS screen.  `fovy` is the vertical
 * field of view of the landscape image and `aspect` its width / height
 * (400/240 for the top screen). Right-handed, camera looks down -z. */
static inline void pica_mtx_persp_tilt(float m[16], float fovy, float aspect, float near_z, float far_z)
{
    float f = 1.0f / tanf(fovy * 0.5f);
    memset(m, 0, sizeof(float) * 16);
    /* Landscape x -> rotated y, landscape y -> rotated -x */
    m[0 * 4 + 1] = f;
    m[1 * 4 + 0] = -f / aspect;
    m[2 * 4 + 2] = near_z / (near_z - far_z);
    m[2 * 4 + 3] = far_z * near_z / (near_z - far_z);
    m[3 * 4 + 2] = -1.0f;
}

#endif /* PICA_MATRIX_H */
