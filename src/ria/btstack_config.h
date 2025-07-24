#ifndef _PICO_BTSTACK_BTSTACK_CONFIG_H
#define _PICO_BTSTACK_BTSTACK_CONFIG_H

// BTstack features that can be enabled
#define ENABLE_LOG_INFO
#define ENABLE_LOG_ERROR
#define ENABLE_PRINTF_HEXDUMP
// #define ENABLE_SCO_OVER_HCI
// #define WANT_HCI_DUMP 1

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
// Enable what's needed for HID Host including enhanced reliability
#define ENABLE_HID_HOST
#define ENABLE_SDP_SERVER
#define ENABLE_SDP_CLIENT
// Enable enhanced retransmission for better HID reliability with modern gamepads
#define ENABLE_L2CAP_ENHANCED_RETRANSMISSION_MODE
// GOEP not needed for HID gamepads
// #define ENABLE_GOEP_L2CAP
#endif

#if defined (ENABLE_CLASSIC) && defined(ENABLE_BLE)
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

// Optimized for 4 Classic HID gamepads
#define MAX_NR_BTSTACK_LINK_KEY_DB_MEMORY_ENTRIES  8  // Increased for multiple gamepad pairing
#define MAX_NR_HCI_CONNECTIONS 4                       // 4 concurrent gamepad connections
#define MAX_NR_HID_HOST_CONNECTIONS 4                  // 4 HID connections
#define MAX_NR_L2CAP_CHANNELS  16                      // Increased: 4 gamepads × 2 channels + SDP overhead + spare
#define MAX_NR_L2CAP_SERVICES  4                       // Increased: HID Control + HID Interrupt + SDP services
#define MAX_NR_SERVICE_RECORD_ITEMS 4                  // Increased for better SDP support

// Remove unused BLE/GATT for Classic-only config
// #define MAX_NR_GATT_CLIENTS 1
// #define MAX_NR_HIDS_CLIENTS 1
// #define MAX_NR_SM_LOOKUP_ENTRIES 3
// #define MAX_NR_WHITELIST_ENTRIES 16
// #define MAX_NR_LE_DEVICE_DB_ENTRIES 16

// Limit number of ACL/SCO Buffer to use by stack to avoid cyw43 shared bus overrun
#define MAX_NR_CONTROLLER_ACL_BUFFERS 6                // Increased for better L2CAP/HID reliability
#define MAX_NR_CONTROLLER_SCO_PACKETS 1                // Minimal SCO (required by BTStack even if unused)

// Enable and configure HCI Controller to Host Flow Control to avoid cyw43 shared bus overrun
#define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL
#define HCI_HOST_ACL_PACKET_LEN 1024
#define HCI_HOST_ACL_PACKET_NUM 6                      // Increased to match controller buffers
#define HCI_HOST_SCO_PACKET_LEN 60                     // Minimal SCO (required by BTStack even if unused)
#define HCI_HOST_SCO_PACKET_NUM 1                      // Minimal SCO packets

// Link Key DB optimized for multiple gamepad pairing
#define NVM_NUM_DEVICE_DB_ENTRIES 8                    // Support more paired devices
#define NVM_NUM_LINK_KEYS 8                            // Store keys for 8 devices (room for extras)

// Minimal ATT DB since we're Classic-only (kept small to save memory)
#define MAX_ATT_DB_SIZE 128                            // Reduced from 512

// BTstack HAL configuration
#define HAVE_EMBEDDED_TIME_MS

// map btstack_assert onto Pico SDK assert()
#define HAVE_ASSERT

// Some USB dongles take longer to respond to HCI reset (e.g. BCM20702A).
#define HCI_RESET_RESEND_TIMEOUT_MS 1000

#define ENABLE_SOFTWARE_AES128
// BLE secure connections not needed for Classic-only
// #define ENABLE_MICRO_ECC_FOR_LE_SECURE_CONNECTIONS

#define HAVE_BTSTACK_STDIN

// To get the audio demos working even with HCI dump at 115200, this truncates long ACL packets
//#define HCI_DUMP_STDOUT_MAX_SIZE_ACL 100

#endif // _PICO_BTSTACK_BTSTACK_CONFIG_H
