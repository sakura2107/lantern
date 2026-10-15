#include "lantern/core.hpp"
#include <jni.h>
using namespace lantern;
static Bytes bytes(JNIEnv* e, jbyteArray a) {
    auto n = e->GetArrayLength(a);
    if (n > int(max_frame))
        throw Error(Code::invalid, "JNI frame bound");
    Bytes b(n);
    e->GetByteArrayRegion(a, 0, n, (jbyte*)b.data());
    return b;
}
static jbyteArray array(JNIEnv* e, const Bytes& b) {
    auto a = e->NewByteArray(jsize(b.size()));
    if (a)
        e->SetByteArrayRegion(a, 0, jsize(b.size()), (const jbyte*)b.data());
    return a;
}
static void fail(JNIEnv* e, const std::exception& x) {
    e->ThrowNew(e->FindClass("java/io/IOException"), x.what());
}
extern "C" JNIEXPORT jbyteArray JNICALL Java_org_lantern_client_Native_packet(JNIEnv* e, jclass, jint op,
                                                                              jbyteArray payload) {
    try {
        auto b = bytes(e, payload);
        Writer w;
        w.u16(protocol_version);
        w.u16(uint16_t(op));
        w.b.insert(w.b.end(), b.begin(), b.end());
        if (w.b.size() > max_frame)
            throw Error(Code::invalid, "Packet bound");
        return array(e, w.b);
    } catch (const std::exception& x) {
        fail(e, x);
        return nullptr;
    }
}
extern "C" JNIEXPORT jbyteArray JNICALL Java_org_lantern_client_Native_response(JNIEnv* e, jclass, jint op,
                                                                                jbyteArray packet) {
    try {
        auto b = bytes(e, packet);
        Reader r{b};
        if (r.u16() != protocol_version || r.u16() != op)
            throw Error(Code::unsupported, "Response protocol mismatch");
        auto code = Code(r.u32());
        if (code != Code::ok) {
            auto msg = r.str();
            r.end();
            throw Error(code, msg);
        }
        return array(e, Bytes(b.begin() + r.at, b.end()));
    } catch (const std::exception& x) {
        fail(e, x);
        return nullptr;
    }
}
