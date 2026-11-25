> Public export checkpoint, November 26, 2026: this source includes the opt-in
> sandbox hook (`XSX_SB_NATIVE_GFX=1`), M2 BO mapping helpers/probe and the
> XMem hook (`XSX_NATIVE_GFX=1`). M1/M1b are recorded passes; newer game
> logs show native XMem activation and successful physical-destination
> storage records. Standalone M2 probe success and a complete native game
> renderer are not established. The roadmap below is the earlier note.
> See [current status](../docs/STATUS.md) and [build notes](../docs/BUILDING.md).

# native-gfx — ветка «нативное исполнение Xbox-графики на хост-RDNA2» (с 25.11.2026)

Идея: не транслировать Xbox D3D12.X/ISA в HLSL/VKD3D, а исполнять ISA, дескрипторы и
регистровое состояние игры напрямую на хост-GPU (Navi21, gfx1030) через amdgpu PM4, с памятью
игры, отображённой на GPU по тем же VA. VKD3D остаётся только для вывода в окно (на первом этапе).
Основная ветка (local-agent-work fork, VKD3D-путь) не трогается.

## Доказано (см. EXPERIMENTS.md, compat/tests/native-isa/)
- E1 raw compute ISA, identity VA (userptr) — PASS.
- E2 исходные VS_Blit+PS_BlitColour игры, RECTLIST, RSRC из блока PSO — PASS 4096/4096.
- E3 текстурный draw игры (матрицы из V#, image_sample через T#) — PASS; негативный контроль FAIL.
- userptr работает ТОЛЬКО на private anonymous памяти; shared/memfd/файл — нет (userptr-shared.c).
- Обратная схема: GTT BO = «физическая память», CPU mmap DRM-fd по фиксированным адресам
  (алиасы OK), GPU VA map того же BO по тем же адресам — PASS (bo-fixed.c). Это база для XMem.
- M1 (здесь): unixlib xsxgpu (builtin PE + .so) внутри Wine 11 открывает amdgpu — PASS;
  M1b: E2 draw изнутри Wine-процесса на памяти VirtualAlloc — PASS 4096/4096.
  Загрузка: заглушка xsxgpu.dll в C:\windows\system32 префикса + WINEDLLPATH=native-gfx/build
  (Wine ищет builtin только для файлов из system32). Данные: XSXGPU_DATA=native-gfx/data.

## Дорожная карта
- M1c Песочница: тот же probe внутри compat/sandbox (нужен read-only bind native-gfx/build и
  data — изменение песочницы ТОЛЬКО с согласия пользователя).
- M2 XMem на GTT BO: физические страницы XMem = BO amdgpu; view = mmap DRM fd по guest-адресу
  (через unixlib, согласованно с резервированием Wine); GPU VA map тех же BO. Алиасы сохраняются.
- M3 Нативная запись командных списков: перехват слотов Xbox-списка в d3d12_xs -> PM4:
  PSO из блока состояния (разметка полей), user SGPR ABI (VS s[0:1] fetch-table 40-bit, s2 base
  vertex, s[4:5] fetch, s8 resource table 32-bit; PS s8 table, s9 PRIM_MASK), fetch-подпрограммы
  из input layout, таблицы дескрипторов в нижних 4 GiB, настоящие T#/V# в CreateSRV/CBV,
  CB/DB из CreateRTV/DSV, барьеры = cache flush/wait.
- M4 Вывод: финальный RT -> swapchain (dma-buf import в Vulkan, на старте — копия).
- M5 Compute: нативные Dispatch (сейчас no-op в основной ветке) — возможная причина «чёрного
  экрана», если игра ждёт результатов GPU.
- M6 Тайлинг: совместимость swizzle-раскладок Series X vs Navi21 на реальных текстурах игры.

Риски: отличия Series X (gfx10.x custom) в регистрах/ISA; сброс GPU при кривом PM4; синхронизация
с VKD3D; объём «драйверной» работы (radeonsi как референс).
