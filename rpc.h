#pragma once
#include <tuple>
#include <type_traits>
#include <functional>
#include <unordered_map>

namespace rpc {

using CallId = uint32_t;
using InstanceId = uint16_t;
using FunctionId = uint16_t;

enum class CallType {
    Call,
    Response
};

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

template<InstanceId instanceId = 0>
struct StaticId {
    constexpr static InstanceId getInstanceId() { return instanceId; }
};

struct DynamicId {
    InstanceId instanceId;

    void setInstanceId(InstanceId id) { instanceId = id; }
    InstanceId getInstanceId() { return instanceId; }
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


struct RpcMessageHeader {
    InstanceId instanceId = 0;
    FunctionId functionId = 0;
    CallId callId = 0;
    // uint16_t payloadSize = 0;
    bool response = false;
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
struct RpcMessage {
    RpcMessageHeader header;
    Payload payload;
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
        return doRemoteCall<CallType::Call>(0, args...);
    }

protected:

    template<CallType calltype>
    inline decltype(auto) doRemoteCall(uint32_t callId, Args... args) {
        constexpr bool response = calltype == CallType::Response;
        Message message;
        message.header.instanceId = interface->getInstanceId();
        message.header.functionId = functionId;
        message.header.callId = response
                ? callId
                : interface->getNextCallId();
        message.header.response = response;
        message.payload.serialize(args...);

        using Return = std::conditional_t<response, void, ReturnType>;
        return static_cast<ActualInterface*>(interface)->template doRemoteCall<Return>(std::move(message));
    }

    std::function<ReturnType(Args...)> remoteCallback;
    FunctionId functionId = 0;
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
        if (message.header.response) {
            auto handlerIt = resultHandlers.find(message.header.functionId);
            if (handlerIt != resultHandlers.end()) {
                handlerIt->second(static_cast<Actual*>(this), message);
            }
        } else {
            auto handlerIt = callHandlers.find(message.header.functionId);
            if (handlerIt != callHandlers.end()) {
                handlerIt->second(message);
            }
        }
    }

    CallId getNextCallId() { return ++callIdCounter; }
protected:

    CallId callIdCounter = 0;
    FunctionId functionIdCounter = 0;
    std::unordered_map<FunctionId, std::function<void(const Message&)>> callHandlers;
    std::unordered_map<FunctionId, std::function<void(Actual*, const Message&)>> resultHandlers; // TODO:: can be plain function pointers
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
            rpcCall.template doRemoteCall<CallType::Response>(message.header.callId, result);
        }
    };
}

template<typename ActualInterface, typename Message, typename Signature>
std::function<void(ActualInterface*, const Message&)> makeResultHandler() {
    using ReturnType = traits::return_from_signatire_t<Signature>;
    return [](ActualInterface* interface, const Message& message) {
        const auto& result = message.payload.template deserialize<std::tuple<ReturnType>>();
        interface->template onResultReturned<ReturnType>(message.header.callId, std::get<0>(result));
    };
}

} // namespace details

} // namespace rpc
