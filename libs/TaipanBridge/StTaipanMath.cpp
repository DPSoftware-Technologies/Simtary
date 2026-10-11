// StTaipanMath.cpp - the Unity math vocabulary for Taipan scripts.
//
// Vector2, Vector3, Quaternion and Color are Taipan VALUE types (0.2.0): immutable,
// garbage-collected copies with constructors, operators and static members, so
//
//     transform.position += Vector3.up * speed * Time.deltaTime
//     transform.rotation = Quaternion.Euler(0, yaw, 0) * transform.rotation
//
// reads exactly as it would in C#. The names, defaults and conventions are Unity's:
// angles in DEGREES, Euler order Z then X then Y, left-handed with +Z forward and
// +Y up (the engine's own frame, so no conversion happens at the boundary).
//
// Pure math: nothing here touches the engine, so it lives in the bridge, next to the
// one translation unit that may include taipan.h.
#include "StTaipanInternal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace st::taipan {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDeg2Rad = kPi / 180.0f;
constexpr float kRad2Deg = 180.0f / kPi;
constexpr float kEpsilon = 1e-5f;

// ---------------------------------------------------------------------------
// vector helpers
// ---------------------------------------------------------------------------
V3 Add (const V3& a, const V3& b) { return V3(a.x + b.x, a.y + b.y, a.z + b.z); }
V3 Sub (const V3& a, const V3& b) { return V3(a.x - b.x, a.y - b.y, a.z - b.z); }
V3 Mul (const V3& a, float k) { return V3(a.x * k, a.y * k, a.z * k); }
float Dot (const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross (const V3& a, const V3& b) {
    return V3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
float Length (const V3& a) { return std::sqrt(Dot(a, a)); }
V3 Normalized (const V3& a) {
    const float l = Length(a);
    return l > kEpsilon ? Mul(a, 1.0f / l) : V3();
}
float Clamp01 (float t) { return std::clamp(t, 0.0f, 1.0f); }

V2 Add2 (const V2& a, const V2& b) { return V2(a.x + b.x, a.y + b.y); }
V2 Sub2 (const V2& a, const V2& b) { return V2(a.x - b.x, a.y - b.y); }
V2 Mul2 (const V2& a, float k) { return V2(a.x * k, a.y * k); }
float Dot2 (const V2& a, const V2& b) { return a.x * b.x + a.y * b.y; }
float Length2 (const V2& a) { return std::sqrt(Dot2(a, a)); }
V2 Normalized2 (const V2& a) {
    const float l = Length2(a);
    return l > kEpsilon ? Mul2(a, 1.0f / l) : V2();
}

// ---------------------------------------------------------------------------
// quaternion helpers (Hamilton product; v' = q v q*)
// ---------------------------------------------------------------------------
Q4 QMul (const Q4& a, const Q4& b) {
    return Q4(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
              a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
              a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
              a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
}
V3 QRotate (const Q4& q, const V3& v) {
    const V3 u(q.x, q.y, q.z);
    const V3 t = Mul(Cross(u, v), 2.0f);
    return Add(Add(v, Mul(t, q.w)), Cross(u, t));
}
float QDot (const Q4& a, const Q4& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
Q4 QNormalize (const Q4& q) {
    const float l = std::sqrt(QDot(q, q));
    return l > kEpsilon ? Q4(q.x / l, q.y / l, q.z / l, q.w / l) : Q4();
}
Q4 QInverse (const Q4& q) {
    const float n = QDot(q, q);
    return n > kEpsilon ? Q4(-q.x / n, -q.y / n, -q.z / n, q.w / n) : Q4();
}
Q4 QAxis (float degrees, const V3& axis) {
    const V3 a = Normalized(axis);
    const float h = degrees * kDeg2Rad * 0.5f, s = std::sin(h);
    return Q4(a.x * s, a.y * s, a.z * s, std::cos(h));
}
// Unity's Euler: roll about Z first, then pitch about X, then yaw about Y.
Q4 QEuler (float x, float y, float z) {
    return QMul(QMul(QAxis(y, V3(0, 1, 0)), QAxis(x, V3(1, 0, 0))), QAxis(z, V3(0, 0, 1)));
}
float WrapDegrees (float a) {
    a = std::fmod(a, 360.0f);
    return a < 0.0f ? a + 360.0f : a;
}
V3 QToEuler (const Q4& qIn) {
    const Q4 q = QNormalize(qIn);
    // rotation matrix entries of R = Ry * Rx * Rz (column vectors)
    const float r02 = 2.0f * (q.x * q.z + q.w * q.y);
    const float r12 = 2.0f * (q.y * q.z - q.w * q.x);
    const float r22 = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    const float r10 = 2.0f * (q.x * q.y + q.w * q.z);
    const float r11 = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
    const float r00 = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    const float r20 = 2.0f * (q.x * q.z - q.w * q.y);
    const float sx = std::clamp(-r12, -1.0f, 1.0f);
    float x = std::asin(sx), y, z;
    if (std::fabs(sx) < 0.9999f) {
        y = std::atan2(r02, r22);
        z = std::atan2(r10, r11);
    } else {
        // gimbal lock: all the remaining rotation is about Y
        y = std::atan2(-r20, r00);
        z = 0.0f;
    }
    return V3(WrapDegrees(x * kRad2Deg), WrapDegrees(y * kRad2Deg), WrapDegrees(z * kRad2Deg));
}
Q4 QFromBasis (const V3& right, const V3& up, const V3& forward) {
    // columns right / up / forward -> quaternion
    const float m00 = right.x, m01 = up.x, m02 = forward.x;
    const float m10 = right.y, m11 = up.y, m12 = forward.y;
    const float m20 = right.z, m21 = up.z, m22 = forward.z;
    const float trace = m00 + m11 + m22;
    Q4 q;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q = Q4((m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s);
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q = Q4(0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s);
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q = Q4((m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s);
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q = Q4((m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s);
    }
    return QNormalize(q);
}
Q4 QLookRotation (const V3& forward, const V3& upHint) {
    const V3 f = Normalized(forward);
    if (Length(f) < kEpsilon) return Q4();
    V3 r = Cross(upHint, f);
    if (Length(r) < kEpsilon) r = Cross(V3(1, 0, 0), f); // forward parallel to up
    r = Normalized(r);
    const V3 u = Cross(f, r);
    return QFromBasis(r, u, f);
}
Q4 QSlerp (const Q4& a, Q4 b, float t) {
    float d = QDot(a, b);
    if (d < 0.0f) { b = Q4(-b.x, -b.y, -b.z, -b.w); d = -d; }
    if (d > 0.9995f) {
        return QNormalize(Q4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t));
    }
    const float th = std::acos(d), s = std::sin(th);
    const float wa = std::sin((1.0f - t) * th) / s, wb = std::sin(t * th) / s;
    return Q4(a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb);
}
Q4 QFromTo (const V3& from, const V3& to) {
    const V3 a = Normalized(from), b = Normalized(to);
    const float d = Dot(a, b);
    if (d > 0.999999f) return Q4();
    if (d < -0.999999f) {
        V3 axis = Cross(V3(1, 0, 0), a);
        if (Length(axis) < kEpsilon) axis = Cross(V3(0, 1, 0), a);
        return QAxis(180.0f, axis);
    }
    const V3 c = Cross(a, b);
    return QNormalize(Q4(c.x, c.y, c.z, 1.0f + d));
}

std::string Fmt (const char* format, float a, float b, float c = 0, float d = 0) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), format, a, b, c, d);
    return buffer;
}

// Optional-argument statics go through the raw API: the typed staticMethod() takes
//	its signature from the C++ callable, which has no default arguments to report.
void RawStatic (VM* vm, const char* name, TaipanNativeFn fn, const char* signature) {
    taipan_register_function(vm->state, name, std::move(fn), signature);
}

V3 ArgV3 (const TaipanArgs& a, int i) { return *static_cast<const V3*>(a.getValue(i, "Vector3")); }

} // namespace

namespace {

void RegisterUnityMathImpl (VM* vm) {
    TaipanState* s = vm->state;

    // ---------------------------------------------------------------- Vector2
    taipan_register_value<V2>(s, "Vector2")
        .constructor<V2(float, float)>()
        .property("x", &V2::x)
        .property("y", &V2::y)
        .property("magnitude", [](void* p) { return TaipanValue::fromFloat(Length2(*static_cast<V2*>(p))); }, nullptr, "float")
        .property("sqrMagnitude", [](void* p) { const V2& v = *static_cast<V2*>(p); return TaipanValue::fromFloat(Dot2(v, v)); }, nullptr, "float")
        .property("normalized", [](void* p) { return TaipanValue::fromValue(Normalized2(*static_cast<V2*>(p))); }, nullptr, "Vector2")
        .op("+", [](const V2& a, const V2& b) { return Add2(a, b); })
        .op("-", [](const V2& a, const V2& b) { return Sub2(a, b); })
        .op("*", [](const V2& a, float k) { return Mul2(a, k); })
        .op("*", [](float k, const V2& a) { return Mul2(a, k); })
        .op("/", [](const V2& a, float k) { return Mul2(a, 1.0f / k); })
        .op("-", [](const V2& a) { return V2(-a.x, -a.y); })
        .op("==", [](const V2& a, const V2& b) { return Length2(Sub2(a, b)) < kEpsilon; })
        .staticMethod("Dot", [](const V2& a, const V2& b) { return Dot2(a, b); })
        .staticMethod("Distance", [](const V2& a, const V2& b) { return Length2(Sub2(a, b)); })
        .staticMethod("Lerp", [](const V2& a, const V2& b, float t) { return Add2(a, Mul2(Sub2(b, a), Clamp01(t))); })
        .staticMethod("LerpUnclamped", [](const V2& a, const V2& b, float t) { return Add2(a, Mul2(Sub2(b, a), t)); })
        .staticMethod("Angle", [](const V2& a, const V2& b) {
            const float d = Length2(a) * Length2(b);
            return d < kEpsilon ? 0.0f : std::acos(std::clamp(Dot2(a, b) / d, -1.0f, 1.0f)) * kRad2Deg;
        })
        .staticMethod("MoveTowards", [](const V2& a, const V2& b, float maxDelta) {
            const V2 d = Sub2(b, a);
            const float l = Length2(d);
            return (l <= maxDelta || l < kEpsilon) ? b : Add2(a, Mul2(d, maxDelta / l));
        })
        .constant("zero", V2(0, 0))
        .constant("one", V2(1, 1))
        .constant("up", V2(0, 1))
        .constant("down", V2(0, -1))
        .constant("left", V2(-1, 0))
        .constant("right", V2(1, 0))
        .toString([](const V2& v) { return Fmt("(%.2f, %.2f)", v.x, v.y); });

    // ---------------------------------------------------------------- Vector3
    taipan_register_value<V3>(s, "Vector3")
        .constructor<V3(float, float, float)>()
        .property("x", &V3::x)
        .property("y", &V3::y)
        .property("z", &V3::z)
        .property("magnitude", [](void* p) { return TaipanValue::fromFloat(Length(*static_cast<V3*>(p))); }, nullptr, "float")
        .property("sqrMagnitude", [](void* p) { const V3& v = *static_cast<V3*>(p); return TaipanValue::fromFloat(Dot(v, v)); }, nullptr, "float")
        .property("normalized", [](void* p) { return TaipanValue::fromValue(Normalized(*static_cast<V3*>(p))); }, nullptr, "Vector3")
        .op("+", [](const V3& a, const V3& b) { return Add(a, b); })
        .op("-", [](const V3& a, const V3& b) { return Sub(a, b); })
        .op("*", [](const V3& a, float k) { return Mul(a, k); })
        .op("*", [](float k, const V3& a) { return Mul(a, k); })
        .op("/", [](const V3& a, float k) { return Mul(a, 1.0f / k); })
        .op("-", [](const V3& a) { return V3(-a.x, -a.y, -a.z); })
        .op("==", [](const V3& a, const V3& b) { return Length(Sub(a, b)) < kEpsilon; })
        .staticMethod("Dot", [](const V3& a, const V3& b) { return Dot(a, b); })
        .staticMethod("Cross", [](const V3& a, const V3& b) { return Cross(a, b); })
        .staticMethod("Distance", [](const V3& a, const V3& b) { return Length(Sub(a, b)); })
        .staticMethod("Normalize", [](const V3& a) { return Normalized(a); })
        .staticMethod("Lerp", [](const V3& a, const V3& b, float t) { return Add(a, Mul(Sub(b, a), Clamp01(t))); })
        .staticMethod("LerpUnclamped", [](const V3& a, const V3& b, float t) { return Add(a, Mul(Sub(b, a), t)); })
        .staticMethod("MoveTowards", [](const V3& a, const V3& b, float maxDelta) {
            const V3 d = Sub(b, a);
            const float l = Length(d);
            return (l <= maxDelta || l < kEpsilon) ? b : Add(a, Mul(d, maxDelta / l));
        })
        .staticMethod("Angle", [](const V3& a, const V3& b) {
            const float d = Length(a) * Length(b);
            return d < kEpsilon ? 0.0f : std::acos(std::clamp(Dot(a, b) / d, -1.0f, 1.0f)) * kRad2Deg;
        })
        .staticMethod("SignedAngle", [](const V3& a, const V3& b, const V3& axis) {
            const float d = Length(a) * Length(b);
            const float angle = d < kEpsilon ? 0.0f : std::acos(std::clamp(Dot(a, b) / d, -1.0f, 1.0f)) * kRad2Deg;
            return Dot(axis, Cross(a, b)) < 0.0f ? -angle : angle;
        })
        .staticMethod("ClampMagnitude", [](const V3& a, float maxLength) {
            const float l = Length(a);
            return l > maxLength && l > kEpsilon ? Mul(a, maxLength / l) : a;
        })
        .staticMethod("Project", [](const V3& a, const V3& onNormal) {
            const float n = Dot(onNormal, onNormal);
            return n < kEpsilon ? V3() : Mul(onNormal, Dot(a, onNormal) / n);
        })
        .staticMethod("ProjectOnPlane", [](const V3& a, const V3& planeNormal) {
            const float n = Dot(planeNormal, planeNormal);
            return n < kEpsilon ? a : Sub(a, Mul(planeNormal, Dot(a, planeNormal) / n));
        })
        .staticMethod("Reflect", [](const V3& dir, const V3& normal) { return Sub(dir, Mul(normal, 2.0f * Dot(dir, normal))); })
        .staticMethod("Scale", [](const V3& a, const V3& b) { return V3(a.x * b.x, a.y * b.y, a.z * b.z); })
        .staticMethod("Min", [](const V3& a, const V3& b) { return V3(std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)); })
        .staticMethod("Max", [](const V3& a, const V3& b) { return V3(std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)); })
        .constant("zero", V3(0, 0, 0))
        .constant("one", V3(1, 1, 1))
        .constant("up", V3(0, 1, 0))
        .constant("down", V3(0, -1, 0))
        .constant("left", V3(-1, 0, 0))
        .constant("right", V3(1, 0, 0))
        .constant("forward", V3(0, 0, 1))
        .constant("back", V3(0, 0, -1))
        .toString([](const V3& v) { return Fmt("(%.2f, %.2f, %.2f)", v.x, v.y, v.z); });

    // ------------------------------------------------------------- Quaternion
    taipan_register_value<Q4>(s, "Quaternion")
        .constructor<Q4(float, float, float, float)>()
        .property("x", &Q4::x)
        .property("y", &Q4::y)
        .property("z", &Q4::z)
        .property("w", &Q4::w)
        .property("eulerAngles", [](void* p) { return TaipanValue::fromValue(QToEuler(*static_cast<Q4*>(p))); }, nullptr, "Vector3")
        .property("normalized", [](void* p) { return TaipanValue::fromValue(QNormalize(*static_cast<Q4*>(p))); }, nullptr, "Quaternion")
        .op("*", [](const Q4& a, const Q4& b) { return QMul(a, b); })
        .op("*", [](const Q4& q, const V3& v) { return QRotate(q, v); })
        .op("==", [](const Q4& a, const Q4& b) { return std::fabs(QDot(a, b)) > 1.0f - kEpsilon; })
        .staticMethod("Euler", [](float x, float y, float z) { return QEuler(x, y, z); })
        .staticMethod("AngleAxis", [](float degrees, const V3& axis) { return QAxis(degrees, axis); })
        .staticMethod("Inverse", [](const Q4& q) { return QInverse(q); })
        .staticMethod("Dot", [](const Q4& a, const Q4& b) { return QDot(a, b); })
        .staticMethod("Angle", [](const Q4& a, const Q4& b) {
            return 2.0f * std::acos(std::min(std::fabs(QDot(QNormalize(a), QNormalize(b))), 1.0f)) * kRad2Deg;
        })
        .staticMethod("Slerp", [](const Q4& a, const Q4& b, float t) { return QSlerp(a, b, Clamp01(t)); })
        .staticMethod("Lerp", [](const Q4& a, const Q4& b, float t) {
            const float k = Clamp01(t), sgn = QDot(a, b) < 0.0f ? -1.0f : 1.0f;
            return QNormalize(Q4(a.x + (sgn * b.x - a.x) * k, a.y + (sgn * b.y - a.y) * k,
                                 a.z + (sgn * b.z - a.z) * k, a.w + (sgn * b.w - a.w) * k));
        })
        .staticMethod("FromToRotation", [](const V3& from, const V3& to) { return QFromTo(from, to); })
        .staticMethod("RotateTowards", [](const Q4& from, const Q4& to, float maxDegrees) {
            const float angle = 2.0f * std::acos(std::min(std::fabs(QDot(QNormalize(from), QNormalize(to))), 1.0f)) * kRad2Deg;
            return angle < kEpsilon ? to : QSlerp(from, to, std::min(1.0f, maxDegrees / angle));
        })
        .constant("identity", Q4(0, 0, 0, 1))
        .toString([](const Q4& q) { return Fmt("(%.4f, %.4f, %.4f, %.4f)", q.x, q.y, q.z, q.w); });
    RawStatic(vm, "Quaternion.LookRotation", [](TaipanArgs a) {
        const V3 up = a.count() > 1 && !a.isNull(1) ? ArgV3(a, 1) : V3(0, 1, 0);
        return TaipanValue::fromValue(QLookRotation(ArgV3(a, 0), up));
    }, "(Vector3, Vector3=) -> Quaternion");

    // ------------------------------------------------------------------ Color
    taipan_register_value<C4>(s, "Color")
        .constructor<C4(float, float, float, float)>()
        .property("r", &C4::r)
        .property("g", &C4::g)
        .property("b", &C4::b)
        .property("a", &C4::a)
        .property("grayscale", [](void* p) {
            const C4& c = *static_cast<C4*>(p);
            return TaipanValue::fromFloat(0.299f * c.r + 0.587f * c.g + 0.114f * c.b);
        }, nullptr, "float")
        .op("+", [](const C4& a, const C4& b) { return C4(a.r + b.r, a.g + b.g, a.b + b.b, a.a + b.a); })
        .op("-", [](const C4& a, const C4& b) { return C4(a.r - b.r, a.g - b.g, a.b - b.b, a.a - b.a); })
        .op("*", [](const C4& a, const C4& b) { return C4(a.r * b.r, a.g * b.g, a.b * b.b, a.a * b.a); })
        .op("*", [](const C4& a, float k) { return C4(a.r * k, a.g * k, a.b * k, a.a * k); })
        .op("*", [](float k, const C4& a) { return C4(a.r * k, a.g * k, a.b * k, a.a * k); })
        .op("==", [](const C4& a, const C4& b) {
            return std::fabs(a.r - b.r) < kEpsilon && std::fabs(a.g - b.g) < kEpsilon &&
                   std::fabs(a.b - b.b) < kEpsilon && std::fabs(a.a - b.a) < kEpsilon;
        })
        .staticMethod("Lerp", [](const C4& a, const C4& b, float t) {
            const float k = Clamp01(t);
            return C4(a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k, a.a + (b.a - a.a) * k);
        })
        .constant("red", C4(1, 0, 0, 1))
        .constant("green", C4(0, 1, 0, 1))
        .constant("blue", C4(0, 0, 1, 1))
        .constant("white", C4(1, 1, 1, 1))
        .constant("black", C4(0, 0, 0, 1))
        .constant("yellow", C4(1, 0.92f, 0.016f, 1))
        .constant("cyan", C4(0, 1, 1, 1))
        .constant("magenta", C4(1, 0, 1, 1))
        .constant("gray", C4(0.5f, 0.5f, 0.5f, 1))
        .constant("grey", C4(0.5f, 0.5f, 0.5f, 1))
        .constant("clear", C4(0, 0, 0, 0))
        .toString([](const C4& c) { return Fmt("RGBA(%.3f, %.3f, %.3f, %.3f)", c.r, c.g, c.b, c.a); });

    // ------------------------------------------------------------- RaycastHit
    const std::string hitClass = vm->hitObjectClass;
    taipan_register_value<Hit>(s, "RaycastHit")
        .property("point", [](void* p) { return TaipanValue::fromValue(static_cast<Hit*>(p)->point); }, nullptr, "Vector3")
        .property("normal", [](void* p) { return TaipanValue::fromValue(static_cast<Hit*>(p)->normal); }, nullptr, "Vector3")
        .property("distance", [](void* p) { return TaipanValue::fromFloat(static_cast<Hit*>(p)->distance); }, nullptr, "float")
        .property("gameObject", [hitClass](void* p) {
            return TaipanValue::fromObject(hitClass.c_str(), static_cast<Hit*>(p)->object);
        }, nullptr, (hitClass + "?").c_str())
        .toString([](const Hit& h) { return Fmt("RaycastHit(%.2f, %.2f, %.2f) d=%.2f", h.point.x, h.point.y, h.point.z, h.distance); });

    // ------------------------------------------------------------------ Mathf
    taipan_register_class(s, "Mathf")
        .constant("PI", kPi)
        .constant("Deg2Rad", kDeg2Rad)
        .constant("Rad2Deg", kRad2Deg)
        .constant("Epsilon", std::numeric_limits<float>::epsilon())
        .constant("Infinity", std::numeric_limits<float>::infinity())
        .constant("NegativeInfinity", -std::numeric_limits<float>::infinity())
        .staticMethod("Abs", [](float v) { return std::fabs(v); })
        .staticMethod("Sign", [](float v) { return v >= 0.0f ? 1.0f : -1.0f; })
        .staticMethod("Min", [](float a, float b) { return std::min(a, b); })
        .staticMethod("Max", [](float a, float b) { return std::max(a, b); })
        .staticMethod("Clamp", [](float v, float lo, float hi) { return std::clamp(v, lo, std::max(lo, hi)); })
        .staticMethod("Clamp01", [](float v) { return Clamp01(v); })
        .staticMethod("Lerp", [](float a, float b, float t) { return a + (b - a) * Clamp01(t); })
        .staticMethod("LerpUnclamped", [](float a, float b, float t) { return a + (b - a) * t; })
        .staticMethod("InverseLerp", [](float a, float b, float v) { return a == b ? 0.0f : Clamp01((v - a) / (b - a)); })
        .staticMethod("MoveTowards", [](float a, float b, float maxDelta) {
            return std::fabs(b - a) <= maxDelta ? b : a + (b > a ? maxDelta : -maxDelta);
        })
        .staticMethod("SmoothStep", [](float a, float b, float t) {
            const float k = Clamp01(t);
            return a + (b - a) * (k * k * (3.0f - 2.0f * k));
        })
        .staticMethod("Repeat", [](float t, float length) { return length == 0.0f ? 0.0f : t - std::floor(t / length) * length; })
        .staticMethod("PingPong", [](float t, float length) {
            if (length == 0.0f) return 0.0f;
            const float r = t - std::floor(t / (length * 2.0f)) * length * 2.0f;
            return length - std::fabs(r - length);
        })
        .staticMethod("DeltaAngle", [](float a, float b) {
            float d = std::fmod(b - a, 360.0f);
            if (d > 180.0f) d -= 360.0f;
            if (d < -180.0f) d += 360.0f;
            return d;
        })
        .staticMethod("Approximately", [](float a, float b) {
            return std::fabs(b - a) < std::max(1e-6f * std::max(std::fabs(a), std::fabs(b)), 8.0f * std::numeric_limits<float>::epsilon());
        })
        .staticMethod("Sqrt", [](float v) { return std::sqrt(v); })
        .staticMethod("Pow", [](float a, float b) { return std::pow(a, b); })
        .staticMethod("Exp", [](float v) { return std::exp(v); })
        .staticMethod("Log", [](float v) { return std::log(v); })
        .staticMethod("Log10", [](float v) { return std::log10(v); })
        .staticMethod("Sin", [](float v) { return std::sin(v); })
        .staticMethod("Cos", [](float v) { return std::cos(v); })
        .staticMethod("Tan", [](float v) { return std::tan(v); })
        .staticMethod("Asin", [](float v) { return std::asin(v); })
        .staticMethod("Acos", [](float v) { return std::acos(v); })
        .staticMethod("Atan", [](float v) { return std::atan(v); })
        .staticMethod("Atan2", [](float y, float x) { return std::atan2(y, x); })
        .staticMethod("Floor", [](float v) { return std::floor(v); })
        .staticMethod("Ceil", [](float v) { return std::ceil(v); })
        .staticMethod("Round", [](float v) { return std::round(v); })
        .staticMethod("FloorToInt", [](float v) { return (int64_t)std::floor(v); })
        .staticMethod("CeilToInt", [](float v) { return (int64_t)std::ceil(v); })
        .staticMethod("RoundToInt", [](float v) { return (int64_t)std::lround(v); });

    // ----------------------------------------------------------------- enums
    // Unity's Space and ForceMode as int constants, so `Translate(v, Space.World)` reads
    //	as it does in C#.
    taipan_register_class(s, "Space").constant("Self", (int64_t)0).constant("World", (int64_t)1);
    taipan_register_class(s, "ForceMode")
        .constant("Force", (int64_t)0)
        .constant("Acceleration", (int64_t)1)
        .constant("Impulse", (int64_t)2)
        .constant("VelocityChange", (int64_t)3);
}

} // namespace

