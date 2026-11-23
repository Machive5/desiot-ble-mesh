#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
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

#define ESP_BLE_MESH_VND_MODEL_ID_CLIENT 0x0000
#define ESP_BLE_MESH_VND_MODEL_OP_SEND   ESP_BLE_MESH_MODEL_OP_3(0x01, CID_ESP)

/* Tombol BOOT built-in pada ESP32 (GPIO 0) */
#define BUTTON_GPIO 14

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
static bool pompa_state = false;

/* Context untuk Configuration Server & Client */
static esp_ble_mesh_cfg_srv_t config_server = {
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .relay = ESP_BLE_MESH_RELAY_DISABLED,
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .beacon = ESP_BLE_MESH_BEACON_ENABLED,
    .default_ttl = 7,
};

static esp_ble_mesh_client_t config_client;
static esp_ble_mesh_client_t onoff_client;

/* Root models: Wajib ada Configuration Server (CFG_SRV) dan Configuration Client (CFG_CLI) */
static esp_ble_mesh_model_t root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&config_server),
    ESP_BLE_MESH_MODEL_CFG_CLI(&config_client),
    ESP_BLE_MESH_MODEL_GEN_ONOFF_CLI(&onoff_pub, &onoff_client),
};

static esp_ble_mesh_model_op_t vend_ops[] = {
    ESP_BLE_MESH_MODEL_OP(ESP_BLE_MESH_VND_MODEL_OP_SEND, 2),
    ESP_BLE_MESH_MODEL_OP_END,
};

static esp_ble_mesh_model_t vnd_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(CID_ESP, ESP_BLE_MESH_VND_MODEL_ID_CLIENT,
                              vend_ops, NULL, NULL),
};

static esp_ble_mesh_elem_t elements[] = {
    ESP_BLE_MESH_ELEMENT(0, root_models, vnd_models),
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

    esp_ble_mesh_generic_client_set_state_t set_state = {
        .onoff_set = {
            .op_en = false,
            .onoff = on ? 1 : 0,
            .tid = rand() % 256,
        }
    };

    esp_err_t err = esp_ble_mesh_generic_client_set_state(&common, &set_state);
    if (err != ESP_OK) {
        ESP_LOGE(BLETAG, "Gagal mengirim perintah OnOff ke node teman: %s", esp_err_to_name(err));
    }
    else {
        ESP_LOGI(BLETAG, "Berhasil mengirim perintah OnOff (%s) ke node teman dengan unicast addr: 0x%04x", on ? "ON" : "OFF", unicast_addr);
    }
}

static void config_client_cb(esp_ble_mesh_cfg_client_cb_event_t event,
    esp_ble_mesh_cfg_client_cb_param_t* param)
{
    ESP_LOGI(BLETAG, "Config client event: %d, opcode: 0x%04" PRIx32, event, param->params ? param->params->opcode : 0);

    if (event == ESP_BLE_MESH_CFG_CLIENT_SET_STATE_EVT) {
        if (param->params->opcode == ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD) {
            uint16_t target_addr = param->params->ctx.addr;
            ESP_LOGI(BLETAG, "AppKey sukses ditambahkan ke node 0x%04x! Memulai Model App Bind...", target_addr);

            esp_ble_mesh_cfg_client_set_state_t bind_state = { 0 };
            esp_ble_mesh_client_common_param_t bind_common = {
                .opcode = ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND,
                .model = &root_models[1],
                .ctx = {
                    .net_idx = ESP_BLE_MESH_KEY_PRIMARY,
                    .app_idx = APP_KEY_IDX,
                    .addr = target_addr,
                    .send_ttl = 7,
                },
                .msg_timeout = 0,
                .msg_role = ROLE_PROVISIONER,
            };

            bind_state.model_app_bind.element_addr = target_addr;
            bind_state.model_app_bind.model_app_idx = APP_KEY_IDX;

            if (target_addr == pompa.unicast_addr) {
                // Bind Generic OnOff Server pada ESP Pompa
                bind_state.model_app_bind.model_id = ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_SRV;
                bind_state.model_app_bind.company_id = ESP_BLE_MESH_CID_NVAL;
                esp_ble_mesh_config_client_set_state(&bind_common, &bind_state);
                ESP_LOGI(BLETAG, "Mengirim Model Bind Generic OnOff Server ke Pompa (0x%04x)", pompa.unicast_addr);
            }
            else if (target_addr == DHT.unicast_addr) {
                // Bind Vendor Model pada ESP DHT
                bind_state.model_app_bind.model_id = ESP_BLE_MESH_VND_MODEL_ID_CLIENT;
                bind_state.model_app_bind.company_id = CID_ESP;
                esp_ble_mesh_config_client_set_state(&bind_common, &bind_state);
                ESP_LOGI(BLETAG, "Mengirim Model Bind Vendor Model ke DHT (0x%04x)", DHT.unicast_addr);
            }
        }
        else if (param->params->opcode == ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND) {
            ESP_LOGI(BLETAG, "=== MODEL APP BIND SUKSES untuk node 0x%04x! Siap berkomunikasi. ===", param->params->ctx.addr);
        }
    }
}

