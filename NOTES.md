# ORC → Steam: разведка (2026-10-21)

## Что лежит в папке игры
- `xlive.dll` + `xlive.pdb` — не настоящий GFWL, а сторонняя заглушка `xlive-dummy` (сообщество, автор неизвестен).
  Экспортирует 163 ординала, имён нет.
- `steam_api.dll` — старый, v1.10.1.46 (2012). Steam AppID: `209100`.
- `GDF_worldwide.dll` — Game Definition File (рейтинги/метаданные для Games Explorer), к сети отношения не имеет.
- Title ID GFWL: `43430FA1` (из манифеста в xlive.dll).

## Импорты RaccoonCity.exe
- `xlive.dll` — 82 функции (по ординалам), полный список в `imports.txt`.
- `steam_api.dll` — только `SteamAPI_Init`, `RunCallbacks`, `Shutdown`, `SteamApps`, `SteamFriends`, `SteamUtils`
  (судя по набору — проверка DLC и оверлей, сетевой игры через Steam нет).
- `WS2_32` (22) / `WSOCK32` (2) — обычные сокеты, назначение пока не ясно.

## xlive по группам (ординал → функция, по таблице GFWL 3.5)
- Сокеты: 3 Create, 4 Close, 5 Shutdown, 6 IOCTL, 7 SetSockOpt, 8 GetSockOpt, 9 GetSockName, 11 Bind,
  12 Connect, 13 Listen, 14 Accept, 15 Select, 18 Recv, 20 RecvFrom, 22 Send, 24 SendTo, 25 Inet_Addr, 26 XWSAGetLastError
- XNet: 51 Startup, 55 RegisterKey, 57 XnAddrToInAddr, 60 InAddrToXnAddr, 62 UnregisterInAddr,
  69 QosLookup, 70 QosServiceLookup, 72 QosGetListenStats, 73 GetTitleXnAddr
- Сессии: 5300 Create, 5316 (InviteSend?), 5317 WriteStats, 5318 Start, 5321 Search, 5322 Modify, 5323 MigrateHost,
  5325 LeaveLocal, 5326 JoinRemote, 5327 JoinLocal, 5330 Delete, 5332 End, 5336 LeaveRemote; 5315 InviteGetAcceptedInfo
- Пользователь/профиль: 5262 GetSigninState, 5263 GetName, 5265 CheckPrivilege, 5267 GetSigninInfo,
  5276/5277 SetProperty/SetContext, 5293 SetPropertyEx, 5331/5339 ReadProfileSettings(ByXuid), 5278 WriteAchievements,
  5284/5286 StatsEnumerator, 5312 FriendsCreateEnumerator, 5314 MuteListQuery
- Ядро/UI: 5001 Input, 5002 Render, 5003 Uninitialize, 5007 OnResetDevice, 5030 PreTranslateMessage, 5297 InitializeEx,
  5310/5311 OnlineStartup/Cleanup, 5324 GetNatType, 5260 ShowSigninUI, 5252 GamerCardUI, 5209/5210/5215 Show*UI, 5365 MarketplaceUI
- Async/уведомления: 651 XNotifyGetNext, 5270 NotifyCreateListener, 1082/1083 Overlapped, 5251 CloseHandle, 5254 CancelOverlapped, 5256 XEnumerate
- Защищённые данные (сохранения!): 5034 ProtectData, 5035 UnprotectData, 5036/5038 ProtectedDataContext
- Голос: 5008 XHVCreateEngine

Ординалы 5209/5210/5276/5277/5316 помечены по памяти — подтвердить логированием.

## Steam: что доступно без замены steam_api.dll
Старый steam_api.dll отдаёт `SteamMatchMaking008` (лобби), `SteamNetworking005` (P2P с релеем Valve),
`SteamUser016`, `STEAMUSERSTATS_INTERFACE_VERSION010` (достижения), `STEAMREMOTESTORAGE004` (облако).

Новый steam_api.dll из SDK **нельзя** просто подложить: в нём нет `SteamAPI_Init`, `SteamApps`,
`SteamFriends`, `SteamUtils` в старом виде — игра не запустится. Решение: работать через интерфейсы
старой DLL (лобби + P2P там есть), либо прокси-steam_api позже, если понадобится новое API.

