# Session 2026-04-18 — Phase B reverse engineering (SOCache memory layout)

## Цель
Найти CDOTAGCClientSystem singleton + расположение CSharedObjectCache чтобы в DLL
прочитать SOCache из памяти (pull) на старте инжекта и восстановить party/lobby/invite
state без ожидания GC сообщений.

## Контекст
Dota 2 running, PID 89032. client.dll loaded at `0x7FFEE6AC0000` (size `0x6F3A000`).
Якоря из предыдущей сессии (SESSION_2026-04-18.md) **устарели** — патч сдвинул
клиент примерно на `+0x2000`. Все RVA ниже — свежие, проверены live на текущем билде.

## Свежие якоря (клиент после обновления)

| Объект | RVA | Abs (при базе `0x7FFEE6AC0000`) |
|--------|-----|--------------------------------|
| Строка `CDOTAGCClientSystem\0` | `+0x4E193F8` | `0x7FFEEB8D93F8` |
| RTTI `.?AVCDOTAGCClientSystem@@` | `+0x5F64510` | — |
| RTTI `.?AVCSharedObjectCache@GCSDK@@` | `+0x6003C10` | — |
| RTTI `.?AVCSharedObjectTypeCache@GCSDK@@` | `+0x6003C40` | — |
| RTTI `.?AVCGCClientSharedObjectCache@GCSDK@@` | `+0x6003C78` | — |
| RTTI `.?AVCGCClientSharedObjectTypeCache@GCSDK@@` | `+0x6003CA8` | (реверс.) |
| COL `CDOTAGCClientSystem` primary (offset=0) | `+0x54779F0` | — |
| Vtable `CDOTAGCClientSystem` primary | `+0x4E1AC20` | `0x7FFEEB8DAC20` |
| Vtable `CDOTAGCClientSystem` 2nd (obj+0x10) | `+0x4E1AE48` | — |
| Vtable `CDOTAGCClientSystem` 3rd (obj+0x418) | `+0x4E1AE70` | — |
| Vtable `CDOTAGCClientSystem` 4th (obj+0x420) | `+0x4E1AEA8` | — |
| Vtable `CGCClientSharedObjectCache` primary | `+0x50F8A80` | `0x7FFEEBBB8A80` |
| Vtable `CGCClientSharedObjectTypeCache` | `+0x50F8A40` | `0x7FFEEBBB8A40` |
| Vtable `CSharedObjectTypeCache` (base) | `+0x50F8880` | `0x7FFEEBBB8880` |
| **CDOTAGCClientSystem singleton (inline)** | `+0x6521F20` | `0x7FFEECFE1F20` |
| Field `&g_CDOTAGCClientSystem` (self-ref) | `+0x6522710` | — |
| Field `name_ptr` ("CDOTAGCClientSystem") | `+0x65226F8` | — |

### Как найдены (кратко)
1. **String scan** с `+R-X` protection (.rdata): `AOBScan("43 44 4F 54 41 47 43 43 6C 69 65 6E 74 53 79 73 74 65 6D 00")` → 1 hit в client.dll `+0x4E193F8`.
2. **LEA xref scanner** через Lua: перебрал 111 MB модуля, нашёл 3 xref на строку. Один из них (в `sub_7FFEE6ECA5B0`) — функция регистрации, использующая глобал `+0x6521F20` как singleton.
3. **Проверка через RTTI walk**: `.?AVCDOTAGCClientSystem@@` → TypeDescriptor (`RTTI_name - 0x10 = +0x5F64500`) → 4 COLs найдены AOB-сканом на RVA в `.rdata` → 4 vtables найдены AOB-сканом на abs pointers to COLs. Совпадает с тем что лежит в singleton по +0, +0x10, +0x418, +0x420.

