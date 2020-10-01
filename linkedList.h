#pragma once
#include <stdlib.h>
struct __ll;
#define LL_TYPE_DEF(type, suffix) typedef struct __ll *ll##suffix;
#define LL_TYPE_FUNCS(type, suffix)                                            \
	inline ll##suffix ll##suffix##Create(type value)                             \
	    __attribute__((always_inline));                                          \
	inline ll##suffix ll##suffix##Create(type value) {                           \
		return __llCreate(&(value), sizeof(type));                                 \
	}                                                                            \
	inline ll##suffix ll##suffix##Empty() __attribute__((always_inline));        \
	inline ll##suffix ll##suffix##Empty() { return NULL; }                       \
	inline ll##suffix ll##suffix##Insert(ll##suffix from, ll##suffix toInsert,   \
	                                     int (*pred)(void *, void *))            \
	    __attribute__((always_inline));                                          \
	inline ll##suffix ll##suffix##Insert(ll##suffix from, ll##suffix toInsert,   \
	                                     int (*pred)(void *, void *)) {          \
		return (ll##suffix){__llInsert(from, toInsert, pred)};                     \
	}                                                                            \
	inline ll##suffix ll##suffix##Remove(ll##suffix from)                        \
	    __attribute__((always_inline));                                          \
	inline ll##suffix ll##suffix##Remove(ll##suffix from) {                      \
		return __llRemoveNode(from);                                               \
	}                                                                            \
	inline type *ll##suffix##ValuePtr(ll##suffix Node)                           \
	    __attribute__((always_inline));                                          \
	inline type *ll##suffix##ValuePtr(ll##suffix Node) {                         \
		return __llValuePtr(Node);                                                 \
	}                                                                            \
	inline ll##suffix ll##suffix##Next(ll##suffix Node)                          \
	    __attribute__((always_inline));                                          \
	inline ll##suffix ll##suffix##Next(ll##suffix Node) {                        \
		return __llNext(Node);                                                     \
	}                                                                            \
	inline ll##suffix ll##suffix##Prev(ll##suffix Node)                          \
	    __attribute__((always_inline));                                          \
	inline ll##suffix ll##suffix##Prev(ll##suffix Node) {                        \
		return (ll##suffix)__llPrev(Node);                                         \
	}                                                                            \
	inline void ll##suffix##Destroy(ll##suffix *node)                            \
	    __attribute__((always_inline));                                          \
	inline void ll##suffix##Destroy(ll##suffix *node) {                          \
		__llDestroy(*node, NULL);                                                  \
	}                                                                            \
	inline ll##suffix ll##suffix##FindLeft(ll##suffix *node, void *data,         \
	                                       int (*pred)(void *, void *))          \
	    __attribute__((always_inline));                                          \
	inline ll##suffix ll##suffix##FindLeft(ll##suffix *node, void *data,         \
	                                       int (*pred)(void *, void *)) {        \
		return (ll##suffix)__llFindLeft(*node, data, pred);                        \
	}                                                                            \
	inline ll##suffix ll##suffix##FindRight(ll##suffix *node, void *data,        \
	                                        int (*pred)(void *, void *))         \
	    __attribute__((always_inline));                                          \
	inline ll##suffix ll##suffix##FindRight(ll##suffix *node, void *data,        \
	                                        int (*pred)(void *, void *)) {       \
		return (ll##suffix)__llFindRight(*node, data, pred);                       \
	}                                                                            \
	inline ll##suffix ll##suffix##First(ll##suffix node)                         \
	    __attribute__((always_inline));                                          \
	inline ll##suffix ll##suffix##First(ll##suffix node) {                       \
		return (ll##suffix)__llGetFirst((ll##suffix)node);                         \
	}                                                                            \
	inline ll##suffix ll##suffix##Last(ll##suffix node)                          \
	    __attribute__((always_inline));                                          \
	inline ll##suffix ll##suffix##Last(ll##suffix node) {                        \
		return (ll##suffix)__llGetEnd((ll##suffix)node);                           \
	}                                                                            \
	inline long ll##suffix##Size(ll##suffix node)                                \
	    __attribute__((always_inline));                                          \
	inline long ll##suffix##Size(ll##suffix node) { return __llSize(node); }     \
	inline void ll##suffix##InsertListBefore(ll##suffix node, ll##suffix item)   \
	    __attribute__((always_inline));                                          \
	inline void ll##suffix##InsertListBefore(ll##suffix node, ll##suffix item) { \
		llInsertListBefore(node, item);                                            \
	}                                                                            \
	inline void ll##suffix##InsertListAfter(ll##suffix node, ll##suffix item)    \
	    __attribute__((always_inline));                                          \
	inline void ll##suffix##InsertListAfter(ll##suffix node, ll##suffix item) {  \
		llInsertListAfter(node, item);                                             \
	}
void __llDestroy(struct __ll *node, void (*killFunc)(void *));
void *__llValuePtr(struct __ll *node);
struct __ll *__llRemoveNode(struct __ll *node);
struct __ll *__llCreate(void *item, long size);
struct __ll *__llInsert(struct __ll *from, struct __ll *newItem,
                        int (*pred)(void *, void *));
int llLastPred(void *a, void *b);
int __llFirstPred(void *a, void *b);
struct __ll *__llPrev(struct __ll *node);
struct __ll *__llNext(struct __ll *node);
struct __ll *__llFindRight(struct __ll *list, void *data,
                           int (*pred)(void *a, void *b));
struct __ll *__llFindLeft(struct __ll *list, void *data,
                          int (*pred)(void *a, void *b));
struct __ll *__llGetEnd(struct __ll *list);
struct __ll *__llGetFirst(struct __ll *list);
long __llSize(struct __ll *list);
void llInsertListAfter(struct __ll *a, struct __ll *b);
void llInsertListBefore(struct __ll *a, struct __ll *b);
struct __ll *__llValueResize(struct __ll *list,long newSize);
