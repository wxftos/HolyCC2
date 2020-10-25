#include <graphColoring.h>
#include <readersWritersLock.h>
#define DEBUG_PRINT_ENABLE 1
#include <debugPrint.h>
static int intCmp(const void *a, const void *b) {
	return *(int *)a - *(int *)b;
}
STR_TYPE_DEF(int, Int);
STR_TYPE_FUNCS(int, Int);
struct vertexInfo {
	struct __graphNode *node;
	int color;
	strGraphNodeP adjUncolored;
	strInt adjColors;
	struct rwLock *lock;
};
LL_TYPE_DEF(struct vertexPriority, Data);
LL_TYPE_FUNCS(struct vertexInfo, Data);
static int ptrPtrCmp(const void *a, const void *b) {
	if (*(void **)a > *(void **)b)
		return 1;
	else if (*(void **)a < *(void **)b)
		return -1;
	else
		return 0;
}
static int llVertexColorInsertCmp(const void *a, const void *b) {
	const struct vertexColoring *A = a, *B = b;
	return ptrPtrCmp(&A->node, &B->node);
}
static int llVertexColorGetCmp(const void *a, const void *b) {
	const struct vertexColoring *B = b;
	return ptrPtrCmp(&a, &B->node);
}
static int llDataGetCmp(const void *a, const void *b) {
	const struct vertexInfo *B = b;
	return ptrPtrCmp(&a, &B->node);
}
static int llDataInsertCmp(const void *a, const void *b) {
	const struct vertexInfo *A = a, *B = b;
	return ptrPtrCmp(&A->node, &B->node);
}
struct vertexColoring *llVertexColorGet(const llVertexColor data,
                                        const struct __graphNode *node) {
	return llVertexColorValuePtr(llVertexColorFindRight(
	    llVertexColorFirst(data), node, llVertexColorGetCmp));
}
static void visitNode(struct __graphNode *node, void *data) {
	strGraphNodeP *vec = data;
	if (NULL == strGraphNodePSortedFind(*vec, node, ptrPtrCmp))
		*vec = strGraphNodePSortedInsert(*vec, node, ptrPtrCmp);
}
static int alwaysTrue(const struct __graphNode *node,
                      const struct __graphEdge *edge, const void *data) {
	return 1;
}
static struct vertexInfo *llDataGet(const llData data,
                                    const struct __graphNode *node) {
	return llDataValuePtr(llDataFindRight(llDataFirst(data), node, llDataGetCmp));
}
// TODO switch data/item
struct __predPair {
	llData data;
	strGraphNodeP preds;
};
static strGraphNodeP adj(struct __graphNode *node) {
	__auto_type out = __graphNodeOutgoing(node);
	__auto_type in = __graphNodeIncoming(node);

	strGraphNodeP retVal = NULL;

	for (long i = 0; i != strGraphEdgePSize(out); i++)
		if (NULL ==
		    strGraphNodePSortedFind(retVal, __graphEdgeOutgoing(out[i]), ptrPtrCmp))
			retVal = strGraphNodePSortedInsert(retVal, __graphEdgeOutgoing(out[i]),
			                                   ptrPtrCmp);

	for (long i = 0; i != strGraphEdgePSize(in); i++)
		if (NULL ==
		    strGraphNodePSortedFind(retVal, __graphEdgeOutgoing(in[i]), ptrPtrCmp))
			retVal = strGraphNodePSortedInsert(retVal, __graphEdgeIncoming(in[i]),
			                                   ptrPtrCmp);

	strGraphEdgePDestroy(&out);
	strGraphEdgePDestroy(&in);
	return retVal;
}
static int removeIfNodeEq(const void *a, const void *b) {
	const struct __graphNode **A = (void *)a, **B = (void *)b;
	return (*A == *B);
}
llVertexColor graphColor(const struct __graphNode *node) {
	__auto_type allNodes =
	    strGraphNodePAppendItem(NULL, (struct __graphNode *)node);
	__graphNodeVisitForward((struct __graphNode *)node, &allNodes, alwaysTrue,
	                        visitNode);
	__graphNodeVisitBackward((struct __graphNode *)node, &allNodes, alwaysTrue,
	                         visitNode);
	__auto_type allNodesLen = strGraphNodePSize(allNodes);
	strGraphNodeP Q[allNodesLen][allNodesLen];
	for (long i = 0; i != allNodesLen; i++)
		for (long i2 = 0; i2 != allNodesLen; i2++)
			Q[i][i2] = NULL;

	llData datas = NULL;
	for (long i = 0; i != allNodesLen; i++) {
		struct vertexInfo tmp;
		tmp.node = allNodes[i];
		tmp.lock = rwLockCreate();
		tmp.color = -1;
		tmp.adjColors = NULL;
		tmp.adjUncolored = adj(allNodes[i]);

		DEBUG_PRINT("NODE: %i,has %li adjs\n",
		            *(int *)__graphNodeValuePtr(allNodes[i]),
		            strGraphNodePSize(tmp.adjUncolored));

		datas = llDataInsert(datas, llDataCreate(tmp), llDataInsertCmp);

		__auto_type ptr = &Q[0][strGraphNodePSize(tmp.adjUncolored)];
		*ptr = strGraphNodePAppendItem(*ptr, allNodes[i]);
	}

	int s = 0;
	while (s >= 0) {
		long maxLen = 0, maxIndex = 0;
		for (long i = 0; i != allNodesLen; i++) {
			__auto_type len = strGraphNodePSize(Q[s][i]);
			if (len > maxLen) {
				maxLen = len;
				maxIndex = i;
			}
		}

		// Pop
		__auto_type v = Q[s][maxIndex][maxLen - 1];
		Q[s][maxIndex] = strGraphNodePResize(Q[s][maxIndex], maxLen - 1);

		__auto_type vData = llDataGet(datas, v);

		strInt valids = strIntResize(NULL, strIntSize(vData->adjColors) + 1);
		for (long i = 0; i != strIntSize(valids); i++)
			valids[i] = i + 1;
		valids = strIntSetDifference(valids, vData->adjColors, intCmp);

		vData->color = valids[0];
		DEBUG_PRINT("NODE: %i,has color %i \n",
		            *(int *)__graphNodeValuePtr(vData->node), valids[0]);

		for (long i = 0; i != strGraphNodePSize(vData->adjUncolored); i++) {
			__auto_type uData = llDataGet(datas, vData->adjUncolored[i]);

			__auto_type ptr = &Q[strIntSize(uData->adjColors)]
			                    [strGraphNodePSize(uData->adjUncolored)];
			*ptr = strGraphNodePRemoveIf(*ptr, removeIfNodeEq, &uData->node);

			DEBUG_PRINT("NODE: %i removed at %li ,%li\n",
			            *(int *)__graphNodeValuePtr(uData->node),
			            strIntSize(uData->adjColors),
			            strGraphNodePSize(uData->adjUncolored));

			uData->adjColors =
			    strIntSortedInsert(uData->adjColors, vData->color, intCmp);
			uData->adjUncolored = strGraphNodePRemoveIf(uData->adjUncolored,
			                                            removeIfNodeEq, &vData->node);

			ptr = &Q[strIntSize(uData->adjColors)]
			        [strGraphNodePSize(uData->adjUncolored)];
			*ptr = strGraphNodePAppendItem(*ptr, uData->node);

			DEBUG_PRINT("NODE: %i inserted at %li ,%li\n",
			            *(int *)__graphNodeValuePtr(uData->node),
			            strIntSize(uData->adjColors),
			            strGraphNodePSize(uData->adjUncolored));

			long newS = strIntSize(uData->adjColors);
			s = (s > newS) ? s : newS;
		}
		while (s >= 0) {
			int isNull = 1;
			for (long i = 0; i != allNodesLen; i++) {
				if (strGraphNodePSize(Q[s][i]) != 0) {
					isNull = 0;
					break;
				}
			}
			if (isNull)
				s--;
			else
				break;
		}
	}

	llVertexColor colors = NULL;
	for (long i = 0; i != strGraphNodePSize(allNodes); i++) {
		struct vertexColoring coloring;
		coloring.color = llDataGet(datas, allNodes[i])->color;
		coloring.node = allNodes[i];

		colors = llVertexColorInsert(colors, llVertexColorCreate(coloring),
		                             llVertexColorInsertCmp);

		rwLockDestroy(llDataGet(datas, allNodes[i])->lock);
	}
	llDataDestroy(&datas, NULL);

	return colors;
}
long vertexColorCount(const llVertexColor colors) {
	__auto_type first = llVertexColorFirst(colors);
	strInt unique = NULL;
	for (__auto_type node = first; node != NULL; node = llVertexColorNext(node))
		if (NULL ==
		    strIntSortedFind(unique, llVertexColorValuePtr(node)->color, intCmp)) {
			unique = strIntSortedInsert(unique, llVertexColorValuePtr(node)->color,
			                            intCmp);
		}
	long count = strIntSize(unique);
	strIntDestroy(&unique);
	return count;
}