Xref функция-прототип (`sub_7FFEE6ECA5B0`, 0x50 байт, REGISTER_GAMESYSTEM тhunk):
```
sub rsp, 0x28
lea rdx, ["CDOTAGCClientSystem"]
lea rcx, [rip+0x6118136]           ; = +0x65226F8 (name slot in singleton)
call sub_7FFEE71C1120               ; регистрация имени
lea rax, [rip+0x4A0EDDA]            ; = +0x4E193A8 (строка на -0x50 от нашей?)
mov qword ptr [rip+0x611813F], 0    ; [+0x6522718] = 0
mov qword ptr [rip+0x6118118], rax  ; [+0x65226F8] = name_ptr (повторно)
lea rcx, [rip+0x6117939]            ; rcx = &singleton (+0x6521F20)
mov rax, qword ptr [rip+0x6117932]  ; rax = *singleton = primary vtable
lea rdx, ["CDOTAGCClientSystem"]    ; 2nd arg to virtual
mov qword ptr [rip+0x6118114], rcx  ; [+0x6522710] = &singleton
add rsp, 0x28
jmp qword ptr [rax+0x1D8]           ; virtual call (slot 59)
```

## SOCache discovery

### Путь к SOCache
```
base = client.dll
pSingleton     = base + 0x6521F20                          ; inline static storage
pSOCache       = *(base + 0x6521F20 + 0x538)               ; = base + 0x6522458
```
На данный момент (menu state, solo) `pSOCache = 0x46C659DAD00` (heap).

Также указатель на SOCache продублирован в:
- `base + 0x6522458` (он же `singleton+0x538`) — основной
- `base + 0x65D9240` (second global, возможно CUtlMap с dup)
- heap `0x46CCA8F7F80+0x0` (некий сторонний owner)

### CGCClientSharedObjectCache layout (0x46C659DAD00 @ menu)
```
+0x000 vtable CGCClientSharedObjectCache (client.dll+0x50F8A80)
+0x008 CSOID / SteamID (64-bit, 0x0569E162AE5B981A в нашем дампе)
+0x010 uint32 m_nTypeCacheCount = 4
+0x014 uint32 m_nGrowSize/allocCount = 0x11060049
+0x018 void** m_ppTypeCaches     (heap-ptr, в дампе 0x46C65B9BDC0)
+0x020 uint32 (dup of count) = 4
+0x028 uint64 = 0
+0x030 uint64 local_steam_id? (0x0110001031172261)
+0x038 ... (flags/counters)
...
```
`m_ppTypeCaches` — массив `CGCClientSharedObjectTypeCache*` размера `m_nTypeCacheCount`.

### CGCClientSharedObjectTypeCache layout (~0x40 байт, на heap)
```
+0x000 vtable CGCClientSharedObjectTypeCache (client.dll+0x50F8A40)
+0x008 uint32 count_dup (low)
+0x00C uint32 alloc_count / hash (high)
+0x010 CSharedObject** m_ppObjects  ; array of ptrs (inline в +0x20 storage)
+0x018 uint32 m_nObjectCount (low)   ; high может быть флаг 0x80000000
+0x01C uint32 flags/grow
+0x020 CSharedObject* m_pInlineStorage  ; первый объект inline, остальные 0x50 подряд (но CSharedObject*-array указывает сюда через +0x10)
+0x028 uint32 m_nTypeID              ; ← **тот самый type_id**
+0x02C uint32 hash_or_instance_id
+0x030 uint64
+0x038 uint64
```

### Текущий SOCache (menu state, solo) — 4 TypeCaches

| idx | TC_addr | type_id | count |
|-----|---------|---------|-------|
| 0 | `0x46C659DAD40` | 1 | 2 |
| 1 | `0x46C659DAEC0` | 7 | 1 |
| 2 | `0x46C659DAF40` | 2002 | 1 |
| 3 | `0x46C659DAF80` | 2012 | 1 |

Нет 2003 (party) / 2004,2006 (invite) / 2010 (lobby) — т.к. в меню, сольно, без приглашений.

## CSharedObject внутренние данные