## Первый лог с игрой (стадия 1)
- Игра с прокси работает нормально. Сохранения грузятся через `XLiveUnprotectData` (сначала запрос размера 0x8007007A, потом данные).
- **Почему «нет подключения к LIVE»:** при каждом входе в сетевое меню игра вызывает XNetStartup → XOnlineStartup ×2 →
  `XUserGetSigninState(0)`, а заглушка возвращает 1 (вход только локально). Нужно 2 (вход в Live).
  Кроме того, `XNetGetTitleXnAddr` возвращает 1 (NONE, адреса нет).
- Ординалы, подтверждённые аргументами: 20 = XSocketRecvFrom, 26 = XSocketInet_Addr (вернул 127.0.0.1),
  25 = XWSASendTo (3 буфера). В списке выше 25 указан как Inet_Addr ошибочно — правильная таблица в `src/xlive_log/names.h`.
- Сокеты: сначала проверочный UDP-сокет, потом основной сокет с протоколом **254 = VDP** (игровые данные GFWL).
  Отдельный поток висит в RecvFrom; главный поток будит его, отправляя 3 байта на 127.0.0.1. Это не сетевая игра, а внутренний механизм.
- XUserGetSigninInfo вызывается каждый кадр (~550 раз/с) — реализация должна быть дешёвой.
- XOnlineGetNatType = 1 (Open), XHVCreateEngine = 0 (голос «создан»).

## Стадия 2a: подмена входа
Свои реализации в `src/orcsteam/overrides.cpp`: 73 XNetGetTitleXnAddr (виртуальный адрес 10.x.y.z из Steam account id),
5262 XUserGetSigninState = 2, 5263 XUserGetName, 5265 XUserCheckPrivilege = TRUE, 5267 XUserGetSigninInfo
(XUID 0x0009… + account id, ник из Steam). Остальное по-прежнему уходит в заглушку.

## Второй лог (после подмены входа)
- Ошибки про Live нет, ник из Steam подхватился (кириллица в UTF-8 — может отображаться криво, проверить).
- Игра проверяет привилегии 0xFE и 0xFC (мультиплеер и т.п.) — отвечаем TRUE.
- Дальше `XSessionCreate` — заглушка возвращает 0xFFFFFFFF, создание срывается. Два вида сессий, обе на 4 слота:
  - флаги `0x223` (HOST | USES_PRESENCE | USES_PEER_NETWORK | JOIN_VIA_PRESENCE_DISABLED) — похоже на «группу»/пати;
  - флаги `0x2D` (HOST | USES_STATS | USES_MATCHMAKING | USES_PEER_NETWORK) — матч; перед ним 10× XUserSetProperty
    (0x10000044–0x1000004D, 0x20000008, 0x2000004A–4D) и XUserSetContext 0x800A/0x800B — это параметры поиска матча.

## Стадия 2b: локальные сессии
`src/xlive_log/sessions.cpp`: Create/Delete/Start/End/Modify/Join*/Leave*/MigrateHost/WriteStats/Search,
плюс XGetOverlappedResult/ExtendedError и XCloseHandle для наших хэндлов. Асинхронные вызовы завершаются сразу.
Хост сам генерирует XNKID (тип online peer 0x80), ключ и nonce. Поиск пока всегда пуст — Steam-лобби дальше.

## Третий лог (локальные сессии) — хост проходит весь цикл
Последовательность у хоста (все вызовы наши, все успешны):
1. 25.4 с: Create «пати» (0x223) → JoinLocal → 30.7 с: Modify (флаги 0x23).
2. 30.8 с: Create «матч» (0x2D) → `XNetQosListen(kid, …, flags 1)` (хост объявляет себя для QoS-проб) → JoinLocal.
3. 51.97 с: Start обеих сессий — **матч запущен**.
4. 87.3 с: End + Delete + XCloseHandle пати, новая пати; 88.7 с: End матча, `XNetQosListen(flags 2)` (снять), Delete, Close.
Все асинхронные вызовы игра проверяет через XGetOverlappedResult — наше немедленное завершение её устраивает.
XSessionSearch ни разу не вызван (хост его не делает) — будет на стороне клиента.

## Стадия 3: Steam-лобби и P2P (собрано, проверено на одном ПК)
- `steam.h/.cpp` — интерфейсы старой steam_api.dll (порядок методов сверен с open-steamworks:
  SteamMatchMaking008, SteamNetworking005, SteamUtils005). Поток-насос: опрашивает асинхронные вызовы
  через IsAPICallCompleted/GetAPICallResult, читает P2P, раз в 250 мс принимает P2P от участников лобби.
  Колбэк P2PSessionRequest зарегистрирован через SteamAPI_RegisterCallback. Насос останавливается в XLiveUninitialize.
