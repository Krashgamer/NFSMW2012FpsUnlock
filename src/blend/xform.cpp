#include "xform.h"

#include <cmath>
#include <cstring>

namespace blend
{
namespace
{
inline float Dot3(const float* p, const float* q)
{
    return p[0] * q[0] + p[1] * q[1] + p[2] * q[2];
}

// m: three normalised rows of four floats.
void MatToQuat(const float* m, float* q)
{
    const float m00 = m[0], m01 = m[1], m02 = m[2];
    const float m10 = m[4], m11 = m[5], m12 = m[6];
    const float m20 = m[8], m21 = m[9], m22 = m[10];
    const float tr = m00 + m11 + m22;
    // The signs pair with QuatToMat below; getting them backwards yields the
    // conjugate, i.e. the transposed rotation.
    if (tr > 0.0f)
    {
        const float s = sqrtf(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (m12 - m21) / s;
        q[1] = (m20 - m02) / s;
        q[2] = (m01 - m10) / s;
    }
    else if (m00 > m11 && m00 > m22)
    {
        const float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
        q[3] = (m12 - m21) / s;
        q[0] = 0.25f * s;
        q[1] = (m01 + m10) / s;
        q[2] = (m02 + m20) / s;
    }
    else if (m11 > m22)
    {
        const float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
        q[3] = (m20 - m02) / s;
        q[0] = (m01 + m10) / s;
        q[1] = 0.25f * s;
        q[2] = (m12 + m21) / s;
    }
    else
    {
        const float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
        q[3] = (m01 - m10) / s;
        q[0] = (m02 + m20) / s;
        q[1] = (m12 + m21) / s;
        q[2] = 0.25f * s;
    }
}

// Writes three rows of four floats, xyz only.
void QuatToMat(const float* q, float* m)
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0]  = 1.0f - 2.0f * (y * y + z * z);
    m[1]  = 2.0f * (x * y + w * z);
    m[2]  = 2.0f * (x * z - w * y);
    m[4]  = 2.0f * (x * y - w * z);
    m[5]  = 1.0f - 2.0f * (x * x + z * z);
    m[6]  = 2.0f * (y * z + w * x);
    m[8]  = 2.0f * (x * z + w * y);
    m[9]  = 2.0f * (y * z - w * x);
    m[10] = 1.0f - 2.0f * (x * x + y * y);
}

void SlerpQuat(const float* qa, const float* qbIn, float t, float* out)
{
    float qb[4] = { qbIn[0], qbIn[1], qbIn[2], qbIn[3] };
    float d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
    if (d < 0.0f)                       // take the short way round
    {
        for (int i = 0; i < 4; ++i)
            qb[i] = -qb[i];
        d = -d;
    }
    if (d > 0.9995f)                    // nearly identical: nlerp is exact enough
    {
        for (int i = 0; i < 4; ++i)
            out[i] = qa[i] + (qb[i] - qa[i]) * t;
    }
    else
    {
        const float th = acosf(d);
        const float sn = sinf(th);
        const float wa = sinf((1.0f - t) * th) / sn;
        const float wb = sinf(t * th) / sn;
        for (int i = 0; i < 4; ++i)
            out[i] = qa[i] * wa + qb[i] * wb;
    }
    const float n = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
    if (n > 1.0e-8f)
        for (int i = 0; i < 4; ++i)
            out[i] /= n;
}

// A sign slip in MatToQuat silently returns the conjugate and every model
// renders with its rotation transposed. The round trip is checked once on all
// four MatToQuat branches; if it ever fails, every blend uses the component
// lerp instead.
bool SlerpSelfTest()
{
    static const float qs[4][4] = {
        { 0.0f, 0.0f, 0.0f, 1.0f },
        { 1.0f, 0.0f, 0.0f, 0.0f },
        { 0.0f, 1.0f, 0.0f, 0.0f },
        { 0.0f, 0.0f, 1.0f, 0.0f },
    };
    for (int i = 0; i < 4; ++i)
    {
        float m[12] = { 0 }, q[4], back[12] = { 0 };
        QuatToMat(qs[i], m);
        MatToQuat(m, q);
        QuatToMat(q, back);
        for (int k = 0; k < 12; ++k)
        {
            if (k % 4 == 3)
                continue;
            const float d = m[k] - back[k];
            if (d > 1.0e-3f || d < -1.0e-3f)
                return false;
        }
    }
    // And the blend must reproduce its endpoint.
    float ma[12] = { 0 }, mb[12] = { 0 }, qa[4], qb[4], qo[4], mo[12] = { 0 };
    const float h = 0.70710678f;
    const float a0[4] = { 0.0f, 0.0f, 0.0f, 1.0f };   // identity
    const float b0[4] = { 0.0f, h, 0.0f, h };         // 90 degrees of yaw
    QuatToMat(a0, ma);
    QuatToMat(b0, mb);
    MatToQuat(ma, qa);
    MatToQuat(mb, qb);
    SlerpQuat(qa, qb, 1.0f, qo);
    QuatToMat(qo, mo);
    for (int k = 0; k < 12; ++k)
    {
        if (k % 4 == 3)
            continue;
        const float d = mb[k] - mo[k];
        if (d > 1.0e-3f || d < -1.0e-3f)
            return false;
    }
    return true;
}

int g_slerpOk = -1;   // -1 untested, 0 disabled by the self test, 1 live
} // namespace

