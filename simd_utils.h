#ifndef SIMD_UTILS_H
#define SIMD_UTILS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __SSE4_2__
#include <nmmintrin.h>
#define SIMD_AVAILABLE 1
#else
#define SIMD_AVAILABLE 0
#endif

// SIMD-оптимизированные функции для HTTP парсинга
#if SIMD_AVAILABLE

// Быстрый поиск символов в строке с использованием SSE4.2
static inline const char* simd_find_char(const char* haystack, size_t len, char needle) {
    const char* end = haystack + len;
    const char* aligned_start = (const char*)((uintptr_t)(haystack + 15) & ~15);
    
    // Обрабатываем неалigned начало
    for (const char* p = haystack; p < aligned_start && p < end; p++) {
        if (*p == needle) return p;
    }
    
    if (aligned_start >= end) return NULL;
    
    // SIMD поиск по 16 байт за раз
    __m128i needle_vec = _mm_set1_epi8(needle);
    for (const char* p = aligned_start; p + 16 <= end; p += 16) {
        __m128i data = _mm_load_si128((__m128i*)p);
        __m128i cmp = _mm_cmpeq_epi8(data, needle_vec);
        int mask = _mm_movemask_epi8(cmp);
        if (mask) {
            return p + __builtin_ctz(mask);
        }
    }
    
    // Обрабатываем остаток
    for (const char* p = end - (end - aligned_start) % 16; p < end; p++) {
        if (*p == needle) return p;
    }
    
    return NULL;
}

// Быстрая валидация URL символов
static inline int simd_validate_url_chars(const char* url, size_t len) {
    const char* end = url + len;
    const char* aligned_start = (const char*)((uintptr_t)(url + 15) & ~15);
    
    // Валидные символы: a-z, A-Z, 0-9, /, -, _, ., ?, =, &
    __m128i lower_bound = _mm_set1_epi8(0x20);  // ' '
    __m128i upper_bound = _mm_set1_epi8(0x7E);  // '~'
    
    // Обрабатываем неaligned начало
    for (const char* p = url; p < aligned_start && p < end; p++) {
        char c = *p;
        if (c < 0x20 || c > 0x7E) return 0;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return 0;
    }
    
    if (aligned_start >= end) return 1;
    
    // SIMD валидация по 16 байт за раз
    for (const char* p = aligned_start; p + 16 <= end; p += 16) {
        __m128i data = _mm_load_si128((__m128i*)p);
        
        // Проверяем границы ASCII
        __m128i too_low = _mm_cmplt_epi8(data, lower_bound);
        __m128i too_high = _mm_cmpgt_epi8(data, upper_bound);
        __m128i invalid = _mm_or_si128(too_low, too_high);
        
        if (_mm_movemask_epi8(invalid)) return 0;
        
        // Проверяем запрещенные символы (пробелы, табы, переводы строк)
        __m128i space = _mm_set1_epi8(' ');
        __m128i tab = _mm_set1_epi8('\t');
        __m128i lf = _mm_set1_epi8('\n');
        __m128i cr = _mm_set1_epi8('\r');
        
        __m128i has_space = _mm_cmpeq_epi8(data, space);
        __m128i has_tab = _mm_cmpeq_epi8(data, tab);
        __m128i has_lf = _mm_cmpeq_epi8(data, lf);
        __m128i has_cr = _mm_cmpeq_epi8(data, cr);
        
        __m128i forbidden = _mm_or_si128(_mm_or_si128(has_space, has_tab), 
                                       _mm_or_si128(has_lf, has_cr));
        
        if (_mm_movemask_epi8(forbidden)) return 0;
    }
    
    // Обрабатываем остаток
    for (const char* p = end - (end - aligned_start) % 16; p < end; p++) {
        char c = *p;
        if (c < 0x20 || c > 0x7E) return 0;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return 0;
    }
    
    return 1;
}

// Быстрый поиск конца HTTP заголовков (\r\n\r\n)
static inline const char* simd_find_header_end(const char* data, size_t len) {
    if (len < 4) return NULL;
    
    const char* end = data + len - 3;
    __m128i pattern = _mm_set_epi32(0x0A0D0A0D, 0x0A0D0A0D, 0x0A0D0A0D, 0x0A0D0A0D);
    
    for (const char* p = data; p <= end - 13; p += 16) {
        __m128i chunk = _mm_loadu_si128((__m128i*)p);
        
        // Ищем \r\n\r\n pattern
        for (int i = 0; i < 13; i++) {
            __m128i shifted = _mm_srli_si128(chunk, i);
            if (_mm_extract_epi32(shifted, 0) == 0x0A0D0A0D) {
                return p + i;
            }
        }
    }
    
    // Fallback для остатка
    for (const char* p = end - 13; p <= end; p++) {
        if (p[0] == '\r' && p[1] == '\n' && p[2] == '\r' && p[3] == '\n') {
            return p;
        }
    }
    
    return NULL;
}

#else

// Fallback реализации без SIMD
static inline const char* simd_find_char(const char* haystack, size_t len, char needle) {
    return memchr(haystack, needle, len);
}

static inline int simd_validate_url_chars(const char* url, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = url[i];
        if (c < 0x20 || c > 0x7E) return 0;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return 0;
    }
    return 1;
}

static inline const char* simd_find_header_end(const char* data, size_t len) {
    for (size_t i = 0; i < len - 3; i++) {
        if (data[i] == '\r' && data[i+1] == '\n' && 
            data[i+2] == '\r' && data[i+3] == '\n') {
            return &data[i];
        }
    }
    return NULL;
}

#endif // SIMD_AVAILABLE

// Утилиты для выравнивания памяти
#define CACHE_LINE_SIZE 64
#define ALIGN_TO_CACHE_LINE(x) (((x) + CACHE_LINE_SIZE - 1) & ~(CACHE_LINE_SIZE - 1))

// Prefetch макросы для лучшей работы с кэшем
#ifdef __GNUC__
#define PREFETCH_READ(addr) __builtin_prefetch((addr), 0, 3)
#define PREFETCH_WRITE(addr) __builtin_prefetch((addr), 1, 3)
#else
#define PREFETCH_READ(addr) 
#define PREFETCH_WRITE(addr)
#endif

// Likely/unlikely макросы для branch prediction
#ifdef __GNUC__
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#endif

#endif // SIMD_UTILS_H