# ESP32-C3 MQTT

Универсальная MQTT-прошивка для ESP32-C3 с веб-конфигуратором, Wi-Fi setup portal и интеграцией с Home Assistant через MQTT Discovery.

Проект ориентирован в первую очередь на `ESP32-C3 Super Mini`, но также подходит для `ESP32-C3 LuatOS` и совместимых плат ESP32-C3.

## Что умеет

- Настраиваемые выходы: `relay`, `pwm`, `ws2812`
- Настраиваемые входы и кнопки в едином редакторе
- Поддержка датчиков: `ds18b20_bus`, `aht20`, `sht3x`, `bme280`
- Wi-Fi клиент + точка доступа для первичной настройки
- Автосканирование Wi-Fi и кэш результатов сканирования
- Веб-интерфейс с переключением `RU/EN`
- MQTT Discovery для Home Assistant
- Хранение конфигурации в NVS через `/api/config` и применение через `/api/apply`

## Сценарий первого запуска

1. Если Wi-Fi ещё не настроен, устройство сначала сканирует доступные сети.
2. Затем поднимает точку доступа вида `ESP32-SETUP-XXXXXX`.
3. Откройте веб-интерфейс по адресу `http://192.168.4.1`.
4. Задайте параметры Wi-Fi, MQTT и конфигурацию GPIO.
5. Сохраните настройки и примените их.

## MQTT

Основные параметры задаются в разделе `connectivity.mqtt` конфигурации:

```json
{
  "connectivity": {
    "mqtt": {
      "enable": true,
      "host": "192.168.1.10",
      "port": 1883,
      "user": "",
      "pass": "",
      "client_id": "",
      "topic_prefix": "",
      "discovery_prefix": "homeassistant",
      "discovery": true,
      "retain": true
    }
  }
}
```

Примечания:

- `host` может быть IPv4, DNS-именем или URI вида `mqtt://192.168.1.10:1883`
- если `client_id` пустой, используется `esp32c3-<MAC>`
- после изменения конфигурации вызывайте `/api/apply`

## Home Assistant

1. Включите MQTT integration в Home Assistant.
2. Укажите `connectivity.mqtt.host` и при необходимости `user/pass`.
3. Сохраните конфигурацию и выполните `POST /api/apply`.
4. После подключения сущности появятся в Home Assistant через MQTT Discovery.

## Сборка через ESP-IDF

```bat
idf.py build
```

## Прошивка

```bat
idf.py -p COM8 flash
```

## Монитор

```bat
idf.py -p COM8 monitor
```

## Сборка через PlatformIO

```bat
pio run
```
