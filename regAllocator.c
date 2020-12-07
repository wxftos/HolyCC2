#include <IRLiveness.h>
#include <SSA.h>
#include <assert.h>
#include <graphColoring.h>
#include <subExprElim.h>
#define DEBUG_PRINT_ENABLE 1
#include <debugPrint.h>
#include <graphDominance.h>
#include <base64.h>
#include <lambda.h>
static char *ptr2Str(const void *a) {
		return base64Enc((void*)&a, sizeof(a));
}
static int IRVarCmp2(const struct IRVar **a,const struct IRVar **b) {
		return IRVarCmp(*a, *b);
}
static char *var2Str(graphNodeIR var) {
		if(debugGetPtrNameConst(var))
				return debugGetPtrName(var);
		
		__auto_type value=(struct IRNodeValue*)graphNodeIRValuePtr(var);
		char buffer[1024];
		sprintf(buffer, "%s-%li", value->val.value.var.value.var->name,value->val.value.var.SSANum);
		char *retVal=malloc(strlen(buffer)+1);
		strcpy(retVal, buffer);

		return retVal;
}
static char *strClone(const char *text) {
		char *retVal=malloc(strlen(text)+1);
		strcpy(retVal, text);

		return retVal;
}
static void debugShowGraphIR(graphNodeIR enter) {
		const char *name=tmpnam(NULL);
		__auto_type map=graphNodeCreateMapping(enter, 1);
		IRGraphMap2GraphViz(map, "viz", name, NULL,NULL,NULL,NULL);
		char buffer[1024];
		sprintf(buffer, "sleep 0.1 &&dot -Tsvg %s > /tmp/dot.svg && firefox /tmp/dot.svg & ", name);

		system(buffer);
}
MAP_TYPE_DEF(struct regSlice,RegSlice);
MAP_TYPE_FUNCS(struct regSlice,RegSlice);
static char *interfereNode2Label(const struct __graphNode * node, mapGraphVizAttr *attrs, const void *data) {
		mapRegSlice map=(void*)data;
		
		__auto_type var=graphNodeIRLiveValuePtr((graphNodeIRLive)node)->ref;
		char *name=debugGetPtrName(var);
		if(name)
				name=name;
		else if(var->value.var->name)
				name=strClone(var->value.var->name);
		else
				name= ptr2Str(var);

		__auto_type key=ptr2Str(node);
		if(map)
		if(mapRegSliceGet(map, key)) {
				const char *format="%s(%s)";
				long len=snprintf(NULL,0, format,name, mapRegSliceGet(map, key)->reg->name);
				char buffer[len+1];
				sprintf(buffer,format,name, mapRegSliceGet(map, key)->reg->name);

				free(name);
				return strClone(buffer);
		}

		free(key);

		return name;
}
static void debugPrintInterferenceGraph(graphNodeIRLive graph,mapRegSlice map) {
		char *fn=tmpnam(NULL);
		FILE *file=fopen(fn, "w");
		graph2GraphVizUndir(file, graph, "interference",interfereNode2Label, map);
		fclose(file);

		char buffer[512];
		sprintf(buffer, "sleep 0.1 && dot -Tsvg %s>/tmp/interfere.svg  && firefox /tmp/interfere.svg &", fn);
		system(buffer);
} 
static const char *IR_ATTR_NODE_SPILL_LOADS_COMPUTED="COMPUTED_SPILLS_AND_LOADS";
static const char *IR_ATTR_NODE_VARS_SPILLED_AT="SPILLED_VAR_AT";
static const char *IR_ATTR_NODE_LOAD_VAR_AT="LOADED_VAR_AT";
STR_TYPE_DEF(struct IRVar*,IRVar);
STR_TYPE_FUNCS(struct IRVar*,IRVar);
struct IRAttrSpilledVarsAt {
		struct IRAttr base;
		strIRVar vars;
};
typedef int (*gnCmpType)(const graphNodeIR *, const graphNodeIR *);
static int ptrPtrCmp(const void *a, const void *b) {
	if (*(void **)a > *(void **)b)
		return 1;
	else if (*(void **)a < *(void **)b)
		return -1;
	else
		return 0;
}
static void transparentKill(graphNodeIR node) {
	__auto_type incoming = graphNodeIRIncoming(node);
	__auto_type outgoing = graphNodeIROutgoing(node);
	for (long i1 = 0; i1 != strGraphEdgeIRPSize(incoming); i1++)
		for (long i2 = 0; i2 != strGraphEdgeIRPSize(outgoing); i2++)
			graphNodeIRConnect(graphEdgeIRIncoming(incoming[i1]),
			                   graphEdgeIROutgoing(outgoing[i2]),
			                   *graphEdgeIRValuePtr(incoming[i1]));

	graphNodeIRKill(&node, IRNodeDestroy, NULL);
}
static int noIncomingPred(const void *data, const graphNodeIR *node) {
	strGraphEdgeIRP incoming __attribute__((cleanup(strGraphEdgePDestroy))) =
	    graphNodeIRIncoming(*node);
	return strGraphEdgeIRPSize(incoming) != 0;
};
static void replaceNodeWithExpr(graphNodeIR node, graphNodeIR valueSink) {
	__auto_type incoming = graphNodeIRIncoming(node);
	__auto_type outgoing = graphNodeIROutgoing(node);

	// Find input nodes(with no incoming)
	__auto_type sources = graphNodeIRAllNodes(valueSink);
	sources = strGraphNodeIRPRemoveIf(graphNodeIRAllNodes(valueSink), NULL,
	                                  noIncomingPred);

	// Connnect incoming to sources
	for (long i1 = 0; i1 != strGraphEdgeIRPSize(incoming); i1++)
		for (long i2 = 0; i2 != strGraphNodeIRPSize(sources); i2++)
			graphNodeIRConnect(graphEdgeIRIncoming(incoming[i1]), sources[i2],
			                   *graphEdgeIRValuePtr(incoming[i1]));

	// Connect sink to outgoing
	for (long i1 = 0; i1 != strGraphEdgeIRPSize(outgoing); i1++)
		graphNodeIRConnect(valueSink, graphEdgeIROutgoing(outgoing[i1]),
		                   *graphEdgeIRValuePtr(outgoing[i1]));

	strGraphEdgeIRPDestroy(&incoming), strGraphEdgeIRPDestroy(&outgoing);
	graphNodeIRKill(&node, IRNodeDestroy, NULL);
}
static int gnIRVarCmp(const graphNodeIR *a, const graphNodeIR *b) {
	__auto_type A = (struct IRNodeValue *)graphNodeIRValuePtr(*a);
	__auto_type B = (struct IRNodeValue *)graphNodeIRValuePtr(*b);
	assert(A->base.type == IR_VALUE);
	assert(B->base.type == IR_VALUE);

	return IRVarCmp(&A->val.value.var, &B->val.value.var);
}
static void removeChooseNode(graphNodeIR chooseNode) {
	// Kill node,remove outgoing nodes too if they variable they point to is
	// unused
	__auto_type outgoingNodes = graphNodeIROutgoingNodes(chooseNode);

	// Remove unused result vars
	for (long i2 = 0; i2 != strGraphNodeIRPSize(outgoingNodes); i2++) {
		__auto_type outgoingEdges = graphNodeIROutgoing(outgoingNodes[i2]);
		__auto_type filtered = IRGetConnsOfType(outgoingEdges, IR_CONN_FLOW);

		// If only flow out,then no need for variable
		int useless =
		    strGraphEdgeIRPSize(filtered) == strGraphEdgeIRPSize(outgoingEdges);

		strGraphEdgeIRPDestroy(&outgoingEdges);
		strGraphEdgeIRPDestroy(&filtered);

		// Remove var if useless
		if (useless)
			transparentKill(outgoingNodes[i2]);
	}

	strGraphNodeIRPDestroy(&outgoingNodes);
	transparentKill(chooseNode);
}
static void removeChooseNodes(strGraphNodeIRP nodes, graphNodeIR start) {
	for (long i = 0; i != strGraphNodeIRPSize(nodes); i++) {
		__auto_type val = graphNodeIRValuePtr(nodes[i]);
		// No value?
		if (!val)
			continue;

		// Ignore non-chooses
		if (val->type != IR_CHOOSE)
			continue;

		removeChooseNode(nodes[i]);
	}
}
STR_TYPE_DEF(strGraphNodeIRP, VarRefs);
STR_TYPE_FUNCS(strGraphNodeIRP, VarRefs);
struct aliasPair {
	graphNodeIR a, b;
};
static int aliasPairCmp(const struct aliasPair *a, const struct aliasPair *b) {
	struct IRNodeValue *Aa = (void *)graphNodeIRValuePtr(a->a);
	struct IRNodeValue *Ba = (void *)graphNodeIRValuePtr(b->a);
	struct IRNodeValue *Ab = (void *)graphNodeIRValuePtr(a->b);
	struct IRNodeValue *Bb = (void *)graphNodeIRValuePtr(b->b);
	int cmpA = IRVarCmp(&Aa->val.value.var, &Ba->val.value.var);
	if (cmpA != 0)
		return cmpA;

	int cmpB = IRVarCmp(&Ab->val.value.var, &Bb->val.value.var);
	if (cmpB != 0)
		return cmpB;

	return 0;
}
static int varRefsGetCmp(const strGraphNodeIRP *a, const strGraphNodeP *b) {
		long aSize=strGraphNodeIRPSize(*a);
		long bSize=strGraphNodeIRPSize(*b);
		long minSize=(aSize<bSize)?aSize:bSize;

		for(long i=0;i!=minSize;i++) {
				int cmp=ptrPtrCmp(a+i, b+i);
				if(cmp!=0)
						return cmp;
		}

		if(aSize>bSize)
				return 1;
		if(aSize<bSize)
				return -1;
		else
				return 0;
}
STR_TYPE_DEF(struct aliasPair, AliasPair);
STR_TYPE_FUNCS(struct aliasPair, AliasPair);
STR_TYPE_DEF(strGraphNodeIRP, AliasBlob);
STR_TYPE_FUNCS(strGraphNodeIRP, AliasBlob);
static int AliasBlobCmp(const strGraphNodeIRP *A, const strGraphNodeIRP *B) {
	long aSize = strGraphNodeIRPSize(*A);
	long bSize = strGraphNodeIRPSize(*B);
	long max = (aSize > bSize) ? aSize : bSize;

	// Check for difference in shared portion
	for (long i = 0; i != max; i++) {
		if (ptrPtrCmp(&A[i], &B[i]))
			return ptrPtrCmp(&A[i], &B[i]);
	}

	// Now compare by sizes
	if (aSize == bSize)
		return 0;
	else if (aSize > bSize)
		return 1;
	else if (aSize < bSize)
		return -1;

	return 0;
}
static int getVarRefIndex(strVarRefs refs,graphNodeIR node) {
		for(long i=0;i!=strVarRefsSize(refs);i++) {
				if(strGraphNodeIRPSortedFind(refs[i], node, (gnCmpType)ptrPtrCmp))
						return i;
		}

		printf("Dear programmer,variable node %s not found in refs\n",debugGetPtrNameConst(node));
		assert(0);
		return -1;
}
void IRCoalesce(strGraphNodeIRP nodes, graphNodeIR start) {
		strVarRefs refs = NULL;
		strAliasPair aliases = NULL;
		for (long i = 0; i != strGraphNodeIRPSize(nodes); i++) {
				__auto_type val = graphNodeIRValuePtr(nodes[i]);
				// No value?
				if (!val)
						continue;

				if (val->type == IR_VALUE) {
						// Is a var,check for direct assign
						if (((struct IRNodeValue *)val)->val.type == IR_VAL_VAR_REF) {
						findVarLoop:;
								strGraphNodeIRLiveP *find=NULL;
								//An an eqivalent variable in refs
								for(long i2=0;i2!=strVarRefsSize(refs);i2++) {
										for(long i3=0;i3!=strGraphNodeIRPSize(refs[i2]);i3++) {
												__auto_type a=(struct IRNodeValue *)val;
												__auto_type b=(struct IRNodeValue *)graphNodeIRValuePtr(refs[i2][i3]);
												if(0==IRVarCmp(&a->val.value.var, &b->val.value.var )) {
														find=&refs[i2];
														goto findVarLoopEnd;
												}
										}
								}
						findVarLoopEnd:

								if (!find) {
										// Add a vec of references(only reference is nodes[i])
										DEBUG_PRINT("Adding var node %s\n", var2Str(nodes[i]));

										__auto_type tmp=strGraphNodeIRPAppendItem(NULL, nodes[i]);
										refs = strVarRefsAppendItem(refs, tmp);
								} else {
										DEBUG_PRINT("Adding existing var to ref %s\n", var2Str(nodes[i]));

										// Add nodes[i] to references
										__auto_type vec = find;
										*vec =
												strGraphNodeIRPSortedInsert(*vec, nodes[i], (gnCmpType)ptrPtrCmp);
								}
						}
				}
		}
				
		for (long i = 0; i != strGraphNodeIRPSize(nodes); i++) {
				struct IRNodeValue *val=(void*)graphNodeIRValuePtr(nodes[i]);
				if(val->base.type!=IR_VALUE)
						continue;
				if(val->val.type!=IR_VAL_VAR_REF)
						continue;
						
				// Check if written into by another variable.
				__auto_type incoming = graphNodeIRIncoming(nodes[i]);
				__auto_type filtered = IRGetConnsOfType(incoming, IR_CONN_DEST);

				// aliasNode is NULL if an alias node isnt found
				graphNodeIR aliasNode = NULL;

				// If one filtered that is a value and a variable,set that as the alias
				// node
				if (strGraphEdgeIRPSize(filtered) == 1) {
						__auto_type inNode =
								graphNodeIRValuePtr(graphEdgeIRIncoming(filtered[0]));
						if (inNode->type == IR_VALUE) {
								struct IRNodeValue *inValueNode = (void *)inNode;
								if (inValueNode->val.type == IR_VAL_VAR_REF) {
										aliasNode = graphEdgeIRIncoming(filtered[0]);
										DEBUG_PRINT("%s aliased to node %s\n",
																						var2Str(aliasNode),
																						var2Str(nodes[i]));

										//Union
										__auto_type aIndex=getVarRefIndex(refs,aliasNode);
										__auto_type bIndex=getVarRefIndex(refs,nodes[i]);

										//
										// Ignore alias to own blob as we delete such blob ahead
										//
										if(aIndex==bIndex)
												continue;
										
										refs[aIndex]=strGraphNodeIRPSetUnion(refs[aIndex], refs[bIndex], (gnCmpType)ptrPtrCmp);


										//Remove bIndex
										memmove(&refs[bIndex], &refs[bIndex+1], (strVarRefsSize(refs)-bIndex-1)*sizeof(*refs));
										//Pop to decrement size
										refs=strVarRefsPop(refs, NULL);
								}
						}
				}

				strGraphEdgeIRPDestroy(&incoming), strGraphEdgeIRPDestroy(&filtered);
		}

		//
		// Replace vars with aliases
		//
		for (long i = 0; i != strVarRefsSize(refs); i++) {
				// Find first ref.
				__auto_type master = refs[i][0];

				// Replace rest of blobs
				for (long i2 = 0; i2 != strGraphNodeIRPSize(refs[i]); i2++) {
						// Dont replace self
 						if (refs[i][i2] == master)
								continue;

						DEBUG_PRINT("Replacing %s with %s\n", var2Str(refs[i][i2]),
																		var2Str(master));
						// Replace with cloned value
						replaceNodeWithExpr(refs[i][i2], cloneNode(master, IR_CLONE_NODE, NULL));
				}
		}
}
static int isVar(graphNodeIR node) {
	if (graphNodeIRValuePtr(node)->type == IR_VALUE) {
		// Is a variable
		struct IRNodeValue *valOut = (void *)graphNodeIRValuePtr(node);
		if (valOut->val.type == IR_VAL_VAR_REF) {
			return 1;
		}
	}
	return 0;
}
void IRRemoveRepeatAssigns(graphNodeIR enter) {
	__auto_type allNodes = graphNodeIRAllNodes(enter);

	strGraphNodeIRP toRemove = NULL;
	for (long i = 0; i != strGraphNodeIRPSize(allNodes); i++) {
		// If not a var,continue
		if (!isVar(allNodes[i]))
			continue;

		// Check for assign
		__auto_type outgoing = graphNodeIROutgoing(allNodes[i]);
		__auto_type outgoingAssign = IRGetConnsOfType(outgoing, IR_CONN_DEST);
		if (strGraphEdgeIRPSize(outgoingAssign) == 1) {
			// Check if assigned to value
			__auto_type out = graphEdgeIROutgoing(outgoingAssign[0]);
			if (isVar(out)) {
				// Compare if vars are equal
				struct IRNodeValue *valIn, *valOut;
				valIn = (void *)graphNodeIRValuePtr(allNodes[i]);
				valOut = (void *)graphNodeIRValuePtr(out);

				if (0 == IRVarCmp(&valIn->val.value.var, &valOut->val.value.var)) {
					// Add first reference to variable(valIn) for removal
					toRemove = strGraphNodeIRPAppendItem(toRemove, allNodes[i]);
				}
			}

			strGraphEdgeIRPDestroy(&outgoingAssign);
			strGraphEdgeIRPDestroy(&outgoing);
		}
	}

	//(Transparently) remove all items marked for removal
	for (long i = 0; i != strGraphNodeIRPSize(toRemove); i++)
		transparentKill(toRemove[i]);

	// Remove removed
	allNodes =
	    strGraphNodeIRPSetDifference(allNodes, toRemove, (gnCmpType)ptrPtrCmp);
	
	
	// Find duds(nodes that arent connected to conditionals or expressions)
	strGraphNodeIRP duds = NULL;
	for (long i = 0; i != strGraphNodeIRPSize(allNodes); i++) {
		if (!isVar(allNodes[i]))
			continue;

		__auto_type incoming = graphNodeIRIncoming(allNodes[i]);
		__auto_type outgoing = graphNodeIROutgoing(allNodes[i]);

		// Check if exprssion incoming
		int isUsedIn = 0;
		for (long i = 0; i != strGraphEdgeIRPSize(incoming); i++) {
			if (*graphEdgeIRValuePtr(incoming[i]) != IR_CONN_FLOW) {
				isUsedIn = 1;
				break;
			}
		}

		// Check if expression outgoing,OR IF CONNECTED TO CONDTIONAL
		int isUsedOut = 0;
		for (long i = 0; i != strGraphEdgeIRPSize(outgoing); i++) {
			if (*graphEdgeIRValuePtr(outgoing[i]) != IR_CONN_FLOW) {
				isUsedOut = 1;
				break;
			}
		}

		// Free
		strGraphEdgeIRPDestroy(&incoming), strGraphEdgeIRPDestroy(&outgoing);

		// IF only flows incoimg/outgoing,node is useless to replace
		if (!isUsedIn  && !isUsedOut) {
			duds = strGraphNodeIRPAppendItem(duds, allNodes[i]);
		}
	}

	// Kill duds
	for (long i = 0; i != strGraphNodeIRPSize(duds); i++) {
		transparentKill(duds[i]);
	}

	strGraphNodeIRPDestroy(&duds);
	strGraphNodeIRPDestroy(&allNodes);
	strGraphNodeIRPDestroy(&toRemove);
}
static int IsInteger(struct object *obj) {
	// Check if ptr
	if (obj->type == TYPE_PTR||obj->type==TYPE_ARRAY)
		return 1;

	// Check if integer type
	struct object *valids[] = {
	    &typeI8i,  &typeI16i, &typeI32i, &typeI64i, &typeU8i,
	    &typeU16i, &typeU32i, &typeU64i, &typeBool,
	};
	for (long i = 0; i != sizeof(valids) / sizeof(*valids); i++)
		if (valids[i] == obj)
			return 1;

	return 0;
}
static int isFloating(struct object *obj) {
		return obj==&typeF64;
}
static double interfereMetric(double cost,graphNodeIRLive node) {
		//
		// graphNodeIRLive is undirected node!!!
		//
		__auto_type connections=graphNodeIRLiveOutgoing(node);
		double retVal=cost/strGraphEdgeIRLivePSize(connections);

		strGraphEdgeIRLivePDestroy(&connections);
		return retVal;
}
struct conflictPair {
		graphNodeIRLive a;
		graphNodeIRLive b;
		double aWeight;
		double bWeight;
};
static int conflictPairWeightCmp(const void *a,const void *b) {
		const struct conflictPair *A=a,*B=b;
		
		double minWeightA=(A->aWeight<A->bWeight)?A->aWeight:A->bWeight;
		double minWeightB=(B->aWeight<B->bWeight)?B->aWeight:B->bWeight;

		if(minWeightA>minWeightB)
				return 1;
		else if(minWeightA<minWeightB)
				return -1;
		return 0;
}
static int conflictPairCmp(const struct conflictPair *a,const struct conflictPair *b) {
		int cmp=ptrPtrCmp(&a->a, &b->a);
		if(cmp!=0)
				return cmp;

		return ptrPtrCmp(&a->b, &b->b);
}
STR_TYPE_DEF(struct conflictPair,ConflictPair);
STR_TYPE_FUNCS(struct conflictPair,ConflictPair);
static strConflictPair recolorAdjacentNodes(mapRegSlice node2RegSlice,graphNodeIRLive node) {
		__auto_type allNodes=graphNodeIRLiveAllNodes(node);
		
		strConflictPair conflicts=NULL;
		for(long i=0;i!=strGraphNodeIRLivePSize(allNodes);i++) {
				__auto_type outgoingNodes=graphNodeIRLiveOutgoingNodes(allNodes[i]);

				//Get current register
				char *key=ptr2Str(allNodes[i]);
				__auto_type find=mapRegSliceGet(node2RegSlice, key);
				free(key);
				if(!find) {
				whineNotColored:
						printf("Dear programmer,try assigning registers to nodes before calling %s", __FUNCTION__);
						abort();
				}

				__auto_type masterSlice=*find;

				//Check for conflicting adjacent items
				for(long i2=0;i2!=strGraphNodeIRLivePSize(outgoingNodes);i2++) {
						char *key=ptr2Str(outgoingNodes[i2]);
						__auto_type find=mapRegSliceGet(node2RegSlice, key);
						//Not a register node
						if(!find)
								continue;

						//If conflict
						if(regSliceConflict(&masterSlice, find)) {
								struct conflictPair pair={allNodes[i],outgoingNodes[i2]};

								//Check if exists if the pair is reverses,there is no need for duplicates
								struct conflictPair backwards={outgoingNodes[i2],allNodes[i]};
								if(NULL!=strConflictPairSortedFind(conflicts, backwards, conflictPairCmp))
										continue;
								
								//Check if exists,if so dont insert
								if(NULL==strConflictPairSortedFind(conflicts, pair, conflictPairCmp)) {
										double aWeight=interfereMetric(1.1, allNodes[i]); //TODO implememnt cost
										double bWeight=interfereMetric(1.1, outgoingNodes[i2]);
										pair.aWeight=aWeight;
										pair.bWeight=bWeight;
#if DEBUG_PRINT_ENABLE
										char *aName=interfereNode2Label(allNodes[i], NULL, NULL);
										char *bName=interfereNode2Label(outgoingNodes[i2], NULL, NULL);
										
										DEBUG_PRINT("Adding [%s,%s] to conflicts\n", aName,bName);
										free(aName),free(bName);
#endif
										
										conflicts=strConflictPairSortedInsert(conflicts, pair,conflictPairCmp);
								}
						}
				}

				strGraphNodeIRLivePDestroy(&outgoingNodes);
		}
		
		strGraphNodeIRLivePDestroy(&allNodes);

		return conflicts;
}
static int filterIntVars(graphNodeIR node,const void *data) {
		struct IRNodeValue *value=(void*)graphNodeIRValuePtr(node);
		if(value->base.type!=IR_VALUE)
				return 0;
		
		return IsInteger(IRValueGetType(&value->val));
}
static int filterFloatVars(graphNodeIR node,const void *data) {
		struct IRNodeValue *value=(void*)graphNodeIRValuePtr(node);
		if(value->base.type!=IR_VALUE)
				return 0;

		return isFloating(IRValueGetType(&value->val));
}
static int isVarNode(const struct IRNode *irNode) {
	if (irNode->type == IR_VALUE) {
		struct IRNodeValue *val = (void *)irNode;
		if (val->val.type == IR_VAL_VAR_REF)
			return 1;
	}

	return 0;
}
STR_TYPE_DEF(int,Int);
STR_TYPE_FUNCS(int,Int);
static int intCmp(const int *a,const int *b) {
		return *a-*b;
}
strInt getColorList(llVertexColor vertexColors) {
		strInt retVal=NULL;
		for(__auto_type node=llVertexColorFirst(vertexColors);node!=NULL;node=llVertexColorNext(node)) {
				//Insert into retVal if color isnt already in retVal
				if(NULL==strIntSortedFind(retVal, llVertexColorValuePtr(node)->color, intCmp))
						retVal=strIntSortedInsert(retVal, llVertexColorValuePtr(node)->color, intCmp);
		}

		return retVal;
}
struct metricPair {
		graphNodeIRLive node;
		double metricValue;
};
static int metricPairCmp(const void *a,const void *b) {
		const struct metricPair *A=a,*B=b;
		if(A->metricValue>B->metricValue)
				return 1;
		else if(A->metricValue<B->metricValue)
				return -1;
		else
				return 0;
}
struct interferencePair {
		struct IRVar *var;
		strIRVar inteferesWith;
};
static int isVarsChooseNode(const struct __graphNode *node,const struct interferencePair *pair) {
		//Check if hit a choose node that "consumes" pair->var. Chooses mark the end of one version of a var and a transition to the next version
		struct IRNode *value=graphNodeIRValuePtr((graphNodeIR)node);
		if(value->type==IR_CHOOSE) {
				struct IRNodeChoose *choose=(void*)value;
				for(long i2=0;i2!=strGraphNodeIRPSize(choose->canidates);i2++) {
						assert(isVar(choose->canidates[i2]));
						struct IRNodeValue *val=(void*)graphNodeIRValuePtr(choose->canidates[i2]);
						if(0==IRVarCmp(&val->val.value.var,pair->var)) {
								return 1;
						}
				}
		}

		return 0;
}
static int spillOrStoreAt(const struct __graphNode *node,const struct interferencePair *pair) {
		//Check if hit a choose node that "consumes" pair->var. Chooses mark the end of one version of a var and a transition to the next version
		struct IRNode *value=graphNodeIRValuePtr((graphNodeIR)node);
		if(isVarsChooseNode(node, pair))
				return 1;

		//Check if hit a varaible that intereferes with pair->var
		if(isVar((graphNodeIR)node)) {
				strIRVar vars2=pair->inteferesWith;
				for(long i=0;i!=strIRVarSize(vars2);i++) {
						struct IRNodeValue *val=(void*)graphNodeIRValuePtr((graphNodeIR)node);
						return 0==IRVarCmp(&val->val.value.var, vars2[i]);
				}
		}

		return 0;
}
static int graphPathCmp(const strGraphEdgeIRP *a,const strGraphEdgeIRP *b) {
		long aSize=strGraphEdgeIRPSize(*a);
		long bSize=strGraphEdgeIRPSize(*b);
		long min=(aSize<bSize)?aSize:bSize;

		for(long i=0;i!=min;i++) {
				__auto_type aNode=graphEdgeIROutgoing(a[0][i]);
				__auto_type bNode=graphEdgeIROutgoing(b[0][i]);
				int cmp=ptrPtrCmp(&aNode,&bNode);
				if(cmp!=0)
						return cmp;
		}

		if(aSize>bSize)
				return 1;
		else if(aSize<bSize)
				return -1;
		else
				return 0;
}