bool Plausible(const float* t)
{
    for (int i = 0; i < 16; ++i)
    {
        if ((i & 3) == 3)
            continue;
        const float v = t[i];
        if (!(v > -100000.0f && v < 100000.0f))   // NaN fails this too
            return false;
    }
    return true;
}

bool Finite16(const float* m)
{
    for (int i = 0; i < 16; ++i)
        if (!(m[i] > -1.0e6f && m[i] < 1.0e6f))
            return false;
    return true;
}

float PosDist2(const float* a, const float* b)
{
    const float dx = a[12] - b[12];
    const float dy = a[13] - b[13];
    const float dz = a[14] - b[14];
    return dx * dx + dy * dy + dz * dz;
}

void LerpTransform(float* out, const float* a, const float* b, float t)
{
    // The component blend supplies the translation, the w column, and the
    // result whenever a check below rejects the basis.
    for (int i = 0; i < 16; ++i)
        out[i] = a[i] + (b[i] - a[i]) * t;

    if (g_slerpOk < 0)
        g_slerpOk = SlerpSelfTest() ? 1 : 0;
    if (g_slerpOk == 0)
        return;

    float la[3], lb[3], na[12], nb[12];
    for (int r = 0; r < 3; ++r)
    {
        la[r] = sqrtf(Dot3(a + r * 4, a + r * 4));
        lb[r] = sqrtf(Dot3(b + r * 4, b + r * 4));
        if (la[r] < 1.0e-6f || lb[r] < 1.0e-6f)
            return;
        for (int c = 0; c < 3; ++c)
        {
            na[r * 4 + c] = a[r * 4 + c] / la[r];
            nb[r * 4 + c] = b[r * 4 + c] / lb[r];
        }
    }

    // Orthogonal? Shear would come back mangled.
    if (fabsf(Dot3(na, na + 4)) > 0.05f || fabsf(Dot3(na, na + 8)) > 0.05f ||
        fabsf(Dot3(na + 4, na + 8)) > 0.05f || fabsf(Dot3(nb, nb + 4)) > 0.05f ||
        fabsf(Dot3(nb, nb + 8)) > 0.05f || fabsf(Dot3(nb + 4, nb + 8)) > 0.05f)
        return;

    // Right-handed? A reflection has no quaternion.
    const float da = na[0] * (na[5] * na[10] - na[6] * na[9]) -
                     na[1] * (na[4] * na[10] - na[6] * na[8]) +
                     na[2] * (na[4] * na[9] - na[5] * na[8]);
    const float db = nb[0] * (nb[5] * nb[10] - nb[6] * nb[9]) -
                     nb[1] * (nb[4] * nb[10] - nb[6] * nb[8]) +
                     nb[2] * (nb[4] * nb[9] - nb[5] * nb[8]);
    if (da < 0.9f || db < 0.9f)
        return;

    float qa[4], qb[4], qo[4], rot[12];
    MatToQuat(na, qa);
    MatToQuat(nb, qb);
    SlerpQuat(qa, qb, t, qo);
    QuatToMat(qo, rot);

    for (int r = 0; r < 3; ++r)
    {
        const float sc = la[r] + (lb[r] - la[r]) * t;
        for (int c = 0; c < 3; ++c)
            out[r * 4 + c] = rot[r * 4 + c] * sc;
    }
}

bool Lerpable(uint32_t b)
{
    const uint32_t exp = (b >> 23) & 0xFFu;
    if (exp == 0xFFu)                        // inf / NaN
        return false;
    if (exp == 0u)                           // zero, or a denormal
        return (b & 0x7FFFFFFFu) == 0u;
    float v;
    memcpy(&v, &b, sizeof(v));
    const float a = v < 0.0f ? -v : v;
    return a >= 1.0e-20f && a <= 1.0e7f;
}

long LerpFields(uint32_t* out, const uint32_t* prev, const uint32_t* cur, size_t n, float t)
{
    long blended = 0;
    for (size_t i = 0; i < n; ++i)
    {
        const uint32_t bp = prev[i], bc = cur[i];
        if (bp == bc || !Lerpable(bp) || !Lerpable(bc))
        {
            out[i] = bc;
            continue;
        }
        float fp, fc;
        memcpy(&fp, &bp, sizeof(fp));
        memcpy(&fc, &bc, sizeof(fc));
        const float r = fp + (fc - fp) * t;
        memcpy(&out[i], &r, sizeof(r));
        ++blended;
    }
    return blended;
}
} // namespace blend
