// Тест алиасинга физических страниц xmem.dll: одна страница, видимая по нескольким VA.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
typedef void* (*VAFn)(void*, size_t, uint32_t, uint64_t, uint32_t);
typedef BOOL (*APFn)(uint32_t, size_t*, uintptr_t*);
typedef void* (*MPFn)(void*, size_t, const uintptr_t*);
typedef BOOL (*MSFn)(void**, size_t, const uintptr_t*);
static int fails = 0, total = 0;
#define CHECK(c, ...) do { total++; if (c) printf("ok   " __VA_ARGS__); else { fails++; printf("FAIL " __VA_ARGS__); } printf("\n"); } while (0)
int main()
{
    HMODULE m = LoadLibraryA("xmem.dll"); if (!m) { printf("FAIL load xmem.dll %lu\n", GetLastError()); return 1; }
    auto va = (VAFn)GetProcAddress(m, "XMemVirtualAlloc"); auto ap = (APFn)GetProcAddress(m, "XMemAllocatePhysicalPages");
    auto mp = (MPFn)GetProcAddress(m, "XMemMapPhysicalPages"); auto ms = (MSFn)GetProcAddress(m, "XMemMapPhysicalPagesScatter");
    const size_t P = 0x10000;
    char* A = (char*)va(nullptr, 64 * P, 0x20402000, 0x21, PAGE_READWRITE);   // RESERVE|PHYSICAL|LARGE
    char* B = (char*)va(nullptr, 64 * P, 0x20402000, 0x21, PAGE_READWRITE);
    CHECK(A && B, "резервы PHYSICAL A=%p B=%p", A, B);
    size_t n = 16; uintptr_t ids[16]; CHECK(ap(0x380, &n, ids) && n == 16, "AllocatePhysicalPages 16 -> %#llx..", (unsigned long long)ids[0]);
    CHECK(mp(A, 8, ids) == A, "Map 8 страниц в A");
    CHECK(ids[1] - ids[0] == 16 && ids[0] % 16 == 0, "id кратны 16 (PFN 4К)");
    strcpy(A + 3 * P + 16, "hello-alias");
    // id страниц кратны 16 (PFN в 4-КиБ единицах), scatter берёт id буквально.
    strcpy(A + 4 * P + 16, "four");
    void* addrs[3] = { B + 15 * P, B + 16 * P, B + 31 * P }; uintptr_t sids[3] = { ids[3], ids[4], ids[0] };
    CHECK(ms(addrs, 3, sids), "Scatter: id3 -> B+15, id4 -> B+16, id0 -> B+31");
    CHECK(!strcmp(B + 15 * P + 16, "hello-alias") && !strcmp(B + 16 * P + 16, "four"), "запись через A видна через B+15/B+16");
    B[31 * P] = 'Z'; CHECK(A[0] == 'Z', "запись через B+31 видна через A (id0)");
    // перемаппить середину крупного view в A: страница A+3 -> id7
    strcpy(A + 7 * P, "seven"); uintptr_t one = ids[7];
    CHECK(mp(A + 3 * P, 1, &one) == A + 3 * P, "ремап A+3 -> id7");
    CHECK(!strcmp(A + 3 * P, "seven"), "A+3 теперь показывает id7");
    CHECK(A[0] == 'Z' && !strcmp(A + 7 * P, "seven"), "соседние страницы A сохранены (хвосты view переотображены)");
    CHECK(!strcmp(B + 15 * P + 16, "hello-alias"), "B+15 по-прежнему id3");
    CHECK(mp(A, 8, nullptr) == A, "unmap A (array=NULL)");
    MEMORY_BASIC_INFORMATION mbi{}; VirtualQuery(A, &mbi, sizeof mbi);
    CHECK(mbi.State == MEM_RESERVE, "A после unmap — резерв (state=%#lx)", mbi.State);
    CHECK(!strcmp(B + 15 * P + 16, "hello-alias"), "данные id3 живы после unmap A");
    // ремап диапазона, состоящего из нескольких placeholder-кусков (после unmap A) — одним вызовом
    CHECK(mp(A, 8, ids + 8) == A, "Map 8 страниц в A поверх разрозненных placeholder'ов");
    A[7 * P] = 'Q'; uintptr_t one15 = ids[15]; CHECK(mp(B + 40 * P, 1, &one15) == B + 40 * P && B[40 * P] == 'Q', "id15 через A+7 и B+40 — одна страница");
    printf("%d/%d\n", total - fails, total); return fails ? 1 : 0;
}