static void generic_client_cb(esp_ble_mesh_generic_client_cb_event_t event,
    esp_ble_mesh_generic_client_cb_param_t* param)
{
    ESP_LOGI(BLETAG, "Generic client event: %d", event);
}

static void dht_msg_cb(esp_ble_mesh_model_cb_event_t event, esp_ble_mesh_model_cb_param_t* param)
{
    switch (event) {
    case ESP_BLE_MESH_MODEL_OPERATION_EVT: {
        if (param->model_operation.opcode == ESP_BLE_MESH_VND_MODEL_OP_SEND) {
            uint16_t src_addr = param->model_operation.ctx->addr;
            uint8_t* data = param->model_operation.msg;
            uint16_t data_len = param->model_operation.length;

            ESP_LOGI(BLETAG, "=== DATA DHT DITERIMA DARI NODE 0x%04x (len: %d) ===", src_addr, data_len);
            char msg_str[256] = { 0 };
            if (data_len < sizeof(msg_str)) {
                memcpy(msg_str, data, data_len);
                ESP_LOGI(BLETAG, "Pesan Sensor: %s", msg_str);
            }
            else {
                ESP_LOG_BUFFER_HEX(BLETAG, data, data_len);
            }
        }
        break;
    }
    default:
        break;
    }
}

