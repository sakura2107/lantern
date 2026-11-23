#include "lantern/core.hpp"
#include <jni.h>
using namespace lantern;
namespace {
struct JniPendingException {};
Bytes bytes(JNIEnv* e, jbyteArray a, uint32_t limit = max_frame) {
    if (!a)
        throw Error(Code::invalid, "Null JNI byte array");
    auto n = e->GetArrayLength(a);
    if (e->ExceptionCheck())
        throw JniPendingException{};
    if (n < 0 || uint64_t(n) > limit)
        throw Error(Code::invalid, "JNI frame bound");
    Bytes b(static_cast<size_t>(n));
    if (n > 0) {
        e->GetByteArrayRegion(a, 0, n, reinterpret_cast<jbyte*>(b.data()));
        if (e->ExceptionCheck())
            throw JniPendingException{};
    }
    return b;
}
jbyteArray array(JNIEnv* e, std::span<const uint8_t> b, uint32_t limit) {
    if (b.size() > limit)
        throw Error(Code::invalid, "JNI output bound");
    auto a = e->NewByteArray(jsize(b.size()));
    if (!a || e->ExceptionCheck())
        return nullptr;
    if (!b.empty()) {
        e->SetByteArrayRegion(a, 0, jsize(b.size()), reinterpret_cast<const jbyte*>(b.data()));
        if (e->ExceptionCheck()) {
            e->DeleteLocalRef(a);
            return nullptr;
        }
    }
    return a;
}
void fail(JNIEnv* e, const std::exception& x) {
    if (e->ExceptionCheck())
        return;
    jclass io = e->FindClass("java/io/IOException");
    if (!io || e->ExceptionCheck())
        return;
    e->ThrowNew(io, x.what());
    e->DeleteLocalRef(io);
}
} // namespace
extern "C" JNIEXPORT jbyteArray JNICALL Java_org_lantern_client_Native_packet(JNIEnv* e, jclass, jint op,
                                                                              jbyteArray payload) {
    if (!e || e->ExceptionCheck())
        return nullptr;
    try {
        if (!payload)
            throw Error(Code::invalid, "Null JNI byte array");
        auto n = e->GetArrayLength(payload);
        if (e->ExceptionCheck())
            return nullptr;
        if (n < 0 || uint64_t(n) > max_frame)
            throw Error(Code::invalid, "JNI frame bound");
        if (uint64_t(n) + 4 > max_frame)
            throw Error(Code::invalid, "Packet bound");
        Bytes out(4 + size_t(n));
        out[0] = uint8_t(protocol_version >> 8);
        out[1] = uint8_t(protocol_version);
        out[2] = uint8_t(uint16_t(op) >> 8);
        out[3] = uint8_t(uint16_t(op));
        if (n > 0) {
            e->GetByteArrayRegion(payload, 0, n, reinterpret_cast<jbyte*>(out.data() + 4));
            if (e->ExceptionCheck())
                return nullptr;
        }
        return array(e, out, max_frame);
    } catch (const JniPendingException&) {
        return nullptr;
    } catch (const std::exception& x) {
        fail(e, x);
        return nullptr;
    }
}
extern "C" JNIEXPORT jbyteArray JNICALL Java_org_lantern_client_Native_response(JNIEnv* e, jclass, jint op,
                                                                                jbyteArray packet) {
    if (!e || e->ExceptionCheck())
        return nullptr;
    try {
        const uint32_t limit = op == 33 ? max_video_frame : max_frame;
        auto b = bytes(e, packet, limit);
        Reader r{b};
        if (r.u16() != protocol_version || r.u16() != op)
            throw Error(Code::unsupported, "Response protocol mismatch");
        auto code = Code(r.u32());
        if (code != Code::ok) {
            auto msg = r.str();
            r.end();
            throw Error(code, msg);
        }
        if (r.at > b.size())
            throw Error(Code::invalid, "Response body bound");
        return array(e, std::span<const uint8_t>(b).subspan(r.at), limit);
    } catch (const JniPendingException&) {
        return nullptr;
    } catch (const std::exception& x) {
        fail(e, x);
        return nullptr;
    }
}
