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


template<class Actual, typename PayloadType, typename Identifier = identifiers::StaticId<>>
class RpcInterface;

template<typename Signature, typename RpcInterface>
struct RpcCall;


namespace details {

template<typename Message, typename Signature, typename Interface>
std::function<void(const Message&)> makeCallHandler(RpcCall<Signature, Interface>& rpcCall);

template<typename ActualInterface, typename Message, typename Signature>
std::function<void(ActualInterface*, const Message&)> makeResultHandler();


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
    bool response = false;
};


///
template<typename ReturnType, typename RpcInterfaceType, typename... Args>
struct RpcCall<ReturnType(Args...), RpcInterfaceType> {
    using Message = typename RpcInterfaceType::Message;
    using Payload = typename RpcInterfaceType::Payload;
    using ActualInterface = typename RpcInterfaceType::Actual;

    friend class RpcInterface<ActualInterface, Payload, typename RpcInterfaceType::Identifier>;
    friend std::function<void(const Message&)> details::makeCallHandler<Message, ReturnType(Args...), RpcInterfaceType>(RpcCall&);
    friend std::function<void(ActualInterface*, const Message&)> details::makeResultHandler<ActualInterface, Message, ReturnType(Args...)>();


    RpcCall(RpcInterfaceType* rpcInterface) {
        rpcInterface->registerCall(*this);
    }

    template<typename Functor>
    void operator = (Functor&& f) {
        remoteCallback = std::forward<Functor>(f);
    }

    inline decltype(auto) operator() (Args... args) {
        return doRemoteCall<false>(0, args...);
    }

protected:

    template<bool response>
    inline decltype(auto) doRemoteCall(uint32_t callId, Args... args) {
        Message message;
        message.instanceId = interface->getInstanceId();
        message.functionId = functionId;
        message.callId = response
                ? callId
                : interface->getNextCallId();
        message.payload.serialize(args...);
        message.response = response;

        using Return = std::conditional_t<response, void, ReturnType>;
        return static_cast<ActualInterface*>(interface)->template doRemoteCall<Return>(std::move(message));
    }

    std::function<ReturnType(Args...)> remoteCallback;
    uint16_t functionId = 0;
    RpcInterfaceType* interface;
};


///
template<class ActualType, typename PayloadType, typename IdentifierType>
class RpcInterface : public IdentifierType {
public:
    using Payload = PayloadType;
    using Message = RpcMessage<Payload>;
    using Actual = ActualType;
    using Identifier = IdentifierType;

    template<typename Signature>
    using Rpc = RpcCall<Signature, RpcInterface>;

    template<typename Signature>
    void registerCall(Rpc<Signature>& call) {
        call.functionId = functionIdCounter++;
        call.interface = this;
        callHandlers[call.functionId] = details::makeCallHandler<Message>(call);

        using ReturnType = traits::return_from_signatire_t<Signature>;
        if constexpr (!std::is_same_v<void, ReturnType>) {
            resultHandlers[call.functionId] = details::makeResultHandler<Actual, Message, Signature>();
        }
    }

    void dispatch(const Message& message) {
        if (message.response) {
            auto handlerIt = resultHandlers.find(message.functionId);
            if (handlerIt != resultHandlers.end()) {
                handlerIt->second(static_cast<Actual*>(this), message);
            }
        } else {
            auto handlerIt = callHandlers.find(message.functionId);
            if (handlerIt != callHandlers.end()) {
                handlerIt->second(message);
            }
        }
    }

    uint32_t getNextCallId() { return ++callIdCounter; }
protected:

    uint32_t callIdCounter = 0;
    uint16_t functionIdCounter = 0;
    std::unordered_map<uint16_t, std::function<void(const Message&)>> callHandlers;
    std::unordered_map<uint16_t, std::function<void(Actual*, const Message&)>> resultHandlers; // TODO:: can be plain function pointers
};



namespace details {

template<typename Message, typename Signature, typename Interface>
std::function<void(const Message&)> makeCallHandler(RpcCall<Signature, Interface>& rpcCall) {
    using TupleType = traits::tuple_from_signatire_t<Signature>;
    using ReturnType = traits::return_from_signatire_t<Signature>;

    return [&rpcCall](const Message& message) {     // rpcCall is a reference to member
        const auto memberCall = [&rpcCall](auto&&... args) -> ReturnType {
            if (rpcCall.remoteCallback) {
                return rpcCall.remoteCallback(args...);
            }
            // TODO: not the best way to handle the case of no handler for RPC-with-result
            return ReturnType();
        };
        if constexpr (std::is_same_v<void, ReturnType>) {
            std::apply(memberCall, message.payload.template deserialize<TupleType>());
        } else {
            auto result = std::apply(memberCall, message.payload.template deserialize<TupleType>());
            rpcCall.template doRemoteCall<true>(message.callId, result);
        }
    };
}

template<typename ActualInterface, typename Message, typename Signature>
std::function<void(ActualInterface*, const Message&)> makeResultHandler() {
    using ReturnType = traits::return_from_signatire_t<Signature>;
    return [](ActualInterface* interface, const Message& message) {
        const auto& result = message.payload.template deserialize<std::tuple<ReturnType>>();
        interface->template onResultReturned<ReturnType>(message.callId, std::get<0>(result));
    };
}

} // namespace details

} // namespace rpc
