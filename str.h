#pragma once
struct __vec;
#include <string.h>
#define STR_TYPE_DEF(type, suffix) typedef type *str##suffix;
#define STR_TYPE_FUNCS(type, suffix)                                                                                                                               \
	/*__attribute__((always_inline))*/ inline void str##suffix##Destroy(str##suffix *vec) {                                                                              \
		__vecDestroy((struct __vec **)vec);                                                                                                                            \
	}                                                                                                                                                                \
	inline str##suffix str##suffix##AppendItem(str##suffix vec, type item) /*__attribute__((always_inline))*/;                                                           \
	inline str##suffix str##suffix##AppendItem(str##suffix vec, type item) {                                                                                         \
		return (str##suffix)__vecAppendItem((struct __vec *)vec, &item, sizeof(type));                                                                                 \
	}                                                                                                                                                                \
	inline str##suffix str##suffix##Reserve(str##suffix vec, long count) /*__attribute__((always_inline))*/;                                                             \
	inline str##suffix str##suffix##Reserve(str##suffix vec, long count) {                                                                                           \
		return (str##suffix)__vecReserve((struct __vec *)vec, count * sizeof(type));                                                                                   \
	}                                                                                                                                                                \
	inline long str##suffix##Size(const str##suffix vec) /*__attribute__((always_inline))*/;                                                                             \
	inline long str##suffix##Size(const str##suffix vec) {                                                                                                           \
		return __vecSize((struct __vec *)vec) / sizeof(type);                                                                                                          \
	}                                                                                                                                                                \
	inline long str##suffix##Capacity(const str##suffix vec) /*__attribute__((always_inline))*/;                                                                         \
	inline long str##suffix##Capacity(const str##suffix vec) {                                                                                                       \
		return __vecCapacity((struct __vec *)vec) / sizeof(type);                                                                                                      \
	}                                                                                                                                                                \
	inline str##suffix str##suffix##Concat(str##suffix vec, const str##suffix vec2) /*__attribute__((always_inline))*/;                                                  \
	inline str##suffix str##suffix##Concat(str##suffix vec, const str##suffix vec2) {                                                                                \
		return (str##suffix)__vecConcat((struct __vec *)vec, (struct __vec *)vec2);                                                                                    \
	}                                                                                                                                                                \
	inline str##suffix str##suffix##Resize(str##suffix vec, long size) /*__attribute__((always_inline))*/;                                                               \
	inline str##suffix str##suffix##Resize(str##suffix vec, long size) {                                                                                             \
		return (str##suffix)__vecResize((struct __vec *)vec, size * sizeof(type));                                                                                     \
	}                                                                                                                                                                \
	inline str##suffix str##suffix##SortedInsert(str##suffix vec, type item, int (*pred)(const type *, const type *)) /*__attribute__((always_inline))*/;                \
	inline str##suffix str##suffix##SortedInsert(str##suffix vec, type item, int (*pred)(const type *, const type *)) {                                              \
		return (str##suffix)__vecSortedInsert((struct __vec *)vec, &item, sizeof(item), (int (*)(const void *, const void *))pred);                                    \
	}                                                                                                                                                                \
	inline str##suffix str##suffix##AppendData(str##suffix vec, const type *data, long count) /*__attribute__((always_inline))*/;                                        \
	inline str##suffix str##suffix##AppendData(str##suffix vec, const type *data, long count) {                                                                      \
		__auto_type oldSize = __vecSize((struct __vec *)vec);                                                                                                          \
		vec = (str##suffix)__vecResize((struct __vec *)vec, oldSize + count * sizeof(type));                                                                           \
		memcpy(&((char *)vec)[oldSize], data, count * sizeof(type));                                                                                                   \
		return vec;                                                                                                                                                    \
	}                                                                                                                                                                \
	inline type *str##suffix##SortedFind(str##suffix vec, const type data, int pred(const type *, const type *)) /*__attribute__((always_inline))*/;                     \
	inline type *str##suffix##SortedFind(str##suffix vec, const type data, int pred(const type *, const type *)) {                                                   \
		return __vecSortedFind((struct __vec *)vec, &data, sizeof(type), (int (*)(const void *, const void *))pred);                                                   \
	}                                                                                                                                                                \
	inline str##suffix str##suffix##SetDifference(str##suffix vec, str##suffix vec2, int pred(const type *, const type *)) /*__attribute__((always_inline))*/;           \
	inline str##suffix str##suffix##SetDifference(str##suffix vec, str##suffix vec2, int pred(const type *, const type *)) {                                         \
		return (str##suffix)__vecSetDifference((struct __vec *)vec, (struct __vec *)vec2, sizeof(type), (int (*)(const void *, const void *))pred);                    \
	}                                                                                                                                                                \
	inline str##suffix str##suffix##RemoveIf(str##suffix vec, void const *data, int pred(const void *, const type *)) /*__attribute__((always_inline))*/;                \
	inline str##suffix str##suffix##RemoveIf(str##suffix vec, void const *data, int pred(const void *, const type *)) {                                              \
		return (str##suffix)__vecRemoveIf((struct __vec *)vec, sizeof(type), (int (*)(const void *, const void *))pred, data);                                         \
	}                                                                                                                                                                \
	inline str##suffix str##suffix##Unique(str##suffix str, int (*pred)(const type *, const type *), void (*kill)(void *)) /*__attribute__((always_inline))*/;           \
	inline str##suffix str##suffix##Unique(str##suffix str, int (*pred)(const type *, const type *), void (*kill)(void *)) {                                         \
		return (str##suffix)__vecUnique((struct __vec *)str, sizeof(type), (int (*)(const void *, const void *))pred, kill);                                           \
	}                                                                                                                                                                \
	inline str##suffix str##suffix##SetIntersection(str##suffix a, const str##suffix b, int (*pred)(const type *, const type *), void (*kill)(void *))               \
	    /*__attribute__((always_inline))*/;                                                                                                                              \
	inline str##suffix str##suffix##SetIntersection(str##suffix a, const str##suffix b, int (*pred)(const type *, const type *), void (*kill)(void *)) {             \
		return (str##suffix)__vecSetIntersection((struct __vec *)a, (struct __vec *)b, sizeof(type), (int (*)(const void *, const void *))pred, kill);                 \
	}                                                                                                                                                                \
	inline str##suffix str##suffix##SetUnion(str##suffix a, const str##suffix b, int (*pred)(const type *, const type *)) /*__attribute__((always_inline))*/;            \
	inline str##suffix str##suffix##SetUnion(str##suffix a, const str##suffix b, int (*pred)(const type *, const type *)) {                                          \
		return (str##suffix)__vecSetUnion((struct __vec *)a, (struct __vec *)b, sizeof(type), (int (*)(const void *, const void *))pred);                              \
	}                                                                                                                                                                \
	inline str##suffix str##suffix##Pop(str##suffix str, type *res) /*__attribute__((always_inline))*/;                                                                  \
	inline str##suffix str##suffix##Pop(str##suffix str, type *res) {                                                                                                \
		long size = str##suffix##Size(str);                                                                                                                            \
		if (res != NULL)                                                                                                                                               \
			*res = str[size - 1];                                                                                                                                        \
		return str##suffix##Resize(str, size - 1);                                                                                                                     \
	}                                                                                                                                                                \
	inline str##suffix str##suffix##Clone(const str##suffix str) /*__attribute__((always_inline))*/;                                                                     \
	inline str##suffix str##suffix##Clone(const str##suffix str) {                                                                                                   \
		return str##suffix##AppendData(NULL, (void *)str, str##suffix##Size(str));                                                                                     \
	}                                                                                                                                                                \
	/*__attribute__((always_inline))*/ inline str##suffix str##suffix##RemoveItem(str##suffix str, type item, int (*pred)(const type *, const type *)) {                 \
		return (str##suffix)__vecRemoveItem((struct __vec *)str, sizeof(item), &item, (int (*)(const void *, const void *))pred);                                      \
	}                                                                                                                                                                \
	/*__attribute__((always_inline))*/ inline str##suffix str##suffix##Reverse(str##suffix str) {                                                                        \
		return (str##suffix)__vecReverse((struct __vec *)str, sizeof(type));                                                                                           \
	 } \
	/*__attribute__((always_inline))*/ inline str##suffix str##suffix##Merge(str##suffix str,str##suffix str2,int (*pred)(const type *, const type *)) {	\
			return (str##suffix)__vecMerge((struct __vec *)str, (struct __vec *)str2,sizeof(type),(int(*)(const void *,const void*))pred);	\
	}
