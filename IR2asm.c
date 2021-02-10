#include <IR.h>
#include <IR2asm.h>
#include <IRTypeInference.h>
#include <asmEmitter.h>
#include <assert.h>
#include <cleanup.h>
#include <ctype.h>
#include <frameLayout.h>
#include <hashTable.h>
#include <ieee754.h>
#include <parse2IR.h>
#include <parserA.h>
#include <parserB.h>
#include <ptrMap.h>
#include <regAllocator.h>
#include <registers.h>
#include <stdarg.h>
#include <stdio.h>
#include <X86AsmSharedVars.h>
#include <abi.h>
static void *IR_ATTR_ADDR_MODE="ADDR_MODE";
struct IRAttrAddrMode {
		struct IRAttr base;
		struct X86AddressingMode *mode;
};
static void IRAttrAddrModeDestroy(struct IRAttr *attr) {
		struct IRAttrAddrMode *Attr=(void*)attr;
		X86AddrModeDestroy(&Attr->mode);
}
typedef int (*regCmpType)(const struct reg **, const struct reg **);
typedef int (*gnCmpType)(const graphNodeIR *, const graphNodeIR *);
static int ptrPtrCmp(const void *a, const void *b) {
	if (*(void **)a > *(void **)b)
		return 1;
	else if (*(void **)a < *(void **)b)
		return -1;
	else
		return 0;
}
STR_TYPE_DEF(char, Char);
STR_TYPE_FUNCS(char, Char);
PTR_MAP_FUNCS(graphNodeIR, strChar, LabelNames);
PTR_MAP_FUNCS(graphNodeIR, int, CompiledNodes);
PTR_MAP_FUNCS(struct parserFunction *, strChar, FuncNames);
static __thread long labelsCount;
static __thread ptrMapFuncNames funcNames;
static __thread ptrMapLabelNames asmLabelNames;
static __thread ptrMapCompiledNodes compiledNodes;
static __thread int insertLabelsForAsmCalled = 0;
static strGraphNodeIRP removeNeedlessLabels(graphNodeIR start) {
	strGraphNodeIRP allNodes CLEANUP(strGraphNodeIRPDestroy) = graphNodeIRAllNodes(start);
	strGraphNodeIRP removed = NULL;
	for (long i = 0; i != strGraphNodeIRPSize(allNodes); i++) {
		if (allNodes[i] == start)
			continue;

		__auto_type val = graphNodeIRValuePtr(allNodes[i]);
		if (val->type != IR_LABEL)
			continue;
		// Dont remove if named
		if (llIRAttrFind(val->attrs, IR_ATTR_LABEL_NAME, IRAttrGetPred))
			continue;

		strGraphEdgeIRP in CLEANUP(strGraphEdgeIRPDestroy) = graphNodeIRIncoming(allNodes[i]);
		strGraphEdgeIRP out CLEANUP(strGraphEdgeIRPDestroy) = graphNodeIROutgoing(allNodes[i]);
		if (strGraphEdgeIRPSize(in) != 1)
			continue;
		if (strGraphEdgeIRPSize(out) > 1)
			continue;
		//"Transparently" remove
		for (long i = 0; i != strGraphEdgeIRPSize(in); i++)
			for (long o = 0; o != strGraphEdgeIRPSize(out); o++)
				graphNodeIRConnect(graphEdgeIRIncoming(in[i]), graphEdgeIROutgoing(out[o]), *graphEdgeIRValuePtr(in[i]));
		graphNodeIRKill(&allNodes[i], (void (*)(void *))IRNodeDestroy, NULL);

		removed = strGraphNodeIRPSortedInsert(removed, allNodes[i], (gnCmpType)ptrPtrCmp);
	}

	return removed;
}
void IR2AsmInit() {
	labelsCount = 0;
	asmFuncNames=ptrMapFuncNamesCreate();
	asmLabelNames = ptrMapLabelNamesCreate();
	compiledNodes = ptrMapCompiledNodesCreate();
	insertLabelsForAsmCalled = 1;
}
/**
 * These are the consumed registers for the current operation,doesn't
 * include registers for variables
 */
static __thread strRegP consumedRegisters = NULL;
strChar uniqueLabel(const char *head) {
	if (!head)
		head = "";
	long count = snprintf(NULL, 0, "$%s_%li", head, ++labelsCount);
	char buffer[count + 1];
	sprintf(buffer, "$%s_%li", head, labelsCount);
	return strCharAppendData(NULL, buffer, count + 1);
}
static struct reg *regForTypeExcludingConsumed(struct object *type) {
	strRegP regs CLEANUP(strRegPDestroy) = regGetForType(type);
	for (long i = 0; i != strRegPSize(regs); i++) {
		for (long i2 = 0; i2 != strRegPSize(consumedRegisters); i2++) {
			if (regConflict(regs[i], consumedRegisters[i2]))
				goto next;
		}
		return regs[i];
	next:;
	}
	return NULL;
}
static graphNodeIR nodeDest(graphNodeIR node) {
	strGraphEdgeIRP out CLEANUP(strGraphEdgeIRPDestroy) = graphNodeIROutgoing(node);
	assert(strGraphEdgeIRPSize(out) == 1);
	return graphEdgeIROutgoing(out[0]);
}
static void binopArgs(graphNodeIR node, graphNodeIR *a, graphNodeIR *b) {
	strGraphEdgeIRP in CLEANUP(strGraphEdgeIRPDestroy) = graphNodeIRIncoming(node);
	strGraphEdgeIRP inA CLEANUP(strGraphEdgeIRPDestroy) = IRGetConnsOfType(in, IR_CONN_SOURCE_A);
	strGraphEdgeIRP inB CLEANUP(strGraphEdgeIRPDestroy) = IRGetConnsOfType(in, IR_CONN_SOURCE_B);
	if (strGraphEdgeIRPSize(inA) == 1)
		*a = graphEdgeIRIncoming(inA[0]);
	else
		assert(0);
	if (strGraphEdgeIRPSize(inB) == 1)
		*b = graphEdgeIRIncoming(inB[0]);
	else
		assert(0);
}
static int isReg(graphNodeIR node) {
	if (graphNodeIRValuePtr(node)->type == IR_VALUE) {
		struct IRNodeValue *val = (void *)graphNodeIRValuePtr(node);
		return val->val.type == IR_VAL_REG;
	}
	return 0;
}
#define ALLOCATE(val)                                                                                                                                              \
	({                                                                                                                                                               \
		typeof(val) *r = malloc(sizeof(val));                                                                                                                          \
		*r = val;                                                                                                                                                      \
		r;                                                                                                                                                             \
	})
static strChar strClone(const char *name) {
	return strCharAppendData(NULL, name, strlen(name) + 1);
}
static strChar unescapeString(const char *str) {
	char *otherValids = "[]{}\\|;:\"\'<>?,./`~!@#$%^&*()-_+=";
	long len = strlen(str);
	strChar retVal = NULL;
	for (long i = 0; i != len; i++) {
		if (isalnum(str[i])) {
			retVal = strCharAppendItem(retVal, str[i]);
		} else {
			if (strchr(otherValids, str[i])) {
				retVal = strCharAppendItem(retVal, str[i]);
			} else {
				long count = snprintf(NULL, 0, "\\%02x", ((uint8_t *)str)[i]);
				char buffer[count + 1];
				sprintf(buffer, "\\%02x", ((uint8_t *)str)[i]);
				retVal = strCharAppendData(retVal, buffer, strlen(buffer));
			}
		}
	}
	return retVal;
}
static const char *getLabelName(graphNodeIR node) {
loop:;
	__auto_type existing = ptrMapLabelNamesGet(asmLabelNames, node);
	if (existing)
		return *existing;

	__auto_type nv = graphNodeIRValuePtr(node);
	__auto_type find = llIRAttrFind(nv->attrs, IR_ATTR_LABEL_NAME, IRAttrGetPred);
	if (find) {
		struct IRAttrLabelName *name = (void *)llIRAttrValuePtr(find);
		ptrMapLabelNamesAdd(asmLabelNames, node, strClone(name->name));
	} else {
		ptrMapLabelNamesAdd(asmLabelNames, node, uniqueLabel(""));
	}
	goto loop;
}
static struct X86AddressingMode *__node2AddrMode(graphNodeIR start) {
	if (graphNodeIRValuePtr(start)->type == IR_VALUE) {
		struct IRNodeValue *value = (void *)graphNodeIRValuePtr(start);
		switch (value->val.type) {
		case __IR_VAL_MEM_FRAME: {
			if (getCurrentArch() == ARCH_TEST_SYSV || getCurrentArch() == ARCH_X86_SYSV || getCurrentArch() == ARCH_X64_SYSV) {
					return X86AddrModeIndirSIB(0, NULL,  X86AddrModeReg(basePointer()), X86AddrModeSint(-value->val.value.__frame.offset), IRNodeType(start));
			} else {
				assert(0); // TODO  implement
			}
		}
		case __IR_VAL_MEM_GLOBAL: {
				struct X86AddressingMode *mode = X86AddrModeIndirLabel(value->val.value.__global.symbol->name, value->val.value.__global.symbol->type);
			return mode;
		}
		case __IR_VAL_LABEL: {
			graphNodeIR label = value->val.value.__label;
			const char *name = NULL;
			if (ptrMapLabelNamesGet(asmLabelNames, label)) {
				name = *ptrMapLabelNamesGet(asmLabelNames, label);
			} else {
				long count = snprintf(NULL, 0, "LBL%li", ++labelsCount);
				char *name2 = malloc(count + 1);
				sprintf(name2, "LBL%li", labelsCount);
				ptrMapLabelNamesAdd(asmLabelNames, label, strClone(name2));
				name = name2;
			}
			return X86AddrModeLabel(name);
		}
		case IR_VAL_REG: {
			return X86AddrModeReg(value->val.value.reg.reg);
		}
		case IR_VAL_VAR_REF: {
			fprintf(stderr, "CONVERT VARIABLE REFERENCES TO FRAME ADDRESSES!!!");
			assert(0);
		}
		case IR_VAL_FUNC: {
				return X86AddrModeFunc(value->val.value.func);
		}
		case IR_VAL_INT_LIT: {
			struct X86AddressingMode mode;
			mode.valueType = NULL;
			if (value->val.value.intLit.type == INT_SLONG) {
				return X86AddrModeSint(value->val.value.intLit.value.sLong);
			} else if (value->val.value.intLit.type == INT_ULONG) {
				return X86AddrModeSint(value->val.value.intLit.value.uLong);
			}
		}
		case IR_VAL_STR_LIT: {
			strChar strLab CLEANUP(strCharDestroy) = uniqueLabel("FLT");
			X86EmitAsmLabel(strLab);
			X86EmitAsmStrLit(value->val.value.strLit);
			__auto_type lab = X86AddrModeLabel(strLab);
			lab->valueType = objectPtrCreate(&typeU8i);
			return lab;
		}
		case IR_VAL_FLT_LIT: {
			struct X86AddressingMode *encoded CLEANUP(X86AddrModeDestroy) = X86AddrModeUint(IEEE754Encode(value->val.value.fltLit));
			strChar fltLab CLEANUP(strCharDestroy) = uniqueLabel("FLT");
			X86EmitAsmLabel(fltLab);
			X86EmitAsmDU64(&encoded, 1);
			__auto_type lab = X86AddrModeLabel(fltLab);
			lab->valueType = &typeF64;
			return lab;
		}
		}
	} else if (graphNodeIRValuePtr(start)->type == IR_LABEL) {
		return X86AddrModeLabel(getLabelName(start));
	}  else {
			fprintf(stderr, "Accessing member requires pointer source.\n");
			abort();
	}
	assert(0);
	return X86AddrModeSint(-1);
}

struct X86AddressingMode *IRNode2AddrMode(graphNodeIR start) {
		struct IRNode *node=graphNodeIRValuePtr(start);
		__auto_type find=llIRAttrFind(node->attrs, IR_ATTR_ADDR_MODE, IRAttrGetPred);
		if(find)
				return X86AddrModeClone(((struct IRAttrAddrMode*)llIRAttrValuePtr(find))->mode);
		
		__auto_type retVal = __node2AddrMode(start);
		if(!retVal->valueType)
				retVal->valueType = IRNodeType(start);
	
	struct IRAttrAddrMode modeAttr;
	modeAttr.base.name=IR_ATTR_ADDR_MODE;
	modeAttr.base.destroy=IRAttrAddrModeDestroy;
	modeAttr.mode=X86AddrModeClone(retVal);
	IRAttrReplace(start, __llCreate(&modeAttr, sizeof(modeAttr)));
	return retVal;
}
static void strX86AddrModeDestroy2(strX86AddrMode *str) {
	for (long i = 0; i != strX86AddrModeSize(*str); i++)
		X86AddrModeDestroy(&str[0][i]);
	strX86AddrModeDestroy(str);
}
static void assembleInst(const char *name, strX86AddrMode args) {
	strOpcodeTemplate ops CLEANUP(strOpcodeTemplateDestroy) = X86OpcodesByArgs(name, args, NULL);
	assert(strOpcodeTemplateSize(ops));
	int err;
	X86EmitAsmInst(ops[0], args, &err);
	assert(!err);
}
static struct reg *destReg() {
	switch (getCurrentArch()) {
	case ARCH_X64_SYSV:
		return &regAMD64RDI;
	case ARCH_TEST_SYSV:
	case ARCH_X86_SYSV:
		return &regX86EDI;
	}
}
static struct reg *sourceReg() {
	switch (getCurrentArch()) {
	case ARCH_X64_SYSV:
		return &regAMD64RSI;
	case ARCH_TEST_SYSV:
	case ARCH_X86_SYSV:
		return &regX86ESI;
	}
}
static int isX87FltReg(const struct reg *r) {
	const struct reg *regs[] = {
	    &regX86ST0, &regX86ST1, &regX86ST2, &regX86ST3, &regX86ST4, &regX86ST5, &regX86ST6, &regX86ST7,
	};
	for (long i = 0; i != sizeof(regs) / sizeof(*regs); i++)
		if (regs[i] == r)
			return 1;
	return 0;
}
static int isGPReg(const struct reg *r) {
	const struct reg *regs[] = {
	    &regAMD64RAX,   &regAMD64RBX,   &regAMD64RCX,    &regAMD64RDX,    &regAMD64RSP,    &regAMD64RBP,    &regAMD64RSI,    &regAMD64RDI,
	    &regAMD64R8u64, &regAMD64R9u64, &regAMD64R10u64, &regAMD64R11u64, &regAMD64R12u64, &regAMD64R13u64, &regAMD64R14u64, &regAMD64R15u64,
	};
	struct regSlice rSlice;
	rSlice.reg = (void *)r;
	rSlice.offset = 0;
	rSlice.type = NULL;
	rSlice.widthInBits = r->size * 8;
	for (long i = 0; i != sizeof(regs) / sizeof(*regs); i++) {
		struct regSlice rSlice2;
		rSlice2.reg = (void *)regs[i];
		rSlice2.offset = 0;
		rSlice2.type = NULL;
		rSlice2.widthInBits = regs[i]->size * 8;
		if (regSliceConflict(&rSlice, &rSlice2))
			return 1;
	}
	return 0;
}
static int isFuncEnd(const struct __graphNode *node, graphNodeIR end) {
	return node == end;
}
static void consumeRegister(struct reg *reg) {
	consumedRegisters = strRegPSortedInsert(consumedRegisters, reg, (regCmpType)ptrPtrCmp);
}
static strRegP regsFromMode(struct X86AddressingMode *mode) {
		if (mode->type == X86ADDRMODE_REG) {
				return strRegPAppendItem(NULL,mode->value.reg);
	} else if (mode->type == X86ADDRMODE_MEM) {
				if (mode->value.m.type == x86ADDR_INDIR_REG) {
						return strRegPAppendItem(NULL,mode->value.m.value.indirReg);
				} else if (mode->value.m.type == x86ADDR_INDIR_SIB) {
						strRegP retVal=NULL;
						if (mode->value.m.value.sib.base)
								retVal=regsFromMode(mode->value.m.value.sib.base);
						if (mode->value.m.value.sib.index) {
								__auto_type tmp=strRegPSetUnion(regsFromMode(mode->value.m.value.sib.index), retVal, (regCmpType)ptrPtrCmp);
								strRegPDestroy(&retVal);
								retVal=tmp;
						}
						return retVal;
				}
		}
		return NULL;
}
static void consumeRegFromMode(struct X86AddressingMode *mode) {
	if (mode->type == X86ADDRMODE_REG) {
		consumeRegister(mode->value.reg);
	} else if (mode->type == X86ADDRMODE_MEM) {
		if (mode->value.m.type == x86ADDR_INDIR_REG)
			consumeRegister(mode->value.m.value.indirReg);
		else if (mode->value.m.type == x86ADDR_INDIR_SIB) {
			if (mode->value.m.value.sib.base)
				consumeRegFromMode(mode->value.m.value.sib.base);
			if (mode->value.m.value.sib.index)
				consumeRegFromMode(mode->value.m.value.sib.index);
		}
	}
}
static void uncomsumeRegister(struct reg *reg) {
	__auto_type find = strRegPSortedFind(consumedRegisters, reg, (regCmpType)ptrPtrCmp);
	assert(find);
	strRegP tmp CLEANUP(strRegPDestroy) = strRegPAppendItem(NULL, reg);
	consumedRegisters = strRegPSetDifference(consumedRegisters, tmp, (regCmpType)ptrPtrCmp);
}
static void unconsumeRegFromMode(struct X86AddressingMode *mode) {
	if (mode->type == X86ADDRMODE_REG)
		uncomsumeRegister(mode->value.reg);
	else if (mode->type == X86ADDRMODE_MEM) {
		if (mode->value.m.type == x86ADDR_INDIR_REG)
			consumeRegister(mode->value.m.value.indirReg);
		else if (mode->value.m.type == x86ADDR_INDIR_SIB) {
			if (mode->value.m.value.sib.base)
				unconsumeRegFromMode(mode->value.m.value.sib.base);
			if (mode->value.m.value.sib.index)
				unconsumeRegFromMode(mode->value.m.value.sib.index);
		}
	}
}
static void __unconsumeRegFromModeDestroy(struct X86AddressingMode **mode) {
	unconsumeRegFromMode(mode[0]);
	X86AddrModeDestroy(mode);
}
// https://mort.coffee/home/obscure-c-features/
#define CONCAT_(a, b) a##b
#define CONCAT(a, b) CONCAT_(a, b)
#define UNINAME CONCAT($, __COUNTER__)
#define __AUTO_LOCK_MODE_REGS(name, mode)                                                                                                                          \
	struct X86AddressingMode *name CLEANUP(__unconsumeRegFromModeDestroy) = X86AddrModeClone(mode);                                                                  \
	consumeRegFromMode(name);