CSharedObject subclasses (CSODOTAParty, CSODOTALobby, CSOEconItem, ...) — это наследники
`google::protobuf::MessageLite`. Размер объекта ~0x50 байт, первое поле — vtable.

Для получения proto bytes **два пути**:
1. **Virtual call `SerializeToString`** — slot в vtable зависит от компилятора, нужно реверсить.
2. **Чтение полей inline** — зная layout proto, читать поля напрямую (быстро, но поле-специфично).

## Offsets для DLL implementation (RVA от client.dll)

```cpp
// Primary verification
constexpr uint32_t RVA_SINGLETON              = 0x6521F20;
constexpr uint32_t RVA_SINGLETON_VTABLE       = 0x4E1AC20;
constexpr uint32_t OFFSET_SOCACHE_PTR         = 0x538;     // singleton+0x538

// SOCache layout
constexpr uint32_t SOC_OFF_VTABLE             = 0x000;
constexpr uint32_t SOC_OFF_TYPECACHE_COUNT    = 0x010;
constexpr uint32_t SOC_OFF_TYPECACHE_ARRAY    = 0x018;
constexpr uint32_t RVA_SOC_VTABLE             = 0x50F8A80;

// TypeCache layout
constexpr uint32_t TC_OFF_VTABLE              = 0x000;
constexpr uint32_t TC_OFF_OBJ_ARRAY           = 0x010;
constexpr uint32_t TC_OFF_OBJ_COUNT           = 0x018;
constexpr uint32_t TC_OFF_TYPEID              = 0x028;
constexpr uint32_t RVA_TC_VTABLE              = 0x50F8A40;

// Type IDs (Dota 2 шаред объекты)
constexpr int32_t SOID_CSODOTAParty           = 2003;
constexpr int32_t SOID_CSODOTAPartyInvite_New = 2004;
constexpr int32_t SOID_CSODOTAPartyInvite_Alt = 2006;
constexpr int32_t SOID_CSODOTALobby           = 2010;
```

## Следующие шаги (implementation Phase B)

1. **Создать `CSOCachePuller` класс** в `Andromeda/BotFarm/GC/`:
   - `Init()`: AOB-verify через RTTI (строка "CDOTAGCClientSystem" + vtable match), сохранить адреса.
   - Обновлять RVA **pattern-based** (AOB scan строки), а не hardcode — следующий патч снова их сдвинет.
2. **`Pull()`**: пройти `m_ppTypeCaches[0..count)`, фильтровать по `type_id in {2003, 2004, 2006, 2010}`, сериализовать каждый CSharedObject и скормить в `ApplyPartyObject` / `ApplySOObject`.
3. **Serialize**: нужно найти vtable slot `SerializeToString` или читать proto inline. Tip: у `CSharedObject` есть виртуальный `BYieldingSerializeForSO` или `BuildDestroyFromMemory`, можно реверсить по RTTI.
4. **Вызывать** `Pull()` в `CGCMessageHandler::OnInit()` сразу после установки VMT hook и периодически (тик 5с) пока lobby/party state пустой.

## Открытые вопросы

- `SOC_OFF_TYPECACHE_COUNT` = 0x10 vs 0x20 (оба содержат `4`) — возможно это CUtlMap, где 0x10 = element count в RBTree, а 0x20 = копия. Нужна ВАЛИДАЦИЯ при live party.
- `TC_OFF_OBJ_COUNT` at 0x08 и 0x18 обе содержат count — выбрать 0x18, т.к. 0x08 имеет high=hash.
- `CSharedObject::SerializeToBinary` vtable slot — НЕ РЕВЕРСНУТ. Нужно для Pull().
- Какой из путей `0x4E1AC20 / 0x4E1AE48 / 0x4E1AE70 / 0x4E1AEA8` — основной для dynamic_cast. Первый (offset=0) — primary.

## Что было сохранено в память

MEMORY index получает новую запись `Dota Phase B RE` с ссылкой на этот файл.
