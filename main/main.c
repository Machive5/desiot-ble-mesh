#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_generic_model_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_ble_mesh_config_model_api.h"

#define CID_ESP 0x02E5
#define PROV_OWN_ADDR 0x0001
#define PROV_START_ADDR 0x0005
#define APP_KEY_IDX 0x0000

static const char* BLETAG = "BLE_MESH_APP";
static uint8_t dev_uuid[16] = "CRV2N4";
static uint8_t app_key[16] = "uh987aw38h98aw3";
ESP_BLE_MESH_MODEL_PUB_DEFINE(onoff_pub, 2 + 1, ROLE_PROVISIONER);

struct ESPNode {
    uint16_t unicast_addr;
    uint8_t  dev_uuid[16];
    char role[16];
};

struct ESPNode pompa;
struct ESPNode DHT;


// static const char* MQTTTAG = "MQTT_APP";

/* Context untuk Configuration Server & Client */
static esp_ble_mesh_cfg_srv_t config_server = {
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay = ESP_BLE_MESH_RELAY_DISABLED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .beacon = ESP_BLE_MESH_BEACON_ENABLED,
    .default_ttl = 7,
};

static esp_ble_mesh_client_t config_client;

/* Root models: Wajib ada Configuration Server (CFG_SRV) dan Configuration Client (CFG_CLI) */
static esp_ble_mesh_model_t root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&config_server),
    ESP_BLE_MESH_MODEL_CFG_CLI(&config_client),
    ESP_BLE_MESH_MODEL_GEN_ONOFF_CLI(&onoff_pub, NULL),
};

static esp_ble_mesh_elem_t elements[] = {
    ESP_BLE_MESH_ELEMENT(0, root_models, ESP_BLE_MESH_MODEL_NONE),
};

static esp_ble_mesh_comp_t composition = {
    .cid = CID_ESP,
    .element_count = ARRAY_SIZE(elements),
    .elements = elements,
};

static esp_ble_mesh_prov_t provision = {
    .uuid = dev_uuid,
    .prov_uuid = dev_uuid,
    .prov_unicast_addr = PROV_OWN_ADDR,
    .prov_start_address = PROV_START_ADDR,
    .flags = 0,
    .iv_index = 0,
};

static void config_client_cb(esp_ble_mesh_cfg_client_cb_event_t event,
    esp_ble_mesh_cfg_client_cb_param_t* param)
{
    ESP_LOGI(BLETAG, "Config client event: %d, opcode: 0x%04" PRIx32, event, param->params ? param->params->opcode : 0);
}