#define AUTO_LOCK_MODE_REGS(mode) __AUTO_LOCK_MODE_REGS(UNINAME, (mode))
static strGraphNodeIRP getFuncNodes(graphNodeIR startN) {
	struct IRNodeFuncStart *start = (void *)graphNodeIRValuePtr(startN);
	strGraphEdgeP allEdges CLEANUP(strGraphEdgePDestroy) = graphAllEdgesBetween(startN, start->end, (int (*)(const struct __graphNode *, const void *))isFuncEnd);
	strGraphNodeIRP allNodes = NULL;
	for (long i = 0; i != strGraphEdgePSize(allEdges); i++) {
		strGraphNodeIRP frontBack = strGraphNodeIRPAppendItem(NULL, graphEdgeIRIncoming(allEdges[i]));
		frontBack = strGraphNodeIRPSortedInsert(frontBack, graphEdgeIROutgoing(allEdges[i]), (gnCmpType)ptrPtrCmp);
		allNodes = strGraphNodeIRPSetUnion(allNodes, frontBack, (gnCmpType)ptrPtrCmp);
	}
	return allNodes;
}
STR_TYPE_DEF(struct parserVar *, PVar);
STR_TYPE_FUNCS(struct parserVar *, PVar);
typedef int (*PVarCmpType)(const struct parserVar **, const struct parserVar **);
static int isPrimitiveType(const struct object *obj) {
		obj=objectBaseType(obj);
		if(obj->type==TYPE_PTR)
				return 1;
		
		const struct object *prims[] = {
	    &typeU0, &typeU8i, &typeU16i, &typeU32i, &typeU64i, &typeI8i, &typeI16i, &typeI32i, &typeI64i, &typeF64,
	};
	for (long i = 0; i != sizeof(prims) / sizeof(*prims); i++) {
		if (prims[i] == obj)
			return 1;
	}
	return 0;
}
static int isNotNoreg(const struct parserVar *var, const void *data) {
	return !var->isNoreg;
}
static int frameEntryCmp(const void *a, const void *b) {
	const struct frameEntry *A = a;
	const struct frameEntry *B = b;
	return IRVarCmp(&A->var, &B->var);
}
static void debugShowGraphIR(graphNodeIR enter) {
	const char *name = tmpnam(NULL);
	__auto_type map = graphNodeCreateMapping(enter, 1);
	IRGraphMap2GraphViz(map, "viz", name, NULL, NULL, NULL, NULL);
	char buffer[1024];
	sprintf(buffer, "sleep 0.1 &&dot -Tsvg %s > /tmp/dot.svg && firefox /tmp/dot.svg & ", name);

	system(buffer);
}
static strGraphNodeIRP insertLabelsForAsm(strGraphNodeIRP nodes) {
	strGraphNodeIRP inserted = NULL;
	insertLabelsForAsmCalled = 1;
	for (long i = 0; i != strGraphNodeIRPSize(nodes); i++) {
		__auto_type node = nodes[i];
		strGraphEdgeIRP in CLEANUP(strGraphEdgeIRPDestroy) = graphNodeIRIncoming(node);
		strGraphEdgeIRP inFlow CLEANUP(strGraphEdgeIRPDestroy) = IRGetConnsOfType(in, IR_CONN_FLOW);
		if (strGraphEdgeIRPSize(inFlow) > 1) {
		insertLabel:
			if (graphNodeIRValuePtr(node)->type == IR_LABEL)
				continue;
			__auto_type new = IRCreateLabel();
			IRInsertBefore(node, new, new, IR_CONN_FLOW);
			inserted = strGraphNodeIRPSortedInsert(inserted, new, (gnCmpType)ptrPtrCmp);
			continue;
		}
		for (long i = 0; i != strGraphEdgeIRPSize(in); i++) {
			switch (*graphEdgeIRValuePtr(in[0])) {
			case IR_CONN_CASE:
			case IR_CONN_COND_TRUE:
			case IR_CONN_COND_FALSE:
				goto insertLabel;
			default:;
			}
		}
	}
	return inserted;
}
static void pushReg(struct reg *r) {
		//Can't push 8-bit registers,so make room on the stack and assign 
		if(r->size==1) {
				strX86AddrMode subArgs CLEANUP(strX86AddrModeDestroy2) =NULL;
				subArgs=strX86AddrModeAppendItem(subArgs, X86AddrModeReg(stackPointer()));
				subArgs=strX86AddrModeAppendItem(subArgs, X86AddrModeSint(1));
				assembleInst("SUB", subArgs);

				strX86AddrMode movArgs CLEANUP(strX86AddrModeDestroy2) =NULL;
				movArgs=strX86AddrModeAppendItem(movArgs, X86AddrModeIndirSIB(0, NULL, X86AddrModeReg(stackPointer()), X86AddrModeSint(1), &typeU8i));
				movArgs=strX86AddrModeAppendItem(movArgs, X86AddrModeReg(r));
				assembleInst("MOV", movArgs);
				return;
		}
	strX86AddrMode ppIndexArgs CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeReg(r));
	assembleInst("PUSH", ppIndexArgs);
}
static void popReg(struct reg *r) {
		//Can't pop 8-bit registers,so make room on the stack and assign 
		if(r->size==1) {
				strX86AddrMode movArgs CLEANUP(strX86AddrModeDestroy2) =NULL;
				movArgs=strX86AddrModeAppendItem(movArgs, X86AddrModeReg(r));
				movArgs=strX86AddrModeAppendItem(movArgs, X86AddrModeIndirSIB(0, NULL, X86AddrModeReg(stackPointer()), X86AddrModeSint(1), &typeU8i));
				assembleInst("MOV", movArgs);

				strX86AddrMode addArgs CLEANUP(strX86AddrModeDestroy2) =NULL;
				addArgs=strX86AddrModeAppendItem(addArgs, X86AddrModeReg(stackPointer()));
				addArgs=strX86AddrModeAppendItem(addArgs, X86AddrModeSint(1));
				assembleInst("ADD", addArgs);
				return;
		}
		
		strX86AddrMode ppIndexArgs CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeReg(r));
	assembleInst("POP", ppIndexArgs);
}
static struct object *getTypeForSize(long size) {
	switch (size) {
	case 1:
		return &typeI8i;
	case 2:
		return &typeI16i;
	case 4:
		return &typeI32i;
	case 8:
		return &typeI64i;
	}
	return &typeU0;
}
void asmAssign(struct X86AddressingMode *a, struct X86AddressingMode *b, long size) {
	if (a->type == X86ADDRMODE_REG) {
		if (isX87FltReg(a->value.reg)) {
		}
	}
	strX86AddrMode args CLEANUP(strX86AddrModeDestroy) = NULL;
	strOpcodeTemplate ops CLEANUP(strOpcodeTemplateDestroy) = NULL;
	if (size == 1 || size == 2 || size == 4 || size == 8) {
		args = strX86AddrModeAppendItem(args, a);
		args = strX86AddrModeAppendItem(args, b);
	}
	if (size == 1 || size == 2 || size == 4 || size == 8) {
			if(a->type==X86ADDRMODE_MEM||a->type==X86ADDRMODE_ITEM_ADDR) {
					if(b->type==X86ADDRMODE_MEM||b->type==X86ADDRMODE_ITEM_ADDR) {
							//Can't move memory to memory,so store b item in a register
							AUTO_LOCK_MODE_REGS(a);
							__auto_type reg=regForTypeExcludingConsumed(getTypeForSize(size));
							pushReg(reg);
							strX86AddrMode  mov1Args CLEANUP(strX86AddrModeDestroy2)=NULL;
							mov1Args=strX86AddrModeAppendItem(mov1Args, X86AddrModeReg(reg));
							mov1Args=strX86AddrModeAppendItem(mov1Args, X86AddrModeClone(b));
							assembleInst("MOV", mov1Args);

							strX86AddrMode  mov2Args CLEANUP(strX86AddrModeDestroy2)=NULL;
							mov2Args=strX86AddrModeAppendItem(mov2Args, X86AddrModeClone(a));
							mov2Args=strX86AddrModeAppendItem(mov2Args, X86AddrModeReg(reg));
							assembleInst("MOV", mov2Args);
							popReg(reg);
							return;
					}
			}
			assembleInst("MOV", args);
		return;
	} else {
		long repCount, width;
		if (size % 8==0&&dataSize()==8) {
			if (size <= (8 * 0xffff)) {
				repCount = size / 8;
				width = 8;
			} else
				goto callMemcpy;
		} else if (size % 4==0) {
			if (size <= (4 * 0xffff)) {
				repCount = size / 4;
				width = 4;
			} else
				goto callMemcpy;
		} else if (size % 2==0) {
			if (size <= (2 * 0xffff)) {
				repCount = size / 2;
				width = 2;
			} else
				goto callMemcpy;
		} else if (size % 1==0) {
			if (size <= (1 * 0xffff)) {
				repCount = size / 1;
				width = 1;
			} else
				goto callMemcpy;
		}
	repeatCode : {
				__auto_type cReg=(ptrSize()==4)?&regX86ECX:&regAMD64RCX;
				strX86AddrMode ppArgsCX CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeReg(cReg));
		strX86AddrMode ppArgsRDI CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeReg(destReg()));
		strX86AddrMode ppArgsRSI CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeReg(sourceReg()));
		assembleInst("PUSH", ppArgsCX);
		assembleInst("PUSH", ppArgsRDI);
		assembleInst("PUSH", ppArgsRSI);
		struct X86AddressingMode *count CLEANUP(X86AddrModeDestroy) = X86AddrModeSint(repCount);
		struct X86AddressingMode *rdi CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(destReg());
		struct X86AddressingMode *rsi CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(sourceReg());

		AUTO_LOCK_MODE_REGS(a);
		AUTO_LOCK_MODE_REGS(b);
		__auto_type storeA=regForTypeExcludingConsumed(objectPtrCreate(&typeU0));
		consumeRegister(storeA);
		__auto_type storeB=regForTypeExcludingConsumed(objectPtrCreate(&typeU0));
		consumeRegister(storeB);
		if(storeB==destReg()) {
				//Swap so dest register is store
				__auto_type tmp=storeA;
				storeA=storeB;
				storeB=tmp;
		}
		if(storeA==sourceReg()) {
				//Swap so dest register is store
				__auto_type tmp=storeA;
				storeA=storeB;
				storeB=tmp;
		}
		if(storeA!=destReg())
				pushReg(storeA);
		if(storeB!=sourceReg())
				pushReg(storeB);
		strX86AddrMode leaArgs1 CLEANUP(strX86AddrModeDestroy2)=NULL;
		leaArgs1=strX86AddrModeAppendItem(leaArgs1, X86AddrModeReg(storeA));
		leaArgs1=strX86AddrModeAppendItem(leaArgs1, X86AddrModeClone(a));
		leaArgs1[1]->valueType=NULL;
		assembleInst( "LEA", leaArgs1);
		strX86AddrMode leaArgs2 CLEANUP(strX86AddrModeDestroy2)=NULL;
		leaArgs2=strX86AddrModeAppendItem(leaArgs2, X86AddrModeReg(storeB));
		leaArgs2=strX86AddrModeAppendItem(leaArgs2, X86AddrModeClone(b));
		leaArgs2[1]->valueType=NULL;
		assembleInst( "LEA", leaArgs2);

		struct X86AddressingMode *aMode CLEANUP(X86AddrModeDestroy)=X86AddrModeReg(storeA);
		struct X86AddressingMode *bMode CLEANUP(X86AddrModeDestroy)=X86AddrModeReg(storeB);
		asmAssign(rdi, aMode, ptrSize());
		asmAssign(rsi, bMode, ptrSize());

		uncomsumeRegister(storeA);
		uncomsumeRegister(storeB);

		if(storeB!=sourceReg())
				popReg(storeB);
		if(storeA!=destReg())
				popReg(storeA);

		struct X86AddressingMode *ecx CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(cReg);
		asmAssign(ecx, count, ptrSize());
		
		assembleInst("REP", NULL);
		switch (width) {
		case 1:
				assembleInst("MOVSB", NULL);
				break;
		case 2:
				assembleInst("MOVSW", NULL);
				break;
		case 4:
				assembleInst("MOVSD", NULL);
			break;
		case 8:
				assembleInst("MOVSQ", NULL);
			break;
		}
		
		assembleInst("POP", ppArgsRSI);
		assembleInst("POP", ppArgsRDI);
		assembleInst("POP", ppArgsCX);
		return;
	}
	}
