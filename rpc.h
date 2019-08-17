#pragma once
#include <tuple>
#include <vector>
#include <unordered_map>
#include <functional>

namespace rpc {

using CallId = uint32_t;
using InstanceId = uint16_t;
using FunctionId = uint16_t;

template<typename ...Args>
using ArgsTuple = std::tuple<std::remove_cv_t<std::remove_reference_t<Args>>...>;

enum class CallType {
    Call,
    Response
};

/// Packet generated on an RPC call
/// `Payload` shoud be customized by user.
/// `Payload` should have 2 special functions:
///
/// ```
/// template<typename... Args>
/// void serialize(Args&&... args)
/// ```
/// this will be called to pack arguments on caller side
///
/// ```
/// template<typename Tuple>
/// auto deserialize() const
/// ```
/// this will be called to get Tuple of arguments from payload
/// Tuple is std::tuple of non-const/volatile non-reference `Args...`
template<typename Payload>
struct RpcPacket {
    InstanceId instanceId = 0;
    FunctionId functionId = 0;
    CallId callId = 0;
    CallType callType = CallType::Call;
    Payload payload;
};

template <class Interface, typename Payload, typename Signature> struct RpcCall;

template<class Interface, typename Payload>
class RpcInterface {
public:

    template<typename Signature>
    using Rpc = RpcCall<Interface, Payload, Signature>;

    template<typename ReturnType, typename ...Args>
    void registerCall(RpcCall<Interface, Payload, ReturnType(Args...)>& call) {
        call.functionId = static_cast<FunctionId>(callHandlers.size());
        call.interface = this;
        callHandlers.emplace_back(std::make_pair(&call, call.makeCallHandler()));

        if constexpr (!std::is_same_v<void, ReturnType>) {
            resultHandlers[call.functionId] = call.makeResultHandler();
        }
    }

    void dispatch(const RpcPacket<Payload>& packet) {
        if (packet.callType == CallType::Call) {
            if (packet.functionId < callHandlers.size()) {
                auto& selfAndHandler = callHandlers[packet.functionId];
                selfAndHandler.second(selfAndHandler.first, packet);
            }
        } else {
            auto handlerIt = resultHandlers.find(packet.functionId);
            if (handlerIt != resultHandlers.end()) {
                handlerIt->second(static_cast<Interface*>(this), packet);
            }
        }
    }

    void setInstanceId(InstanceId id) { instanceId = id; }
    InstanceId getInstanceId() { return instanceId; }
    CallId getNextCallId() { return ++callIdCounter; }

protected:
    CallId callIdCounter = 0;
    InstanceId instanceId = 0;

    std::vector<std::pair<void*, void(*)(void*, const RpcPacket<Payload>&)>> callHandlers;
    std::unordered_map<FunctionId, void(*)(Interface*, const RpcPacket<Payload>&)> resultHandlers;
};


template <class Interface, typename Payload, typename ReturnType, typename ...Args>
struct RpcCall<Interface, Payload, ReturnType(Args...)> {

    RpcCall(RpcInterface<Interface, Payload>* interface) {
        interface->registerCall(*this);
    }

    template<typename Functor>
    void operator = (Functor&& f) {
        remoteCallback = std::forward<Functor>(f);
    }

    inline decltype(auto) operator() (Args ...args) {
        return doRemoteCall<CallType::Call>(interface->getNextCallId(), std::forward<Args>(args)...); // move copied args if any
    }

protected:
    friend class RpcInterface<Interface, Payload>;

    template<CallType callType, typename ...Arguments>
    inline decltype(auto) doRemoteCall(uint32_t callId, Arguments&& ...args) {
        RpcPacket<Payload> packet;
        packet.instanceId = interface->getInstanceId();
        packet.functionId = functionId;
        packet.callId = callId;
        packet.callType = callType;
        packet.payload.serialize(std::forward<Arguments>(args)...);

        return static_cast<Interface*>(interface)->template sendRpcPacket<ReturnType>(std::move(packet));
    }

    auto makeCallHandler() {
        return [](void* selfPtr, const RpcPacket<Payload>& packet) {
            auto* self = static_cast<RpcCall<Interface, Payload, ReturnType(Args...)>*>(selfPtr);

            using Tuple = ArgsTuple<Args...>;
            if constexpr (std::is_same_v<void, ReturnType>) {
                std::apply(self->remoteCallback, packet.payload.template deserialize<Tuple>());
            } else {
                auto result = std::apply(self->remoteCallback, packet.payload.template deserialize<Tuple>());
                self->template doRemoteCall<CallType::Response>(packet.callId, std::move(result));
            }
        };
    }

    auto makeResultHandler() {
        return [](Interface* interface, const RpcPacket<Payload>& packet) {
            const auto& result = packet.payload.template deserialize<std::tuple<ReturnType>>();
            interface->template onResultReturned<ReturnType>(packet.callId, std::get<0>(result));
        };
    }

    std::function<ReturnType(Args...)> remoteCallback;
    RpcInterface<Interface, Payload>* interface;
    FunctionId functionId = 0;
};

} // namespace rpc