static void provisioner_cb(esp_ble_mesh_prov_cb_event_t event, esp_ble_mesh_prov_cb_param_t* param)
{
    switch (event) {
    case ESP_BLE_MESH_PROVISIONER_PROV_ENABLE_COMP_EVT:
        ESP_LOGI(BLETAG, "Provisioner aktif berhasil, err_code: %d",
            param->provisioner_prov_enable_comp.err_code);
        break;

    case ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_APP_KEY_COMP_EVT: {
        ESP_LOGI(BLETAG, "Local AppKey berhasil ditambahkan, err_code: %d", param->provisioner_add_app_key_comp.err_code);
        if (param->provisioner_add_app_key_comp.err_code == ESP_OK) {
            /* 1. Bind local Generic OnOff Client model ke AppKey lokal */
            esp_err_t err = esp_ble_mesh_provisioner_bind_app_key_to_local_model(
                PROV_OWN_ADDR, APP_KEY_IDX,
                ESP_BLE_MESH_MODEL_ID_GEN_ONOFF_CLI, ESP_BLE_MESH_CID_NVAL);
            if (err != ESP_OK) {
                ESP_LOGE(BLETAG, "Gagal bind OnOff Client lokal: %s", esp_err_to_name(err));
            } else {
                ESP_LOGI(BLETAG, "OnOff Client lokal berhasil di-bind ke AppKey!");
            }

            /* 2. Bind local Vendor Client model ke AppKey lokal */
            err = esp_ble_mesh_provisioner_bind_app_key_to_local_model(
                PROV_OWN_ADDR, APP_KEY_IDX,
                ESP_BLE_MESH_VND_MODEL_ID_CLIENT, CID_ESP);
            if (err != ESP_OK) {
                ESP_LOGE(BLETAG, "Gagal bind Vendor Client lokal: %s", esp_err_to_name(err));
            } else {
                ESP_LOGI(BLETAG, "Vendor Client lokal berhasil di-bind ke AppKey!");
            }
        }
        break;
    }

    case ESP_BLE_MESH_PROVISIONER_BIND_APP_KEY_TO_MODEL_COMP_EVT:
        ESP_LOGI(BLETAG, "Provisioner bind local model selesai, err_code: %d", param->provisioner_bind_app_key_to_model_comp.err_code);
        break;

    case ESP_BLE_MESH_PROVISIONER_RECV_UNPROV_ADV_PKT_EVT: {
        uint8_t* dev_uuid = param->provisioner_recv_unprov_adv_pkt.dev_uuid;
        char uuid_str[17];
        memcpy(uuid_str, dev_uuid, 16);
        uuid_str[16] = '\0';

        if (memcmp(dev_uuid, "CRV", 3) != 0) {
            ESP_LOGI(BLETAG, "Beacon unprovisioned diterima dari perangkat yang tidak dikenal, UUID=%s, diabaikan.", uuid_str);
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

    case ESP_BLE_MESH_PROVISIONER_PROV_COMPLETE_EVT: {
        uint16_t remote_addr = param->provisioner_prov_complete.unicast_addr;
        uint8_t* dev_uuid = param->provisioner_prov_complete.device_uuid;

        char uuid_str[17];
        memcpy(uuid_str, dev_uuid, 16);
        uuid_str[16] = '\0';

        if (memcmp(dev_uuid, "CRV2N1", 6) == 0) {
            pompa.unicast_addr = remote_addr;
            memcpy(pompa.dev_uuid, dev_uuid, 16);
            strcpy(pompa.role, "POMPA");
            ESP_LOGI(BLETAG, "Node teridentifikasi sebagai: POMPA (0x%04x)", remote_addr);
        }
        else if (memcmp(dev_uuid, "CRV2N2", 6) == 0) {
            DHT.unicast_addr = remote_addr;
            memcpy(DHT.dev_uuid, dev_uuid, 16);
            strcpy(DHT.role, "DHT");
            ESP_LOGI(BLETAG, "Node teridentifikasi sebagai: DHT (0x%04x)", remote_addr);
        }

        ESP_LOGI(BLETAG, "=== PROVISION SELESAI! PERANGKAT TEMAN TERHUBUNG ===");
        ESP_LOGI(BLETAG, "Node Index: 0x%04x, Unicast Addr: 0x%04x, NetKey Idx: 0x%04x",
            param->provisioner_prov_complete.node_idx,
            param->provisioner_prov_complete.unicast_addr,
            param->provisioner_prov_complete.netkey_idx);

        /* Kirim AppKey ke node teman */
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
        }
        else {
            ESP_LOGI(BLETAG, "Sedang mengirimkan AppKey ke node teman (0x%04x, UUID: %s)...", remote_addr, uuid_str);
        }
        break;
    }

    default:
        break;
    }
}

/* Thread / Task pembaca input Button untuk mengontrol Pompa */
static void button_task(void* pvParameters)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    ESP_LOGI(BLETAG, "Button task aktif! Gunakan tombol pada GPIO %d untuk ON/OFF Pompa.", BUTTON_GPIO);

    while (1) {
        /* Tombol ditekan (Active LOW pada GPIO 0) */
        if (gpio_get_level(BUTTON_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50)); // Debounce
            if (gpio_get_level(BUTTON_GPIO) == 0) {
                if (pompa.unicast_addr != 0) {
                    pompa_state = !pompa_state;
                    ESP_LOGI(BLETAG, "[BUTTON] Tombol ditekan! Mengirim perintah Pompa -> %s (addr: 0x%04x)",
                        pompa_state ? "ON" : "OFF", pompa.unicast_addr);
                    send_on_off_motor(pompa.unicast_addr, pompa_state);
                }
                else {
                    ESP_LOGW(BLETAG, "[BUTTON] Tombol ditekan, tetapi node POMPA belum terhubung / diprovision!");
                }

                /* Tunggu hingga tombol dilepas */
                while (gpio_get_level(BUTTON_GPIO) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
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

    /* Daftarkan callback provisioning, config client, generic client, dan vendor model */
    ESP_ERROR_CHECK(esp_ble_mesh_register_prov_callback(provisioner_cb));
    ESP_ERROR_CHECK(esp_ble_mesh_register_config_client_callback(config_client_cb));
    ESP_ERROR_CHECK(esp_ble_mesh_register_generic_client_callback(generic_client_cb));
    ESP_ERROR_CHECK(esp_ble_mesh_register_custom_model_callback(dht_msg_cb));

    /* Inisialisasi BLE Mesh stack */
    ESP_ERROR_CHECK(esp_ble_mesh_init(&provision, &composition));

    /* Aktifkan Provisioner pada PB-ADV */
    ESP_ERROR_CHECK(esp_ble_mesh_provisioner_prov_enable(ESP_BLE_MESH_PROV_ADV));

    /* Daftarkan local AppKey pada Provisioner (harus setelah prov_enable) */
    ESP_ERROR_CHECK(esp_ble_mesh_provisioner_add_local_app_key(app_key, ESP_BLE_MESH_KEY_PRIMARY, APP_KEY_IDX));

    /* Buat thread/task terpisah untuk membaca input button */
    xTaskCreate(button_task, "button_task", 3072, NULL, 5, NULL);

    ESP_LOGI(BLETAG, "Provisioner siap! Menunggu beacon unprovisioned dari node lain...");
}