callMemcpy : {
	strRegP regs CLEANUP(strRegPDestroy) = NULL;
	switch (ptrSize()) {
	case 4:
		regs = regGetForType(&typeU32i);
		break;
	case 8:
		regs = regGetForType(&typeU64i);
		break;
	default:
		assert(0);
	}
	// PUSH rcx,rsi,rdi
	// MOV rcx,*size*
	// LABEL:
	// STOSB
	// LOOP LABEL
	// POP rdi,rsi,rcx

	strX86AddrMode ppArgsCount CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeReg(regs[0]));
	strX86AddrMode ppArgsRDI CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeReg(destReg()));
	strX86AddrMode ppArgsRSI CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeReg(sourceReg()));
	assembleInst("PUSH", ppArgsCount);
	assembleInst("PUSH", ppArgsRDI);
	assembleInst("PUSH", ppArgsRSI);
	char *labName = X86EmitAsmLabel(NULL);
	assembleInst("REP_MOVSB", NULL);
	strX86AddrMode labArg CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeLabel(labName));
	assembleInst("LOOP", NULL);
	free(labName);
	assembleInst("POP", ppArgsRSI);
	assembleInst("POP", ppArgsRDI);
	assembleInst("POP", ppArgsCount);
	return;
}
}
void IRCompile(graphNodeIR start,int isFunc) {
		debugShowGraphIR(start);
		if(isFunc) {
				struct IRNodeFuncStart *funcNode=(void*)graphNodeIRValuePtr(start);
				X86EmitAsmLabel(funcNode->func->name);
		}
		
		__auto_type originalStart=start;
		__auto_type entry = IRCreateLabel();
	graphNodeIRConnect(entry, start, IR_CONN_FLOW);
	start = entry;

	strGraphNodeIRP nodes CLEANUP(strGraphNodeIRPDestroy) = graphNodeIRAllNodes(start);
	{
		strGraphNodeIRP removed CLEANUP(strGraphNodeIRPDestroy) = removeNeedlessLabels(start);
		nodes = strGraphNodeIRPSetDifference(nodes, removed, (gnCmpType)ptrPtrCmp);
		strGraphNodeIRP inserted CLEANUP(strGraphNodeIRPDestroy) = insertLabelsForAsm(nodes);
		inserted = strGraphNodeIRPSetUnion(nodes, inserted, (gnCmpType)ptrPtrCmp);
	}

	strGraphNodeIRP funcsWithin CLEANUP(strGraphNodeIRPDestroy)=NULL;
	//Get list of function nodes,then we remove them from the main
	for (long n = 0; n != strGraphNodeIRPSize(nodes); n++) {
			if(nodes[n]==originalStart&&isFunc)
					continue;
			struct IRNodeFuncStart *start=(void*)graphNodeIRValuePtr(nodes[n]);
			if(start->base.type!=IR_FUNC_START)
					continue;
			// Make a "transparent" label to route all traffic in/out of the function's "space" in the graph(pretend the function never existed and compile it later)
			__auto_type lab=IRCreateLabel();
			IRInsertBefore(nodes[n], lab, lab, IR_CONN_FLOW);
			
			// Kill all incoming and outgoing nodes to isolate the function
			strGraphNodeIRP in CLEANUP(strGraphNodeIRPDestroy)=graphNodeIRIncomingNodes(nodes[n]);
			for(long i=0;i!=strGraphNodeIRPSize(in);i++)
					graphEdgeIRKill(in[i], nodes[n], NULL, NULL, NULL);
			strGraphNodeIRP out CLEANUP(strGraphNodeIRPDestroy)=graphNodeIROutgoingNodes(start->end);
			for(long o=0;o!=strGraphNodeIRPSize(out);o++) {
					graphNodeIRConnect(lab, out[o], IR_CONN_FLOW);
					graphEdgeIRKill(start->end, out[o],NULL, NULL, NULL);
			}

			funcsWithin=strGraphNodeIRPSortedInsert(funcsWithin, nodes[n], (gnCmpType)ptrPtrCmp);
	}
	strGraphNodeIRPDestroy(&nodes);
	nodes=graphNodeIRAllNodes(start);
	
	// If start is a function-start,make a list of arguments and store them into variables,replace references to function argument node with variable
	
	// "Push" asmFuncArgVars
	__auto_type oldAsmFuncArgVars=asmFuncArgVars;
	
	// Get list of variables that will always be stored in memory
	// - variables that are referenced by ptr
	// - Classes/unions with primitive base that have members references(I64.u8[1] etc)
	// - Vars marked as noreg
	strPVar noregs CLEANUP(strPVarDestroy) = NULL;
	strPVar inRegs CLEANUP(strPVarDestroy) = NULL;
	for (long i = 0; i != strGraphNodeIRPSize(nodes); i++) {
		struct IRNodeValue *value = (void *)graphNodeIRValuePtr(nodes[i]);
		if (value->base.type !=IR_VALUE)
			continue;
		if (value->val.type != IR_VAL_VAR_REF)
			continue;
		
		strGraphNodeIRP out CLEANUP(strGraphNodeIRPDestroy) = graphNodeIROutgoingNodes(nodes[i]);

		if(value->val.value.var.var->isGlobal)
				goto markAsNoreg;
		
		if (!isPrimitiveType(objectBaseType(IRNodeType(nodes[i]))))
			goto markAsNoreg;

		if (strGraphNodeIRPSize(out) == 1) {
			if (graphNodeIRValuePtr(out[0])->type == IR_ADDR_OF)
				goto markAsNoreg;

			if (isPrimitiveType(objectBaseType(IRNodeType(nodes[i]))))
				if (graphNodeIRValuePtr(out[0])->type == IR_MEMBERS)
					goto markAsNoreg;
		}

		if (value->val.value.var.var->isNoreg)
			goto markAsNoreg;

		__auto_type var = value->val.value.var.var;
		if (!strPVarSortedFind(inRegs, var, (PVarCmpType)ptrPtrCmp))
			inRegs = strPVarSortedInsert(inRegs, var, (PVarCmpType)ptrPtrCmp);
		continue;
	markAsNoreg : {
		__auto_type var = value->val.value.var.var;
		if (!strPVarSortedFind(noregs, var, (PVarCmpType)ptrPtrCmp))
			noregs = strPVarSortedInsert(noregs, var, (PVarCmpType)ptrPtrCmp);
	}
	}
	for (long i = 0; i != strPVarSize(noregs); i++)
		noregs[i]->isNoreg = 1;

	//
	// ABI section requires args to be converted to variables for register allocator.
	// This will also load the function arguments into the variables.
	//
	if(isFunc) {
			IRABIInsertLoadArgs(originalStart);
	}
	
	IRInsertNodesBetweenExprs(start, NULL, NULL);
	IRRegisterAllocate(start, NULL, NULL, isNotNoreg, noregs);
	debugShowGraphIR(start);

	strGraphNodeIRP regAllocedNodes CLEANUP(strGraphNodeIRPDestroy) = graphNodeIRAllNodes(start);
	{
		strGraphNodeIRP removedNodes CLEANUP(strGraphNodeIRPDestroy) = NULL;
		strGraphNodeIRP addedNodes CLEANUP(strGraphNodeIRPDestroy) = NULL;
		// Replace all spills/loads with variable,then we compute a frame layout
		for (long i = 0; i != strGraphNodeIRPSize(regAllocedNodes); i++) {
			if (graphNodeIRValuePtr(regAllocedNodes[i])->type == IR_SPILL_LOAD) {
				struct IRNodeSpill *spillLoad = (void *)graphNodeIRValuePtr(regAllocedNodes[i]);
				
				__auto_type var = spillLoad->item.value.var.var;
				strGraphNodeP toReplace CLEANUP(strGraphNodePDestroy) = strGraphNodePAppendItem(NULL, regAllocedNodes[i]);
				__auto_type newNode = IRCreateVarRef(var);
				graphReplaceWithNode(toReplace, newNode, NULL, (void (*)(void *))IRNodeDestroy, sizeof(enum IRConnType));

				removedNodes = strGraphNodeIRPSortedInsert(removedNodes, regAllocedNodes[i], (gnCmpType)ptrPtrCmp);
				addedNodes = strGraphNodeIRPSortedInsert(addedNodes, newNode, (gnCmpType)ptrPtrCmp);
			}
		}
		regAllocedNodes = strGraphNodeIRPSetDifference(regAllocedNodes, removedNodes, (gnCmpType)ptrPtrCmp);
		regAllocedNodes = strGraphNodeIRPSetUnion(regAllocedNodes, addedNodes, (gnCmpType)ptrPtrCmp);
	}

	//"Push" the old frame layout
	__auto_type oldOffsets = localVarFrameOffsets;

	//Frame allocate
	long frameSize;
	{
			strFrameEntry layout CLEANUP(strFrameEntryDestroy) = IRComputeFrameLayout(start, &frameSize);
			localVarFrameOffsets = ptrMapFrameOffsetCreate();
			for (long i = 0; i != strFrameEntrySize(layout); i++)
					ptrMapFrameOffsetAdd(localVarFrameOffsets, layout[i].var.var, layout[i].offset);

			strGraphNodeIRP removed CLEANUP(strGraphNodeIRPDestroy)=NULL;
			strGraphNodeIRP added CLEANUP(strGraphNodeIRPDestroy)=NULL;
			for(long n=0;n!=strGraphNodeIRPSize(regAllocedNodes);n++) {
					struct IRNodeValue *ir=(void*)graphNodeIRValuePtr(regAllocedNodes[n]);
					if(ir->base.type!=IR_VALUE)
							continue;
					if(ir->val.type!=IR_VAL_VAR_REF)
							continue;
					if(ir->val.value.var.var->isGlobal)
							continue;
					
					__auto_type find=ptrMapFrameOffsetGet(localVarFrameOffsets, ir->val.value.var.var);
					assert(find);
					removed=strGraphNodeIRPSortedInsert(removed, regAllocedNodes[n], (gnCmpType)ptrPtrCmp);
					__auto_type frameReference=IRCreateFrameAddress(*find, ir->val.value.var.var->type);
					added=strGraphNodeIRPSortedInsert(added, frameReference, (gnCmpType)ptrPtrCmp);

					strGraphNodeIRP dummy CLEANUP(strGraphNodeIRPDestroy)=strGraphNodeIRPAppendItem(NULL, regAllocedNodes[n]); 
					graphIRReplaceNodes(dummy, frameReference, NULL, (void(*)(void*))IRNodeDestroy);
			}

			regAllocedNodes=strGraphNodeIRPSetDifference(regAllocedNodes, removed, (gnCmpType)ptrPtrCmp);
			regAllocedNodes=strGraphNodeIRPSetUnion(regAllocedNodes, added, (gnCmpType)ptrPtrCmp);
	}

	//Replace all global variables with 
	{
		strGraphNodeIRP removed CLEANUP(strGraphNodeIRPDestroy)=NULL;
		strGraphNodeIRP added CLEANUP(strGraphNodeIRPDestroy)=NULL;	
		for(long n=0;n!=strGraphNodeIRPSize(regAllocedNodes);n++) {
				struct IRNodeValue *val=(void*)graphNodeIRValuePtr(regAllocedNodes[n]);
				if(val->base.type!=IR_VALUE)
						continue;
				if(val->val.type!=IR_VAL_VAR_REF)
						continue;
				if(!val->val.value.var.var->isGlobal)
						continue;
				__auto_type sym=IRCreateGlobalVarRef(val->val.value.var.var);
				strGraphNodeIRP toReplace CLEANUP(strGraphNodeIRPDestroy)=strGraphNodeIRPAppendItem(NULL, regAllocedNodes[n]);
				graphIRReplaceNodes(toReplace, sym, NULL, (void(*)(void*))IRNodeDestroy);

				removed=strGraphNodeIRPSortedInsert(removed, regAllocedNodes[n], (gnCmpType)ptrPtrCmp);
				added=strGraphNodeIRPSortedInsert(added, sym, (gnCmpType)ptrPtrCmp);
		}
		regAllocedNodes=strGraphNodeIRPSetDifference(regAllocedNodes, removed, (gnCmpType)ptrPtrCmp);
		regAllocedNodes=strGraphNodeIRPSetUnion(regAllocedNodes, added, (gnCmpType)ptrPtrCmp);
	}
	// For all non-reg globals,dump them to global scope
	for (long p = 0; p != strPVarSize(noregs); p++) {
		if (!noregs[p]->isGlobal)
			continue;
		X86EmitAsmGlobalVar(noregs[p]);
	}

	if(isFunc) {
			IRABIAsmPrologue();
	} else {
			//Make EBP equal to ESP
			struct X86AddressingMode *bp=X86AddrModeReg(basePointer());
			struct X86AddressingMode *sp=X86AddrModeReg(stackPointer());
			asmAssign(bp, sp, ptrSize());
	}
	//This computes calling information for the ABI
	debugShowGraphIR(start);
	IRComputeABIInfo(start);
	//debugShowGraphIR(start);
	//Add to stack pointer to make room for locals
	strX86AddrMode addArgs CLEANUP(strX86AddrModeDestroy2)=NULL;
	addArgs=strX86AddrModeAppendItem(addArgs, X86AddrModeReg(stackPointer()));
	addArgs=strX86AddrModeAppendItem(addArgs, X86AddrModeSint(frameSize));
	assembleInst("SUB", addArgs);
	
	IR2Asm(start);
	
	ptrMapFrameOffsetDestroy(localVarFrameOffsets, NULL);
	//"Pop" the old frame layout
	localVarFrameOffsets = oldOffsets;

	for(long f=0;f!=strGraphNodeIRPSize(funcsWithin);f++) {
			IRCompile(funcsWithin[f],1);
	}
}
static int isPtrType(struct object *obj) {
	__auto_type type = objectBaseType(obj)->type;
	return type == TYPE_PTR || type == TYPE_ARRAY;}

static int isPtrNode(graphNodeIR start) {
	return isPtrType(IRNodeType(start));
}
static int isFltType(struct object *obj) {
	return objectBaseType(obj) == &typeF64;
}
static int isFltNode(graphNodeIR start) {
	return isFltType(IRNodeType(start));
}
static int isIntType(struct object *obj) {
	const struct object *intTypes[] = {
	    &typeI8i, &typeI16i, &typeI32i, &typeI64i, &typeU8i, &typeU16i, &typeU32i, &typeU64i,
	};
	for (long i = 0; i != sizeof(intTypes) / sizeof(*intTypes); i++)
		if (objectBaseType(obj) == intTypes[i])
			return 1;
	return 0;
}
static int isIntNode(graphNodeIR start) {
	return isIntType(IRNodeType(start));
}