static void replaceVarWithLoad(struct __graphNode *node ,void * data) {
		strIRVar vars=data;
		
		if(isVar(node))  {
				__auto_type nodeVal=(struct IRNodeValue*)graphNodeIRValuePtr(node);
				for(long i=0;i!=strIRVarSize(vars);i++) {
						__auto_type var=vars[i];
						
						if(0==IRVarCmp(var, &nodeVal->val.value.var)) {
								__auto_type load=createLoad(var);
								replaceNodeWithExpr(node, load);
						}
				}
		}
}
static int untillStartOfExpr(const struct __graphNode* node,const struct __graphEdge* edge,const void *data) {
				return IRIsExprEdge(*graphEdgeIRValuePtr((graphEdgeIR)edge));
};

static void insertLoadsInExpression(graphNodeIR expressionNode,strIRVar varsToReplace) {
		strGraphNodeIRP  references=NULL;
		
		__auto_type end=IRGetEndOfExpr(expressionNode);
		graphNodeIRVisitBackward(end, varsToReplace, untillStartOfExpr, replaceVarWithLoad);
}
struct varToLiveNode {
		struct IRVar *var;
		graphNodeIRLive live;
};
static int varToLiveNodeCompare(const struct varToLiveNode *a,const struct varToLiveNode *b) {
		return IRVarCmp(a->var, b->var);
}
STR_TYPE_DEF(struct varToLiveNode,VarToLiveNode);
STR_TYPE_FUNCS(struct varToLiveNode,VarToLiveNode);
static void findVarInterfereAt(mapRegSlice liveNodeRegs,strGraphNodeIRLiveP spillNodes,strGraphNodeIRLiveP allLiveNodes,graphNodeIR startAt,struct IRVar *startAtVar) {
		//Quit if already did spill/loads on node
		__auto_type find=llIRAttrFind( graphNodeIRValuePtr(startAt)->attrs,IR_ATTR_NODE_SPILL_LOADS_COMPUTED, IRAttrGetPred);
		if(find)
				return;

		//Mark node as computed
		struct IRAttr attr;
		attr.name=(void*)IR_ATTR_NODE_SPILL_LOADS_COMPUTED;
		__auto_type newAttr= __llCreate(&attr, sizeof(attr));
		llIRAttrInsert(graphNodeIRValuePtr(startAt)->attrs, newAttr, IRAttrInsertPred);
		graphNodeIRValuePtr(startAt)->attrs=newAttr;
		
		graphNodeIRLive liveNode=NULL;
		
		//Find var in interference graph
		for(long i=0;i!=strGraphNodeIRLivePSize(allLiveNodes);i++) {
				if(0==IRVarCmp(graphNodeIRLiveValuePtr(allLiveNodes[i])->ref,startAtVar)) {
						liveNode=allLiveNodes[i];
						break;
				}
		}

		if(!liveNode) {
				//Var isnt part of (this) interference graph,so quit
				return;
		}

		//Find variables that interfere with var
		__auto_type interfere=graphNodeIRLiveOutgoingNodes(liveNode);
		strIRVar allVars=NULL;

		char *key=ptr2Str(liveNode);
		struct regSlice *currReg=mapRegSliceGet(liveNodeRegs, key);
		free(key);
		if(!currReg) {
		whineNoReg:
				fprintf(stderr, "Dear programmer,try assigning registers to variables before calling %s." , __func__);
				abort();
		}

		//Also while looking for conflicting nodes,make an asscotiative array for var->live node
		strVarToLiveNode varToLive=NULL;		
		for(long i=0;i!=strGraphNodeIRLivePSize(interfere);i++) {
				char *key=ptr2Str(interfere[i]);
				struct regSlice *reg=mapRegSliceGet(liveNodeRegs, key);
				free(key);

				if(reg) {
						if(regSliceConflict(currReg,  reg)) {
								__auto_type var=graphNodeIRLiveValuePtr(interfere[i])->ref;

								struct varToLiveNode pair;
								pair.live=interfere[i];
								pair.var=var;
								varToLive=strVarToLiveNodeSortedInsert(varToLive, pair, varToLiveNodeCompare);
								
								allVars=strIRVarSortedInsert(allVars, var, IRVarCmp2);
						}
				}
		}

		struct interferencePair pair;
		pair.inteferesWith=allVars;
		pair.var=startAtVar;
		//
		// Find all paths to either
		// A) A choose node that signals the end of the current version of var or
		// B) a reference to a var that interferes with var
		__auto_type paths1=graphAllPathsToPredicate(startAt, &pair,  (int(*)(const struct __graphNode*,const void*))spillOrStoreAt);
		for(long i2=0;i2!=strGraphPathSize(paths1);i2++) {
				if(!paths1[i2])
						continue;
										
				__auto_type last=paths1[i2][strGraphEdgePSize(paths1[i2])-1];
				__auto_type lastNode=graphEdgeIROutgoing(last);
				if(graphNodeIRValuePtr(lastNode)->type==IR_CHOOSE) {
						continue;
				}

				//Check if points to variable(which is should)
				else if(graphNodeIRValuePtr(lastNode)->type==IR_VALUE) {
						if(isVar(lastNode)) {
								//Mark node as spilled to by current variable
								struct IRVar *currVar=graphNodeIRLiveValuePtr(liveNode)->ref;
								
								//Quit spill/load if current variable is already spilled here
								__auto_type find=llIRAttrFind(graphNodeIRValuePtr(lastNode)->attrs,  IR_ATTR_NODE_VARS_SPILLED_AT ,  IRAttrGetPred);
								if(find) {
										//Check if current spilled variables include currVar
										__auto_type spilledAt=(struct IRAttrSpilledVarsAt*)llIRAttrValuePtr(find);
										if(strIRVarSortedFind(spilledAt->vars, currVar, IRVarCmp2)) {
												continue; //No need to re-spill
										} else {
												//Insert currVar into the spilled variables
												spilledAt->vars=strIRVarSortedInsert(spilledAt->vars, currVar, IRVarCmp2);
										}
												
								} else {
										//Insert a new attribute of spilled vars at 
										struct IRAttrSpilledVarsAt attr;
										attr.base.name=(void*)IR_ATTR_NODE_VARS_SPILLED_AT;
										attr.vars=strIRVarAppendItem(NULL, currVar);

										__auto_type newAttr= __llCreate(&attr, sizeof(attr));
										llIRAttrInsert(graphNodeIRValuePtr(lastNode)->attrs, newAttr, IRAttrInsertPred);
										graphNodeIRValuePtr(lastNode)->attrs=newAttr;
								}
								
										
								//Store var's value before entering new variable,then load new variable's value
								__auto_type spill=createSpill(startAtVar);
								__auto_type spillReg=createRegRef(currReg);
								graphNodeIRConnect(spillReg,spill , IR_CONN_DEST);
														
								//Insert load
								__auto_type newVar=&((struct IRNodeValue*)graphNodeIRValuePtr(lastNode))->val.value.var;
								__auto_type load=createLoad(newVar);

								//Find liveness node from variable,then get regslice from node
								struct varToLiveNode pair;
								pair.live=NULL;
								pair.var=&((struct IRNodeValue*)graphNodeIRValuePtr(lastNode))->val.value.var;
								__auto_type liveNode=strVarToLiveNodeSortedFind(varToLive, pair, varToLiveNodeCompare)->live;

								char *key=ptr2Str(liveNode);
								__auto_type loadReg=createRegRef(mapRegSliceGet(liveNodeRegs, key));
								free(key);
								
								graphNodeIRConnect(load,loadReg , IR_CONN_DEST);
														
								graphNodeIRConnect(spill, load, IR_CONN_FLOW);
														
								IRInsertBefore(IRGetStmtStart(lastNode), spillReg,loadReg, IR_CONN_FLOW);
								// 
								//  Spill Register
								//   ||
								//   \/
								// Spill
								//   ||
								//   \/
								//  load
								//   ||
								//   \/
								// newVar
								//   ||
								//   \/
								//Start of expr
								
								//Replace conflicting vairalbles within expression with loads
								
								//Get list of spillNode variables exluding self
								strIRVar spillVars=NULL;
								for(long i=0;i!=strGraphNodeIRLivePSize(spillNodes);i++) {
										__auto_type var=graphNodeIRLiveValuePtr(spillNodes[i])->ref;
										//Dont spill self
										if(var==startAtVar)
												continue;
										
										if(NULL!=strIRVarSortedFind(allVars, var, IRVarCmp2))
												spillVars=strIRVarSortedInsert(spillVars,var, IRVarCmp2);
								}
								//Replace
								__auto_type endOfExpression=IRGetEndOfExpr(lastNode);
								insertLoadsInExpression(endOfExpression, spillVars);

								strIRVarDestroy(&spillVars);

								debugShowGraphIR(startAt);
								
								//Re-run interferance at end of epxression
								findVarInterfereAt(liveNodeRegs, spillNodes,allLiveNodes, endOfExpression, newVar);
								continue;
						}
				}
		}
}
//
// This a function for turning colors into registers
//