struct __vec *__vecAppendItem(struct __vec *a, const void *item, long itemSize);
struct __vec *__vecReserve(struct __vec *a, long capacity);
struct __vec *__vecConcat(struct __vec *a, const struct __vec *b);
long __vecCapacity(const struct __vec *a);
long __vecSize(const struct __vec *a);
struct __vec *__vecResize(struct __vec *a, long size);
struct __vec *__vecSortedInsert(struct __vec *a, const void *item, long itemSize, int predicate(const void *, const void *));
void *__vecSortedFind(const struct __vec *a, const void *item, long itemSize, int predicate(const void *, const void *));
struct __vec *__vecSetDifference(struct __vec *a, const struct __vec *b, long itemSize, int (*pred)(const void *, const void *));
struct __vec *__vecRemoveIf(struct __vec *a, long itemSize, int predicate(const void *, const void *), const void *data);
struct __vec *__vecUnique(struct __vec *vec, long itemSize, int (*pred)(const void *, const void *), void (*kill)(void *));
struct __vec *__vecSetIntersection(struct __vec *a, const struct __vec *b, long itemSize, int (*pred)(const void *, const void *), void (*kill)(void *));
struct __vec *__vecSetUnion(struct __vec *a, struct __vec *b, long itemSize, int (*pred)(const void *, const void *));
void __vecDestroy(struct __vec **vec);
struct __vec *__vecRemoveItem(struct __vec *str, long itemSize, const void *item, int (*pred)(const void *, const void *));
struct __vec *__vecReverse(struct __vec *str, long itemSize);
struct __vec* __vecMerge(struct __vec *a,struct __vec *b,long itemSize,int (*cmp)(const void *,const void *));