static void provisioner_cb(esp_ble_mesh_prov_cb_event_t event, esp_ble_mesh_prov_cb_param_t* param)
{
    switch (event) {
    case ESP_BLE_MESH_PROVISIONER_PROV_ENABLE_COMP_EVT:
        ESP_LOGI(BLETAG, "Provisioner aktif berhasil, err_code: %d",
            param->provisioner_prov_enable_comp.err_code);
        break;

    case ESP_BLE_MESH_PROVISIONER_RECV_UNPROV_ADV_PKT_EVT: {
        uint8_t* dev_uuid = param->provisioner_recv_unprov_adv_pkt.dev_uuid;
        uint8_t dev_header[3] = { dev_uuid[0], dev_uuid[1], dev_uuid[2] };

        if (dev_header[0] != 'C' || dev_header[1] != 'R' || dev_header[2] != 'V') {
            ESP_LOGI(BLETAG, "Beacon unprovisioned diterima dari perangkat yang tidak dikenal, diabaikan.");
            break;
        }

        esp_ble_mesh_unprov_dev_add_t add_dev = { 0 };
        memcpy(add_dev.addr, param->provisioner_recv_unprov_adv_pkt.addr, sizeof(esp_ble_mesh_bd_addr_t));
        add_dev.addr_type = param->provisioner_recv_unprov_adv_pkt.addr_type;
        memcpy(add_dev.uuid, param->provisioner_recv_unprov_adv_pkt.dev_uuid, sizeof(add_dev.uuid));
        add_dev.oob_info = param->provisioner_recv_unprov_adv_pkt.oob_info;
        add_dev.bearer = param->provisioner_recv_unprov_adv_pkt.bearer;

        ESP_LOGI(BLETAG, "Menerima beacon unprovisioned: MAC=%02x:%02x:%02x:%02x:%02x:%02x, UUID=%02x%02x%02x%02x..., bearer=%s, RSSI=%d",
            add_dev.addr[0], add_dev.addr[1], add_dev.addr[2], add_dev.addr[3], add_dev.addr[4], add_dev.addr[5],
            add_dev.uuid[0], add_dev.uuid[1], add_dev.uuid[2], add_dev.uuid[3],
            (add_dev.bearer == ESP_BLE_MESH_PROV_ADV) ? "PB-ADV" : (add_dev.bearer == ESP_BLE_MESH_PROV_GATT) ? "PB-GATT" : "Unknown",
            param->provisioner_recv_unprov_adv_pkt.rssi);

        esp_err_t err = esp_ble_mesh_provisioner_add_unprov_dev(&add_dev,
            ADD_DEV_RM_AFTER_PROV_FLAG | ADD_DEV_START_PROV_NOW_FLAG | ADD_DEV_FLUSHABLE_DEV_FLAG);
        if (err != ESP_OK) {
            ESP_LOGE(BLETAG, "Gagal memulai provisioning unprovisioned device: %s", esp_err_to_name(err));
        }
        else {
            ESP_LOGI(BLETAG, "Sedang menghubungkan & memprovisioning perangkat teman...");
        }
        break;
    }

    case ESP_BLE_MESH_PROVISIONER_PROV_LINK_OPEN_EVT:
        ESP_LOGI(BLETAG, "Link provisioning terbuka via %s",
            param->provisioner_prov_link_open.bearer == ESP_BLE_MESH_PROV_ADV ? "PB-ADV" : "PB-GATT");
        break;

    case ESP_BLE_MESH_PROVISIONER_PROV_LINK_CLOSE_EVT:
        ESP_LOGI(BLETAG, "Link provisioning ditutup, reason: 0x%02x",
            param->provisioner_prov_link_close.reason);
        break;

    case ESP_BLE_MESH_PROVISIONER_PROV_COMPLETE_EVT:
        uint16_t remote_addr = param->provisioner_prov_complete.unicast_addr;
        uint8_t* dev_uuid = param->provisioner_prov_complete.device_uuid;

        esp_ble_mesh_cfg_client_set_state_t set_state = { 0 };

        set_state.app_key_add.net_idx = ESP_BLE_MESH_KEY_PRIMARY;
        set_state.app_key_add.app_idx = APP_KEY_IDX;
        memcpy(set_state.app_key_add.app_key, app_key, 16);

        esp_ble_mesh_client_common_param_t common = {
            .opcode = ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD,
            .model = &root_models[1], // Configuration Client model
            .ctx = {
                .net_idx = ESP_BLE_MESH_KEY_PRIMARY,
                .app_idx = ESP_BLE_MESH_KEY_UNUSED,
                .addr = remote_addr,
                .send_ttl = 7,
            },
            .msg_timeout = 0,
            .msg_role = ROLE_PROVISIONER,
        };

        esp_err_t err = esp_ble_mesh_config_client_set_state(&common, &set_state);
        if (err != ESP_OK) {
            ESP_LOGE(BLETAG, "Gagal menambahkan AppKey ke node teman: %s", esp_err_to_name(err));
            break;
        }
        else {
            ESP_LOGI(BLETAG, "Berhasil menambahkan AppKey ke node teman dengan unicast addr: 0x%04x", remote_addr);
        }

        if (memcmp(dev_uuid, "CRV2N1", 16) == 0) {
            pompa.unicast_addr = remote_addr;
            memcpy(pompa.dev_uuid, dev_uuid, 16);
            strcpy(pompa.role, "POMPA");
        }
        else if (memcmp(dev_uuid, "CRV2N2", 16) == 0) {
            DHT.unicast_addr = remote_addr;
            memcpy(DHT.dev_uuid, dev_uuid, 16);
            strcpy(DHT.role, "DHT");
        }

        ESP_LOGI(BLETAG, "=== PROVISION SELESAI! PERANGKAT TEMAN TERHUBUNG ===");
        ESP_LOGI(BLETAG, "Node Index: 0x%04x, Unicast Addr: 0x%04x, NetKey Idx: 0x%04x",
            param->provisioner_prov_complete.node_idx,
            param->provisioner_prov_complete.unicast_addr,
            param->provisioner_prov_complete.netkey_idx);
        break;

    default:
        break;
    }
}

void send_on_off_motor(uint16_t unicast_addr, bool on) {
    esp_ble_mesh_client_common_param_t common = {
        .opcode = ESP_BLE_MESH_MODEL_OP_GEN_ONOFF_SET,
        .model = &root_models[2], // Generic OnOff Client model
        .ctx = {
            .net_idx = ESP_BLE_MESH_KEY_PRIMARY,
            .app_idx = APP_KEY_IDX,
            .addr = unicast_addr,
            .send_ttl = 7,
        },
        .msg_timeout = 0,
        .msg_role = ROLE_PROVISIONER,
    };

    esp_ble_mesh_gen_onoff_set_t set_state = {
        .onoff = on ? 1 : 0,
        .tid = rand() % 256, // Random transaction ID
    };

    esp_err_t err = esp_ble_mesh_generic_client_set_state(&common, &set_state);
    if (err != ESP_OK) {
        ESP_LOGE(BLETAG, "Gagal mengirim perintah OnOff ke node teman: %s", esp_err_to_name(err));
    }
    else {
        ESP_LOGI(BLETAG, "Berhasil mengirim perintah OnOff ke node teman dengan unicast addr: 0x%04x", unicast_addr);
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Inisialisasi Bluetooth Controller & Bluedroid Host */
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    /* Salin MAC address BLE ke UUID agar unik */
    const uint8_t* mac = esp_bt_dev_get_address();
    if (mac) {
        memcpy(dev_uuid + 2, mac, 6);
    }

    /* Daftarkan callback provisioning dan config client */
    ESP_ERROR_CHECK(esp_ble_mesh_register_prov_callback(provisioner_cb));
    ESP_ERROR_CHECK(esp_ble_mesh_register_config_client_callback(config_client_cb));

    /* Inisialisasi BLE Mesh stack */
    ESP_ERROR_CHECK(esp_ble_mesh_init(&provision, &composition));

    /* Aktifkan Provisioner pada PB-ADV */
    ESP_ERROR_CHECK(esp_ble_mesh_provisioner_prov_enable(ESP_BLE_MESH_PROV_ADV));
}