- `sessions.cpp` — хост: на сессию с флагом USES_MATCHMAKING создаётся публичное лобби, данные:
  `orcsteam=1`, `info` (XSESSION_INFO hex), `ctx`, `props`, `pub/priv/filled`, `type/mode`, `started`.
  Клиент: XSessionSearch → RequestLobbyList (фильтр orcsteam=1, весь мир), фильтр по контекстам 0x800A/0x800B,
  своё лобби пропускается; XSessionCreate клиента входит в лобби найденного хоста.
  XUserSetContext/SetProperty теперь наши — запоминают параметры для лобби.
- `net.cpp` — виртуальные адреса 10.x.y.z ↔ Steam account id (полный id в XNADDR.abOnline).
  SendTo/WSASendTo на 10.x → SendP2PPacket (заголовок 6 байт: магия, порт назначения, порт источника;
  ≤1200 байт — unreliable, больше — reliable). Приём: пакет подкладывается в сокет игры через loopback
  с заголовком 10 байт, наш RecvFrom его снимает и подставляет виртуальный адрес отправителя.
  XNetXnAddrToInAddr/InAddrToXnAddr/RegisterKey и QoS (Lookup: все хосты «доступны», 50 мс) — наши.
- Тесты (`build/selftest`): транспорт через loopback — ok; с живым Steam: лобби создаётся за ~0,3 с,
  поиск его находит и пропускает как своё, выход из лобби и остановка насоса — ok.
- Не проверено: второй игрок (поиск чужого лобби, вход клиента, обмен пакетами, миссия).
- Архив для второго игрока: `orcsteam-coop-test1.zip` (install.bat / uninstall.bat / ПРОЧТИ.txt).

## Первая проверка вдвоём (22.10, test1) — вылет у клиента при поиске
- Хост: лобби создано, на 148 с принят P2P-сеанс от клиента.
- Клиент: поиск (procedure 11, 50 результатов, 6 props, 2 contexts, type 1 mode 0) нашёл лобби хоста,
  QoS-проба прошла, затем вылет.
- Причина (дизассемблер, `build/qos_disasm.txt`): после QoS игра в функции 0xBF2D10 ищет в каждом результате
  свойство 0x40008109 (X_PROPERTY_GAMER_HOSTNAME, UNICODE) и без проверки передаёт указатель в копирование строки
  0xBF19F0 → NULL → access violation. Настоящий GFWL добавлял это свойство сам.
  Других системных свойств (0x1000810x/0x2000810x/0x3000810x) exe не использует.
- Исправлено в test2: хост пишет `name` в лобби, поиск добавляет свойство имени хоста. Игра копирует его,
  беря младший байт каждого символа, поэтому кириллица в имени хоста будет отображаться криво.
- В test2 добавлен журнал исключений (`!!!! EXCEPTION ...`: модуль+смещение, регистры, стек, последний вызов xlive).

## Вторая проверка (test2) — клиент дошёл до входа в лобби хоста
- Клиент: поиск → QoS → QoS release → XSessionCreate (клиент, флаги 0x2C) → вход в
  Steam-лобби хоста → XNetXnAddrToInAddr → `#62 XNetInAddrToString(ina, buf, 0x20)` → вылет с EIP=0x20.
- Причина: в xlive-dummy #62 делает `ret` без снятия аргументов (нужно `ret 0Ch`) → стек сдвигается.
  `tools/audit_stub.ps1` сверил все 50 функций заглушки, которые ещё вызывает игра: сломаны только #62 и #5316
  (XInviteSend, `ret` вместо `ret 14h`). Обе теперь наши (test3).
- Ник: игра рисует имена только ASCII-шрифтом (gamertag). Steam-ник транслитерируется (`Иван` → `Ivan`),
  то же имя пишется в лобби для чужого списка поиска.

## Третья проверка (test3, 22.10) — РАБОТАЕТ
Хост + клиент, кампания (type 1 mode 0), одно задание пройдено до конца, видимых багов нет.
Лог хоста: P2P-сеанс принят на 75 с, XSessionJoinRemote на 83 с, игрок вышел на 1187 с (~18,5 мин в матче).
Хост отправил 36 596 пакетов, получил 29 105, потерь (`dropped`) нет, исключений нет.
Клиент в сессии хоста представлен офлайн-XUID (E000… + account id) — игру это устраивает.

