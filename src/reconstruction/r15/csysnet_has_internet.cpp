// Quest libr15.so VA 0x10989fc; ReVault binary libr15.so.
// ReVault raw parent 100208359, reconstruction node 100600603; grade H.
#include <cstddef>
#include <cstdint>
#include <cstring>

// @0x10989fc — NRadEngine::CSysNet::HasInternet
/* @confidence: H */

struct _jobject;
using jobject = _jobject*;

namespace NRadEngine {

class CJavaObject {
public:
    explicit CJavaObject(jobject object);
    ~CJavaObject();

    jobject CallObjectMethod(const char* method, const char* signature,
                             CJavaObject* argument);
    jobject CallObjectMethod(const char* method, const char* signature);
    int CallBoolMethod(const char* method, const char* signature);

    alignas(8) std::byte raw[0x10];
};
static_assert(sizeof(CJavaObject) == 0x10);

class CJavaString : public CJavaObject {
public:
    explicit CJavaString(const char* value);
    ~CJavaString();
};
static_assert(sizeof(CJavaString) == 0x10);

class CSysJava {
public:
    static CJavaObject Activity();
};

namespace CSysNet {

bool HasInternet() {
    constexpr char service_name[] = "connectivity";
    constexpr char get_system_service[] = "getSystemService";
    constexpr char get_system_service_signature[] =
        "(Ljava/lang/String;)Ljava/lang/Object;";
    constexpr char get_active_network_info[] = "getActiveNetworkInfo";
    constexpr char get_active_network_info_signature[] =
        "()Landroid/net/NetworkInfo;";
    constexpr char is_connected[] = "isConnected";
    constexpr char is_connected_signature[] = "()Z";

    CJavaObject activity = CSysJava::Activity();
    CJavaObject service = [&activity, &service_name, &get_system_service,
                           &get_system_service_signature]() {
        CJavaString name(service_name);
        return CJavaObject(activity.CallObjectMethod(
            get_system_service, get_system_service_signature, &name));
    }();
    CJavaObject network_info = service.CallObjectMethod(
        get_active_network_info, get_active_network_info_signature);

    std::uintptr_t network_object = 0;
    std::memcpy(&network_object, network_info.raw + 8,
                sizeof(network_object));
    if (network_object == 0) {
        return false;
    }
    return network_info.CallBoolMethod(is_connected,
                                       is_connected_signature) != 0;
}

}  // namespace CSysNet
}  // namespace NRadEngine
