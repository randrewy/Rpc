# RPC
This is a simple **type-safe** RPC-based interface class to make remote calls easier.
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
    /// Tuple is the same as `rpc::ArgsTuple<Args...>` , where `Args...` are corresponding RpcCall argument types
    template<typename Tuple>
    decltype(auto) deserialize() const { /*...*/ }
};
```
2. Provide your own interface derived from `rpc::RpcInterface` and define special functions to
be used to send your call data elsewhere or receive it:
```c++
struct MyInterface : public rpc::RpcInterface<MyInterface, Payload> {

    /// `R` is an Rpc return type hint to decide whether additional work is needed to prepare to result handling
    /// and possibly change return type to std::future/awaitable/etc
    template<typename R>
    auto sendRpcPacket(rpc::RpcPacket<Payload>&& packet) {
    
    /// this is only needed if PRCs with result are used.
    /// `callId` is corresponding call identifier for which this result is returned 
    template<typename R>
    void onResultReturned(uint32_t callId, const R& result)
```


3. Now it's time to fill this class with you methods. Use `Rpc` template already defined for you:
```c++
    /*...*/
    // strange constructor `= this` helps to register calls for this interface
    Rpc<void(const std::map<std::string, int>& phonebook)> addPhonebook = this;
    Rpc<void()> notify = this;
    
    // for rpc with result you will be able to change return type inside `sendRpcPacket`
    // function so that `sender` would return `std::future` or even an Awaitable type
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
    void serialize(Args&&... args) {/**/}

    template<typename Tuple>
    decltype(auto) deserialize() const {/**/}
};

struct ExampleInterface : public rpc::RpcInterface<ExampleInterface, Payload> {
    Rpc<void(int id, const std::string& name, double money)> addAccount = this;
    Rpc<void(const std::map<std::string, int>& phonebook)> addPhonebook = this;
    Rpc<void()> notifyOne = this;
    Rpc<void()> notifyTwo = this;
    Rpc<int(int v)> square = this;

    template<typename R>
    auto sendRpcPacket(Message&& message) {/**/}
    
    template<typename R>
    void onResultReturned(uint32_t callId, const R& result) {/**/}
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

    runReceiver(); // =>
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
```