typedef int(*regCmpType)(const struct reg **,const struct reg **);
typedef struct regSlice (*color2RegPredicate)(strRegSlice adjacent,strRegP avail,graphNodeIRLive live,int color,const void *data,long colorCount,const int *colors);
static struct regSlice color2Reg(strRegSlice adjacent,strRegP avail,graphNodeIRLive live,int color,const void *data,long colorCount,const int *colors) {
		__auto_type avail2=strRegPClone(avail);
		strRegP adjRegs=NULL;
		for(long i=0;i!=strRegSliceSize(adjacent);i++) {
				adjRegs=strRegPSortedInsert(adjRegs, adjacent[i].reg, (regCmpType)ptrPtrCmp);
		}
		avail2=strRegPSetDifference(avail2, adjRegs, (regCmpType)ptrPtrCmp);

		if(strRegPSize(avail2)==0) {
				strRegPDestroy(&avail2);
				avail2=strRegPClone(avail);
		}
		
		int *find=bsearch(&color , colors, colorCount, sizeof(int), (int(*)(const void*,const void*))intCmp);
		assert(find);

		struct regSlice slice;
		slice.reg=avail2[*find%strRegPSize(avail2)];
		slice.offset=0;
		slice.widthInBits=slice.reg->size*8;

		strRegPDestroy(&avail2);
		return slice;
}
STR_TYPE_DEF(struct metricPair,MetricPair);
STR_TYPE_FUNCS(struct metricPair,MetricPair);
static struct conflictPair *__conflictPairFindAffects(graphNodeIRLive node,const struct conflictPair *start,const struct conflictPair *end) {
		for(;start!=end;start++) {
				if(start->a==node)
						return (void*)start;
				else if(start->a==node)
						return (void*)start;
		}