Не проверено: 3–4 игрока, режимы против игроков, приглашения через Steam, смена хоста при выходе хоста.

## Стадия 4: приглашения и «Присоединиться» через Steam (test4, проверено на одном ПК)
`invites.cpp`:
- Вход: колбэк GameLobbyJoinRequested (333) или `+connect_lobby <id>` в командной строке (через 20 с после
  готовности Steam) → JoinLobby → из данных лобби берётся XSESSION_INFO → наше уведомление
  XN_LIVE_INVITE_ACCEPTED (0x02000002, игра сравнивает с ним в 0x49BDEC) всем слушателям с областью LIVE →
  игра вызывает XInviteGetAcceptedInfo → получает данные хоста → создаёт клиентскую сессию (лобби уже известно).
  Лобби, которое игра так и не использовала за 120 с, покидается.
- Хост: XFriendsCreateEnumerator/XEnumerate отдают игре онлайн-друзей Steam (играющие в ORC первыми),
  XInviteSend → InviteUserToLobby, XShowGuideUI → оверлей приглашений Steam (или друзья), XShowGamerCardUI → профиль.
- XNotifyCreateListener/GetNext теперь наши (обёртки над заглушкой + своя очередь).
- Отладка: `ORCSTEAM_TEST_JOIN=<lobby id>` имитирует «Присоединиться» (Steam не шлёт его владельцу лобби).
- Тест: уведомление 02000002 доставлено, XInviteGetAcceptedInfo вернул данные лобби, клиентская сессия вошла в лобби.
- Не проверено вживую: реальное приглашение между двумя аккаунтами, запуск через +connect_lobby.

## Проверка test4 — вылет при входе по «Присоединиться»
- Клиент: запрос 333 пришёл, лобби хоста найдено, уведомление доставлено двум
  слушателям, XInviteGetAcceptedInfo отработал — и игра закрылась без исключения в нашем журнале.
- Причина: игра собрана с XINVITE_INFO и XONLINE_FRIEND, упакованными по 4 (84 и 0xC4 байта; видно по кадру
  стека в 0xBCE3A0 и шагу `add esi,0C4h` в 0xACA754). Мы писали 88 байт → затёрли GS-cookie → __fastfail
  (идёт в обход векторных обработчиков, поэтому журнал падений молчал).
- test5: обе структуры под `#pragma pack(4)` + static_assert на размеры; тест проверяет метку после 84 байт.
- `tools/find_callers.ps1` — находит места вызова любого ординала xlive в exe (IAT → thunk → call).

## Проверка test5 (22.10) — приглашения РАБОТАЮТ, смена хоста тоже
По логам обоих игроков исключений нет ни у кого:
1. Товарищ нажал «Присоединиться» (колбэк 333) → вошёл в лобби хоста → уведомление → XInviteGetAcceptedInfo →
   игра пересоздала свою пати как клиент (флаги 0x222) и вошла в матч (0x2C). Миссия запущена на 131 с.
2. Хост вышел из игры → на 162–164 с игра товарища вызвала XSessionMigrateHost для обеих сессий → «we are the host
   now», данные лобби переписаны (Steam сам передал владение лобби оставшемуся участнику).
3. Хост перезапустил игру через приглашение (`+connect_lobby`, вход через 20 с) → вошёл к товарищу как клиент,
   прошли ещё ~1,5 мин вместе. Повторный запрос 333 при запуске корректно проигнорирован («already in it»).
Не проверено: 3–4 игрока, режимы против игроков, приглашение из внутриигрового меню (XInviteSend / список друзей).

## Версия 1.0 — без xlive-dummy
- Поведение заглушки восстановлено по её коду (символы из `xlive.pdb`, `tools/show_stub.ps1`), а не по документации:
  - сохранения: Protect/Unprotect — одна функция копирования (`8007007A` + размер при малом буфере) → сейвы совместимы;
  - профиль (5331): 8 + 0x28 на настройку + место под все строки/блобы; обычные настройки — DEFAULT (нули,
    пустая строка cb=2, обнулённый блоб), title-specific (≥0x3FFD) — NO_VALUE/0xFF; 5339 → 0x490;
  - уведомления: общая очередь, первым спросивший забирает; XLiveInitializeEx кладёт XN_SYS_SIGNINCHANGED (0x0A, 1);
  - XHVCreateEngine: объект на 20 методов IXHV2Engine с кодами возврата как у заглушки;
  - сокеты — Winsock (VDP→UDP), остальные функции — константы заглушки (0/1/0x12, дескриптор −1 и т.д.).