STR_TYPE_DEF(long, Long);
STR_TYPE_FUNCS(long, Long);
static struct X86AddressingMode *demoteAddrMode(struct X86AddressingMode *addr, struct object *type) {
	__auto_type mode = X86AddrModeClone(addr);
	switch (mode->type) {
	case X86ADDRMODE_REG: {
		__auto_type subReg = subRegOfType(mode->value.reg, type);
		if (!subReg)
			return NULL;
		mode->value.reg = subReg;
		break;
	}
	case X86ADDRMODE_STR:
	case X86ADDRMODE_FLT:
	case X86ADDRMODE_ITEM_ADDR:
	case X86ADDRMODE_LABEL:
	case X86ADDRMODE_MEM:
	case X86ADDRMODE_SINT:
	case X86ADDRMODE_UINT:
		mode->valueType = type;
		break;
	}
	return mode;
}
static void setCond(const char *cond, struct X86AddressingMode *oMode) {
	struct X86AddressingMode *zero CLEANUP(X86AddrModeDestroy) = X86AddrModeSint(0);
	struct X86AddressingMode *oMode2 CLEANUP(X86AddrModeDestroy) = demoteAddrMode(oMode, &typeI8i);
	// Not all modes can be demoted
	if (!oMode2) {
		char buffer[32];
		strChar t CLEANUP(strCharDestroy) = uniqueLabel(NULL);
		strChar end CLEANUP(strCharDestroy) = uniqueLabel(NULL);
		sprintf(buffer, "J%s", cond);
		strX86AddrMode tLab CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeLabel(t));
		strX86AddrMode endLab CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeLabel(end));
		struct X86AddressingMode *one CLEANUP(X86AddrModeDestroy) = X86AddrModeSint(1);
		// Jcc t
		// oMode=0
		// JMP end
		// t:
		// oMode=1
		// end:
		assembleInst(buffer, tLab);
		asmAssign(oMode, zero, objectSize(oMode->valueType, NULL));
		assembleInst("JMP", endLab);
		X86EmitAsmLabel(t);
		asmAssign(oMode, one, objectSize(oMode->valueType, NULL));
		X86EmitAsmLabel(end);
	} else {
		strX86AddrMode setccArgs CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeClone(oMode2));
		char buffer[32];
		sprintf(buffer, "SET%s", cond);

		asmAssign(oMode, zero, objectSize(oMode->valueType, NULL));
		assembleInst(buffer, setccArgs);
	}
}
static void compileX87Expr(graphNodeIR start) {
	strGraphNodeIRP execOrder CLEANUP(strGraphNodeIRPDestroy) = NULL;
	strGraphNodeIRP stack = strGraphNodeIRPAppendItem(NULL, start);
	const struct reg *fpuRegisters[] = {
	    &regX86ST0, &regX86ST1, &regX86ST2, &regX86ST3, &regX86ST4, &regX86ST5, &regX86ST6, &regX86ST7,
	};
	strX86AddrMode addrModeStack = NULL;
	while (strGraphNodeIRPSize(stack)) {
		graphNodeIR node;
		stack = strGraphNodeIRPPop(stack, &node);
		strGraphEdgeIRP in CLEANUP(strGraphEdgeIRPDestroy) = graphNodeIRIncoming(node);
		strGraphEdgeIRP dst CLEANUP(strGraphEdgeIRPDestroy) = IRGetConnsOfType(in, IR_CONN_DEST);
		if (strGraphEdgeIRPSize(dst)) {
			__auto_type inNode = graphEdgeIRIncoming(dst[0]);
			const char *instName = NULL;
			switch (graphNodeIRValuePtr(inNode)->type) {
			case IR_INC:
				goto unop;
			case IR_DEC:
				goto unop;
			case IR_ADD:
				goto binop;
			case IR_SUB:
				goto binop;
			case IR_POS:
				goto unop;
			case IR_NEG:
				goto unop;
			case IR_MULT:
				goto binop;
			case IR_DIV:
				goto binop;
			case IR_POW:
				goto binop;
			case IR_GT:
				goto binop;
			case IR_LT:
				goto binop;
			case IR_GE:
				goto binop;
			case IR_LE:
				goto binop;
			case IR_EQ:
				goto binop;
			case IR_NE:
				goto binop;
			case IR_VALUE:
				execOrder = strGraphNodeIRPAppendItem(execOrder, node);
				continue;
			case IR_TYPECAST:
				execOrder = strGraphNodeIRPAppendItem(execOrder, node);
				continue;
			default:
				assert(0);
			}
		unop : {
			strGraphEdgeIRP arg CLEANUP(strGraphEdgeIRPDestroy) = IRGetConnsOfType(in, IR_CONN_SOURCE_A);
			execOrder = strGraphNodeIRPAppendItem(execOrder, node);
			stack = strGraphNodeIRPAppendItem(stack, graphEdgeIRIncoming(arg[0]));
			continue;
		}
		binop : {
			strGraphEdgeIRP argA CLEANUP(strGraphEdgeIRPDestroy) = IRGetConnsOfType(in, IR_CONN_SOURCE_A);
			execOrder = strGraphNodeIRPAppendItem(execOrder, node);
			stack = strGraphNodeIRPAppendItem(stack, graphEdgeIRIncoming(argA[0]));

			strGraphEdgeIRP argB CLEANUP(strGraphEdgeIRPDestroy) = IRGetConnsOfType(in, IR_CONN_SOURCE_B);
			stack = strGraphNodeIRPAppendItem(stack, graphEdgeIRIncoming(argB[0]));
			continue;
		}
		}
	}
	execOrder = strGraphNodeIRPReverse(execOrder);
	for (long i = 0; i != strGraphNodeIRPSize(execOrder); i++) {
		strGraphEdgeIRP out CLEANUP(strGraphEdgeIRPDestroy) = graphNodeIROutgoing(execOrder[i]);
		strGraphEdgeIRP dst CLEANUP(strGraphEdgeIRPDestroy) = IRGetConnsOfType(out, IR_CONN_DEST);
		switch (graphNodeIRValuePtr(execOrder[i])->type) {
		case IR_INC: {
			assembleInst("FLD1", NULL);
			assembleInst("FADDP", NULL);
			continue;
		}
		case IR_DEC: {
			assembleInst("FLD1", NULL);
			assembleInst("FSUBP", NULL);
			continue;
		}
		case IR_ADD: {
			assembleInst("FADDP", NULL);
			continue;
		}
		case IR_SUB: {
			assembleInst("FSUBP", NULL);
			continue;
		}
		case IR_POS: {
			continue;
		}
		case IR_NEG: {
			assembleInst("FCHS", NULL);
			continue;
		}
		case IR_MULT: {
			assembleInst("FMULP", NULL);
			continue;
		}
		case IR_DIV: {
			assembleInst("FDIVP", NULL);
			continue;
		}
		case IR_POW: {
			// TODO
			assembleInst("FMULP", NULL);
			continue;
		}
		case IR_GT: {
			assembleInst("FCOM", NULL);
			struct X86AddressingMode *oMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(graphEdgeIROutgoing(dst[0]));
			continue;
		}
		case IR_LT: {
			assembleInst("FCOM", NULL);
			strX86AddrMode setccArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
			struct X86AddressingMode *oMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(graphEdgeIROutgoing(dst[0]));
			setCond("L", oMode);
			continue;
		}
		case IR_GE: {
			assembleInst("FCOM", NULL);
			strX86AddrMode setccArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
			struct X86AddressingMode *oMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(graphEdgeIROutgoing(dst[0]));
			setCond("GE", oMode);
			continue;
		}
		case IR_LE: {
			assembleInst("FCOM", NULL);
			strX86AddrMode setccArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
			struct X86AddressingMode *oMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(graphEdgeIROutgoing(dst[0]));
			setCond("LE", oMode);
			continue;
		}
		case IR_EQ: {
			assembleInst("FCOM", NULL);
			strX86AddrMode setccArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
			struct X86AddressingMode *oMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(graphEdgeIROutgoing(dst[0]));
			setCond("E", oMode);
			continue;
		}
		case IR_NE: {
			assembleInst("FCOM", NULL);
			strX86AddrMode setccArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
			struct X86AddressingMode *oMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(graphEdgeIROutgoing(dst[0]));
			setCond("NE", oMode);
			continue;
		}
		case IR_VALUE: {
			struct X86AddressingMode *b CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(execOrder[i]);
			if (b->type == X86ADDRMODE_FLT || b->type == X86ADDRMODE_UINT || b->type == X86ADDRMODE_SINT) {
				double value;
				if (b->type == X86ADDRMODE_SINT) {
					value = b->value.sint;
				} else if (b->type == X86ADDRMODE_UINT) {
					value = b->value.uint;
				} else if (b->type == X86ADDRMODE_FLT) {
					value = b->value.flt;
				}
				struct X86AddressingMode *enc CLEANUP(X86AddrModeDestroy) = X86AddrModeUint(IEEE754Encode(b->value.flt));
				strX86AddrMode args CLEANUP(strX86AddrModeDestroy2) = NULL;
				args = strX86AddrModeAppendItem(NULL, X86AddrModeClone(b));

				// X86EmitAsmDU64 Takes array arg,so take pointer to emulate array
				struct X86AddressingMode *addr = X86EmitAsmDU64(&enc, 1);
				addr->valueType = &typeU64i;
				args = strX86AddrModeAppendItem(args, addr);
				assembleInst("FLD", args);
			} else if (b->type == X86ADDRMODE_REG) {
				if (isX87FltReg(b->value.reg)) {
					strX86AddrMode args CLEANUP(strX86AddrModeDestroy2) = NULL;
					args = strX86AddrModeAppendItem(NULL, X86AddrModeClone(b));
					assembleInst("FLD", args);
				} else if (isGPReg(b->value.reg)) {
					strX86AddrMode args CLEANUP(strX86AddrModeDestroy2) = NULL;
					args = strX86AddrModeAppendItem(NULL, X86AddrModeClone(b));
					assembleInst("FILD", args);
				} else {
					assert(0);
				}
			}
			continue;
		}
		case IR_TYPECAST: {
			strGraphEdgeIRP in CLEANUP(strGraphEdgeIRPDestroy) = graphNodeIRIncoming(execOrder[i]);
			strGraphEdgeIRP src CLEANUP(strGraphEdgeIRPDestroy) = IRGetConnsOfType(in, IR_CONN_SOURCE_A);
			__auto_type inNode = graphEdgeIRIncoming(src[0]);
			if (isIntNode(inNode) || isPtrNode(inNode)) {
				long size = objectSize(IRNodeType(inNode), NULL);
				switch (size) {
				case 1: {
					__auto_type tmpReg = regForTypeExcludingConsumed(&typeI64i);
					// PUSH g16,
					// MOVSX reg16,rm8
					// FILD reg16
					// POP reg16

					// PUSH
					strX86AddrMode ppArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
					ppArgs = strX86AddrModeAppendItem(ppArgs, X86AddrModeReg(tmpReg));
					assembleInst("PUSH", ppArgs);
					// MOVSX
					strX86AddrMode movsxArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
					movsxArgs = strX86AddrModeAppendItem(movsxArgs, X86AddrModeReg(tmpReg));
					movsxArgs = strX86AddrModeAppendItem(movsxArgs, IRNode2AddrMode(inNode));
					assembleInst("MOVSX", movsxArgs);
					// FILD reg16
					assembleInst("FILD", ppArgs);
					// POP reg16
					assembleInst("POP", ppArgs);
					break;
				}
				case 2:
				case 4:
				case 8: {
					strX86AddrMode fildArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
					fildArgs = strX86AddrModeAppendItem(fildArgs, IRNode2AddrMode(inNode));
					assembleInst("FILD", fildArgs);
					break;
				}
				}
			} else if (isFltNode(inNode)) {
			}
			continue;
		}
		default:
			assert(0);
		}
	}
}
static strGraphNodeIRP nextNodesToCompile(graphNodeIR node) {
	strGraphEdgeIRP out CLEANUP(strGraphEdgeIRPDestroy) = graphNodeIROutgoing(node);
	strGraphEdgeIRP flow CLEANUP(strGraphEdgeIRPDestroy) = IRGetConnsOfType(out, IR_CONN_FLOW);
	strGraphNodeIRP retVal = strGraphNodeIRPResize(NULL, strGraphEdgeIRPSize(flow));
	for (long e = 0; e != strGraphEdgeIRPSize(flow); e++)
		retVal[e] = graphEdgeIROutgoing(flow[e]);
	qsort(retVal, strGraphEdgeIRPSize(flow), (sizeof *retVal), ptrPtrCmp);
	return retVal;
}

