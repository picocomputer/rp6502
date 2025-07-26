#ifndef _PICO_BTSTACK_BTSTACK_CONFIG_H
#define _PICO_BTSTACK_BTSTACK_CONFIG_H

// BTstack features that can be enabled
#define ENABLE_PRINTF_HEXDUMP
#define WANT_HCI_DUMP 0
// #define ENABLE_LOG_DEBUG
// #define ENABLE_LOG_ERROR
// #define ENABLE_LOG_INFO

#ifdef ENABLE_BLE
#define ENABLE_GATT_CLIENT_PAIRING
#define ENABLE_L2CAP_LE_CREDIT_BASED_FLOW_CONTROL_MODE
#define ENABLE_LE_CENTRAL
#define ENABLE_LE_DATA_LENGTH_EXTENSION
#define ENABLE_LE_PERIPHERAL
#define ENABLE_LE_PRIVACY_ADDRESS_RESOLUTION
#define ENABLE_LE_SECURE_CONNECTIONS
#endif

#ifdef ENABLE_CLASSIC
// Enable what's needed for HID Host supporting gamepads
#define ENABLE_HID_HOST
#define ENABLE_SDP_SERVER
#define ENABLE_SDP_CLIENT
#define ENABLE_L2CAP_ENHANCED_RETRANSMISSION_MODE
#endif

#if defined(ENABLE_CLASSIC) && defined(ENABLE_BLE)
#define ENABLE_CROSS_TRANSPORT_KEY_DERIVATION
#endif

// BTstack configuration. buffers, sizes, ...
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4

// Remove unused audio/networking services for gamepad-only config
// #define MAX_NR_AVDTP_CONNECTIONS 1
// #define MAX_NR_AVDTP_STREAM_ENDPOINTS 1
// #define MAX_NR_AVRCP_CONNECTIONS 2
// #define MAX_NR_BNEP_CHANNELS 1
// #define MAX_NR_BNEP_SERVICES 1
// #define MAX_NR_HFP_CONNECTIONS 1
// #define MAX_NR_RFCOMM_CHANNELS 1
// #define MAX_NR_RFCOMM_MULTIPLEXERS 1
// #define MAX_NR_RFCOMM_SERVICES 1

// Optimized for 1 Classic HID gamepad with extra connections for discovery
#define MAX_NR_BTSTACK_LINK_KEY_DB_MEMORY_ENTRIES 4 // Keep 4 DB records as requested
#define MAX_NR_HCI_CONNECTIONS 3                    // Extra HCI connections for device discovery
#define MAX_NR_HID_HOST_CONNECTIONS 1               // 1 HID connection (btstack limitation)
#define MAX_NR_L2CAP_CHANNELS 8                     // 1 gamepad × 2 channels + SDP overhead + spare
#define MAX_NR_L2CAP_SERVICES 4                     // Keep services: HID Control + HID Interrupt + SDP services
#define MAX_NR_SERVICE_RECORD_ITEMS 4               // Keep for SDP support

// Remove unused BLE/GATT for Classic-only config
// #define MAX_NR_GATT_CLIENTS 1
// #define MAX_NR_HIDS_CLIENTS 1
// #define MAX_NR_SM_LOOKUP_ENTRIES 3
// #define MAX_NR_WHITELIST_ENTRIES 16
// #define MAX_NR_LE_DEVICE_DB_ENTRIES 16

#define MAX_NR_CONTROLLER_ACL_BUFFERS 3
#define MAX_NR_CONTROLLER_SCO_PACKETS 1

// Enable and configure HCI Controller to Host Flow Control to avoid cyw43 shared bus overrun
#define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL
#define HCI_HOST_ACL_PACKET_LEN 1024
#define HCI_HOST_ACL_PACKET_NUM 3
#define HCI_HOST_SCO_PACKET_LEN 60
#define HCI_HOST_SCO_PACKET_NUM 1

// Link Key DB optimized for device pairing
#define NVM_NUM_DEVICE_DB_ENTRIES 4 // Keep 4 DB records
#define NVM_NUM_LINK_KEYS 4         // Store keys for 4 devices

// Minimal ATT DB since we're Classic-only
#define MAX_ATT_DB_SIZE 128

// BTstack HAL configuration
#define HAVE_EMBEDDED_TIME_MS

// map btstack_assert onto Pico SDK assert()
#define HAVE_ASSERT

#endif // _PICO_BTSTACK_BTSTACK_CONFIG_H