- `core.cpp`: экспортируются ровно 82 ординала из импорта exe; трассировка — `ORCSTEAM_TRACE=1` или `orcsteam.ini`
  (`[orcsteam] trace=1`); лог `orcsteam.log`. Ресурс версии 1.0.
- Проверки: `selftest.exe [steam]` (в т.ч. сейвы, профиль, уведомление входа, голосовой движок),
  `tools/audit_impl.ps1` — все 82 функции снимают со стека ровно размер аргументов GFWL.
- Релиз: `scripts/build.ps1` → `scripts/package.ps1` → `orcsteam-1.0.zip` (установщик помнит, был ли xlive.dll).
- Проверено в игре (22.10, оба на 1.0): ни одного исключения; уведомление входа 0x0A доставлено на ~2,5 с у обоих;
  кооп через приглашение (хост — я), ~5 мин игры; хост вышел из матча → лобби перешло к товарищу → я вернулся
  к нему по «Присоединиться» → ещё ~2,5 мин; смена хоста в обе стороны; трафик ~13 тыс. пакетов в каждую сторону,
  потерь нет; корректный выход у обоих (насос остановлен, `exit`).

## Версия 1.1
- `orcsteam.ini` читается всеми модулями (`OrcsteamIniInt/String` в core.cpp).
- `lobby=friends` (по умолчанию выключено): хостовое лобби создаётся как FriendsOnly — в RequestLobbyList его
  не видно никому, вход только по приглашению / «Присоединиться». Тест: поиск такого лобби не находит.
- Rich presence (ISteamFriends009::SetRichPresence, метод 34): только `steam_player_group` = id лобби и
  `steam_player_group_size` — игроки одного лобби группируются в списке друзей. Текстовый `status` убран:
  без токенов локализации издателя Steam показывает его лишь в «Посмотреть информацию об игре».
- Безопасность сетевого приёма (принимать P2P только от своих) отложена по решению автора: аудитория маленькая.

## 4-й игрок не входит к троим (24.10, лог Alexious-94, у него 1.0)
- Пати (0x222) он проходит, все трое `joined` сразу. Но в сессию матча (0x2C) игра так и не вызывает
  XSessionJoinRemote, а через ~16 с сама выходит из неё; после двух таких попыток «соединение разорвано».
  Вдвоём (22.10) у него всё работало → транспорт исправен, дело в сетке на 4 (связи клиент↔клиент?).
- Тестовая сборка `orcsteam-diag1.zip` (1.1-diag1, `diag=1` + `trace=1` в orcsteam.ini): первые 40 пакетов
  на пира в каждую сторону (порты, размер, 16 байт), раз в 10 с счётчики + GetP2PSessionState, колбэки
  P2PSessionConnectFail (1203) и LobbyChatUpdate (506), состав и владелец лобби при изменении, сокеты
  (создание/bind/закрытие), ошибки recvfrom и инжектора, неизвестные адреса в XNet, Start/End/Modify/Delete.

### Проверка diag1 (25–26.10) — вчетвером РАБОТАЕТ
- Хост DimacAkiyama (89420542) + Alexious-94 + Max Payne (156310710) + Lesley Bright (129907410, четвёртый):
  все вошли через «Присоединиться», миссия ~13 мин вчетвером, Steam P2P «active», потерь и ошибок нет.
  Прошлый сбой (другой состав: 149007750, 118293589; у Alexious 1.0) не повторился.
- Баг: у четвёртого JoinLobby сессии матча вернул 4 (full) — лобби на 4 места уже полное, а он сам в нём
  (вошёл через приглашение). Сессия осталась без лобби → через 120 с invites.cpp вышел из него как из «неиспользуемого».
  Игре это не мешает, но игрок выпадает из Steam-лобби (группа в друзьях, передача лобби при смене хоста).
  Исправлено: если мы уже в лобби, сессия берёт его без JoinLobby (`AlreadyInLobby` в sessions.cpp).