static strGraphNodeIRP compileX87IfNeded(graphNodeIR start, int *compiled) {
	if (compiled)
		*compiled = 0;
	if (isFltNode(start)) {
		switch (getCurrentArch()) {
		case ARCH_TEST_SYSV:
		case ARCH_X86_SYSV: {
			if (compiled)
				*compiled = 1;
			compileX87Expr(start);
			return nextNodesToCompile(start);
		}
		case ARCH_X64_SYSV:;
		}
	}
	return NULL;
}
static int typeIsSigned(struct object *obj) {
	const struct object *signedTypes[] = {
	    &typeI8i, &typeI16i, &typeI32i, &typeI64i, &typeF64,
	};
	for (int i = 0; i != sizeof(signedTypes) / sizeof(*signedTypes); i++) {
		if (objectBaseType(obj) == signedTypes[i])
			return 1;
	}
	return 0;
}
static void __typecastSignExt(struct X86AddressingMode *outMode, struct X86AddressingMode *inMode) {
	long iSize = objectSize(inMode->valueType, NULL);
	long oSize = objectSize(outMode->valueType, NULL);
	AUTO_LOCK_MODE_REGS(inMode);
	AUTO_LOCK_MODE_REGS(outMode);

	strX86AddrMode ppArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
	struct reg *dumpToReg = NULL;
	// PUSH reg if assigning to non-reg dest(movsx needs reg as dst)
	if (outMode->type != X86ADDRMODE_REG) {
		__auto_type tmpReg = regForTypeExcludingConsumed(&typeI64i);
		ppArgs = strX86AddrModeAppendItem(ppArgs, X86AddrModeReg(tmpReg));
		dumpToReg = tmpReg;
		assembleInst("PUSH", ppArgs);
	} else {
		dumpToReg = outMode->value.reg;
	}
	strX86AddrMode movArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
	movArgs = strX86AddrModeAppendItem(movArgs, X86AddrModeReg(dumpToReg));
	movArgs = strX86AddrModeAppendItem(movArgs, X86AddrModeClone(inMode));
	if (oSize == 8 && iSize == 4) {
		assembleInst("MOVSX", movArgs);
	} else {
		assembleInst("MOVSXD", movArgs);
	}
	struct X86AddressingMode *dumpToRegMode CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(dumpToReg);
	asmAssign(outMode, dumpToRegMode, objectSize(outMode->valueType, NULL));
	// POP reg if assigning to non-reg dest(movsx needs reg as dst)
	if (outMode->type != X86ADDRMODE_REG) {
		assembleInst("POP", ppArgs);
	}
}
static int addrModeConflict(struct X86AddressingMode *a,struct X86AddressingMode *b) {
		strRegP aModeRegs CLEANUP(strRegPDestroy)=regsFromMode(a);
		strRegP bModeRegs CLEANUP(strRegPDestroy)=regsFromMode(b);
		for(long A=0;A!=strRegPSize(aModeRegs);A++)
				for(long B=0;B!=strRegPSize(bModeRegs);B++)
						if(regConflict(aModeRegs[A], bModeRegs[B]))
								return 1;
		return 0;
}
static graphNodeIR assembleOpIntLogical(graphNodeIR start, const char *suffix) {
		graphNodeIR a, b, out = nodeDest(start);
		binopArgs(start, &a, &b);
		struct X86AddressingMode *aMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(a);
		struct X86AddressingMode *bMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(b);

		if(aMode->type!=X86ADDRMODE_REG&&bMode->type!=X86ADDRMODE_REG) {
				struct X86AddressingMode *oMode CLEANUP(X86AddrModeDestroy)=IRNode2AddrMode(out);
				AUTO_LOCK_MODE_REGS(bMode);
				AUTO_LOCK_MODE_REGS(oMode);
				__auto_type reg=regForTypeExcludingConsumed(IRNodeType(a));
				pushReg(reg);
				struct X86AddressingMode *regMode CLEANUP(X86AddrModeDestroy)=X86AddrModeReg(reg);
				asmAssign(regMode, aMode, objectSize(IRNodeType(start), NULL));
				strX86AddrMode cmpArgs CLEANUP(strX86AddrModeDestroy2)=NULL;
				cmpArgs=strX86AddrModeAppendItem(cmpArgs, X86AddrModeReg(reg));
				cmpArgs=strX86AddrModeAppendItem(cmpArgs, X86AddrModeClone(bMode));
				assembleInst("CMP", cmpArgs);
				setCond(suffix, oMode);
				popReg(reg);
		} else {
				struct X86AddressingMode *oMode CLEANUP(X86AddrModeDestroy)=IRNode2AddrMode(out);
				//One must be a register if reaching here
				__auto_type regMode=aMode->type==X86ADDRMODE_REG?aMode:bMode;
				__auto_type other=aMode->type==X86ADDRMODE_REG?bMode:aMode;
				strX86AddrMode cmpArgs CLEANUP(strX86AddrModeDestroy2)=NULL;
				cmpArgs=strX86AddrModeAppendItem(cmpArgs, X86AddrModeClone(regMode));
				cmpArgs=strX86AddrModeAppendItem(cmpArgs, X86AddrModeClone(other));
				assembleInst("CMP", cmpArgs);
				setCond(suffix, oMode);
		}
		return out;
}
static graphNodeIR assembleOpInt(graphNodeIR start, const char *opName) {
	graphNodeIR a, b, out = nodeDest(start);
	binopArgs(start, &a, &b);
	struct X86AddressingMode *aMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(a);
	AUTO_LOCK_MODE_REGS(aMode);
	struct X86AddressingMode *bMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(b);
	AUTO_LOCK_MODE_REGS(bMode);
	struct X86AddressingMode *oMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(out);
	int hasReg = isReg(a)||isReg(b);
	if (hasReg && out) {
		// Load a into out then OP DEST,SRC as DEST=DEST OP SRC if sizeof(DEST)==sizeof(a)
		long oSize = objectSize(IRNodeType(out), NULL);
		long aSize = objectSize(IRNodeType(a), NULL);
		long bSize = objectSize(IRNodeType(b), NULL);
		assert(aSize == bSize && aSize == oSize);

		//Check if a/b conflicts with dest,if so use a tempory variable
		if(addrModeConflict(aMode,oMode)||addrModeConflict(bMode,oMode)) {
				AUTO_LOCK_MODE_REGS(bMode);
				AUTO_LOCK_MODE_REGS(oMode);
				__auto_type reg=regForTypeExcludingConsumed(IRNodeType(start));
				pushReg(reg);
				struct X86AddressingMode *regMode CLEANUP(X86AddrModeDestroy)=X86AddrModeReg(reg);
				asmAssign(regMode, aMode,reg->size);

				strX86AddrMode opArgs CLEANUP(strX86AddrModeDestroy) = strX86AddrModeAppendItem(NULL, X86AddrModeClone(regMode));
				opArgs = strX86AddrModeAppendItem(opArgs, X86AddrModeClone(bMode));
				assembleInst(opName, opArgs);
				
				asmAssign(oMode, regMode,reg->size);
				popReg(reg);
				return out;
		}
		
		asmAssign(oMode, aMode, objectSize(IRNodeType(out), NULL));

		strX86AddrMode opArgs CLEANUP(strX86AddrModeDestroy) = strX86AddrModeAppendItem(NULL, IRNode2AddrMode(out));
		opArgs = strX86AddrModeAppendItem(opArgs, X86AddrModeClone(bMode));
		assembleInst(opName, opArgs);
	} else if (out && !hasReg) {
		long oSize = objectSize(IRNodeType(out), NULL);
		long aSize = objectSize(IRNodeType(a), NULL);
		long bSize = objectSize(IRNodeType(b), NULL);
		assert(aSize == bSize && aSize == oSize);

		// Pick a register to store the result in,then push/pop that register
		__auto_type tmpReg = regForTypeExcludingConsumed(IRNodeType(out));
		struct X86AddressingMode *regMode CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(tmpReg);
		strX86AddrMode ppArgs CLEANUP(strX86AddrModeDestroy) = strX86AddrModeAppendItem(NULL, X86AddrModeClone(regMode));
		assembleInst("PUSH", ppArgs);

		// tmpReg=a;
		asmAssign(regMode, aMode, objectSize(IRNodeType(out), NULL));
		// OP tmpReg,b
		strX86AddrMode opArgs CLEANUP(strX86AddrModeDestroy) = strX86AddrModeAppendItem(NULL, X86AddrModeClone(regMode));
		opArgs = strX86AddrModeAppendItem(opArgs, X86AddrModeClone(bMode));
		assembleInst(opName, opArgs);
		// out=tmpReg
		asmAssign(oMode, regMode, objectSize(IRNodeType(out), NULL));

		assembleInst("POP", ppArgs);
	}
	return out;
}
static int interferesWithConsumedReg(struct reg *r) {
	for (long i = 0; i != strRegPSize(consumedRegisters); i++)
		if (regConflict(r, consumedRegisters[i]))
			return 1;
	return 0;
}
void asmTypecastAssign(struct X86AddressingMode *outMode, struct X86AddressingMode *inMode) {
	switch (inMode->type) {
	case X86ADDRMODE_FLT: {
		if (isIntType(outMode->valueType) || isPtrType(outMode->valueType)) {
			int64_t value = inMode->value.flt;
			asmAssign(outMode, X86AddrModeSint(value), objectSize(outMode->valueType, NULL));
		} else if (isFltType(inMode->valueType)) {
			asmAssign(outMode, inMode, objectSize(outMode->valueType, NULL));
		} else
			assert(0);
		return;
	}
	case X86ADDRMODE_MEM:
	case X86ADDRMODE_LABEL:
	case X86ADDRMODE_ITEM_ADDR:
	case X86ADDRMODE_REG: {
		// If destination is bigger than source,sign extend if dest is signed
		if (isPtrType(outMode->valueType) || isIntType(outMode->valueType)) {
			long iSize = objectSize(inMode->valueType, NULL);
			long oSize = objectSize(outMode->valueType, NULL);
			if (oSize > iSize) {
				if (typeIsSigned(inMode->valueType)) {
					__typecastSignExt(outMode, inMode);
				} else {
					strX86AddrMode ppArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
					struct reg *dumpToReg = NULL;
					// PUSH reg if assigning to non-reg dest(movzx needs reg as dst)
					if (outMode->type != X86ADDRMODE_REG) {
						__auto_type tmpReg = regForTypeExcludingConsumed(outMode->valueType);
						ppArgs = strX86AddrModeAppendItem(ppArgs, X86AddrModeReg(tmpReg));
						dumpToReg = tmpReg;
						assembleInst("PUSH", ppArgs);
					} else {
						dumpToReg = outMode->value.reg;
					}

					strX86AddrMode movzxArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
					struct X86AddressingMode *dumpToRegMode CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(dumpToReg);
					AUTO_LOCK_MODE_REGS(dumpToRegMode);

					movzxArgs = strX86AddrModeAppendItem(movzxArgs, X86AddrModeClone(dumpToRegMode));
					movzxArgs = strX86AddrModeAppendItem(movzxArgs, X86AddrModeClone(inMode));
					assembleInst("MOVZX", movzxArgs);
					asmAssign(outMode, dumpToRegMode, objectSize(outMode->valueType, NULL));

					// POP reg if assigning to non-reg dest(movzx needs reg as dst)
					if (outMode->type != X86ADDRMODE_REG) {
						assembleInst("POP", ppArgs);
					}
				}
				return;
			} else if (iSize > oSize) {
				struct X86AddressingMode *mode CLEANUP(X86AddrModeDestroy) = demoteAddrMode(outMode, outMode->valueType);
				if (!mode) {
					// Cant demote current mode,so use RAX register as accumatior(which can be demoted)
					struct X86AddressingMode *rax CLEANUP(X86AddrModeDestroy) = NULL;
					switch (getCurrentArch()) {
					case ARCH_X64_SYSV:
						rax = X86AddrModeReg(&regAMD64RAX);
						break;
					case ARCH_X86_SYSV:
					case ARCH_TEST_SYSV:
						rax = X86AddrModeReg(&regX86EAX);
						break;
					}
					struct X86AddressingMode *demoted2Out CLEANUP(X86AddrModeDestroy) = demoteAddrMode(rax, outMode->valueType);
					struct X86AddressingMode *demoted2In CLEANUP(X86AddrModeDestroy) = demoteAddrMode(rax, inMode->valueType);

					strX86AddrMode ppArgs CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeClone(demoted2Out));
					assembleInst("PUSH", ppArgs);

					struct X86AddressingMode *zero CLEANUP(X86AddrModeDestroy) = X86AddrModeSint(0);
					asmAssign(demoted2Out, zero, objectSize(outMode->valueType, NULL));
					asmAssign(demoted2In, inMode, objectSize(inMode->valueType, NULL));
					asmAssign(outMode, demoted2Out, objectSize(outMode->valueType, NULL));

					assembleInst("POP", ppArgs);
				} else {
					asmAssign(outMode, mode, objectSize(outMode->valueType, NULL));
				}
				return;
			} else {
					asmAssign(outMode, inMode, iSize);
			}
		} else if (isFltType(outMode->valueType)) {
				// Assign handles flt<-int
				asmAssign(outMode, inMode, objectSize(outMode->valueType, NULL));
		} else {
				__auto_type base=objectBaseType(outMode->valueType);
				if(base->type==TYPE_CLASS||base->type==TYPE_UNION) {
						asmAssign(outMode, inMode, objectSize(base, NULL));
				} else 
						assert(0);
		};
		return;
	}
	case X86ADDRMODE_UINT:
	case X86ADDRMODE_SINT:
		asmAssign(outMode, inMode, objectSize(outMode->valueType, NULL));
		return;
	}
	return;
}
static struct reg *addrModeReg(struct X86AddressingMode *mode) {
	if (mode->type != X86ADDRMODE_REG)
		return NULL;
	return mode->value.reg;
}
static int IRTableRangeCmp(const struct IRJumpTableRange *a, const struct IRJumpTableRange *b) {
	if (a->start > b->start)
		return 1;
	else if (a->start < b->start)
		return -1;
	else
		return 0;
}
static void storeMemberPtrInReg(struct reg *memReg,graphNodeIR sourceNode,strObjectMember members) {
		struct X86AddressingMode *memRegMode CLEANUP(X86AddrModeDestroy)=X86AddrModeReg(memReg);
		strX86AddrMode leaArgs CLEANUP(strX86AddrModeDestroy2)=NULL;
			leaArgs=strX86AddrModeAppendItem(leaArgs, X86AddrModeReg(memReg));
			leaArgs=strX86AddrModeAppendItem(leaArgs, IRNode2AddrMode(sourceNode));
			leaArgs[1]->valueType=NULL;
			assembleInst("LEA", leaArgs);

			__auto_type currentType=IRNodeType(sourceNode);
			long currentOffset=0;
			for(long m=0;m!=strObjectMemberSize(members);m++) {
					if(currentType->type==TYPE_PTR) {
							if(currentOffset) {
									strX86AddrMode addArgs CLEANUP(strX86AddrModeDestroy2)=NULL;
									addArgs=strX86AddrModeAppendItem(addArgs, X86AddrModeReg(memReg));
									addArgs=strX86AddrModeAppendItem(addArgs, X86AddrModeSint(currentOffset));
									assembleInst("ADD", addArgs);
									currentOffset=0;
							}
							
							//De-reference
							struct X86AddressingMode *indir CLEANUP(X86AddrModeDestroy)=X86AddrModeIndirReg(memReg,objectPtrCreate(&typeU0));
							asmAssign(memRegMode, indir, ptrSize());
					}
					memRegMode->valueType=objectPtrCreate(members[m].type);
					currentOffset+=members[m].offset;
					currentType=members[m].type;
			}
			if(currentOffset) {
					strX86AddrMode addArgs CLEANUP(strX86AddrModeDestroy2)=NULL;
					addArgs=strX86AddrModeAppendItem(addArgs, X86AddrModeReg(memReg));
					addArgs=strX86AddrModeAppendItem(addArgs, X86AddrModeSint(currentOffset));
					assembleInst("ADD", addArgs);
			}
}
static strGraphNodeIRP __IR2Asm(graphNodeIR start) {
	if (ptrMapCompiledNodesGet(compiledNodes, start)) {
		// If encountering already "compiled" label node,jump to it
			if (graphNodeIRValuePtr(start)->type == IR_LABEL) {
			strX86AddrMode jmpArgs CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeLabel(getLabelName(start)));
			assembleInst("JMP", jmpArgs);
		}
		return NULL;
	}
	ptrMapCompiledNodesAdd(compiledNodes, start, 1);
	
	strGraphEdgeIRP out CLEANUP(strGraphEdgeIRPDestroy) = graphNodeIROutgoing(start);
	switch (graphNodeIRValuePtr(start)->type) {
	case IR_MEMBERS_ADDR_OF: {
			strGraphEdgeIRP in CLEANUP(strGraphEdgeIRPDestroy)=graphNodeIRIncoming(start);
			strGraphEdgeIRP inSource CLEANUP(strGraphEdgeIRPDestroy)=IRGetConnsOfType(in, IR_CONN_SOURCE_A);
			__auto_type sourceNode=graphEdgeIRIncoming(inSource[0]);
			
			struct IRNodeMembersAddrOf *mems=(void*)graphNodeIRValuePtr(start);
			strGraphEdgeIRP outAssn CLEANUP(strGraphEdgeIRPDestroy)=IRGetConnsOfType(out, IR_CONN_DEST);
			if(strGraphEdgeIRPSize(outAssn)==1) {
					struct X86AddressingMode *oMode CLEANUP(X86AddrModeDestroy)=IRNode2AddrMode(graphEdgeIROutgoing(outAssn[0]));
					if(oMode->type==X86ADDRMODE_REG) {
							storeMemberPtrInReg(oMode->value.reg, sourceNode, mems->members);
					} else {
							AUTO_LOCK_MODE_REGS(oMode);
							struct reg *r=regForTypeExcludingConsumed(objectPtrCreate(&typeU0));
							pushReg(r);
							storeMemberPtrInReg(r, sourceNode, mems->members);
							struct X86AddressingMode *regMode CLEANUP(X86AddrModeDestroy)=X86AddrModeReg(r);
							regMode->valueType=IRNodeType(graphEdgeIROutgoing(outAssn[0]));
							asmTypecastAssign(oMode,  regMode);
							popReg(r);
					}
			}
			return nextNodesToCompile(start);
	}
	case IR_ADD: {
#define COMPILE_87_IFN                                                                                                                                             \
	({                                                                                                                                                               \
		int compiled;                                                                                                                                                  \
		__auto_type tmp = compileX87IfNeded(start, &compiled);                                                                                                         \
		if (compiled)                                                                                                                                                  \
			return tmp;                                                                                                                                                  \
	})
		COMPILE_87_IFN;
		graphNodeIR a, b;
		binopArgs(start, &a, &b);
		if (isIntNode(a) || isPtrNode(a))
			return nextNodesToCompile(assembleOpInt(start, "ADD"));
		else
			assert(0);
	}
	case IR_ADDR_OF: {
			strGraphEdgeIRP out CLEANUP(strGraphEdgeIRPDestroy)=graphNodeIROutgoing(start);
			strGraphEdgeIRP dst CLEANUP(strGraphEdgeIRPDestroy)=IRGetConnsOfType(out, IR_CONN_DEST);
			if(strGraphEdgeIRPSize(dst)==1) {
					strGraphEdgeIRP in CLEANUP(strGraphEdgeIRPDestroy)=graphNodeIRIncoming(start);
					strGraphEdgeIRP source CLEANUP(strGraphEdgeIRPDestroy)=IRGetConnsOfType(in, IR_CONN_SOURCE_A);
			struct X86AddressingMode *iMode CLEANUP(X86AddrModeDestroy)=IRNode2AddrMode(graphEdgeIRIncoming(source[0]));
			struct X86AddressingMode *oMode CLEANUP(X86AddrModeDestroy)=IRNode2AddrMode(graphEdgeIROutgoing(dst[0]));
			AUTO_LOCK_MODE_REGS(iMode);
			if(iMode->type==X86ADDRMODE_MEM||iMode->type==X86ADDRMODE_ITEM_ADDR) {
					__auto_type regAddr=regForTypeExcludingConsumed(objectPtrCreate(&typeU0));
					struct X86AddressingMode *regAddrMode CLEANUP(X86AddrModeDestroy)=X86AddrModeReg(regAddr);
					pushReg(regAddr);
					strX86AddrMode leaArgs CLEANUP(strX86AddrModeDestroy2)=NULL;
					leaArgs=strX86AddrModeAppendItem(leaArgs, X86AddrModeReg(regAddr));
					iMode->valueType=NULL;
					leaArgs=strX86AddrModeAppendItem(leaArgs, X86AddrModeClone(iMode));
					assembleInst("LEA", leaArgs);
					
					asmAssign(oMode, regAddrMode, ptrSize());
					popReg(regAddr);
			} else {
					fputs("IR_ADDR_OF needs an item that points to something", stderr);
					abort();
			}
			}
			return nextNodesToCompile(start);
	}
	case IR_ARRAY:
	case IR_TYPECAST: {
		strGraphEdgeIRP in CLEANUP(strGraphEdgeIRPDestroy) = graphNodeIRIncoming(start);
		__auto_type inNode = graphEdgeIRIncoming(in[0]);
		strGraphEdgeIRP out CLEANUP(strGraphEdgeIRPDestroy) = graphNodeIROutgoing(start);
		assert(strGraphEdgeIRPSize(out) == 1);
		__auto_type outNode = graphEdgeIROutgoing(out[0]);
		struct X86AddressingMode *aMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(inNode);
		AUTO_LOCK_MODE_REGS(aMode);
		struct X86AddressingMode *oMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(outNode);
		AUTO_LOCK_MODE_REGS(oMode);
		asmTypecastAssign(oMode, aMode);
		return nextNodesToCompile(outNode);
	}
	case IR_STATEMENT_START: {
		struct IRNodeStatementStart *stmtStart = (void *)graphNodeIRValuePtr(start);
		IR2Asm(stmtStart->end);
		return nextNodesToCompile(stmtStart->end);
	}
	case IR_STATEMENT_END: {
		strGraphNodeIRP nodes CLEANUP(strGraphNodeIRPDestroy) = graphNodeIRIncomingNodes(start);
		assert(strGraphNodeIRPSize(nodes) == 1);
		assert(graphNodeIRValuePtr(nodes[0])->type != IR_STATEMENT_START);
		IR2Asm(nodes[0]);
		return nextNodesToCompile(nodes[0]);
	}
	case IR_INC: {
		COMPILE_87_IFN;

		strGraphEdgeIRP in CLEANUP(strGraphEdgeIRPDestroy) = graphNodeIRIncoming(start);
		__auto_type inNode = graphEdgeIRIncoming(in[0]);
		if (isIntNode(inNode)) {
			strX86AddrMode args CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, IRNode2AddrMode(inNode));
			assembleInst("INC", args);
		} else if (isPtrNode(inNode)) {
			struct objectPtr *ptr = (void *)IRNodeType(inNode);
			// ADD ptr,ptrSize
			struct X86AddressingMode *iMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(start);
			AUTO_LOCK_MODE_REGS(iMode);

			strX86AddrMode addArgs CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeClone(iMode));
			addArgs = strX86AddrModeAppendItem(addArgs, X86AddrModeUint(objectSize(ptr->type, NULL)));
			assembleInst("ADD", addArgs);
		} else
			assert(0);

		if (strGraphEdgeIRPSize(out))
			asmAssign(IRNode2AddrMode(graphEdgeIROutgoing(out[0])), IRNode2AddrMode(inNode), objectSize(IRNodeType(start), NULL));
		return strGraphNodeIRPAppendItem(NULL, nodeDest(start));
		;
		return NULL;
	}
	case IR_DEC: {
		COMPILE_87_IFN;

		strGraphEdgeIRP in CLEANUP(strGraphEdgeIRPDestroy) = graphNodeIRIncoming(start);
		__auto_type inNode = graphEdgeIRIncoming(in[0]);
		if (isIntNode(inNode)) {
			strX86AddrMode args CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, IRNode2AddrMode(inNode));
			assembleInst("DEC", args);
		} else if (isPtrNode(inNode)) {
			struct objectPtr *ptr = (void *)IRNodeType(inNode);
			// ADD ptr,ptrSize
			struct X86AddressingMode *iMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(start);
			AUTO_LOCK_MODE_REGS(iMode);

			strX86AddrMode addArgs CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeClone(iMode));
			addArgs = strX86AddrModeAppendItem(addArgs, X86AddrModeUint(objectSize(ptr->type, NULL)));
			assembleInst("SUB", addArgs);
		} else
			assert(0);

		if (strGraphEdgeIRPSize(out))
			asmAssign(IRNode2AddrMode(graphEdgeIROutgoing(out[0])), IRNode2AddrMode(inNode), objectSize(IRNodeType(start), NULL));
		return strGraphNodeIRPAppendItem(NULL, nodeDest(start));
	}
	case IR_SUB: {
		int compiled;
		__auto_type tmp = compileX87IfNeded(start, &compiled);
		if (compiled)
			return tmp;
		graphNodeIR a, b;
		binopArgs(start, &a, &b);
		// Are assumed to be same type if valid IR graph
		if (isIntNode(a) || isPtrNode(a))
			return strGraphNodeIRPAppendItem(NULL, assembleOpInt(start, "SUB"));
		assert(0);
		return NULL;
	}
	case IR_POS: {
		COMPILE_87_IFN;

		strGraphEdgeIRP in CLEANUP(strGraphEdgeIRPDestroy) = graphNodeIRIncoming(start);
		__auto_type inNode = graphEdgeIRIncoming(in[0]);
		asmAssign(IRNode2AddrMode(graphEdgeIROutgoing(out[0])), IRNode2AddrMode(inNode), objectSize(IRNodeType(start), NULL));
		return strGraphNodeIRPAppendItem(NULL, nodeDest(start));
	}
	case IR_NEG: {
		COMPILE_87_IFN;

		strGraphEdgeIRP in CLEANUP(strGraphEdgeIRPDestroy) = graphNodeIRIncoming(start);
		__auto_type inNode = graphEdgeIRIncoming(in[0]);
		if (isIntNode(inNode) || isPtrNode(inNode)) {
			// MOV dest,source
			// NOT dest
			__auto_type outNode = graphEdgeIROutgoing(out[0]);
			struct X86AddressingMode *oMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(outNode);
			AUTO_LOCK_MODE_REGS(oMode);
			struct X86AddressingMode *iMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(inNode);
			AUTO_LOCK_MODE_REGS(iMode);

			asmAssign(oMode, iMode, objectSize(IRNodeType(inNode), NULL));
			strX86AddrMode nArgs CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeClone(oMode));
			assembleInst("NEG", nArgs);

			return strGraphNodeIRPAppendItem(NULL, nodeDest(start));
		}
		assert(0);
	}
	case IR_MULT: {
		COMPILE_87_IFN;

		graphNodeIR a, b;
		binopArgs(start, &a, &b);
		// Are assumed to be same type if valid IR graph

		if (isIntNode(a) || isPtrNode(a)) {
			if (typeIsSigned(IRNodeType(a))) {
				assembleOpInt(start, "IMUL2");
			} else {
				struct X86AddressingMode *aMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(a);
				struct X86AddressingMode *bMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(b);
				//
				// RAX is assigned into by the first operand,so if the second operand if RAX,swap the order of a and b
				//
				if (bMode->type == X86ADDRMODE_REG) {
					if (regConflict(&regAMD64RAX, bMode->value.reg)) {
						__auto_type tmp = aMode;
						aMode = bMode;
						bMode = tmp;
					}
				}

				strX86AddrMode mulArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
				strX86AddrMode ppArgsA CLEANUP(strX86AddrModeDestroy2) = NULL;
				strX86AddrMode ppArgsD CLEANUP(strX86AddrModeDestroy2) = NULL;

				strRegP pushPopRegs CLEANUP(strRegPDestroy) = NULL;
				struct reg *outReg = NULL;

				long outSize = objectSize(IRNodeType(a), NULL);
				switch (outSize) {
				case 1: {
					struct X86AddressingMode *alMode CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(&regX86AL);
					asmAssign(alMode, aMode, 1);
					mulArgs = strX86AddrModeAppendItem(mulArgs, X86AddrModeClone(bMode));
					pushPopRegs = strRegPAppendItem(pushPopRegs, &regX86AX);
					outReg = &regX86AL;
					break;
				}
				case 2: {
					struct X86AddressingMode *axMode CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(&regX86AX);
					asmAssign(axMode, aMode, 2);
					mulArgs = strX86AddrModeAppendItem(mulArgs, X86AddrModeClone(bMode));
					pushPopRegs = strRegPAppendItem(pushPopRegs, &regX86AX);
					pushPopRegs = strRegPAppendItem(pushPopRegs, &regX86DX);
					outReg = &regX86AX;
					break;
				}
				case 4: {
					struct X86AddressingMode *eaxMode CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(&regX86EAX);
					asmAssign(eaxMode, aMode, 4);
					mulArgs = strX86AddrModeAppendItem(mulArgs, X86AddrModeClone(bMode));
					pushPopRegs = strRegPAppendItem(pushPopRegs, &regX86EAX);
					pushPopRegs = strRegPAppendItem(pushPopRegs, &regX86EDX);
					outReg = &regX86EAX;
					break;
				}
				case 8: {
					struct X86AddressingMode *raxMode CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(&regAMD64RAX);
					asmAssign(raxMode, aMode, 8);
					mulArgs = strX86AddrModeAppendItem(mulArgs, X86AddrModeClone(bMode));
					pushPopRegs = strRegPAppendItem(pushPopRegs, &regAMD64RAX);
					pushPopRegs = strRegPAppendItem(pushPopRegs, &regAMD64RDX);
					outReg = &regAMD64RAX;
					break;
				}
					assert(0);
				}

				// Make room on stack for  result
				strX86AddrMode addSPArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
				addSPArgs = strX86AddrModeAppendItem(addSPArgs, X86AddrModeSint(outSize));
				addSPArgs = strX86AddrModeAppendItem(addSPArgs, X86AddrModeReg(stackPointer()));
				assembleInst("ADD", addSPArgs);

				// Push uncomsumed registers that are affected
				for (long r = 0; r != strRegPSize(pushPopRegs); r++)
					pushReg(pushPopRegs[r]);

				struct X86AddressingMode *resRegMode CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(outReg);
				assembleInst("MUL", mulArgs);

				// Assign to area below pushed args
				long offset = 0;
				for (long r = 0; r != strRegPSize(pushPopRegs); r++)
					offset += pushPopRegs[r]->size;
				struct X86AddressingMode *resultPointer =
						X86AddrModeIndirSIB(0, 0, X86AddrModeReg((ptrSize() == 4 ? &regX86ESP : &regAMD64RSP)), X86AddrModeSint(-offset), IRNodeType(nodeDest(start)));
				asmAssign(resultPointer, resRegMode, outSize);

				// Pop uncomsumed registers that are affected
				for (long r = strRegPSize(pushPopRegs) - 1; r >= 0; r--)
					popReg(pushPopRegs[r]);

				struct X86AddressingMode *oMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(nodeDest(start));
				asmAssign(oMode, X86AddrModeIndirReg(stackPointer(), IRNodeType(nodeDest(start))), outSize);
				// Free room for result on stack
				assembleInst("SUB", addSPArgs);
			}
		}
		return strGraphNodeIRPAppendItem(NULL, nodeDest(start));
	}
	case IR_MOD:
	case IR_DIV: {
		COMPILE_87_IFN;

		graphNodeIR a, b;
		binopArgs(start, &a, &b);
		struct X86AddressingMode *outMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(graphEdgeIROutgoing(out[0]));
		AUTO_LOCK_MODE_REGS(outMode);
		struct reg *outReg = NULL;
		if (outMode->type == X86ADDRMODE_REG)
			outReg = outMode->value.reg;

			//
			// We will either use EAX or RAX if x86 or x86 respeectivly
			//
			// Also,we will not push/pop rdx or rax if it conflicts with the result register
			//
#define IF_NOT_CONFLICT(reg, code)                                                                                                                                 \
	({                                                                                                                                                               \
		if (out) {                                                                                                                                                     \
			if (!regConflict(&reg, outReg)) {                                                                                                                            \
				code;                                                                                                                                                      \
			}                                                                                                                                                            \
		} else {                                                                                                                                                       \
			code;                                                                                                                                                        \
		}                                                                                                                                                              \
	})

		strX86AddrMode ppRAX CLEANUP(strX86AddrModeDestroy2) = NULL;
		strX86AddrMode ppRDX CLEANUP(strX86AddrModeDestroy2) = NULL;
		switch (objectSize(IRNodeType(start), NULL)) {
		case 1:
			IF_NOT_CONFLICT(regX86AH, ppRAX = strX86AddrModeAppendItem(NULL, X86AddrModeReg(&regX86AH)));
			IF_NOT_CONFLICT(regX86AL, ppRDX = strX86AddrModeAppendItem(NULL, X86AddrModeReg(&regX86AL)));
			break;
		case 2:
			IF_NOT_CONFLICT(regX86AX, ppRAX = strX86AddrModeAppendItem(NULL, X86AddrModeReg(&regX86AX)));
			IF_NOT_CONFLICT(regX86DX, ppRDX = strX86AddrModeAppendItem(NULL, X86AddrModeReg(&regX86DX)));
			break;
		case 4:
			IF_NOT_CONFLICT(regX86EAX, ppRAX = strX86AddrModeAppendItem(NULL, X86AddrModeReg(&regX86EAX)));
			IF_NOT_CONFLICT(regX86EDX, ppRDX = strX86AddrModeAppendItem(NULL, X86AddrModeReg(&regX86EDX)));
			break;
		case 8:
			IF_NOT_CONFLICT(regAMD64RAX, ppRAX = strX86AddrModeAppendItem(NULL, X86AddrModeReg(&regAMD64RAX)));
			IF_NOT_CONFLICT(regAMD64RDX, ppRDX = strX86AddrModeAppendItem(NULL, X86AddrModeReg(&regAMD64RDX)));
			break;
		}
		if (ppRAX)
			assembleInst("PUSH", ppRAX), consumeRegFromMode(ppRAX[0]);
		if (ppRDX)
			assembleInst("PUSH", ppRDX), consumeRegFromMode(ppRDX[0]);
		strX86AddrMode movRaxArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
		strX86AddrMode dxorArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
		strX86AddrMode divArgs CLEANUP(strX86AddrModeDestroy2) = NULL;

		struct X86AddressingMode *aMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(a);
		AUTO_LOCK_MODE_REGS(aMode);
		struct X86AddressingMode *bMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(b);
		AUTO_LOCK_MODE_REGS(bMode);

		const char *op = typeIsSigned(IRNodeType(start)) ? "IDIV" : "DIV";
		int isDivOrMod = graphNodeIRValuePtr(start)->type == IR_DIV;
		switch (objectSize(IRNodeType(start), NULL)) {
		case 1:
			movRaxArgs = strX86AddrModeAppendItem(movRaxArgs, X86AddrModeReg(&regX86AX));
			movRaxArgs = strX86AddrModeAppendItem(movRaxArgs, X86AddrModeClone(aMode));
			divArgs = strX86AddrModeAppendItem(divArgs, X86AddrModeClone(bMode));
			assembleInst("MOV", movRaxArgs);
			assembleInst(op, divArgs);
			if (isDivOrMod) {
				struct X86AddressingMode *al CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(&regX86AL);
				asmAssign(outMode, al, 1);
			} else {
				struct X86AddressingMode *ah CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(&regX86AH);
				asmAssign(outMode, ah, 1);
			}
			break;
		case 2:
			movRaxArgs = strX86AddrModeAppendItem(movRaxArgs, X86AddrModeReg(&regX86AX));
			movRaxArgs = strX86AddrModeAppendItem(movRaxArgs, X86AddrModeClone(aMode));
			divArgs = strX86AddrModeAppendItem(divArgs, X86AddrModeClone(bMode));
			dxorArgs = strX86AddrModeAppendItem(dxorArgs, X86AddrModeReg(&regX86DX));
			dxorArgs = strX86AddrModeAppendItem(dxorArgs, X86AddrModeReg(&regX86DX));
			assembleInst("MOV", movRaxArgs);
			assembleInst("XOR", dxorArgs);
			assembleInst(op, divArgs);
			if (isDivOrMod) {
				struct X86AddressingMode *ax CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(&regX86AX);
				asmAssign(outMode, ax, 1);
			} else {
				struct X86AddressingMode *dx CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(&regX86DX);
				asmAssign(outMode, dx, 1);
			}
			break;
		case 4:
			movRaxArgs = strX86AddrModeAppendItem(movRaxArgs, X86AddrModeReg(&regX86EAX));
			movRaxArgs = strX86AddrModeAppendItem(movRaxArgs, X86AddrModeClone(aMode));
			divArgs = strX86AddrModeAppendItem(divArgs, X86AddrModeClone(bMode));
			dxorArgs = strX86AddrModeAppendItem(dxorArgs, X86AddrModeReg(&regX86EDX));
			dxorArgs = strX86AddrModeAppendItem(dxorArgs, X86AddrModeReg(&regX86EDX));
			assembleInst("MOV", movRaxArgs);
			assembleInst("XOR", dxorArgs);
			assembleInst(op, divArgs);
			if (isDivOrMod) {
				struct X86AddressingMode *eax CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(&regX86EAX);
				asmAssign(outMode, eax, 1);
			} else {
				struct X86AddressingMode *edx CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(&regX86EDX);
				asmAssign(outMode, edx, 1);
			}
			break;
		case 8:
			movRaxArgs = strX86AddrModeAppendItem(movRaxArgs, X86AddrModeReg(&regAMD64RAX));
			movRaxArgs = strX86AddrModeAppendItem(movRaxArgs, X86AddrModeClone(aMode));
			divArgs = strX86AddrModeAppendItem(divArgs, X86AddrModeClone(bMode));
			dxorArgs = strX86AddrModeAppendItem(dxorArgs, X86AddrModeReg(&regAMD64RDX));
			dxorArgs = strX86AddrModeAppendItem(dxorArgs, X86AddrModeReg(&regAMD64RDX));
			assembleInst("MOV", movRaxArgs);
			assembleInst("XOR", dxorArgs);
			assembleInst(op, divArgs);
			if (isDivOrMod) {
				struct X86AddressingMode *rdx CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(&regAMD64RDX);
				asmAssign(outMode, rdx, 1);
			} else {
				struct X86AddressingMode *rax CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(&regAMD64RAX);
				asmAssign(outMode, rax, 1);
			}
			break;
		}
		if (ppRAX)
			assembleInst("POP", ppRAX), unconsumeRegFromMode(ppRAX[0]);
		if (ppRDX)
			assembleInst("POP", ppRDX), unconsumeRegFromMode(ppRDX[0]);
		return strGraphNodeIRPAppendItem(NULL, nodeDest(start));
	}
	case IR_POW: {
		// TODO
		assert(0);
	}
	case IR_LOR: {
		COMPILE_87_IFN;

		struct X86AddressingMode *outMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(graphEdgeIROutgoing(out[0]));
		graphNodeIR a, b;
		binopArgs(start, &a, &b);
		struct X86AddressingMode *aMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(a);
		struct X86AddressingMode *bMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(b);
		// CMP a,0
		// JNE next1
		// MOV out,1
		// next1:
		// CMP b,0
		// JNE next2
		// MOV out,1
		// JMP end
		// next2:
		// MOV out,0
		// end:
		strChar endLab CLEANUP(strCharDestroy) = uniqueLabel("LOR");
		struct X86AddressingMode *one CLEANUP(X86AddrModeDestroy) = X86AddrModeSint(1);
		for (int i = 0; i != 2; i++) {
			strX86AddrMode cmpArgs CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeClone(i ? aMode : bMode));
			cmpArgs = strX86AddrModeAppendItem(cmpArgs, X86AddrModeSint(0));
			assembleInst("CMP", cmpArgs);

			strChar nextLab CLEANUP(strCharDestroy) = uniqueLabel("LOR");
			strX86AddrMode jneArgs CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeLabel(nextLab));
			assembleInst("JNE", jneArgs);
			asmAssign(outMode, one, objectSize(IRNodeType(start), NULL));

			strX86AddrMode jmpArgs CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeLabel(endLab));
			assembleInst("JMP", jmpArgs);

			// Returns copy of label name
			free(X86EmitAsmLabel(nextLab));
		}
		// MOV out,0
		struct X86AddressingMode *zero CLEANUP(X86AddrModeDestroy) = X86AddrModeSint(0);
		asmAssign(outMode, zero, objectSize(IRNodeType(start), NULL));

		// Returns copy of label name
		free(X86EmitAsmLabel(endLab));
		return strGraphNodeIRPAppendItem(NULL, nodeDest(start));
	}
	case IR_LXOR: {
		if (isFltNode(start)) {
			switch (getCurrentArch()) {
			case ARCH_TEST_SYSV:
			case ARCH_X86_SYSV: {
				compileX87Expr(start);
				return nextNodesToCompile(start);
			}
			case ARCH_X64_SYSV:;
			}
		}

		struct X86AddressingMode *outMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(graphEdgeIROutgoing(out[0]));
		graphNodeIR a, b;
		binopArgs(start, &a, &b);
		struct X86AddressingMode *aMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(a);
		struct X86AddressingMode *bMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(b);
		// MOV outMode,0
		// CMP aMode,0
		// SETNE outMode
		// CMP bMode,0
		// JE END
		// XOR outNode,1
		// end:
		struct X86AddressingMode *zero CLEANUP(X86AddrModeDestroy) = X86AddrModeSint(0);
		struct X86AddressingMode *one CLEANUP(X86AddrModeDestroy) = X86AddrModeSint(1);
		asmAssign(outMode, zero, objectSize(IRNodeType(start), NULL));

		strX86AddrMode cmpAArgs CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeClone(aMode));
		cmpAArgs = strX86AddrModeAppendItem(cmpAArgs, X86AddrModeSint(0));
		assembleInst("CMP", cmpAArgs);

		setCond("NE", outMode);

		strX86AddrMode cmpBArgs CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeClone(bMode));
		cmpBArgs = strX86AddrModeAppendItem(cmpBArgs, X86AddrModeSint(0));
		assembleInst("CMP", cmpBArgs);

		strChar endLabel CLEANUP(strCharDestroy) = uniqueLabel("XOR");
		strX86AddrMode jmpeArgs CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeLabel(endLabel));
		assembleInst("JE", jmpeArgs);

		strX86AddrMode xorArgs CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeClone(outMode));
		xorArgs = strX86AddrModeAppendItem(xorArgs, X86AddrModeClone(one));

		X86EmitAsmLabel(endLabel);
		return strGraphNodeIRPAppendItem(NULL, nodeDest(start));
	}
	case IR_LAND: {
		COMPILE_87_IFN;

		__auto_type outNode = graphEdgeIROutgoing(out[0]);
		struct X86AddressingMode *outMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(outNode);
		graphNodeIR a, b;
		binopArgs(start, &a, &b);
		struct X86AddressingMode *aMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(a);
		struct X86AddressingMode *bMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(b);
		// CMP aMode,0
		// JE emd
		// CMP bMode,0
		// MOV outMode,0
		// JE end
		// MOV outMode,1
		// end:
		struct X86AddressingMode *zero CLEANUP(X86AddrModeDestroy) = X86AddrModeSint(0);
		struct X86AddressingMode *one CLEANUP(X86AddrModeDestroy) = X86AddrModeSint(1);
		strX86AddrMode cmpAArgs CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeClone(aMode));
		cmpAArgs = strX86AddrModeAppendItem(cmpAArgs, X86AddrModeSint(0));
		assembleInst("CMP", cmpAArgs);

		strChar endLabel CLEANUP(strCharDestroy) = uniqueLabel("XOR");
		strX86AddrMode jmpeArgs CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeLabel(endLabel));
		assembleInst("JE", jmpeArgs);

		strX86AddrMode cmpBArgs CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeClone(aMode));
		cmpBArgs = strX86AddrModeAppendItem(cmpBArgs, X86AddrModeSint(0));
		assembleInst("CMP", cmpBArgs);
		asmAssign(outMode, zero, objectSize(IRNodeType(outNode), NULL));
		assembleInst("JE", jmpeArgs);

		asmAssign(outMode, one, objectSize(IRNodeType(outNode), NULL));

		X86EmitAsmLabel(endLabel);
		return strGraphNodeIRPAppendItem(NULL, nodeDest(start));
	}
	case IR_LNOT: {
		COMPILE_87_IFN;

		strGraphEdgeIRP in CLEANUP(strGraphEdgeIRPDestroy) = graphNodeIRIncoming(start);
		__auto_type inNode = graphEdgeIRIncoming(in[0]);
		__auto_type outNode = graphEdgeIROutgoing(out[0]);
		// MOV outMode,0
		// CMP inMode,0
		// SETE outMode

		struct X86AddressingMode *zero CLEANUP(X86AddrModeDestroy) = X86AddrModeSint(0);
		struct X86AddressingMode *oMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(outNode);
		struct X86AddressingMode *iMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(inNode);

		asmAssign(oMode, zero, objectSize(IRNodeType(outNode), NULL));

		strX86AddrMode cmpArgs CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeClone(iMode));
		assembleInst("CMP", cmpArgs);

		setCond("E", oMode);

		return strGraphNodeIRPAppendItem(NULL, nodeDest(start));
	}
	case IR_BNOT: {
		COMPILE_87_IFN;

		strGraphEdgeIRP in CLEANUP(strGraphEdgeIRPDestroy) = graphNodeIRIncoming(start);
		__auto_type inNode = graphEdgeIRIncoming(in[0]);
		__auto_type outNode = graphEdgeIROutgoing(out[0]);
		struct X86AddressingMode *oMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(outNode);
		struct X86AddressingMode *iMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(inNode);

		// MOv out,in
		// NOT out
		asmAssign(oMode, iMode, objectSize(IRNodeType(outNode), NULL));

		strX86AddrMode notArgs CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeClone(oMode));
		assembleInst("NOT", notArgs);
		return strGraphNodeIRPAppendItem(NULL, nodeDest(start));
	}
	case IR_BAND: {
		COMPILE_87_IFN;

		if (isIntNode(start) || isPtrNode(start)) {
			assembleOpInt(start, "AND");
		} else
			assert(0);
		return strGraphNodeIRPAppendItem(NULL, nodeDest(start));
	}
	case IR_BXOR: {
		COMPILE_87_IFN;

		if (isIntNode(start) || isPtrNode(start)) {
			assembleOpInt(start, "XOR");
		} else
			assert(0);
		return strGraphNodeIRPAppendItem(NULL, nodeDest(start));
	}
	case IR_BOR: {
		COMPILE_87_IFN;

		if (isIntNode(start) || isPtrNode(start)) {
			assembleOpInt(start, "OR");
		} else
			assert(0);
		return strGraphNodeIRPAppendItem(NULL, nodeDest(start));
	}
	case IR_LSHIFT: {
		COMPILE_87_IFN;

		if (isIntNode(start) || isPtrNode(start)) {
			graphNodeIR a, b;
			binopArgs(start, &a, &b);
			if (typeIsSigned(IRNodeType(start)))
				assembleOpInt(start, "SAL");
			else
				assembleOpInt(start, "SHL");
		} else
			assert(0);
		return strGraphNodeIRPAppendItem(NULL, nodeDest(start));
	}
	case IR_RSHIFT: {
		COMPILE_87_IFN;

		if (isIntNode(start) || isPtrNode(start)) {
			graphNodeIR a, b;
			binopArgs(start, &a, &b);
			if (typeIsSigned(IRNodeType(start)))
				assembleOpInt(start, "SAR");
			else
				assembleOpInt(start, "SHR");
		} else
			assert(0);
		return strGraphNodeIRPAppendItem(NULL, nodeDest(start));
	}
	case IR_ARRAY_ACCESS: {
		strGraphEdgeIRP in CLEANUP(strGraphEdgeIRPDestroy) = graphNodeIRIncoming(start);
		strGraphEdgeIRP inAssign CLEANUP(strGraphEdgeIRPDestroy) = IRGetConnsOfType(in, IR_CONN_DEST);

		graphNodeIR a, b;
		binopArgs(start, &a, &b);
		assert(isPtrNode(a));
		__auto_type aType = objectBaseType(IRNodeType(a));
		long itemSize;
		if (aType->type == TYPE_PTR) {
			struct objectPtr *ptr = (void *)aType;
			itemSize = objectSize(ptr->type, NULL);
		} else if (aType->type == TYPE_ARRAY) {
			struct objectArray *arr = (void *)aType;
			itemSize = objectSize(arr->type, NULL);
		} else
			assert(0);

		struct X86AddressingMode *aMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(a);
		struct X86AddressingMode *bMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(b);
		struct X86AddressingMode *oMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(nodeDest(start));

		strGraphEdgeIRP incomingAssign CLEANUP(strGraphEdgeIRPDestroy) = IRGetConnsOfType(in, IR_CONN_DEST);
		struct X86AddressingMode *inMode CLEANUP(X86AddrModeDestroy) = X86AddrModeSint(-1);
		int hasIncomingAssign = 0;
		if (strGraphEdgeIRPSize(incomingAssign)) {
			X86AddrModeDestroy(&inMode);
			inMode = IRNode2AddrMode(graphEdgeIRIncoming(incomingAssign[0]));
			hasIncomingAssign = 1;
		}
		AUTO_LOCK_MODE_REGS(inMode);

		switch (itemSize) {
		case 1:
		case 2:
		case 4:
		case 8: {
			int baseNeedsPop = 0;
			struct reg *base;
			if (!addrModeReg(aMode)) {
				base = regForTypeExcludingConsumed((struct object *)getTypeForSize(ptrSize()));
				pushReg(base);
				baseNeedsPop = 1;
			} else
				base = addrModeReg(aMode);
			consumeRegister(base);

			struct X86AddressingMode *baseMode CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(base);
			asmAssign(baseMode, aMode, ptrSize());

			int indexNeedsPop = 0;
			struct reg *index = NULL;
			if (!addrModeReg(bMode)) {
				index = regForTypeExcludingConsumed((struct object *)getTypeForSize(ptrSize()));
				consumeRegister(index);
				indexNeedsPop = 1;
				pushReg(index);
			} else
				index = addrModeReg(bMode);
			consumeRegister(index);

			struct X86AddressingMode *indexMode CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(index);
			// Promote to ptr size if necessary
			if (objectSize(bMode->valueType, NULL) != ptrSize()) {
				if (bMode->type != X86ADDRMODE_SINT && bMode->type != X86ADDRMODE_UINT && bMode->type != X86ADDRMODE_FLT) {
					// Push if not pushed already(will need pop if so)
					if (!indexNeedsPop)
						pushReg(index);
					indexNeedsPop = 1;

					asmTypecastAssign(indexMode, bMode);
				} else
					asmAssign(indexMode, bMode, ptrSize());
			} else
				asmAssign(indexMode, bMode, ptrSize());

			struct X86AddressingMode *indirMode CLEANUP(X86AddrModeDestroy) = X86AddrModeIndirSIB(itemSize, X86AddrModeReg(index), X86AddrModeReg(base), 0, (struct object *)getTypeForSize(itemSize));
			if (hasIncomingAssign)
				asmAssign(indirMode, inMode, objectSize(IRNodeType(nodeDest(start)), NULL));
			asmAssign(oMode, indirMode, objectSize(IRNodeType(nodeDest(start)), NULL));

			uncomsumeRegister(base);
			uncomsumeRegister(index);
			if (indexNeedsPop)
				popReg(index);
			if (baseNeedsPop)
				popReg(base);
			return strGraphNodeIRPAppendItem(NULL, nodeDest(start));
		}
		default: {
			int baseNeedsPop = 0;
			struct reg *base;
			if (!addrModeReg(aMode)) {
				base = regForTypeExcludingConsumed((struct object *)getTypeForSize(ptrSize()));
				pushReg(base);
				baseNeedsPop = 1;
			} else
				base = addrModeReg(aMode);
			consumeRegister(base);

			int indexNeedsPop = 0;
			struct reg *index = NULL;
			if (!addrModeReg(bMode)) {
				index = regForTypeExcludingConsumed((struct object *)getTypeForSize(ptrSize()));
				consumeRegister(index);
				indexNeedsPop = 1;
				pushReg(index);
			} else
				index = addrModeReg(aMode);
			consumeRegister(index);

			struct X86AddressingMode *indexMode CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(index);

			// Promote to ptr size if necessary
			if (objectSize(bMode->valueType, NULL) != ptrSize()) {
				if (bMode->type != X86ADDRMODE_SINT && bMode->type != X86ADDRMODE_UINT && bMode->type != X86ADDRMODE_FLT) {
					// Push if not pushed already(will need pop if so)
					if (!indexNeedsPop)
						pushReg(index);
					indexNeedsPop = 1;

					asmTypecastAssign(indexMode, bMode);
				} else
					asmAssign(indexMode, bMode, ptrSize());
			} else
				asmAssign(indexMode, bMode, ptrSize());

			// IMUL index,itemSize
			// ADD base,index
			// assign oMode,[base]
			strX86AddrMode imulArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
			imulArgs = strX86AddrModeAppendItem(imulArgs, X86AddrModeReg(index));
			imulArgs = strX86AddrModeAppendItem(imulArgs, X86AddrModeSint(itemSize));
			assembleInst("IMUL2", imulArgs);
			strX86AddrMode addArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
			addArgs = strX86AddrModeAppendItem(addArgs, X86AddrModeReg(base));
			addArgs = strX86AddrModeAppendItem(addArgs, X86AddrModeReg(index));
			assembleInst("ADD", addArgs);

			struct X86AddressingMode *indirMode CLEANUP(X86AddrModeDestroy) = X86AddrModeIndirReg(base, (struct object *)getTypeForSize(ptrSize()));
			if (hasIncomingAssign)
				asmAssign(indirMode, inMode, objectSize(IRNodeType(nodeDest(start)), NULL));
			asmAssign(oMode, indirMode, objectSize(IRNodeType(nodeDest(start)), NULL));

			uncomsumeRegister(base);
			uncomsumeRegister(index);
			if (indexNeedsPop)
				popReg(index);
			if (baseNeedsPop)
				popReg(base);
		} break;
		}
		return strGraphNodeIRPAppendItem(NULL, nodeDest(start));
	}
	case IR_SIMD: {
		assert(0);
	}
	case IR_GT: {
		COMPILE_87_IFN;

		assembleOpInt(start, "CMP");
		struct X86AddressingMode *oMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(nodeDest(start));
		graphNodeIR a, b;
		binopArgs(start, &a, &b);
		if (typeIsSigned(IRNodeType(a)) || typeIsSigned(IRNodeType(b))) {
				assembleOpIntLogical(start, "G");
		} else {
			assembleOpIntLogical(start, "A");
		}
		return strGraphNodeIRPAppendItem(NULL, nodeDest(start));
	}
	case IR_LT: {
		COMPILE_87_IFN;

		assembleOpInt(start, "CMP");
		struct X86AddressingMode *oMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(nodeDest(start));
		graphNodeIR a, b;
		binopArgs(start, &a, &b);
		if (typeIsSigned(IRNodeType(a)) || typeIsSigned(IRNodeType(b))) {
				assembleOpIntLogical(start, "L");
		} else {
				assembleOpIntLogical(start, "B");
		}
		return strGraphNodeIRPAppendItem(NULL, nodeDest(start));
	}
	case IR_GE: {
		COMPILE_87_IFN;

		assembleOpInt(start, "CMP");
		struct X86AddressingMode *oMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(nodeDest(start));
		graphNodeIR a, b;
		binopArgs(start, &a, &b);
		if (typeIsSigned(IRNodeType(a)) || typeIsSigned(IRNodeType(b))) {
				assembleOpIntLogical(start, "GE");
		} else {
				assembleOpIntLogical(start, "AE");
		}
		return strGraphNodeIRPAppendItem(NULL, nodeDest(start));
	}
	case IR_LE: {
		COMPILE_87_IFN;

		assembleOpInt(start, "CMP");
		struct X86AddressingMode *oMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(nodeDest(start));
		graphNodeIR a, b;
		binopArgs(start, &a, &b);
		if (typeIsSigned(IRNodeType(a)) || typeIsSigned(IRNodeType(b))) {
				assembleOpIntLogical(start, "LE");
		} else {
				assembleOpIntLogical(start, "BE");
		}
		return strGraphNodeIRPAppendItem(NULL, nodeDest(start));
	}
	case IR_EQ: {
		COMPILE_87_IFN;

		assembleOpInt(start, "CMP");
		struct X86AddressingMode *oMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(nodeDest(start));
		assembleOpIntLogical(start, "E");
		return strGraphNodeIRPAppendItem(NULL, nodeDest(start));
	}
	case IR_NE: {
		COMPILE_87_IFN;

		assembleOpInt(start, "CMP");
		struct X86AddressingMode *oMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(nodeDest(start));
		assembleOpIntLogical(start, "NE");
		return strGraphNodeIRPAppendItem(NULL, nodeDest(start));
	}
	case IR_CHOOSE: {
		fprintf(stderr, "Remove choose nodes with IRSSAReplaceChooseWithAssigns before calling me!\n");
		assert(0);
	}
	case IR_COND_JUMP: {
		strGraphEdgeIRP in CLEANUP(strGraphEdgeIRPDestroy) = graphNodeIRIncoming(start);
		strGraphEdgeIRP inSource CLEANUP(strGraphEdgeIRPDestroy) = IRGetConnsOfType(in, IR_CONN_SOURCE_A);
		__auto_type inNode = graphEdgeIRIncoming(inSource[0]);
		struct X86AddressingMode *inMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(inNode);

		strGraphEdgeIRP out CLEANUP(strGraphEdgeIRPDestroy) = graphNodeIROutgoing(start);
		strGraphEdgeIRP outT CLEANUP(strGraphEdgeIRPDestroy) = IRGetConnsOfType(out, IR_CONN_COND_TRUE);
		strGraphEdgeIRP outF CLEANUP(strGraphEdgeIRPDestroy) = IRGetConnsOfType(out, IR_CONN_COND_FALSE);

		__auto_type trueNode = graphEdgeIROutgoing(outT[0]);
		__auto_type falseNode = graphEdgeIROutgoing(outF[0]);
		struct X86AddressingMode *trueLab CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(trueNode);
		struct X86AddressingMode *falseLab CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(falseNode);

		AUTO_LOCK_MODE_REGS(inMode);
		strX86AddrMode cmpArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
		cmpArgs = strX86AddrModeAppendItem(cmpArgs, X86AddrModeClone(inMode));
		cmpArgs = strX86AddrModeAppendItem(cmpArgs, X86AddrModeSint(0));
		assembleInst("CMP", cmpArgs);

		strX86AddrMode jmpTArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
		jmpTArgs = strX86AddrModeAppendItem(jmpTArgs, X86AddrModeClone(trueLab));
		assembleInst("JNE", jmpTArgs);

		strX86AddrMode jmpFArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
		jmpFArgs = strX86AddrModeAppendItem(jmpFArgs, X86AddrModeClone(falseLab));
		assembleInst("JE", jmpFArgs);

		__auto_type retVal = strGraphNodeIRPAppendItem(NULL, trueNode);
		retVal = strGraphNodeIRPAppendItem(retVal, falseNode);
		return retVal;
	}
	case IR_JUMP_TAB: {
		strGraphEdgeIRP in CLEANUP(strGraphEdgeIRPDestroy) = graphNodeIRIncoming(start);
		strGraphEdgeIRP inSource CLEANUP(strGraphEdgeIRPDestroy) = IRGetConnsOfType(in, IR_CONN_SOURCE_A);
		__auto_type inNode = graphEdgeIRIncoming(inSource[0]);
		struct X86AddressingMode *inMode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(inNode);

		struct IRNodeJumpTable *table = (void *)graphNodeIRValuePtr(start);
		strIRTableRange ranges CLEANUP(strIRTableRangeDestroy) = strIRTableRangeClone(table->labels);
		qsort(ranges, strIRTableRangeSize(ranges), sizeof(*ranges), (int (*)(const void *, const void *))IRTableRangeCmp);
		int64_t smallest = ranges[0].start;
		int64_t largest = ranges[strIRTableRangeSize(ranges) - 1].start;
		int64_t diff = largest - smallest;

		strGraphEdgeIRP out CLEANUP(strGraphEdgeIRPDestroy) = graphNodeIROutgoing(start);
		strGraphEdgeIRP outDft CLEANUP(strGraphEdgeIRPDestroy) = IRGetConnsOfType(in, IR_CONN_DFT);
		assert(strGraphEdgeIRPSize(outDft) == 1);
		__auto_type dftNode = graphEdgeIROutgoing(outDft[0]);

		strX86AddrMode jmpTable CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeResize(NULL, diff);
		for (long i = 0; i != strIRTableRangeSize(ranges); i++) {
			jmpTable[i] = IRNode2AddrMode(dftNode);
		}
		for (long i = 0; i != strIRTableRangeSize(ranges); i++) {
			for (long j = ranges[i].start + smallest; j != ranges[i].end + smallest; j++) {
				X86AddrModeDestroy(&jmpTable[i]);
				jmpTable[i] = IRNode2AddrMode(ranges[i].to);
			}
		}

		strChar jmpTabLabel CLEANUP(strCharDestroy) = uniqueLabel("JmpTab");
		// Returns copy of label
		free(X86EmitAsmLabel(jmpTabLabel));
		switch (ptrSize()) {
		case 2:
			fprintf(stderr, "HolyC on a commodore 64 isn't possible yet,bro.\n");
			assert(0);
		case 4: {
			X86EmitAsmDU32(jmpTable, strX86AddrModeSize(jmpTable));
			break;
		}
		case 8: {
			X86EmitAsmDU64(jmpTable, strX86AddrModeSize(jmpTable));
			break;
		}
		default:
			fprintf(stderr, "Are you in the future where 64bit address space is obselete?\n");
			assert(0);
		}
		strX86AddrMode cmpArgs1 CLEANUP(strX86AddrModeDestroy2) = NULL;
		cmpArgs1 = strX86AddrModeAppendItem(cmpArgs1, X86AddrModeClone(inMode));
		cmpArgs1 = strX86AddrModeAppendItem(cmpArgs1, X86AddrModeSint(smallest));
		assembleInst("CMP", cmpArgs1);

		strX86AddrMode jmpDftArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
		jmpDftArgs = strX86AddrModeAppendItem(jmpDftArgs, IRNode2AddrMode(dftNode));
		assembleInst("JL", jmpDftArgs);

		strX86AddrMode cmpArgs2 CLEANUP(strX86AddrModeDestroy2) = NULL;
		cmpArgs2 = strX86AddrModeAppendItem(cmpArgs2, X86AddrModeClone(inMode));
		cmpArgs2 = strX86AddrModeAppendItem(cmpArgs2, X86AddrModeSint(largest));
		assembleInst("CMP", cmpArgs2);

		assembleInst("JGE", jmpDftArgs);

		struct reg *b = regForTypeExcludingConsumed((struct object *)getTypeForSize(ptrSize()));
		struct X86AddressingMode *regMode CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(b);
		AUTO_LOCK_MODE_REGS(regMode);
		pushReg(b);

		struct X86AddressingMode *jmpTabLabMode CLEANUP(X86AddrModeDestroy) = X86AddrModeLabel(jmpTabLabel);
		asmAssign(regMode, jmpTabLabMode, ptrSize());
		strX86AddrMode leaArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
		leaArgs = strX86AddrModeAppendItem(leaArgs, X86AddrModeIndirSIB(ptrSize(), NULL, X86AddrModeReg(b), 0, (struct object *)getTypeForSize(ptrSize())));

		popReg(b);

		strGraphNodeIRP retVal = strGraphNodeIRPAppendItem(NULL, graphEdgeIROutgoing(outDft[0]));
		strGraphEdgeIRP outCases CLEANUP(strGraphEdgeIRPDestroy) = IRGetConnsOfType(out, IR_CONN_CASE);
		for (long i = 0; strGraphEdgeIRPSize(outCases); i++)
			retVal = strGraphNodeIRPAppendItem(retVal, graphEdgeIROutgoing(outCases[i]));
		return retVal;
	}
	case IR_VALUE: {
			struct X86AddressingMode *currMode CLEANUP(X86AddrModeDestroy)=IRNode2AddrMode(start);
			AUTO_LOCK_MODE_REGS(currMode);
			
			strGraphEdgeIRP incoming CLEANUP(strGraphEdgeIRPDestroy) = graphNodeIRIncoming(start);
			strGraphEdgeIRP assigns CLEANUP(strGraphEdgeIRPDestroy) = IRGetConnsOfType(incoming, IR_CONN_DEST);
			if (strGraphEdgeIRPSize(assigns) == 1) {
					// Operator automatically assign into thier destination,so ensure isn't an operator.
					if (graphNodeIRValuePtr(graphEdgeIRIncoming(assigns[0]))->type == IR_VALUE)
							asmAssign(currMode, IRNode2AddrMode(graphEdgeIRIncoming(assigns[0])), objectSize(IRNodeType(start), NULL));
			}
			strGraphEdgeIRP fromPtrAssigns CLEANUP(strGraphEdgeIRPDestroy) = IRGetConnsOfType(incoming, IR_CONN_ASSIGN_FROM_PTR);
			if (strGraphEdgeIRPSize(fromPtrAssigns) == 1) {
					__auto_type incoming=graphEdgeIRIncoming(fromPtrAssigns[0]);
					struct X86AddressingMode *inMode CLEANUP(X86AddrModeDestroy)=IRNode2AddrMode(incoming);
					AUTO_LOCK_MODE_REGS(inMode);
					struct reg *ppReg=NULL; //pushPop
					
					struct X86AddressingMode *readFrom CLEANUP(X86AddrModeDestroy)=NULL;
					if(inMode->type==X86ADDRMODE_REG) {
							readFrom=X86AddrModeIndirReg(inMode->value.reg, IRNodeType(start));
					} else {
							ppReg=regForTypeExcludingConsumed(objectPtrCreate(&typeU0));
							pushReg(ppReg);

							strX86AddrMode leaArgs CLEANUP(strX86AddrModeDestroy2)=NULL;
							leaArgs=strX86AddrModeAppendItem(leaArgs, X86AddrModeReg(ppReg));
							leaArgs=strX86AddrModeAppendItem(leaArgs, X86AddrModeClone(inMode));
							leaArgs[1]->valueType=NULL;
							assembleInst("LEA", leaArgs);
							readFrom=X86AddrModeIndirReg(ppReg, IRNodeType(start));
					}

					asmTypecastAssign(currMode, readFrom);
					
					if(ppReg)
							popReg(ppReg);
			}
			return nextNodesToCompile(start);
	}
	case IR_LABEL_LOCAL:
	case IR_LABEL: {
		X86EmitAsmLabel(getLabelName(start));
		return nextNodesToCompile(start);
	}
	case IR_X86_INST: {
		struct IRNodeX86Inst *inst = (void *)graphNodeIRValuePtr(start);
		assembleInst(inst->name, inst->args);
		return nextNodesToCompile(start);
	}
	case IR_ASM_DU8: {
		struct IRNodeAsmDU8 *du8 = (void *)graphNodeIRValuePtr(start);
		strX86AddrMode addrModes CLEANUP(strX86AddrModeDestroy2) = NULL;
		for (long i = 0; i != du8->count; i++)
			addrModes = strX86AddrModeAppendItem(addrModes, X86AddrModeUint(du8->data[i]));
		X86EmitAsmDU8(addrModes, du8->count);
		return nextNodesToCompile(start);
	}
	case IR_ASM_DU16: {
		struct IRNodeAsmDU16 *du16 = (void *)graphNodeIRValuePtr(start);
		strX86AddrMode addrModes CLEANUP(strX86AddrModeDestroy2) = NULL;
		for (long i = 0; i != du16->count; i++)
			addrModes = strX86AddrModeAppendItem(addrModes, X86AddrModeUint(du16->data[i]));
		X86EmitAsmDU16(addrModes, du16->count);
		return nextNodesToCompile(start);
	}
	case IR_ASM_DU32: {
		struct IRNodeAsmDU32 *du32 = (void *)graphNodeIRValuePtr(start);
		strX86AddrMode addrModes CLEANUP(strX86AddrModeDestroy2) = NULL;
		for (long i = 0; i != du32->count; i++)
			addrModes = strX86AddrModeAppendItem(addrModes, X86AddrModeUint(du32->data[i]));
		X86EmitAsmDU32(addrModes, du32->count);
		return nextNodesToCompile(start);
	}
	case IR_ASM_DU64: {
		struct IRNodeAsmDU64 *du64 = (void *)graphNodeIRValuePtr(start);
		strX86AddrMode addrModes CLEANUP(strX86AddrModeDestroy2) = NULL;
		for (long i = 0; i != du64->count; i++)
			addrModes = strX86AddrModeAppendItem(addrModes, X86AddrModeUint(du64->data[i]));
		X86EmitAsmDU64(addrModes, du64->count);
		return nextNodesToCompile(start);
	}
	case IR_ASM_IMPORT: {
		struct IRNodeAsmImport *import = (void *)graphNodeIRValuePtr(start);
		X86EmitAsmIncludeBinfile(import->fileName);
		return nextNodesToCompile(start);
	}
	case IR_FUNC_CALL: {
			IRABICall2Asm(start);
			return nextNodesToCompile(start);
	}
	case IR_FUNC_ARG: {
	}
	case IR_FUNC_RETURN:
	case IR_FUNC_END: {
			IRABIReturn2Asm(start);
			return NULL;
	}
	case IR_FUNC_START: {
			return nextNodesToCompile(start);
	}
	case IR_DERREF: {
			strGraphEdgeIRP in CLEANUP(strGraphEdgeIRPDestroy)=graphNodeIRIncoming(start);
			strGraphEdgeIRP source CLEANUP(strGraphEdgeIRPDestroy)=IRGetConnsOfType(in, IR_CONN_SOURCE_A);
			struct X86AddressingMode *iMode CLEANUP(X86AddrModeDestroy)=IRNode2AddrMode(graphEdgeIRIncoming(source[0]));
			AUTO_LOCK_MODE_REGS(iMode);
			__auto_type regAddr=regForTypeExcludingConsumed(objectPtrCreate(&typeU0));
			pushReg(regAddr);
			struct X86AddressingMode *regAddrMode CLEANUP(X86AddrModeDestroy)=X86AddrModeReg(regAddr);
			asmAssign(regAddrMode, iMode, ptrSize());

			strGraphEdgeIRP inAsn CLEANUP(strGraphEdgeIRPDestroy)=IRGetConnsOfType(in, IR_CONN_DEST);
			if(strGraphEdgeIRPSize(inAsn)==1) {
					struct X86AddressingMode *oMode CLEANUP(X86AddrModeDestroy)=X86AddrModeIndirReg(regAddr, IRNodeType(start));
					struct X86AddressingMode *iMode2 CLEANUP(X86AddrModeDestroy)=IRNode2AddrMode(graphEdgeIRIncoming(inAsn[0]));
					asmTypecastAssign(oMode, iMode2);
			}
			
			strGraphEdgeIRP out CLEANUP(strGraphEdgeIRPDestroy)=graphNodeIROutgoing(start);
			strGraphEdgeIRP dst CLEANUP(strGraphEdgeIRPDestroy)=IRGetConnsOfType(out, IR_CONN_DEST);
			if(strGraphEdgeIRPSize(dst)==1) {
					struct X86AddressingMode *oMode CLEANUP(X86AddrModeDestroy)=IRNode2AddrMode(graphEdgeIROutgoing(dst[0]));
					struct X86AddressingMode *iMode2 CLEANUP(X86AddrModeDestroy)=X86AddrModeIndirReg(regAddr, oMode->valueType);
					asmTypecastAssign(oMode,iMode2);
			}
			
			popReg(regAddr);
			return nextNodesToCompile(start);
	}
	case IR_MEMBERS: {
			struct IRNodeMembers *mems=(void*)graphNodeIRValuePtr(start);
			
			strGraphEdgeIRP in CLEANUP(strGraphEdgeIRPDestroy)=graphNodeIRIncoming(start);
			strGraphEdgeIRP inAssn CLEANUP(strGraphEdgeIRPDestroy)=IRGetConnsOfType(in, IR_CONN_DEST);
			strGraphEdgeIRP out CLEANUP(strGraphEdgeIRPDestroy)=graphNodeIROutgoing(start);
			strGraphEdgeIRP outAssn CLEANUP(strGraphEdgeIRPDestroy)=IRGetConnsOfType(out, IR_CONN_DEST);

			struct X86AddressingMode *inAssnMode CLEANUP(X86AddrModeDestroy)=NULL; 
			if(strGraphEdgeIRPSize(inAssn)==1) {
					inAssnMode=IRNode2AddrMode(graphEdgeIRIncoming(inAssn[0]));
					consumeRegFromMode(inAssnMode);
			}
			struct X86AddressingMode *outAssnMode CLEANUP(X86AddrModeDestroy)=NULL;
			if(strGraphEdgeIRPSize(outAssn)==1) {
					outAssnMode=IRNode2AddrMode(graphEdgeIROutgoing(outAssn[0]));
					consumeRegFromMode(outAssnMode);
			}

			//Assign the memReg with pointer to source
			struct reg *memReg=regForTypeExcludingConsumed(IRNodeType(start));
			pushReg(memReg);
			
			//Load the address of the source arg into register
			struct X86AddressingMode *memRegMode CLEANUP(X86AddrModeDestroy)=X86AddrModeReg(memReg);
			memRegMode->valueType=IRNodeType(start);
			strGraphEdgeIRP source CLEANUP(strGraphEdgeIRPDestroy)=IRGetConnsOfType(in, IR_CONN_SOURCE_A);
			storeMemberPtrInReg(memReg, graphEdgeIRIncoming(source[0]), mems->members);
			
			X86AddrModeDestroy(&memRegMode);
			memRegMode=X86AddrModeIndirReg(memReg, IRNodeType(start));
			
			//Reserve a register for the member ptr
			if(strGraphEdgeIRPSize(inAssn)==1) {
					struct X86AddressingMode *asnMode=IRNode2AddrMode(graphEdgeIRIncoming(inAssn[0]));
					asmTypecastAssign(memRegMode,asnMode);
			}

			if(strGraphEdgeIRPSize(outAssn)==1) {
						struct X86AddressingMode *toMode=IRNode2AddrMode(graphEdgeIROutgoing(outAssn[0]));
						asmTypecastAssign(toMode,memRegMode);
			}

			if(inAssnMode)
					unconsumeRegFromMode(inAssnMode);
			if(outAssnMode)
					unconsumeRegFromMode(outAssnMode);
			popReg(memReg);
			
			return nextNodesToCompile(start);
	}
	case IR_SUB_SWITCH_START_LABEL:
	case IR_SPILL_LOAD:
		assert(0);
	}
}
static int isNotArgEdge(const void *data, const graphEdgeIR *edge) {
	__auto_type type = *graphEdgeIRValuePtr(*edge);
	return !IRIsExprEdge(type);
}
static int isUnvisited(const void *data, const graphNodeIR *node) {
	return NULL != ptrMapCompiledNodesGet(compiledNodes, *node);
}
void __IR2AsmExpr(graphNodeIR start) {
computeArgs:;
		strGraphEdgeIRP incoming CLEANUP(strGraphEdgeIRPDestroy) = IREdgesByPrec(start);
	for (long a = 0; a != strGraphEdgeIRPSize(incoming); a++) {
		__IR2AsmExpr(graphEdgeIRIncoming(incoming[a]));
	}
	__IR2Asm(start);
}

void IR2Asm(graphNodeIR start) {
	strGraphNodeIRP next CLEANUP(strGraphNodeIRPDestroy) = NULL;
	if (IREndOfExpr(start) != start) {
		// Start of statement can be label,so compile label first(__IR2Asm only compiles node once)
		__auto_type exprStart = IRStmtStart(start);
		if (graphNodeIRValuePtr(exprStart)->type == IR_LABEL)
			__IR2Asm(exprStart);

		__IR2AsmExpr(IREndOfExpr(start));
		next = graphNodeIROutgoingNodes(IREndOfExpr(start));
	} else {
		next = __IR2Asm(start);
	}
	next=strGraphNodeIRPUnique(next, (gnCmpType)ptrPtrCmp, NULL);

	for (long n = 0; n != strGraphNodeIRPSize(next); n++)
		IR2Asm(next[n]);
}
