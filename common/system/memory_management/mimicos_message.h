#ifndef MIMICOS_MESSAGE_H
#define MIMICOS_MESSAGE_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <cstdlib>
#include <cassert>

/**
 * @brief Message structure for MimicOS <-> VirtuOS communication
 * 
 * This is the raw message format passed between sniper-space and userspace.
 */
struct MimicOSMessage {
    int argc;           ///< Number of arguments (including protocol code)
    uint64_t* argv;     ///< Argument array (argv[0] = protocol code)
    
    MimicOSMessage() : argc(0), argv(nullptr) {}
    
    ~MimicOSMessage() {
        clear();
    }
    
    // Disable copy
    MimicOSMessage(const MimicOSMessage&) = delete;
    MimicOSMessage& operator=(const MimicOSMessage&) = delete;
    
    // Allow move
    MimicOSMessage(MimicOSMessage&& other) noexcept 
        : argc(other.argc), argv(other.argv) {
        other.argc = 0;
        other.argv = nullptr;
    }
    
    MimicOSMessage& operator=(MimicOSMessage&& other) noexcept {
        if (this != &other) {
            clear();
            argc = other.argc;
            argv = other.argv;
            other.argc = 0;
            other.argv = nullptr;
        }
        return *this;
    }
    
    void clear() {
        if (argv) {
            free(argv);
            argv = nullptr;
        }
        argc = 0;
    }
    
    bool isValid() const { return argc > 0 && argv != nullptr; }
};

/**
 * @brief Protocol codec for MimicOS message encoding/decoding
 * 
 * Handles the encoding and decoding of protocol codes and messages
 * for communication between sniper-space MimicOS and userspace VirtuOS.
 */
class MimicOSProtocol {
public:
    // Protocol codes
    static constexpr uint64_t INVALID_CODE = 0;
    static constexpr uint64_t PAGE_FAULT = 1;
    static constexpr uint64_t SYSCALL = 2;
    /* Scheduler lifecycle events.  Posted per core and consumed by the
       userspace kernel's dispatch loop; the producers land with K5. */
    static constexpr uint64_t NEW_THREAD = 3;
    static constexpr uint64_t THREAD_EXIT = 4;
    /* Quantum-based preemption.  Posted by the TraceThread once it has run
       `quantum` detailed instructions in app mode; argv[1] is the running
       app_thread_id. */
    static constexpr uint64_t QUANTUM_EXPIRED = 5;
    /* Simulation shutdown, synthesized by the SimReceiveMessage handler when
       the TraceManager has stopped and the core's queue is empty, so a
       kernel pthread waiting for an event leaves its loop rather than
       asserting on an invalid message. */
    static constexpr uint64_t SHUTDOWN = 6;
    
    /**
     * @brief Encode a protocol name to its code
     */
    static uint64_t encode(const std::string& name) {
        auto it = s_encode_map.find(name);
        if (it != s_encode_map.end()) {
            return it->second;
        }
        return INVALID_CODE;
    }
    
    /**
     * @brief Decode a protocol code to its name
     */
    static const std::string& decode(uint64_t code) {
        auto it = s_decode_map.find(code);
        if (it != s_decode_map.end()) {
            return it->second;
        }
        static const std::string unknown = "unknown";
        return unknown;
    }
    
    /**
     * @brief Build a message with the given type and arguments
     * 
     * @tparam Args Argument types (must be convertible to uint64_t)
     * @param msg The message to populate
     * @param message_type Protocol message type name
     * @param args Arguments to include in the message
     */
    template <typename... Args>
    static void buildMessage(MimicOSMessage& msg, const std::string& message_type, Args&&... args) {
        const uint64_t protocol_code = encode(message_type);
        assert(protocol_code != INVALID_CODE && "Invalid protocol message type");
        
        constexpr size_t extra = 1; // argv[0] = protocol_code
        
        msg.clear();
        msg.argc = extra + sizeof...(args);
        msg.argv = static_cast<uint64_t*>(malloc(sizeof(uint64_t) * msg.argc));
        
        msg.argv[0] = protocol_code;
        
        if constexpr (sizeof...(args) > 0) {
            uint64_t args_array[] = { static_cast<uint64_t>(args)... };
            for (size_t i = 0; i < sizeof...(args); ++i) {
                msg.argv[i + 1] = args_array[i];
            }
        }
    }
    
    /**
     * @brief Get the protocol code from a message
     */
    static uint64_t getProtocolCode(const MimicOSMessage& msg) {
        if (msg.argc > 0 && msg.argv) {
            return msg.argv[0];
        }
        return INVALID_CODE;
    }
    
    /**
     * @brief Get the protocol name from a message
     */
    static const std::string& getProtocolName(const MimicOSMessage& msg) {
        return decode(getProtocolCode(msg));
    }

private:
    static std::unordered_map<std::string, uint64_t> s_encode_map;
    static std::unordered_map<uint64_t, std::string> s_decode_map;
};

#endif // MIMICOS_MESSAGE_H
