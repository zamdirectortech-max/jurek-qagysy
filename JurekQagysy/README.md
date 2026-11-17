# Jurek Qagysy — ESP32-S3 HRV + Firebase Realtime Database

Это отдельная копия проекта. Исходный `../HRV_ESP32_Dissertation.ino` не изменяется.

## 1. Настройка Firebase

1. Создайте проект в Firebase Console.
2. Откройте **Build → Realtime Database → Create Database**.
3. Скопируйте URL базы, например:
   `https://project-name-default-rtdb.asia-southeast1.firebasedatabase.app`
4. Для первого короткого теста можно использовать временные тестовые правила. Не оставляйте публичную запись включённой постоянно.

## 2. Настройка кода

Откройте `JurekQagysy.ino` и заполните:

```cpp
constexpr char WIFI_SSID[] = "имя Wi-Fi";
constexpr char WIFI_PASSWORD[] = "пароль Wi-Fi";
constexpr char FIREBASE_DATABASE_URL[] = "https://...firebasedatabase.app";
constexpr char FIREBASE_AUTH_TOKEN[] = "";
```

Пустой `FIREBASE_AUTH_TOKEN` работает только тогда, когда правила базы разрешают запись без авторизации. Для защищённой базы укажите действующий Firebase ID token и настройте правила для этого пользователя. Не публикуйте файл с настоящим паролем или токеном.

## 3. Arduino IDE

- Открывайте именно `JurekQagysy/JurekQagysy.ino`.
- Board: **ESP32S3 Dev Module**
- Flash Size: **16MB**
- PSRAM: **OPI PSRAM**
- USB CDC On Boot: **Enabled**
- Upload Speed: **115200**
- Serial Monitor: **115200 baud**

Дополнительная библиотека Firebase не нужна: используются `WiFi`, `WiFiClientSecure` и `HTTPClient` из пакета ESP32.

После каждого успешно завершённого 60-секундного измерения плата выполняет HTTPS `POST`. Результат появляется в Realtime Database по пути:

```text
hrv_measurements/<автоматический Firebase ID>/
```

В мониторе порта успешная отправка выглядит так:

```text
#FIREBASE upload OK HTTP=200
```

Код использует `setInsecure()` для простого первого запуска. Перед эксплуатацией с реальными персональными/медицинскими данными добавьте проверку TLS-сертификата, Firebase Authentication и закрытые Security Rules.