- Мелочь (лог хоста, второй запуск): после выхода хоста лобби переходит к оставшемуся участнику, и «Присоединиться»
  к нему ведёт в мёртвую сессию (данные лобби — от старого хоста). Игра сама отбрасывает такой вход за 0,3 с.

## Версия 1.2 (26.10)
- Лог Alexious-94 (1.1, тот же матч вчетвером): чисто, исключений и ошибочных кодов нет. Но сравнение
  XSessionMigrateHost во всех логах показало баг: игра зовёт его у **каждого** оставшегося игрока —
  у нового хоста с userIndex 0 (pSessionInfo — выход), у остальных с 0xFE = XUSER_INDEX_NONE (pSessionInfo —
  вход, данные нового хоста). Мы всегда считали себя хостом и затирали игре данные хоста своими.
  Теперь при 0xFE сессия становится клиентской с info нового хоста.
- Лобби после смены хоста: Steam отдаёт его случайному оставшемуся, а писать данные может только владелец.
  `publishPending` (ставится при MigrateHost и при неудачном SetLobbyData) → SessionsPump переписывает
  данные, как только лобби наше, даже если мы не хост (у клиента в info — данные нового хоста). Имя хоста
  пишет только сам хост. Это и чинит «мелочь» выше, когда смена хоста была.
- Плюс `AlreadyInLobby` (выше), `diag=1` остаётся настройкой. selftest проверяет оба вида MigrateHost.
- Не проверено в игре: смена хоста вчетвером на 1.2.

## Голосовой чат: разведка (26.10)
- В игре есть своя голосовая система (`VoiceChatSystemPC`, `VoiceChatServiceProvider`, значки `VoiceChatIconPulse/
  Muted/Connected`, `Mute_Player%d`, `User_Headset_Volume`). Объект — синглтон `[010CAEEC]`, движок XHV в `+10h`;
  перед созданием проверяется настройка `DisableGFWL`.
- XHVCreateEngine (0xACBB75): XHV_INIT_PARAMS — 30 удалённых, 1 локальный, режим VOICECHAT (2) с обеих сторон,
  без своего VAD, hwnd окна, рабочий поток не нужен (NULL).
- Вызываемые методы IXHV2Engine (смещение в vtable): +04 Release, +0C StartLocalProcessingModes, +14 StartRemote…,
  +20/+24 Register/UnregisterLocalTalker, +28/+2C Register/UnregisterRemoteTalker, +34 IsHeadsetPresent,
  +44 GetLocalChatData, +48 SetPlaybackPriority, +4C SubmitIncomingChatData. IsLocal/RemoteTalking и
  GetDataReadyFlags в этом коде не вызываются.
- Отправка (0xACBA10): если флаг `+14h` включён — GetLocalChatData(0, буфер 0x1A4 = 420 байт, &size, &packets);
  при packets != 0 данные уходят сообщением 0xAD (размер 32 бита + байты) каждому из ≤8 пиров (0xACC0D0) — т. е.
  через наш транспорт. Приём (0xACB4C0): SubmitIncomingChatData(xuid отправителя, data, &size). Данные для игры
  непрозрачны → можно класть сжатый голос Steam кусками ≤ ~200 байт (каждый кусок GetVoice декодируется отдельно).
- Steam: голос есть в ISteamUser016 (vtable 7 Start, 8 Stop, 9 GetAvailableVoice, 10 GetVoice, 11 DecompressVoice,
  12 GetVoiceOptimalSampleRate) — проверено с живым Steam: оптимальная частота 24000; без речи — NoData (3).
- Звук игры — Wwise через WASAPI (MMDeviceEnumerator), XAudio2/DirectSound не используются → свой XAudio2_9
  (тоже WASAPI shared) не мешает.

## Версия 1.3: голосовой чат (26.10)
- `voice.cpp` — настоящий IXHV2Engine (vtable как у прежней заглушки, она убрана из compat.cpp). Игра сама
  возит данные; в них наш блоб: `56 01` + [u16 длина][кусок GetVoice]..., не больше 200 байт (сообщение игры
  собирается в ~300-байтном буфере). Каждый кусок GetVoice декодируется отдельно (DecompressVoice, частота —
  GetVoiceOptimalSampleRate, 24000).