		return NULL;
}
strConflictPair conflictPairFindAffects(graphNodeIRLive node,strConflictPair pairs) {
		strConflictPair retVal=NULL;
		__auto_type start=pairs;
		__auto_type end=pairs+strConflictPairSize(pairs);
		for(;start!=end;) {
				__auto_type find=__conflictPairFindAffects(node, start, end);
				//Quit if no find
				if(!find)
						break;

				//Insert
				retVal=strConflictPairSortedInsert(retVal, *find, conflictPairCmp);

				//Continue from after find
				start=find+1;
		}

		#if DEBUG_PRINT_ENABLE
					char *firstName=interfereNode2Label(node, NULL, NULL);
					DEBUG_PRINT("These conflict with %s\n",  firstName);
					free(firstName);
					
					for(long i=0;i!=strConflictPairSize(retVal);i++) {
										char *aName=interfereNode2Label(retVal[i].a, NULL, NULL);
										char *bName=interfereNode2Label(retVal[i].b, NULL, NULL);
										
										DEBUG_PRINT("    [%s,%s]\n", aName,bName);
										free(aName),free(bName);
					}
#endif
		
		return retVal;
}
static int conflictPairContains(graphNodeIRLive node,const struct conflictPair *pair) {
		return pair->a==node||pair->b==node;
}
static void replaceVarsWithRegisters(mapRegSlice map,strGraphNodeIRLiveP allLiveNodes,graphNodeIR enter) {
		//
		// Create an associative array to turn variables into live nodes
		//
		strVarToLiveNode varToLive;
		for(long i=0;i!=strGraphNodeIRLivePSize(allLiveNodes);i++) {
				struct varToLiveNode pair;
				pair.live=allLiveNodes[i];
				pair.var=graphNodeIRLiveValuePtr(allLiveNodes[i])->ref;
				varToLive=strVarToLiveNodeSortedInsert(varToLive, pair, varToLiveNodeCompare);
		}
		
		__auto_type allNodes=graphNodeIRAllNodes(enter);
		for(long i=0;i!=strGraphNodeIRPSize(allNodes);i++) {
				//Look for vairable
				if(isVar(allNodes[i])) {
						struct IRNodeValue *value=(void*)graphNodeIRValuePtr(allNodes[i]);

						//Ensure variable is in the current liveness graph
						struct varToLiveNode dummy;
						dummy.live=NULL;
						dummy.var=&value->val.value.var;
						__auto_type find=strVarToLiveNodeSortedFind(varToLive, dummy, varToLiveNodeCompare);
						if(find) {
								//Get register slice from liveness node to register slice map
								char *key=ptr2Str(find->live);
								__auto_type slice=*mapRegSliceGet(map, key);
								free(key);
								
								//Replace
								replaceNodeWithExpr(allNodes[i],  createRegRef(&slice));
						}
				}
		}
}
void IRRegisterAllocate(graphNodeIR start,color2RegPredicate colorFunc,void *colorData) {
		//SSA
		__auto_type allNodes = graphNodeIRAllNodes(start);
		removeChooseNodes(allNodes, start);
		IRToSSA(start);
		//debugShowGraphIR(start);
	
__auto_type allNodes2 = graphNodeIRAllNodes(start);
	for(long i=0;i!=strGraphNodeIRPSize(allNodes2);i++) {
			if(graphNodeIRValuePtr(allNodes2[i])->type==IR_CHOOSE)
					IRSSAReplaceChooseWithAssigns(allNodes2[i]);
	}
	
	//Merge variables that can be merges
		strGraphNodeIRPDestroy(&allNodes);
		allNodes = graphNodeIRAllNodes(start);
		//debugShowGraphIR(start);
		IRCoalesce(allNodes, start);
		//debugShowGraphIR(start);
		IRRemoveRepeatAssigns(start);
		
		debugShowGraphIR(start);

	__auto_type intInterfere=IRInterferenceGraphFilter(start, filterIntVars,NULL);
	
	__auto_type floatInterfere=IRInterferenceGraphFilter(start, filterFloatVars,NULL);
	
	//Compute int and flaoting interfernce seperatly
	strGraphNodeIRLiveP spillNodes=NULL;
	for(long i=0;i!=strGraphNodeIRLivePSize(intInterfere);i++)
	{
			__auto_type interfere=intInterfere[i];
			debugPrintInterferenceGraph(interfere,NULL);
			
			__auto_type vertexColors=graphColor(interfere);

			__auto_type allNodes=graphNodeIRAllNodes(start);

			__auto_type colors=getColorList(vertexColors);

			//Choose registers
 			mapRegSlice regsByLivenessNode=mapRegSliceCreate(); //TODO rename
			__auto_type allColorNodes=graphNodeIRLiveAllNodes(interfere);
			for(long i=0;i!=strGraphNodeIRLivePSize(allColorNodes);i++) {
					//Get adjacent items
					__auto_type outgoing=graphNodeIRLiveOutgoingNodes(allColorNodes[i]);
					strRegSlice adj=NULL;
					for(long i=0;i!=strGraphNodeIRLivePSize(outgoing);i++) {
							//search for adjacent
							char *key=ptr2Str(allColorNodes[i]);
							__auto_type find=mapRegSliceGet(regsByLivenessNode, key);
							free(key);

							//Insert if exists
							if(find)
									adj=strRegSliceAppendItem(adj, *find);
					}
#if DEBUG_PRINT_ENABLE
					for(long i2=0;i2!=strRegSliceSize(adj);i2++) {
							char *nodeName=interfereNode2Label(allColorNodes[i], NULL, NULL);
							DEBUG_PRINT("Register %s is adjacent to %s\n", adj[i2].reg->name,nodeName);
							free(nodeName);
					}
#endif

					//Assign register to

					//Make dummy value to find type of variable
					struct IRValue value;
					value.type=IR_VAL_VAR_REF;
					value.value.var=*graphNodeIRLiveValuePtr(allColorNodes[i])->ref;
					__auto_type type= IRValueGetType(&value);
					
					__auto_type regsForType=regGetForType(type);
					__auto_type slice=color2Reg(adj,regsForType, allColorNodes[i], llVertexColorGet(vertexColors, allColorNodes[i])->color, NULL,strIntSize(colors), colors);
#if DEBUG_PRINT_ENABLE
					 char *nodeName=interfereNode2Label(allColorNodes[i], NULL, NULL);
						DEBUG_PRINT("Assigning register %s to %s\n", slice.reg->name,nodeName);
						free(nodeName);
#endif
					
					//Insert find
					char *key=ptr2Str(allColorNodes[i]);
					mapRegSliceInsert(regsByLivenessNode, key, slice);
					free(key);

#if DEBUG_PRINT_ENABLE
							char *nodeName2=interfereNode2Label(allColorNodes[i], NULL, NULL);
							char buffer[512];
							sprintf(buffer, "%s:REG(%s)", nodeName2,slice.reg->name);
							debugAddPtrName(graphNodeIRLiveValuePtr(allColorNodes[i])->ref->value.var, buffer);
							free(nodeName2);
#endif
					
					strRegSliceDestroy(&adj);
			}
 			debugPrintInterferenceGraph(interfere,regsByLivenessNode);

			//Get conflicts and spill nodes
			strGraphNodeIRLiveP spillNodes=NULL;
			__auto_type conflicts=recolorAdjacentNodes(regsByLivenessNode,allColorNodes[0]);

			//Sort conflicts by minimum spill metric
			__auto_type conflictsSortedByWeight=strConflictPairClone(conflicts);
			qsort(conflictsSortedByWeight, strConflictPairSize(conflicts), sizeof(*conflicts),  conflictPairWeightCmp);

			//
			// Choose spill Nodes:
			//
			// To do this,we first find a list of unspilled nodes that affect unspilled node(i),we then spill the one with the lowest metric.
			// Then we mark the spilled node.We repeat untill there all conflicts are resolved(all unspilled nodes are not adjacent to another unspilled node).
			//
			for(;0!=strConflictPairSize(conflicts);) {
					//Get a list of items that conflict with conflicts[0]
					__auto_type conflictsWithFirst=conflictPairFindAffects(conflicts[0].a, conflicts);
					
					//
					//Do a set union of all items that conflict with each other untill there is no change
					//
					for(;;) {
							strConflictPair clone __attribute__((cleanup(strConflictPairDestroy)))=strConflictPairClone(conflictsWithFirst);
							
							long oldSize=strConflictPairSize(conflictsWithFirst);
							//union
							for(long i=0;i!=strConflictPairSize(clone);i++) {
									conflictsWithFirst=strConflictPairSetUnion(conflictPairFindAffects(clone[i].a, conflicts), conflictsWithFirst, conflictPairCmp);
									conflictsWithFirst=strConflictPairSetUnion(conflictPairFindAffects(clone[i].b, conflicts), conflictsWithFirst, conflictPairCmp);
							}

							//Break if no change
							long newSize=strConflictPairSize(conflictsWithFirst);

							DEBUG_PRINT("OldSize %li,newSize %li\n", oldSize,newSize);
							if(oldSize==newSize)
									break;
					}

					//
					// Find the node with the lowest cost according conflictsSortedByWeight
					//
					long lowestConflictI=-1;
					for(long i=0;i!=strConflictPairSize(conflictsWithFirst);i++) {
							for(long i2=0;i2!=strConflictPairSize(conflictsSortedByWeight);i2++)
									if(conflictsWithFirst[i].a==conflictsSortedByWeight[i2].a)
											if(conflictsWithFirst[i].b==conflictsSortedByWeight[i2].b)
													lowestConflictI=i;
					}

					assert(lowestConflictI!=-1);
#if DEBUG_PRINT_ENABLE
					char *lowestName=interfereNode2Label(conflicts[i].a, NULL, NULL);
					DEBUG_PRINT("Lowest metric conflict is %s\n", lowestName);
					free(lowestName);
#endif

					//Add to spill nodes
					spillNodes=strGraphNodeIRLivePSortedInsert(spillNodes, conflicts[lowestConflictI].a,(gnCmpType)ptrPtrCmp);
					
					//Remove all references to spilled node in conflicts and conflictsSortedByWeight
					typedef int(*removeIfPred)(const void*,const struct conflictPair*);
					__auto_type node=conflicts[lowestConflictI].a;

					conflicts=strConflictPairRemoveIf(conflicts, node,(removeIfPred)conflictPairContains);
					conflictsSortedByWeight=strConflictPairRemoveIf(conflictsSortedByWeight, node,(removeIfPred)conflictPairContains);
			}
			strConflictPairDestroy(&conflicts);
			
			//Get list of variables that have conflicts(are adjacent to spill nodes)
			strGraphNodeIRLiveP nodesWithRegisterConflict=NULL;

			for(long i=0;i!=strGraphNodeIRLivePSize(allColorNodes);i++) {
					//Check if not a spill node
					if(NULL!=strGraphNodeIRLivePSortedFind(spillNodes, allColorNodes[i], (gnCmpType)ptrPtrCmp))
							continue;
					
					__auto_type outgoing=graphNodeIRLiveOutgoingNodes(allColorNodes[i]);
					for(long i2=0;i2!=strGraphNodeIRLivePSize(outgoing);i2++) {
							//If adjacent to spilled node?
							if(NULL!=strGraphNodeIRLivePSortedFind(spillNodes, outgoing[i2], (gnCmpType)ptrPtrCmp)) {
									//Registers conflict ?
									char * key1=ptr2Str(allColorNodes[i]);
									char * key2=ptr2Str(outgoing[i2]);
									__auto_type currReg=mapRegSliceGet(regsByLivenessNode, key1);
									__auto_type outReg=mapRegSliceGet(regsByLivenessNode, key2);
									free(key1),free(key2);

									//Both vairables are put in registers?
									if(currReg&&outReg) {
											//Do they conflict
											if(!regSliceConflict(currReg, outReg))
													continue;
											
											nodesWithRegisterConflict=strGraphNodeIRLivePSortedInsert(nodesWithRegisterConflict, allColorNodes[i], (gnCmpType)ptrPtrCmp);
#if DEBUG_PRINT_ENABLE
											char *name=interfereNode2Label( allColorNodes[i], NULL,  NULL);
											DEBUG_PRINT("Node %s interferes with an adajecnt item.\n", name);
#endif
											break;
									}
							}
					}
					
					strGraphNodeIRLivePDestroy(&outgoing);
			}

			//Add spilled nodes to conflicts
			nodesWithRegisterConflict=strGraphNodeIRLivePSetUnion(nodesWithRegisterConflict, spillNodes, (gnCmpType)ptrPtrCmp);

			//Get list of spilled vars
			strIRVar spillVars=NULL;
			for(long i=0;i!=strGraphNodeIRLivePSize(spillNodes);i++) {
					__auto_type var=graphNodeIRLiveValuePtr(spillNodes[i])->ref;
									
					spillVars=strIRVarSortedInsert(spillVars,var, IRVarCmp2);
			}
			
			//Find all IR nodes with conflict then run a load/spill insert operation starting from that node
			for(long i=0;i!=strGraphNodeIRPSize(allNodes);i++) {
					if(isVar(allNodes[i])) {
							struct IRNodeValue *nodeValue=(void*)graphNodeIRValuePtr(allNodes[i]);
							
							//Check if nodeValue variable is eqivalent to varable in nodesWithRegisterConflict
							for(long i2=0;i2!=strGraphNodeIRLivePSize(nodesWithRegisterConflict);i2++) {
									__auto_type conflict=graphNodeIRLiveValuePtr(nodesWithRegisterConflict[i2])->ref;
									__auto_type have=&nodeValue->val.value.var;
									if(0==IRVarCmp(conflict, have)) {
											findVarInterfereAt(regsByLivenessNode, spillNodes, allColorNodes, allNodes[i], &nodeValue->val.value.var);
											break;
									}
							}
					}
			}
			
			replaceVarsWithRegisters(regsByLivenessNode,allColorNodes,start);
			debugShowGraphIR(start);
			strGraphNodeIRLivePDestroy(&allColorNodes);
	}
}
