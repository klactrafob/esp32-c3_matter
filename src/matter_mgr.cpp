#include "matter_mgr.h"
#include "modules/relay/mod_relay.h"
#include "esp_log.h"
#include <esp_matter.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>

static const char *TAG = "matter_mgr";
static uint16_t s_relay_endpoint_id = 0xFFFF;

static esp_err_t app_attribute_update_cb(esp_matter::attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    (void)priv_data;

    if (type != esp_matter::attribute::PRE_UPDATE) {
        return ESP_OK;
    }

    if (endpoint_id != s_relay_endpoint_id) {
        return ESP_OK;
    }

    if (cluster_id == chip::app::Clusters::OnOff::Id &&
        attribute_id == chip::app::Clusters::OnOff::Attributes::OnOff::Id) {
        bool on = (val && val->type == ESP_MATTER_VAL_TYPE_BOOLEAN) ? val->val.b : false;
        esp_err_t err = mod_relay_set_state(on);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set relay state from Matter: %s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "Matter set relay: %s", on ? "ON" : "OFF");
    }

    return ESP_OK;
}

static esp_err_t app_identification_cb(esp_matter::identification::callback_type_t type, uint16_t endpoint_id,
                                       uint8_t effect_id, uint8_t effect_variant, void *priv_data)
{
    (void)effect_id;
    (void)effect_variant;
    (void)priv_data;
    ESP_LOGI(TAG, "Identify event type=%d endpoint=%u", (int)type, endpoint_id);
    return ESP_OK;
}

static void app_event_cb(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t arg)
{
    (void)arg;
    if (!event) return;

    switch (event->Type) {
        case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
            ESP_LOGI(TAG, "Matter commissioning complete");
            break;
        case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
            ESP_LOGW(TAG, "Matter fabric removed");
            break;
        default:
            break;
    }
}

static void sync_initial_relay_state_to_matter(esp_matter::endpoint_t *endpoint)
{
    bool relay_on = false;
    if (mod_relay_get_state(&relay_on) != ESP_OK) {
        relay_on = false;
    }

    esp_matter::cluster_t *onoff_cluster =
        esp_matter::cluster::get(endpoint, chip::app::Clusters::OnOff::Id);
    if (!onoff_cluster) {
        ESP_LOGW(TAG, "OnOff cluster not found on relay endpoint");
        return;
    }

    esp_matter::attribute_t *onoff_attr =
        esp_matter::attribute::get(onoff_cluster, chip::app::Clusters::OnOff::Attributes::OnOff::Id);
    if (!onoff_attr) {
        ESP_LOGW(TAG, "OnOff attribute not found on relay endpoint");
        return;
    }

    esp_matter_attr_val_t val = esp_matter_bool(relay_on);
    esp_err_t err = esp_matter::attribute::set_val(onoff_attr, &val);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to sync initial relay state to Matter: %s", esp_err_to_name(err));
    }
}

esp_err_t matter_mgr_start(void *unused)
{
    (void)unused;

    esp_matter::node::config_t node_config;
    esp_matter::node_t *node = esp_matter::node::create(&node_config, app_attribute_update_cb,
                                                         app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return ESP_FAIL;
    }

    esp_matter::endpoint::on_off_light::config_t endpoint_config;
    esp_matter::endpoint_t *endpoint = esp_matter::endpoint::on_off_light::create(
        node, &endpoint_config, esp_matter::ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create Matter On/Off endpoint");
        return ESP_FAIL;
    }

    s_relay_endpoint_id = esp_matter::endpoint::get_id(endpoint);
    sync_initial_relay_state_to_matter(endpoint);

    esp_err_t err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_matter::start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Matter started. Relay endpoint=%u (On/Off Light)", s_relay_endpoint_id);
    return ESP_OK;
}
