#ifndef _PICO_BTSTACK_BTSTACK_CONFIG_H
#define _PICO_BTSTACK_BTSTACK_CONFIG_H

// BTstack features that can be enabled
#define ENABLE_PRINTF_HEXDUMP
#define WANT_HCI_DUMP 0
// #define ENABLE_LOG_DEBUG
// #define ENABLE_LOG_ERROR
// #define ENABLE_LOG_INFO

#ifdef ENABLE_BLE
// BLE/GATT features for HID gamepad support
#define ENABLE_GATT_CLIENT_PAIRING
#define ENABLE_L2CAP_LE_CREDIT_BASED_FLOW_CONTROL_MODE
#define ENABLE_LE_CENTRAL
#define ENABLE_LE_DATA_LENGTH_EXTENSION
#define ENABLE_LE_PERIPHERAL
#define ENABLE_LE_PRIVACY_ADDRESS_RESOLUTION
#define ENABLE_LE_SECURE_CONNECTIONS
// Enable GATT and HID over GATT
#define ENABLE_GATT_CLIENT
#define ENABLE_ATT_CLIENT
#define ENABLE_HIDS_CLIENT
#define MAX_NR_GATT_CLIENTS 8
#define MAX_NR_HIDS_CLIENTS 8
#define MAX_NR_SM_LOOKUP_ENTRIES 16
#define MAX_NR_WHITELIST_ENTRIES 16
#define MAX_NR_LE_DEVICE_DB_ENTRIES 16
#endif

#ifdef ENABLE_CLASSIC
// Enable what's needed for HID Host supporting gamepads
#define ENABLE_HID_HOST
#define ENABLE_SDP_SERVER
#define ENABLE_SDP_CLIENT
#define ENABLE_L2CAP_ENHANCED_RETRANSMISSION_MODE
#define MAX_NR_HID_HOST_CONNECTIONS 1
#define MAX_NR_SERVICE_RECORD_ITEMS 4
#endif

#if defined(ENABLE_CLASSIC) && defined(ENABLE_BLE)
#define ENABLE_CROSS_TRANSPORT_KEY_DERIVATION
#endif

// BTstack configuration. buffers, sizes, ...
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4

#define MAX_NR_BTSTACK_LINK_KEY_DB_MEMORY_ENTRIES 4
#define MAX_NR_HCI_CONNECTIONS 8
#define MAX_NR_L2CAP_CHANNELS 16
#define MAX_NR_L2CAP_SERVICES 8

#define MAX_NR_CONTROLLER_ACL_BUFFERS 6
#define MAX_NR_CONTROLLER_SCO_PACKETS 1

// Enable and configure HCI Controller to Host Flow Control to avoid cyw43 shared bus overrun
#define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL
#define HCI_HOST_ACL_PACKET_LEN 1024
#define HCI_HOST_ACL_PACKET_NUM 6
#define HCI_HOST_SCO_PACKET_LEN 60
#define HCI_HOST_SCO_PACKET_NUM 1

// Link Key DB optimized for device pairing (increased for 8 BLE devices)
#define NVM_NUM_DEVICE_DB_ENTRIES 16 // Keep 8 DB records
#define NVM_NUM_LINK_KEYS 16         // Store keys for 8 devices

// Expanded ATT DB for BLE GATT services (increased for 8 BLE HID devices)
#define MAX_ATT_DB_SIZE 1024

// BTstack HAL configuration
#define HAVE_EMBEDDED_TIME_MS

// map btstack_assert onto Pico SDK assert()
#define HAVE_ASSERT

#endif // _PICO_BTSTACK_BTSTACK_CONFIG_H