- Свой поток раз в 20 мс: Start/StopVoiceRecording (пока игра держит локального собеседника; с voice_key — пока
  зажата клавиша и окно игры активно), GetVoice → очередь для GetLocalChatData; входящие блобы → DecompressVoice →
  XAudio2 (источник на игрока, предбуфер 60 мс, задержка ≤ 400 мс — как в ERVoiceChat). XAudio2_9/_8 грузится
  через LoadLibrary: статической зависимости нет, на Win7 просто нет звука собеседников.
- SetPlaybackPriority(NEVER) = игрок выключен в меню игры → его голос не играем. IsHeadsetPresent = voice включён.
- Поток останавливается в XLiveUninitialize (до остановки насоса Steam) и при Release движка.
- selftest: регистрация, нет данных без микрофона, гарнитура, «говорит» после входящего блоба, выключенный — нет.
- Не проверено в игре: всё. Совместимость: 1.2 молча выбрасывает наши блобы.

## 1.4 (объёмный голос + эхо по местности): разведка (26.10)
- Exe **экспортирует Wwise** по именам (dumpbin /exports; адреса там — RVA, база 0x400000, но exe с ASLR →
  считать от GetModuleHandle(NULL)). Нужное: RegisterGameObj(id[, name]) RVA 0x5116C0 (обе перегрузки — одно тело),
  UnregisterGameObj 0x511710, SetPosition(id, AkSoundPosition&, listener) 0x5123C0,
  SetGameObjectEnvironmentsValues(id, AkEnvironmentValue*, n) 0x511120 (старый Wwise: «environments» = зоны реверба),
  Query::GetListenerPosition(idx, AkListenerPosition&) 0x8655D0 (берёт критическую секцию Wwise — можно с любого потока),
  Query::GetPosition 0x865890, GetGameObjectEnvironmentsValues 0x865B80. Все cdecl. AkGameObjectID — 32 бита.
- Чего нет в Wwise: какой объект — какой игрок. Поэтому `probe.cpp` (только diag=1, MinHook в third_party):
  таблица объектов (имя, позиция, окружения) из перехватов; каждый P2P-пакет ищется на три float = позиция свежего
  объекта (±2 м, с любого бита — сообщения игры битовые). Попадание «IN from A … obj X at bit B» = персонаж A — это X.
  Отчёт раз в 2 с при движении камеры: слушатель, 5 ближайших объектов с окружениями.
- Проверено без игры (scratchpad probetest): позиция со сдвигом 13 бит и +0,3 м находится. В игре — нет.
- Тестовый архив `orcsteam-1.3-test1.zip` (голос 1.3 + diag=1). Риск: если игра шлёт координаты не float
  (квантованные), попаданий не будет — тогда искать формат по логам пакетов.

### Проверка 1.3-test1 (27.10, я + Erniskilla619, вдвоём)
- Рация не проверялась: у обоих `open mic`, voice_key в ini не было.
- Мой голос дошёл (у товарища received 935, decode errors 0, «playing account 429520184»), но у меня
  sent 936 / dropped 1513: кусок GetVoice за такт 20 мс (на деле ~30 мс) часто > 196 байт → выброшен.
  Буфер сообщения игры — RakNet-подобный BitStream (0x9618E0: 0x800 бит на стеке, 0x961D20 растит), поэтому
  test2: блоб до 400 байт (буфер игры 420), такт 10 мс, очередь 32, статистика по причинам потерь и числу запросов игры.
- У товарища sent 0 за всю игру при `recording started` — Steam не отдал ни одного куска: смотреть его
  Steam → Голос (устройство, порог, проверка микрофона).
- Разведка: видны только 3 объекта в (0,0,0) — это «плоские» звуки: игра ставит их через SetPosition
  (0x970C1F, при флаге 0x968F80), а 3D-излучатели — через SetMultiplePositions (0x970DDD; ID объекта 16 бит,
  movsx; **Z отрицается**, xorps с маской знака). test2 перехватывает и её, а пакеты сверяет с Z и −Z.
  Ось «вверх» — Y (у слушателя Y ≈ −3,8 и front.y = 0). Окружение D6D165DB у объекта 2 менялось 0,54 → 1,00.

### Проверка 1.3-test2 (27.10, лог Erniskilla619)
- Голос у товарища заработал (sent 162, received 736), потерь почти нет: 10 «too big» (кусок Steam до 754 байт
  в начале фразы), очередь не переполнялась — игра спрашивает данные >1000 раз/с.
