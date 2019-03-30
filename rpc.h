#pragma once
#include <tuple>
#include <type_traits>
#include <functional>
#include <unordered_map>

namespace rpc {

namespace traits {

template<typename Signature>
struct tuple_from_signatire;

template<typename R, typename... Args>
struct tuple_from_signatire<R(Args...)> {
    using type = std::tuple<std::remove_reference_t<std::decay_t<Args>>...>;
    using ReturnType = R;
};

template<typename T>
using tuple_from_signatire_t = typename tuple_from_signatire<T>::type;

template<typename T>
using return_from_signatire_t = typename tuple_from_signatire<T>::ReturnType;

} // namespace traits


namespace identifiers {

template<uint16_t instanceId = 0>
struct StaticId {
    constexpr static uint16_t getInstanceId() { return instanceId; }
};

struct DynamicId {
    uint16_t instanceId;

    void setInstanceId(uint16_t id) { instanceId = id; }
    uint16_t getInstanceId() { return instanceId; }
};

} // namespace identifiers


template<class Derived, typename PayloadType, typename Identifier = identifiers::StaticId<>>
class RpcInterface;

template<typename Signature, typename RpcInterface>
struct RpcCall;


namespace details {

template<typename Payload, typename Signature, typename Interface>
std::function<void(const Payload&)> makeHandler(RpcCall<Signature, Interface>& rpcCall);



} // namespace details



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
struct RpcMessage {
    uint16_t instanceId = 0;
    uint16_t functionId = 0;
    uint32_t callId = 0;
    Payload payload;
};


///
template<typename ReturnType, typename RpcInterfaceType, typename... Args>
struct RpcCall<ReturnType(Args...), RpcInterfaceType> {
    static_assert (std::is_same_v<void, ReturnType>, "RPC with return type are not supported");

    using Message = typename RpcInterfaceType::Message;
    using Payload = typename RpcInterfaceType::Payload;
    using DerivedInterface = typename RpcInterfaceType::Derived;

    friend class RpcInterface<DerivedInterface, Payload, typename RpcInterfaceType::Identifier>;
    friend std::function<void(const Payload&)> details::makeHandler<Payload, ReturnType(Args...), RpcInterfaceType>(RpcCall&);

    RpcCall(RpcInterfaceType* rpcInterface) {
        rpcInterface->registerCall(*this);
    }

    template<typename Functor>
    void operator = (Functor&& f) {
        remoteCallback = std::forward<Functor>(f);
    }

    inline decltype(auto) operator() (Args... args) {
        Message message;
        message.instanceId = interface->getInstanceId();
        message.functionId = functionId;
        message.callId = interface->getNextCallId();
        message.payload.serialize(args...);

        return static_cast<DerivedInterface*>(interface)->template doRemoteCall<ReturnType>(std::move(message));
    }

protected:
    std::function<ReturnType(Args...)> remoteCallback;
    uint16_t functionId = 0;
    RpcInterfaceType* interface;
};


///
template<class DerivedType, typename PayloadType, typename IdentifierType>
class RpcInterface : public IdentifierType {
public:
    using Payload = PayloadType;
    using Message = RpcMessage<Payload>;
    using Derived = DerivedType;
    using Identifier = IdentifierType;

    template<typename Signature>
    using Rpc = RpcCall<Signature, RpcInterface>;

    template<typename Signature>
    void registerCall(Rpc<Signature>& call) {
        call.functionId = functionIdCounter++;
        call.interface = this;
        callHandlers[call.functionId] = details::makeHandler<Payload>(call);
    }

    void dispatch(const Message& message) {
        auto handlerIt = callHandlers.find(message.functionId);
        if (handlerIt != callHandlers.end()) {
            handlerIt->second(message.payload);
        }
    }

    uint32_t getNextCallId() { return ++callIdCounter; }
protected:

    uint32_t callIdCounter = 0;
    uint16_t functionIdCounter = 0;
    std::unordered_map<uint16_t, std::function<void(const Payload&)>> callHandlers;
    std::unordered_map<uint16_t, std::function<void(const Payload&)>> resultHandlers;
};



namespace details {

template<typename Payload, typename Signature, typename Interface>
std::function<void(const Payload&)> makeHandler(RpcCall<Signature, Interface>& rpcCall) {
    // rpcCall is a reference to member
    return [&rpcCall](const Payload& payload) {
        using TupleType = traits::tuple_from_signatire_t<Signature>;
        const auto memberCall = [&rpcCall](auto&&... args) {
            if (rpcCall.remoteCallback) {
                return rpcCall.remoteCallback(args...);
            }
        };
        std::apply(memberCall, payload.template deserialize<TupleType>());
    };
}

} // namespace details

} // namespace rpc
