/* saugns: Generic array module.
 * Copyright (c) 2018-2020 Joel K. Pettersson
 * <joelkpettersson@gmail.com>.
 *
 * This file and the software of which it is part is distributed under the
 * terms of the GNU Lesser General Public License, either version 3 or (at
 * your option) any later version, WITHOUT ANY WARRANTY, not even of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * View the file COPYING for details, or if missing, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once
#include "common.h"

/*
 * Generic array meta-type. A given type is used for the elements,
 * macros being used to declare and define the concrete type.
 *
 * Each concrete type uses inline wrappers around generic methods.
 */

struct SAU_MemPool;

/**
 * Declare array type using \p Name, with \p ElementType.
 *
 * Only declares type, not methods. See sauArrType().
 */
#define sauArrTypeStruct(Name, ElementType) \
typedef struct Name { \
	ElementType *a; \
	size_t count; \
	size_t asize; \
} Name;

/**
 * Declare array methods for \p Name, with \p ElementType.
 *
 * Only declares methods, not type. See sauArrType().
 *
 * The Name_*() methods defined are inline wrappers around the
 * generic methods. If not blank, \p MethodPrefix will be used
 * to prefix their names.
 */
#define sauArrTypeMethods(Name, ElementType, MethodPrefix) \
static inline ElementType sauMaybeUnused \
*MethodPrefix##Name##_add(Name *restrict o, \
		const ElementType *restrict item) { \
	return SAU_ArrType_add(o, item, sizeof(ElementType)); \
} \
static inline bool sauMaybeUnused \
MethodPrefix##Name##_upsize(Name *restrict o, size_t count) { \
	return SAU_ArrType_upsize(o, count, sizeof(ElementType)); \
} \
static inline void sauMaybeUnused \
MethodPrefix##Name##_clear(Name *restrict o) { \
	SAU_ArrType_clear(o); \
} \
static inline bool sauMaybeUnused \
MethodPrefix##Name##_memdup(Name *restrict o, \
		const ElementType **restrict dst) { \
	return SAU_ArrType_memdup(o, (const void**) dst, sizeof(ElementType)); \
} \
static inline bool sauMaybeUnused \
MethodPrefix##Name##_mpmemdup(Name *restrict o, \
		const ElementType **restrict dst, \
		struct SAU_MemPool *restrict mempool) { \
	return SAU_ArrType_mpmemdup(o, (const void**) dst, \
		sizeof(ElementType), mempool); \
}

/**
 * Declare both type and methods for \p Name, with \p ElementType.
 *
 * Combines sauArrTypeStruct() and sauArrTypeMethods().
 *
 * The Name_*() methods defined are inline wrappers around the
 * generic methods. If not blank, \p MethodPrefix will be used
 * to prefix their names.
 */
#define sauArrType(Name, ElementType, MethodPrefix) \
sauArrTypeStruct(Name, ElementType) \
sauArrTypeMethods(Name, ElementType, MethodPrefix)

void *SAU_ArrType_add(void *restrict o,
		const void *restrict item, size_t item_size);
bool SAU_ArrType_upsize(void *restrict o,
		size_t count, size_t item_size);
void SAU_ArrType_clear(void *restrict o);
bool SAU_ArrType_memdup(void *restrict o,
		const void **restrict dst, size_t item_size);
bool SAU_ArrType_mpmemdup(void *restrict o,
		const void **restrict dst, size_t item_size,
		struct SAU_MemPool *mempool);

/** Byte (uint8_t) array type. */
sauArrType(SAU_ByteArr, uint8_t, )
