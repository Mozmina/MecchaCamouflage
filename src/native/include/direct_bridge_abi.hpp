#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

// This ABI is deliberately pointer-free. The injector copies the whole block into
// the target process, BridgeStartV2 copies it into bridge-owned state, and then the
// injector may safely reclaim the remote block after the remote start thread exits.
// Keep the fixed offsets in sync with the controller serializer.
enum BridgeStartResult : std::uint32_t
{
    BRIDGE_START_UNINITIALIZED = 0,
    BRIDGE_START_STARTING = 1,
    BRIDGE_START_LISTENING = 2,
    BRIDGE_START_INVALID_BLOCK = 3,
    BRIDGE_START_PROCESS_MISMATCH = 4,
    BRIDGE_START_ALREADY_STARTED = 5,
    BRIDGE_START_WINSOCK_FAILED = 6,
    BRIDGE_START_SOCKET_FAILED = 7,
    BRIDGE_START_BIND_FAILED = 8,
    BRIDGE_START_LISTEN_FAILED = 9,
    BRIDGE_START_WORKER_FAILED = 10,
};

// V2 keeps the controller-provided DLL integrity hash and additionally binds
// the bridge to the complete runtime bundle (DLL, profiles, and protocol ABIs).
constexpr std::uint32_t BridgeStartMagicV2 = 0x3253434D; // bytes: "MCS2"
constexpr std::uint32_t BridgeStartAbiV2 = 2;
constexpr std::uint32_t BridgeBootstrapProtocolV2 = 2;

#pragma pack(push, 1)
struct BridgeStartBlockV2
{
    std::uint32_t magic;
    std::uint32_t size;
    std::uint32_t abi;
    std::uint32_t pid;
    std::uint8_t instance_guid[16];
    std::uint8_t token[32];
    std::uint8_t sha256[32];
    std::uint8_t runtime_bundle_sha256[32];
    std::uint32_t requested_port;
    std::uint32_t result_state;
    std::uint32_t bound_port;
    std::uint32_t protocol;
    std::uint32_t win32_error;
    std::uint32_t winsock_error;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
};
#pragma pack(pop)

static_assert(std::is_standard_layout_v<BridgeStartBlockV2>);
static_assert(sizeof(BridgeStartBlockV2) == 160);
static_assert(offsetof(BridgeStartBlockV2, magic) == 0);
static_assert(offsetof(BridgeStartBlockV2, size) == 4);
static_assert(offsetof(BridgeStartBlockV2, abi) == 8);
static_assert(offsetof(BridgeStartBlockV2, pid) == 12);
static_assert(offsetof(BridgeStartBlockV2, instance_guid) == 16);
static_assert(offsetof(BridgeStartBlockV2, token) == 32);
static_assert(offsetof(BridgeStartBlockV2, sha256) == 64);
static_assert(offsetof(BridgeStartBlockV2, runtime_bundle_sha256) == 96);
static_assert(offsetof(BridgeStartBlockV2, requested_port) == 128);
static_assert(offsetof(BridgeStartBlockV2, result_state) == 132);
static_assert(offsetof(BridgeStartBlockV2, bound_port) == 136);
static_assert(offsetof(BridgeStartBlockV2, protocol) == 140);
static_assert(offsetof(BridgeStartBlockV2, win32_error) == 144);
static_assert(offsetof(BridgeStartBlockV2, winsock_error) == 148);
static_assert(offsetof(BridgeStartBlockV2, reserved0) == 152);
static_assert(offsetof(BridgeStartBlockV2, reserved1) == 156);
