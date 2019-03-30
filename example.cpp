#include "rpc.h"

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <any>
#include <map>


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

static std::vector<rpc::RpcMessage<Payload>> senderToReceiver;

struct ExampleInterface : public rpc::RpcInterface<ExampleInterface, Payload> {
    // strange constructor `= this` helps to register calls for this interface
    Rpc<void(int id, const std::string& name, double money)> addAccount = this;
    Rpc<void(const std::map<std::string, int>& phonebook)> addPhonebook = this;
    Rpc<void()> notifyOne = this;
    Rpc<void()> notifyTwo = this;

    /// Mandatory customization point #2
    template<typename R>
    auto doRemoteCall(Message&& message) {
        senderToReceiver.emplace_back(std::move(message));
    }
};


int main() {
    /// ----------------- sener side
    ExampleInterface sender;
    sender.addAccount(1, "Eddart", 1000.1);
    sender.addPhonebook({{"John", 3355450}, {"Rob", 1194517}});
    sender.notifyOne();
    sender.notifyTwo();


    /// ----------------- receiver side
    ExampleInterface receiver;
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
    // receiver.notifyTwo will be skipped as ho handler is attached


    /// receiver event loop
    for (const auto& message : senderToReceiver) {
        // here we can handle message domain ID and caller ID
        // but in this simple example there is only one matching pair sender-receiver
        receiver.dispatch(message);
    }
}