- Разведка: 3D-объекты видны. Имена: `SES:Vector`, `SES:Spectre` (люди), `SES:Lupo_AI`, `SES:Bertha_AI` (боты),
  `CollisionSFX`, `BulletImpact`, `Sound Effect La…`. По 2 объекта на персонажа. Ближе всего к камере (~1,9 м) —
  свой персонаж. Окружение D6D165DB плавно 0,2…1,0 при переходах.
- Поиск координат в пакетах: 1 случайное попадание — игра шлёт их не float'ами. Не нужен (см. 1.4).

## Версия 1.4: объёмный голос (27.10)
- `spatial.cpp` (вместо probe.cpp; хуки ставятся всегда, если voice и voice_spatial включены, или при diag):
  RegisterGameObj, UnregisterGameObj, SetPosition, SetMultiplePositions, SetGameObjectEnvironmentsValues,
  GetIDFromString(char*) RVA 0x512330 (пролог 81 EC 04 01 00 00 56) — имена, которые игра хэширует в ID Wwise
  (так узнаётся имя зоны эха). «Я» = ближайший к слушателю живой (позиция < 1 с) объект `SES:*` без `_AI`.
- Голосовой блоб остаётся версии 1, первым куском — «OSP1» + позиция (3 float, пространство Wwise) + до 2 окружений
  (ID + 0..255), 17–27 байт. 1.3 принимает его за кусок голоса, не декодирует и идёт дальше (decode error).
- Воспроизведение: громкость = 1 до voice_near (5), дальше near/d (не тише 0,08); за voice_far (40) — рация:
  BandPass 1800 Гц, 1/Q 1, громкость 0,6, по центру, без эха; выход из рации ниже far·0,85. Панорама —
  равномощная по dot(dir, top×front) (Wwise левосторонняя: X вправо, Y вверх, Z вперёд), центр = 1/1, дальнее ухо ≥ 0,25.
  Эхо: общий стерео-submix с CreateAudioReverb (XAudio2_9/_8), пресет I3DL2 по словам в имени зоны
  (sewer/tunnel/hall/room/street…, иначе generic), уровень = окружение говорящего × 0,45 × доля расстояния.
  Пресет — по окружению нашего персонажа. Каждая новая зона пишется в лог один раз с именем и пресетом.
- Проверено без игры (scratchpad spatialtest, настоящий XAudio2, 8 каналов, громкость 0): перед/справа/слева/20 м/
  60 м рация/36 м ещё рация/30 м снова прямо, фильтр, отправка в эхо, пресет по имени, кусок «OSP1» туда-обратно.
- Не проверено в игре: всё. Риски: совпадают ли координаты Wwise у разных игроков (должны — один уровень),
  правильная ли сторона панорамы, есть ли у зон имена через GetIDFromString.

### Проверка 1.4-test1 (27.10, лог Erniskilla619) — объём РАБОТАЕТ
- Расстояния верные (рядом 2,7–4,4 м, вдали 24–29 м), рация с 40 м и обратно, панорама меняется при обходе.
- Имена зон через GetIDFromString есть: D6D165DB = `hospital_Room_Large` → large room,
  DEE77578 = `Hall_Dark_hospital` → large hall. 891 из 1012 блобов с позицией.
- Жалоба «рядом очень тихо»: громкость там 1,00 — тихий сам голос Steam рядом со звуком игры. test2:
  выравнивание (Level): RMS к 3500, усиление до ×6, пик ≤ 30000, быстро вниз / плавно вверх; voice_volume до 400 %.
- 12 из ~140 фраз теряли начало: кусок Steam до 748 байт. test2: режется на части «OFR1» (id, номер, всего,
  ≤ 300 байт), получатель собирает и декодирует целиком; потеря части — кусок отбрасывается (счётчик incomplete).
- Мелочь: первая строка «our character found at -2.3 -5.3 23.1» при камере в (234, −4, 127) — сразу после
  загрузки уровня; дальше позиции верные.

## Архитектура
`RaccoonCity.exe` → (импорт переименован `xlive.dll` → `orcsteam.dll`) → своя DLL:
- XSession* → Steam Lobby (008); XInvite* → приглашения через оверлей
- XSocket*/XNet* → виртуальные адреса ↔ SteamID, пакеты через SendP2PPacket/ReadP2PPacket
- XUser* → SteamUser/SteamFriends; Protect/Unprotect → локальные файлы
- UI/Marketplace/Signin → пустышки
