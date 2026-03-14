# ESP32-C3 MQTT Relay (1 channel)

Прошивка переделана под базовое устройство для Home Assistant через MQTT.

## Что делает сейчас

- Управляет одним реле (по умолчанию GPIO12).
- Подключается к MQTT-брокеру.
- Публикует состояние реле (`ON`/`OFF`).
- Принимает команду реле (`ON`/`OFF`/`TOGGLE`).
- Публикует MQTT Discovery для Home Assistant (тип `switch`).
- Сохраняет настройки в NVS (`/api/config`) и применяет через `/api/apply`.

## MQTT топики

По умолчанию (`topic_prefix = esp32-c3/relay1`):

- Команда: `esp32-c3/relay1/relay/set`
- Состояние: `esp32-c3/relay1/relay/state`
- Доступность: `esp32-c3/relay1/status`

Discovery topic:

- `homeassistant/switch/<node_id>/relay/config`

## Конфиг MQTT в JSON

Секция `mqtt` в `/api/config`:

```json
{
  "mqtt": {
    "enable": true,
    "host": "192.168.1.10",
    "port": 1883,
    "user": "",
    "pass": "",
    "client_id": "",
    "topic_prefix": "esp32-c3/relay1",
    "discovery_prefix": "homeassistant",
    "device_name": "ESP32 C3 Relay",
    "discovery": true,
    "retain": true
  }
}
```

Примечания:

- `host` может быть именем/IPv4 (`192.168.1.10`) или полным URI (`mqtt://192.168.1.10:1883`).
- Если `client_id` пустой, берется `esp32c3-<MAC>`.
- После изменения конфига вызывайте `/api/apply`.

## Home Assistant

1. Убедитесь, что в HA включен MQTT integration.
2. В конфиге устройства укажите `mqtt.host` и при необходимости `user/pass`.
3. Выполните `POST /api/apply`.
4. После подключения устройство автоматически появится в HA через MQTT Discovery.

## Сборка

```bat
pio run
```

## Прошивка

```bat
pio run -t upload
```

## Монитор порта

```bat
pio device monitor
```

## Полная очистка flash

```bat
flash_clean.bat COM4
```
