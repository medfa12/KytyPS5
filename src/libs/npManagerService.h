#pragma once

#include "libs/npManagerClient.h"

namespace Libs::NpManagerService {

// Native IPMI descriptor layout, shared with the narrow libC client bridge.
struct DataInfo { const void* data; uint64_t size; };
struct BufferInfo { void* buf; uint64_t capacity; uint64_t written; };
static_assert(sizeof(DataInfo) == 16 && sizeof(BufferInfo) == 24);
static_assert(offsetof(DataInfo, size) == 8 && offsetof(BufferInfo, written) == 16);

// Explicit HLE service registration. This service owns real local connections,
// queues and named kernel event flags; its account backend is unavailable.
// It neither registers a native handler nor claims native ShellCore initialized.
// Start/Stop belong to VSH lifecycle. No automatic fallback or sign-in simulation.
int StartUnavailableService() noexcept;
int StopService() noexcept;
bool Running() noexcept;
int ConnectEndpoint(std::shared_ptr<NpManagerClient::Endpoint>& output) noexcept;

// Exact SceNpMgrIpc client-vtable bridge. No guest callback or worker is entered.
// Descriptor pointers follow the surrounding libC guest-memory contract.
int ConnectGuest(void* client, const void* argument, uint64_t size, int* acceptance) noexcept;
int InvokeGuest(void* client, uint32_t method, const DataInfo* input, uint32_t input_count,
                int* service_result, BufferInfo* output, uint32_t output_count) noexcept;
int DisconnectGuest(void* client) noexcept;

// Trusted service-producer boundary. Records are the recovered 24-byte firmware
// shape, not a network protocol. No production producer is currently attached;
// startup, subscription and unavailable queries never manufacture events.
int PublishEvent(uint32_t channel, std::span<const uint8_t, 24> record) noexcept;

} // namespace Libs::NpManagerService

namespace Libs::LibIpmiClient { void InitIpmiClient_1(Loader::SymbolDatabase* symbols); }
