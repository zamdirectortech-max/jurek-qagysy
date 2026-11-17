# Jürek — React + Firebase MVP

Четыре раздела: обзор, история, устройства, настройки. Стек: React 19, Vite 7, Firebase JavaScript SDK, SVG-графики, Lucide. Старое приложение сохранено в `legacy/`. Прошивки не изменены.

## Запуск

Node.js 22.12+ (проверяется на Node 24). Из папки `web`:

```powershell
npm install
npm run dev
# http://127.0.0.1:5173 — точный порт выводится в терминале
npm test
npm run build
npm run preview
```

Без конфигурации сразу работает явно обозначенный демонстрационный режим. В настройках выберите источник ESP32 → Firebase для реальных измерений. CSV загружается локально, максимум 5 МБ; SDNN из CSV не выдумывается. Демо и импорт не записываются в Firebase. Все источники ограничены 300 последними записями. Экспорт выгружает текущую выборку истории; это аналитический CSV, а не входной Serial-формат.

## Firebase

Выбрана **Realtime Database**, поскольку `main/main.ino` и `JurekQagysy/JurekQagysy.ino` уже делают POST в `/hrv_measurements.json`. SDK `onValue` подписан на `query(ref(db,'hrv_measurements'), orderByKey(), limitToLast(300))`. Подписки очищаются при смене источника и выходе. Ошибки доступа видны; при разрыве связи предыдущие данные могут оставаться на экране, индикатор Firebase показывает отсутствие соединения. Демо не подмешивается.

1. Создайте Firebase-проект, Web App и Realtime Database в закрытом режиме.
2. Скопируйте `.env.example` в `.env`. Заполните пять `VITE_FIREBASE_*` из Web App config. URL должен соответствовать базе прошивки. Это публичная конфигурация клиента, а не секрет. Никогда не размещайте service account, пароли или ID/refresh tokens в VITE-переменных.
3. Authentication → Sign-in method → Email/Password: включите. Создайте отдельные аккаунты исследователя и платы. При необходимости добавьте localhost и будущий домен в Authorized domains.
4. В Realtime Database Console создайте `readers/<UID исследователя>: true` и `writers/<UID платы>: true`. Только администратор через Console/Admin SDK может менять эти списки. Самостоятельной регистрации в UI нет. Знание Web API key не даёт доступа к измерениям.
5. Вкладка Rules: вставьте `database.rules.json`, Publish. Либо установленным Firebase CLI: `firebase deploy --only database --project YOUR_PROJECT_ID` из `web`.
6. Перезапустите Vite, выберите Firebase, войдите исследовательским аккаунтом. Пароль не сохраняется приложением; Firebase использует session persistence и обновляет токен автоматически в рамках сессии вкладки.

Это общий стенд одной лаборатории: все разрешённые readers видят общий поток. Для изоляции участников нужен отдельный проект или новая схема по пользователям. Запись разрешена только writers, только создание новых измерений; изменение/удаление запрещено. Чтение разрешено только с ограниченным запросом. Корень базы и списки ролей закрыты.

Структура (существующие записи мигрировать не требуется):

```json
{
 "readers": {"RESEARCHER_UID": true},
 "writers": {"DEVICE_UID": true},
 "hrv_measurements": {
  "-FirebasePushKey": {
   "boot_id":"a6c2", "session_id":1, "device_uptime_ms":83000,
   "duration_s":60, "average_bpm":72.1, "mean_rr_ms":832.2,
   "rmssd_ms":34.5, "sdnn_ms":41.2, "signal_quality":"GOOD"
  }
 }
}
```

Остальные диагностические поля прошивки допускаются. Uptime не дата; boot_id меняется при перезапуске и не является идентификатором устройства. UI не утверждает, что плата онлайн: heartbeat в протоколе отсутствует. Показатели — итог 60 секунд, не непрерывный пульс. Пропущенные числа отображаются «—».

## Подключить существующую ESP32

Откройте один из Firebase-скетчей, не основной Serial-скетч. Сохраните настройки сенсора и алгоритм расчёта. Перед загрузкой:

1. Вынесите WIFI_SSID, WIFI_PASSWORD, FIREBASE_DATABASE_URL и FIREBASE_AUTH_TOKEN из блока констант в локальный `secrets.h`, подключите `#include "secrets.h"`. `secrets.h` исключён корневым .gitignore. Не дублируйте прежние объявления.
2. URL базы — без завершающего `/`. Оставьте `/hrv_measurements.json` и существующие имена полей.
3. Для краткого теста получите **Firebase ID token** отдельного writer-аккаунта через официальный REST signInWithPassword (Web API key + email/password). В ответе есть `idToken`, `refreshToken`, `expiresIn`. Передавайте ID token как FIREBASE_AUTH_TOKEN, не Web API key и не пароль. Не выводите токены в логи. Истёкший ID token приведёт к отказу записи.
4. Для постоянной работы замените статическую константу токена на String и функцию обновления перед upload: HTTPS POST `https://securetoken.googleapis.com/v1/token?key=WEB_API_KEY`, content-type `application/x-www-form-urlencoded`, body `grant_type=refresh_token&refresh_token=<URL-encoded refresh token>`. Сохраните новые `id_token`, `refresh_token`, `expires_in` из ответа; обновляйте заранее, например за 60 секунд до истечения. Выполняйте сеть после завершения измерения, чтобы не нарушать сбор FIFO. При отказе авторизации не отправляйте запрос анонимно.
5. Обязательно замените `secureClient.setInsecure()` на `secureClient.setCACert(FIREBASE_ROOT_CA)` с доверенным корневым сертификатом цепочки вашего Firebase endpoint. Синхронизируйте время через `configTime` перед TLS-подключением. Не передавайте реальные токены при отключённой проверке TLS. Получите CA из доверенного источника, проверьте срок действия; не вставляйте произвольный сертификат из примера.
6. Если статистика может быть NaN, сериализуйте это значение как JSON `null`, а не `nan` (невалидный JSON). Например `isfinite(s.rmssd) ? String(s.rmssd, 2) : String("null")`. Сделайте это для всех floating-point полей. Пустые показатели допускаются, числовые валидируются правилами.
7. Загрузите прошивку. Serial 115200: ожидайте `#FIREBASE upload OK HTTP=200`. В базе должна появиться новая запись, сайт обновится по подписке. Ошибки 401/403 проверяйте по токену writer, UID и правилам.

Опционально добавьте `"timestamp": {".sv":"timestamp"}` в JSON — это время получения сервером, не время измерения. Для нескольких плат отдельно добавьте постоянный `device_id` и обновите модель/UI; текущий MVP предназначен для одного стенда. Сетевые повторные отправки и очередь offline в существующей прошивке не реализованы.

Основной `HRV_ESP32_Dissertation.ino` не отправляет в Firebase. Для него сохраните Serial Monitor и импортируйте:

```csv
time_s,IR,BPM,Avg_BPM,RR_ms,RMSSD_ms,Status
1.00,92000,72.00,71.80,833,35.20,VALID
```

## Публикация по необходимости

`npm run build`, затем `firebase deploy --only hosting --project YOUR_PROJECT_ID` (Firebase CLI должен быть установлен и авторизован). firebase.json уже содержит SPA fallback. В этой задаче сайт подготавливается и запускается локально, облачный проект не создаётся.

## Проверки и границы

`npm test` проверяет совместимость Serial и Firebase, отсутствующие значения, сортировку и правила на уровне структуры. `npm run build` собирает production. Полный интеграционный тест правил требует Firebase Emulator/реального проекта: проверить запрет анонимного чтения, доступ reader с limitToLast(300), запрет reader-write, создание writer и запрет update/delete. Проверка с реальной платой и защищённой базой требует ваших настроек и оборудования.

Официальные источники: https://firebase.google.com/docs/database/web/read-and-write , https://firebase.google.com/docs/database/security , https://firebase.google.com/docs/reference/rest/auth .

Результат проверки MVP: зависимости установлены; production build успешен; 6/6 unit/contract tests; HTTP 200 локального сайта. В браузере проверены обзор, история, поиск без результатов, отсутствие конфигурации Firebase и возврат к демо. Визуально проверена адаптивная компоновка на доступной ширине встроенного браузера. Реальная Firebase Authentication/RTDB, эмулятор правил и физическая ESP32 не тестировались. Установлен override @grpc/grpc-js >=1.13.6 для устранения уязвимости транзитивной зависимости Firebase; после установки npm audit: 0 уязвимостей. Firestore и gRPC не используются приложением.
