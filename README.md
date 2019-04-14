# RPC
This is a simple **example** of a **type-safe** RPC-based interface class to make remote calls easier.
This library is intended to make easy to use interface for Rpc-call definition and
binding and does not implement all layers for real RPC call. Thus before use 
several mandatory customization points should be specified by user.
These points are arguments serializing (`Payload` class) and actual message passing
(`doRemoteCall` function).

You can inspire by several examples where all this code is defined.

## How-to-use:
1. Provide a `Payload` class that will be used to store rpc call arguments:
```c++
struct Payload {
    /*...*/
    
    /// serialize args into this payload class
    template<typename... Args>
    void serialize(Args&&... args) { /*...*/ }

    /// deserialize this class into std::tuple passed as a template parameter Tuple
    template<typename Tuple>
    auto deserialize() const { /*...*/ }
};
```
2. Provide your own interface derived from `rpc::RpcInterface` and define special functions to
be used to send your call data elsewhere or receive it:
```c++
struct MyInterface : public rpc::RpcInterface<MyInterface, Payload> {
    template<typename R>
    auto doRemoteCall(Message&& message) { /*...*/ }
    
    /// this is only needed if PRCs with result are used
    template<typename R>
    void onResultReturned(uint32_t callId, const R& result)
```


3. Now it's time to fill this class with you methods. Use `Rpc` template already defined for you:
```c++
    /*...*/
    // strange constructor `= this` helps to register calls for this interface
    Rpc<void(const std::map<std::string, int>& phonebook)> addPhonebook = this;
    Rpc<void()> notify = this;
    
    // for rpc with result you will be able to change return type inside `doRemoteCall`
    // function so that `sender` would return `std::future` or even an Awairable type
    // but receicer would have to return `int` anyway
    Rpc<int(int v)> square = this;
    /*...*/
};
```

## Short Example
Look examples for possible definitions
```c++
struct Payload {
    template<typename... Args>
    void serialize(Args&&... args);

    template<typename Tuple>
    auto deserialize() const;
};

struct ExampleInterface : public rpc::RpcInterface<ExampleInterface, Payload> {
    Rpc<void(int id, const std::string& name, double money)> addAccount = this;
    Rpc<void(const std::map<std::string, int>& phonebook)> addPhonebook = this;
    Rpc<void()> notifyOne = this;
    Rpc<void()> notifyTwo = this;
    Rpc<int(int v)> square = this;

    template<typename R>
    auto doRemoteCall(Message&& message);
    
    template<typename R>
    void onResultReturned(uint32_t callId, const R& result);
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
    sender.notifyTwo(); // will be skipped, as no handler is attached to receiver
    std::future<int> future = sender.square(5);

    // future is not ready yet, receiver should handle it, then sender should handle response
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
```