Quat EulerToQuaternion (Vec3 d) {
    const Q4 q = QEuler(d.x, d.y, d.z);
    return Quat{ q.x, q.y, q.z, q.w };
}

Vec3 QuaternionToEuler (Quat q) {
    const V3 e = QToEuler(Q4(q.x, q.y, q.z, q.w));
    return Vec3{ e.x, e.y, e.z };
}

Quat LookRotation (Vec3 forward, Vec3 up) {
    const Q4 q = QLookRotation(V3(forward.x, forward.y, forward.z), V3(up.x, up.y, up.z));
    return Quat{ q.x, q.y, q.z, q.w };
}

Quat QuaternionMultiply (Quat a, Quat b) {
    const Q4 q = QMul(Q4(a.x, a.y, a.z, a.w), Q4(b.x, b.y, b.z, b.w));
    return Quat{ q.x, q.y, q.z, q.w };
}

Vec3 RotateVector (Quat q, Vec3 v) {
    const V3 r = QRotate(Q4(q.x, q.y, q.z, q.w), V3(v.x, v.y, v.z));
    return Vec3{ r.x, r.y, r.z };
}

void RegisterUnityMath (VM* vm, const char* objectClass) {
    // A catch boundary like every other bridge entry point: registration allocates and
    //	the builder can throw.
    try {
        vm->hitObjectClass = objectClass ? objectClass : "GameObject";
        RegisterUnityMathImpl(vm);
    } catch (std::exception& e) {
        vm->lastError = e.what();
        if (vm->errorFn) vm->errorFn(vm->lastError.c_str(), vm->errorUser);
    } catch (...) {
        vm->lastError = "RegisterUnityMath failed";
        if (vm->errorFn) vm->errorFn(vm->lastError.c_str(), vm->errorUser);
    }
}

} // namespace st::taipan
