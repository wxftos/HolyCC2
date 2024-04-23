#include <stdlib.h>
#pragma once
struct __map;
#define MAP_TYPE_DEF(type, suffix) typedef struct __map *map##suffix;
#define MAP_TYPE_FUNCS(type, suffix)                                                                                                                               \
	inline map##suffix map##suffix##Create() __attribute__((always_inline));                                                                                         \
	inline map##suffix map##suffix##Create() {                                                                                                                       \
		return (map##suffix)__mapCreate();                                                                                                                             \
	};                                                                                                                                                               \
	inline int map##suffix##Insert(map##suffix map, const char *key, type item) __attribute__((always_inline));                                                      \
	inline int map##suffix##Insert(map##suffix map, const char *key, type item) {                                                                                    \
		return __mapInsert(map, key, &item, sizeof(type));                                                                                                             \
	};                                                                                                                                                               \
	inline type *map##suffix##Get(const map##suffix map, const char *key) __attribute__((always_inline));                                                            \
	inline type *map##suffix##Get(const map##suffix map, const char *key) {                                                                                          \
		return __mapGet(map, key);                                                                                                                                     \
	};                                                                                                                                                               \
	inline void map##suffix##Remove(map##suffix map, const char *key, void (*kill)(void *)) __attribute__((always_inline));                                          \
	inline void map##suffix##Remove(map##suffix map, const char *key, void (*kill)(void *)) {                                                                        \
		 __mapRemove(map, key, kill);                                                                                                                            \
	}                                                                                                                                                                \
	inline void map##suffix##Destroy(map##suffix map, void (*kill)(void *)) __attribute__((always_inline));                                                          \
	inline void map##suffix##Destroy(map##suffix map, void (*kill)(void *)) {                                                                                        \
		__mapDestroy(map, kill);                                                                                                                                       \
	}                                                                                                                                                                \
	inline const char *map##suffix##ValueKey(const void *value) __attribute__((always_inline));                                                                      \
	inline const char *map##suffix##ValueKey(const void *value) {                                                                                                    \
		return __mapKeyByPtr(value);                                                                                                                                   \
	}                                                                                                                                                                \
	inline map##suffix map##suffix##Clone(const map##suffix toClone, void (*cloneData)(void *, const void *)) __attribute__((always_inline));                        \
	inline map##suffix map##suffix##Clone(const map##suffix toClone, void (*cloneData)(void *, const void *)) {                                                      \
		return (map##suffix)__mapClone((struct __map *)toClone, cloneData, sizeof(type));                                                                              \
	}                                                                                                                                                                \
	inline void map##suffix##Keys(const map##suffix map, const char **dumpTo, long *count) __attribute__((always_inline));                                           \
	inline void map##suffix##Keys(const map##suffix map, const char **dumpTo, long *count) {                                                                         \
		__mapKeys((struct __map *)map, dumpTo, count);                                                                                                                 \
	}
void __mapDestroy(struct __map *map, void (*kill)(void *));
int __mapInsert(struct __map *map, const char *key, const void *item, const long itemSize);
void *__mapGet(const struct __map *map, const char *key);
struct __map *__mapCreate();
void __mapRemove(struct __map *map, const char *key, void (*kill)(void *));
const char *__mapKeyByPtr(const void *valuePtr);
struct __map *__mapClone(struct __map *map, void (*cloneData)(void *, const void *), long itemSize);
void __mapKeys(const struct __map *map, const char **dumpTo, long *count);
