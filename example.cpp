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


/// Mandatory customization point #1
///
/// super simple payload example
///
/// library requirments - serialize/deserialize pair
/// serialize  : construct Payload from `Args...`
/// deserialize: construct Tuple from payload.
///              Tuple is std::tuple of non-const/volatile non-reference `Args...`
struct Payload {
    std::any data;

    template<typename... Args>
    void serialize(Args&&... args) {
        using Type = std::tuple<std::remove_reference_t<std::decay_t<Args>>...>;
        data = std::make_any<Type>(std::forward<Args>(args)...);
    }

    template<typename Tuple>
    auto deserialize() const {
        return std::any_cast<Tuple>(data);
    }
};

// to simplyfy the example we just store `messages` inside this dummy queue
static std::map<uint16_t, std::deque<rpc::RpcPacket<Payload>>> dummyQueue;

struct ExampleInterface : public rpc::RpcInterface<ExampleInterface, Payload> {
    // strange constructor `= this` helps to register calls for this interface
    Rpc<void(int id, const std::string& name, double money)> addAccount = this;
    Rpc<void(const std::map<std::string, int>& phonebook)> addPhonebook = this;
    Rpc<void()> notifyOne = this;
    Rpc<void()> notifyTwo = this;
    Rpc<int(int v)> square = this;

    /// Mandatory customization point #2
    template<typename R>
    auto sendRpcPacket(rpc::RpcPacket<Payload>&& packet) {
        // this part is used only for rpc with result
        if constexpr (!std::is_same_v<void, R>) {
            auto promise = std::shared_ptr<std::promise<R>>(new std::promise<R>);
            auto future = promise->get_future();

            promises[packet.callId] = promise;
            dummyQueue[packet.instanceId].emplace_back(std::move(packet));

            // it is legit to change return type here, for example `square` was declaread as Rpc<int(int v)>
            // but we want some kind of asynchrony, we use here std::future. Another good variant is a coroutine
            return std::move(future);
        }

        dummyQueue[packet.instanceId].emplace_back(std::move(packet));
    }

    /// Mandatory customization point #3
    /// it is needed only if rpc with result are used
    template<typename R>
    void onResultReturned(uint32_t callId, const R& result) {
        std::any_cast<std::shared_ptr<std::promise<R>>>(promises[callId])->set_value(result);
    }

    std::unordered_map<uint32_t, std::any> promises;
};


int main() {
    /// ----------------- sender side
    ExampleInterface sender;
    sender.setInstanceId(0);

    /// ----------------- receiver side
    ExampleInterface receiver;
    receiver.setInstanceId(1);

    // binding handlers
    receiver.addAccount = [](int id, const std::string& name, double money) {
        std::cout << "receiver addAccount: " << id << " " << name << " " << money << "\n";
    };
    receiver.addPhonebook = [](const std::map<std::string, int>& phonebook) {
        std::cout << "receiver addPhonebook: ";
        for (auto& [k,v] : phonebook) {
            std::cout << "{" << k << " : " << v << "} ";
        }
        std::cout << "\n";
    };
    receiver.notifyOne = []() { std::cout << "receiver notify1: .\n";};
    receiver.square = [](int v) { return v * v; };


    /// ----------------- sender side
    sender.addAccount(1, "Eddart", 1000.1);
    sender.addPhonebook({{"John", 3355450}, {"Rob", 1194517}});
    sender.notifyOne();
    // sender.notifyTwo(); // will throw `bad_function_call`
    std::future<int> future = sender.square(5);

    // future not ready yet, receiver should handle it, then sender should handle response
    assert(future.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);

    /// ----------------- receiver side
    for (const auto& message : dummyQueue[0]) {
        receiver.dispatch(message);
    }
    // =>
    // receiver addAccount: 1 Eddart 1000.1
    // receiver addPhonebook : {John: 3355450} {Rob: 1194517}
    // receiver notify1 : .

    /// ----------------- sender side
    // future not ready yet, sender should handle response
    assert(future.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);

    for (const auto& message : dummyQueue[1]) {
        sender.dispatch(message);
    }

    // now the future is ready
    std::cout << "sender square: " << future.get() << "\n"; // => 25
}

