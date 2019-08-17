#include "rpc.h"

#include <iostream>
#include <string>
#include <vector>
#include <deque>
#include <thread>
#include <future>
#include <any>
#include <map>
#include <cassert>


/// Mandatory customization point #1 - Payload
/// Payload must have two function templates:
/// 1. ` template<typename... Args> void serialize(Args&&... args)`
///    This is invoked on sender side after an rpc call to store arguments in any appropriate way
/// 2. `template<typename Tuple> decltype(auto) deserialize()`
///    This is invoked on receiver side to restire arguments from payload.
///    `Tuple` is a hint type that must be returned from a function
///    `Tuple` is the same as `rpc::ArgsTuple<Args...>` , where `Args...` are corresponding RpcCall argument types
struct LocalPayload {
    /// Super simple payload example:
    /// As our packets don't leave the executable, we can go an easy way. 
    /// Let's store arguments in a tuple that is expected from `deserialize` function. So the only work we
    /// have to do is to serialize args. On deserialization we just return them as is.

    // Here we use std::any to erase actual tuple type
    std::any data;

    template<typename... Args>
    void serialize(Args&&... args) {
        data = std::make_any<rpc::ArgsTuple<Args...>>(std::forward<Args>(args)...);
    }

    template<typename Tuple>
    decltype(auto) deserialize() const {
        return std::any_cast<Tuple>(data);
    }
};

/// to simplyfy the example we just store `packets` inside this dummy queue
static std::map<rpc::InstanceId, std::deque<rpc::RpcPacket<LocalPayload>>> dummyQueue;


/// This is our interface base where we define:
/// 1. how to send packets
/// 2. how to deal with received rpc results
struct LocalRpcInterface : public rpc::RpcInterface<LocalRpcInterface, LocalPayload> {

    /// Mandatory customization point #2 - Packet sending
    /// `template<typename R> auto sendRpcPacket(rpc::RpcPacket<LocalPayload>&& packet)` is invoked to send serialized RpcPacket
    /// `R` is an Rpc return type hint to decide whether additional work is needed to prepare to result handling
    template<typename R>
    auto sendRpcPacket(rpc::RpcPacket<LocalPayload>&& packet) {

        // just store packets in queue
        rpc::CallId callId = packet.callId;
        dummyQueue[packet.instanceId].emplace_back(std::move(packet));

        // This part is only for Rpc with result
        if constexpr (!std::is_same_v<void, R>) {
            // it is legit to change return type here, for example `square` was declaread as Rpc<int(int v)>
            // but we want some kind of asynchrony and we use here std::future. Another good variant is a coroutine.
            // We store a promise by `callId` key and return corresponding future
            // shared_ptr is used to fin std::promise inside std::any
            auto promise = std::shared_ptr<std::promise<R>>(new std::promise<R>);
            auto future = promise->get_future();

            promises[packet.callId] = std::move(promise);
            return std::move(future);
        }
    }

    /// [Optional customization point #3] - Handling results
    /// `template<typename R> void onResultReturned(uint32_t callId, const R& result)` is invoked to handle result for corresponding callId
    ///
    /// it is needed only if rpc with result are used
    /// When we send a call we store a promise identified by callId,
    /// When we receive result with callId, we set corresponding promise value
    template<typename R>
    void onResultReturned(uint32_t callId, const R& result) {
        std::any_cast<std::shared_ptr<std::promise<R>>>(promises[callId])->set_value(result);
    }

    std::unordered_map<uint32_t, std::any> promises;
};


/// Actual definitions of Rpc methods
struct ExampleInterface : public LocalRpcInterface {
    // strange constructor `= this` helps to register calls for this interface
    Rpc<void(int id, const std::string& name, double money)> addAccount = this;
    Rpc<void(const std::map<std::string, int>& phonebook)> addPhonebook = this;
    Rpc<void()> notifyOne = this;
    Rpc<void()> notifyTwo = this;
    Rpc<int(int v)> square = this;
};


const rpc::InstanceId senderId = 0;
const rpc::InstanceId receiverId = 1;

void runReceiver() {
    ExampleInterface receiver;
    receiver.setInstanceId(receiverId);

    // binding handlers
    receiver.addAccount = [](int id, const std::string& name, double money) {
        std::cout << "Receiver addAccount: " << id << " " << name << " " << money << "\n";
    };
    receiver.addPhonebook = [](const std::map<std::string, int>& phonebook) {
        std::cout << "Receiver addPhonebook: ";
        for (auto& [k, v] : phonebook) {
            std::cout << "{" << k << " : " << v << "} ";
        }
        std::cout << "\n";
    };
    receiver.notifyOne = []() { std::cout << "Receiver notifyOne called\n"; };
    receiver.square = [](int v) { return v * v; };

    /// loop over sender packets
    for (const auto& packet : dummyQueue[senderId]) {
        try {
            receiver.dispatch(packet);
        } catch (std::exception & e) {
            std::cout << "Receiver caught exception: '" << e.what() << "'\n";
        }
    }
}


int main() {
    ExampleInterface sender;
    sender.setInstanceId(senderId);

    sender.addAccount(1, "Eddart", 1000.1);
    sender.addPhonebook({{"John", 3355450}, {"Rob", 1194517}});
    sender.notifyOne();
    sender.notifyTwo(); // will throw `bad_function_call` on receiver side
    std::future<int> future = sender.square(5);

    // future not ready yet, receiver should handle it, then sender should handle response
    assert(future.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);

    runReceiver();
    // =>
    // Receiver addAccount : 1 Eddart 1000.1
    // Receiver addPhonebook : {John: 3355450} {Rob: 1194517}
    // Receiver notifyOne called
    // Receiver caught exception : 'bad_function_call'

    // receiver handled `square` call, and returned result but it's not handled by sender yet and the future is pending
    assert(future.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);

    // loop over packets from receiver
    for (const auto& packet : dummyQueue[receiverId]) {
        sender.dispatch(packet);
    }
    // now the future is ready
    std::cout << "Sender square: " << future.get() << "\n"; // => 25
}

