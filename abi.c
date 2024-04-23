#include <common.h>
#include "IR2asm.h"
#include "IRLiveness.h"
#include "abi.h"
#include "asmEmitter.h"
#include <assert.h>
#include "cleanup.h"
#include "abiSysVx64.h"
#define DEBUG_PRINT_ENABLE 1
void *IR_ATTR_ABI_INFO = "ABI_INFO";
static void *IR_ATTR_FUNC = "FUNC";
static __thread char calledInsertArgs = 0;
static __thread char computedABIInfo = 0;
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
void IRAttrABIInfoDestroy(struct IRAttr *a) {
	struct IRAttrABIInfo *abi = (void *)a;
	strRegPDestroy(&abi->toPushPop);
	strRegPDestroy(&abi->liveIn);
	strRegPDestroy(&abi->liveOut);
}
static void *IR_ATTR_OLD_REG_SLICE = "OLD_REG_SLICE";
static void strX86AddrModeDestroy2(strX86AddrMode *str) {
	for (long i = 0; i != strX86AddrModeSize(*str); i++)
		X86AddrModeDestroy(&str[0][i]);
	strX86AddrModeDestroy(str);
}
struct IRATTRoldRegSlice {
	struct IRAttr base;
	graphNodeIR old;
};
struct IRAttrFunc {
	struct IRAttr base;
	struct parserFunction *func;
};
typedef int (*gnCmpType)(const graphNodeMapping *, const graphNodeMapping *);
static int ptrPtrCmp(const void *a, const void *b) {
		if(*(void**)a>*(void**)b)
				return 1;
		else if(*(void**)a<*(void**)b)
				return -1;
		return 0;
}
typedef int (*regCmpType)(const struct reg **, const struct reg **);
typedef int (*varCmpType)(const struct parserVar **, const struct parserVar **);
static int killEdgeIfEq(void *a, void *b) {
		return *(enum IRConnType*)a == *(enum IRConnType*)b;
}
static void swapNode(graphNodeIR old, graphNodeIR with) {
	strGraphEdgeIRP in CLEANUP(strGraphEdgeIRPDestroy) = graphNodeIRIncoming(old);
	strGraphEdgeIRP out CLEANUP(strGraphEdgeIRPDestroy) = graphNodeIROutgoing(old);
	for (long i = 0; i != strGraphEdgeIRPSize(in); i++) {
		__auto_type from = graphEdgeIRIncoming(in[i]);
		graphNodeIRConnect(from, with, *graphEdgeIRValuePtr(in[i]));
		__auto_type val=*graphEdgeIRValuePtr(in[i]);
		graphEdgeIRKill(from, old, &val, killEdgeIfEq, NULL);
	}
	for (long o = 0; o != strGraphEdgeIRPSize(out); o++) {
		__auto_type to = graphEdgeIROutgoing(out[o]);
		graphNodeIRConnect(with, to, *graphEdgeIRValuePtr(out[o]));
		__auto_type val=*graphEdgeIRValuePtr(out[o]);
		graphEdgeIRKill(old, to, &val, killEdgeIfEq, NULL);
	}
}
STR_TYPE_DEF(struct parserVar *, Variable);
STR_TYPE_FUNCS(struct parserVar *, Variable);
static int isSelectVariable(graphNodeIR node, const void *data) {
	const strVariable *select = data;
	struct IRNodeValue *val = (void *)graphNodeIRValuePtr(node);
	if (val->base.type == IR_VALUE)
		if (val->val.type == IR_VAL_VAR_REF)
			return NULL != strVariableSortedFind(*select, val->val.value.var.var, (varCmpType)ptrPtrCmp);

	return 0;
}
static char *strDup(const char *txt) {
		return strcpy(calloc(strlen(txt) + 1,1), txt);
}
#define ARRAY_SIZE(array) (sizeof(array) / sizeof(*array))
PTR_MAP_FUNCS(struct parserVar *, struct reg *, Var2Reg);
PTR_MAP_FUNCS(struct reg *, struct parserVar *, Reg2Var);
void findRegisterLiveness(graphNodeIR start) {
	strGraphNodeIRP allNodes CLEANUP(strGraphNodeIRPDestroy) = graphNodeIRAllNodes(start);
	// Replace Registers with variables and do liveness analysis on the variables
	strVariable regVars CLEANUP(strVariableDestroy) = NULL;
	ptrMapVar2Reg var2Reg = ptrMapVar2RegCreate();
	const struct reg *fpu[]={
				&regX86ST0,
				&regX86ST1,
				&regX86ST2,
				&regX86ST3,
				&regX86ST4,
				&regX86ST5,
				&regX86ST6,
				&regX86ST7,
		};
	
	for (long i = 0; i != strGraphNodeIRPSize(allNodes); i++) {
		struct IRNodeValue *value = (void *)graphNodeIRValuePtr(allNodes[i]);
		if (value->base.type != IR_VALUE)
				continue;
		if (value->val.type != IR_VAL_REG)
			continue;
		struct reg *_reg = value->val.value.reg.reg;
		for(long r=0;r!=8;r++) if(fpu[r]== _reg) goto next;

		__auto_type var=IRCreateVirtVar(&typeU0);
		var->name=strcpy(calloc(strlen(_reg->name)+1,1), _reg->name);
		__auto_type newNode = IRCreateVarRef(var);
		ptrMapVar2RegAdd(var2Reg, var, _reg);
		
		
		struct IRATTRoldRegSlice newAttr;
		newAttr.base.name = IR_ATTR_OLD_REG_SLICE;
		newAttr.base.destroy = NULL;
		newAttr.old = allNodes[i];
		IRAttrReplace(newNode, __llCreate(&newAttr, sizeof(newAttr)));
		swapNode(allNodes[i], newNode);

	next:;
	}
	strGraphNodeIRPDestroy(&allNodes);
	allNodes=graphNodeIRAllNodes(start);
	//
	// This has a side effect of attributing basic block attributes to nodes,which tell the in/out variables for each node in an expression
	//
	strGraphNodeIRLiveP livenessGraphs CLEANUP(strGraphNodeIRLivePDestroy) = IRInterferenceGraphFilter(start, NULL,NULL);
	for (long n = 0; n != strGraphNodeIRPSize(allNodes); n++) {
		struct IRNodeFuncCall *call = (void *)graphNodeIRValuePtr(allNodes[n]);
		__auto_type attr = llIRAttrFind(call->base.attrs, IR_ATTR_BASIC_BLOCK, IRAttrGetPred);
		if(!attr)
				continue;
		
		assert(attr);
		struct IRAttrBasicBlock *block = (void *)llIRAttrValuePtr(attr);

		strVar inVars CLEANUP(strVarDestroy) = NULL;
		strVar outVars CLEANUP(strVarDestroy) = NULL;
		for (long i = 0; i != strVarSize(block->block->in); i++) 
				if(ptrMapVar2RegGet(var2Reg,  block->block->in[i].var))
						inVars = strVarSortedInsert(inVars, block->block->in[i], IRVarCmp);
		for (long o = 0; o != strVarSize(block->block->out); o++)
				if(ptrMapVar2RegGet(var2Reg,  block->block->out[o].var))
						outVars = strVarSortedInsert(outVars, block->block->out[o], IRVarCmp);

		// Find insrection in/out registers that conflict
		strRegP conflicts = NULL;
		for (long i = 0; i != strVarSize(inVars); i++) {
				__auto_type iFind = *ptrMapVar2RegGet(var2Reg, inVars[i].var);
				if(!iFind) continue;
				conflicts = strRegPSortedInsert(conflicts, iFind, (regCmpType)ptrPtrCmp);
		}
			for (long o = 0; o != strVarSize(outVars); o++) {
				__auto_type oFind = *ptrMapVar2RegGet(var2Reg, outVars[o].var);
				if(!oFind) continue;
				conflicts = strRegPSortedInsert(conflicts, oFind, (regCmpType)ptrPtrCmp);
			}
			
		strRegP inRegs =NULL;
		for (long i = 0; i != strVarSize(inVars); i++) {
				__auto_type iFind = *ptrMapVar2RegGet(var2Reg, inVars[i].var);
				inRegs=strRegPSortedInsert(inRegs, iFind, (regCmpType)ptrPtrCmp);
		}
		inRegs=mergeConflictingRegs(inRegs);

		strRegP outRegs =NULL;
		for (long i = 0; i != strVarSize(outVars); i++) {
				__auto_type oFind = *ptrMapVar2RegGet(var2Reg, outVars[i].var);
				outRegs=strRegPSortedInsert(outRegs, oFind, (regCmpType)ptrPtrCmp);
		}
		outRegs=mergeConflictingRegs(outRegs);
		
		conflicts = mergeConflictingRegs(conflicts);

		struct IRAttrABIInfo info;
		info.base.name = IR_ATTR_ABI_INFO;
		info.base.destroy = IRAttrABIInfoDestroy;
		info.toPushPop = conflicts;
		info.liveIn=inRegs;
		info.liveOut=outRegs;
		
		llIRAttr infoAttr = __llCreate(&info, sizeof(info));
		call->base.attrs = llIRAttrInsert(call->base.attrs, infoAttr, IRAttrInsertPred);
	}

	// Repalce temp variables with original registers
	for (long n = 0; n != strGraphNodeIRPSize(allNodes); n++) {
		struct IRNodeValue *node = (void *)graphNodeIRValuePtr(allNodes[n]);
		if (node->base.type != IR_VALUE)
			continue;
		if (node->val.type != IR_VAL_VAR_REF)
			continue;
		__auto_type find = llIRAttrFind(node->base.attrs, IR_ATTR_OLD_REG_SLICE, IRAttrGetPred);
		if (!find)
			continue;
		struct IRATTRoldRegSlice *attr = (void *)llIRAttrValuePtr(find);
		swapNode(allNodes[n], attr->old);
		graphNodeIRKill(&allNodes[n], (void (*)(void *))IRNodeDestroy, NULL);
	}

	ptrMapVar2RegDestroy(var2Reg, NULL);
}
static strGraphNodeIRP getFuncArgs(graphNodeIR call) {
	strGraphEdgeIRP in CLEANUP(strGraphEdgeIRPDestroy) = IREdgesByPrec(call);
	strGraphNodeIRP args = NULL;
	for (long i = 0; i != strGraphEdgeIRPSize(in); i++) {
		//switch (*graphEdgeIRValuePtr(in[i])) {
		//default:
		//	break;
		//case IR_CONN_FUNC_ARG_1 ... IR_CONN_FUNC_ARG_128:
		//	args = strGraphNodeIRPAppendItem(args, graphEdgeIRIncoming(in[i]));
		//}
		auto v = *graphEdgeIRValuePtr(in[i]);
		if (v >= IR_CONN_FUNC_ARG_1 && v<= IR_CONN_FUNC_ARG_128) {
			args = strGraphNodeIRPAppendItem(args, graphEdgeIRIncoming(in[i]));
		}
	}
	return args;
}
void IRComputeABIInfo(graphNodeIR start) {
	findRegisterLiveness(start);
	computedABIInfo = 1;
}
static void assembleInst(const char *name, strX86AddrMode args) {
		__auto_type template = X86OpcodeByArgs(name, args);
		assert(template);
		int err;
	X86EmitAsmInst(template, args, &err);
	assert(!err);
}
static int conflictsWithRegisters(const void *_regs, const struct reg **r) {
	strRegP regs = (void *)_regs;
	for (long c = 0; c != strRegPSize(regs); c++)
		if (regConflict((struct reg *)*r, regs[c]))
			return 1;
	return 0;
}
//
// http://www.sco.com/developers/devspecs/abi386-4.pdf
//
static strVar IR_ABI_I386_SYS_InsertLoadArgs(graphNodeIR start);
static void IR_ABI_I386_SYSV_2Asm(graphNodeIR start ,struct X86AddressingMode *funcMode,strX86AddrMode args,struct X86AddressingMode *outMode) {
	if (!computedABIInfo) {
		fprintf(stderr, "CALL computedABIInfo before calling me!!!\n");
		assert(computedABIInfo); // Call IRComputeABIInfo!!!
	}
	struct IRNode *call = (void *)graphNodeIRValuePtr(start);
	llIRAttr llInfo = (void *)llIRAttrFind(call->attrs, IR_ATTR_ABI_INFO, IRAttrGetPred);
	assert(llInfo);
	struct IRAttrABIInfo *info = (void *)llIRAttrValuePtr(llInfo);

	struct object *funcType = funcMode->valueType;
	if (funcType->type == TYPE_PTR)
		funcType = ((struct objectPtr *)funcType)->type;
	assert(funcType->type == TYPE_FUNCTION);
	struct objectFunction *func = (struct objectFunction *)funcType;
	assert(strX86AddrModeSize(args) >= strFuncArgSize(func->args));

	strRegP clobbered CLEANUP(strRegPDestroy) = strRegPClone(info->toPushPop);
	// EAX is used as a scracth register here
	clobbered = strRegPSortedInsert(clobbered, &regX86EAX, (regCmpType)ptrPtrCmp);
	clobbered = mergeConflictingRegs(clobbered);

	strRegP preserved CLEANUP(strRegPDestroy) = NULL;
	preserved = strRegPSortedInsert(preserved, &regX86EBX, (regCmpType)ptrPtrCmp);
	preserved = strRegPSortedInsert(preserved, &regX86ESI, (regCmpType)ptrPtrCmp);
	preserved = strRegPSortedInsert(preserved, &regX86EDI, (regCmpType)ptrPtrCmp);
	preserved = strRegPSortedInsert(preserved, &regX86ESP, (regCmpType)ptrPtrCmp);
	preserved = strRegPSortedInsert(preserved, &regX86EBP, (regCmpType)ptrPtrCmp);
	clobbered = strRegPRemoveIf(clobbered, preserved, conflictsWithRegisters);

	long stackSize = 0;
	long eaxOffset = 0;

	// Passes a secret first arg if returns a struct/union(that isn't based on a primtive)
	// For example(I32 is treated as a I32i so dont return a union)
	__auto_type retType = objectBaseType(func->retType);
	int retsStruct = retType->type == TYPE_CLASS || retType->type == TYPE_UNION;
	int retsFloat = retType == &typeF64;

	// Is an int
	if (!retsStruct && !retsFloat && retType != &typeU0) {
		// Make a dummy area to store retVal when poping registers
		strX86AddrMode dpp CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeSint(0));
		assembleInst("PUSH", dpp);
		stackSize += 4;
	}

	// Push all the clobered
	for (long p = 0; p != strRegPSize(clobbered); p++) {
		if (clobbered[p] == &regX86EAX)
			eaxOffset = stackSize;

		strX86AddrMode r CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeReg(clobbered[p],getTypeForSize(clobbered[p]->size)));
		pushMode(r[0]);
		stackSize += clobbered[p]->size;
	}

	long stackSizeBeforeArgs = stackSize;
	long varLenArgListStart=0;
	long offset=0;
	for (long i = strX86AddrModeSize(args) - 1; i >= 0; i--) {
			if(strFuncArgSize(func->args)-offset++==0)
					varLenArgListStart=stackSize;
			
			__auto_type type = objectBaseType(args[i]->valueType);
		long itemSize = objectSize(type, NULL);
		if (type->type == TYPE_CLASS || type->type == TYPE_UNION) {
				struct X86AddressingMode *eaxMode CLEANUP(X86AddrModeDestroy)=X86AddrModeReg(&regX86EAX,&typeI32i);
				struct X86AddressingMode *stackPtr CLEANUP(X86AddrModeDestroy)=X86AddrModeReg(stackPointer(),getTypeForSize(ptrSize()));
				asmAssign(start,eaxMode, stackPtr, ptrSize(), ASM_ASSIGN_X87FPU_POP);
				
				struct X86AddressingMode *top CLEANUP(X86AddrModeDestroy) = X86AddrModeIndirSIB(0,NULL,NULL,X86AddrModeReg(&regX86EAX,&typeI32i), type);
				X86AddrModeIndirSIBAddOffset(top, -itemSize);
			struct X86AddressingMode *val CLEANUP(X86AddrModeDestroy) = X86AddrModeClone(args[i]);
			asmTypecastAssign(start,top, val,0);

			// Must be aligned to 4 bytes
			long aligned = itemSize / 4 * 4 + ((itemSize % 4) ? 4 : 0);
			strX86AddrMode addSP CLEANUP(strX86AddrModeDestroy2) = NULL;
			addSP = strX86AddrModeAppendItem(addSP, X86AddrModeReg(stackPointer(),getTypeForSize(ptrSize())));
			addSP = strX86AddrModeAppendItem(addSP, X86AddrModeSint(aligned));
			assembleInst("SUB", addSP);

			stackSize+=aligned;
		} else if(type==&typeF64) {
				struct X86AddressingMode *mode CLEANUP(X86AddrModeDestroy)=X86AddrModeClone(args[i]);
				pushMode(mode);
				stackSize+=8;
		} else {
				strX86AddrMode pushArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
				struct X86AddressingMode *mode CLEANUP(X86AddrModeDestroy) = X86AddrModeClone(args[i]);
				//
				// EAX is trashed during the calling sequence,so retrieve EAX from the pushed registers if wants an EAX affecting register
				//
				int loadEAX = 0;
				if (mode->type == X86ADDRMODE_REG)
						if (regConflict(mode->value.reg, &regX86EAX)) {
								loadEAX = 1;
						}
				if (loadEAX) {
						strX86AddrMode xchgArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
						xchgArgs = strX86AddrModeAppendItem(xchgArgs, X86AddrModeReg(&regX86EAX,&typeI32i));
						xchgArgs =
				    strX86AddrModeAppendItem(xchgArgs, X86AddrModeIndirSIB(0, NULL, X86AddrModeReg(stackPointer(),getTypeForSize(ptrSize())), X86AddrModeSint(-(eaxOffset - stackSize)-4), &typeU32i)); 
						assembleInst("MOV", xchgArgs);
				}

				if (itemSize != 4) {
						struct X86AddressingMode *eax CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(&regX86EAX,&typeI32i);
						eax->valueType=&typeI32i;
						asmTypecastAssign(start,eax, mode,0);
						pushArgs = strX86AddrModeAppendItem(pushArgs, X86AddrModeReg(&regX86EAX,&typeI32i));
				} else {
						pushArgs = strX86AddrModeAppendItem(pushArgs, X86AddrModeClone(args[i]));
				}
				stackSize+=4;
				pushMode(pushArgs[0]);
		}
	}

	if(strFuncArgSize(func->args)<strX86AddrModeSize(args)) {
			//
			//HolyC specific,store I32 count in ECX
			//
			long words=(varLenArgListStart-stackSizeBeforeArgs)/dataSize();
			struct X86AddressingMode *ecxMode CLEANUP(X86AddrModeDestroy)=X86AddrModeReg(&regX86ECX,&typeI32i);
			struct X86AddressingMode *wordsCount CLEANUP(X86AddrModeDestroy)=X86AddrModeSint(words);
			asmAssign(start,ecxMode, wordsCount, 4, 0);
	}

	//Structure pointer is last argument pushed
	if (retsStruct) {
		
		strX86AddrMode leaArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
		leaArgs = strX86AddrModeAppendItem(leaArgs, X86AddrModeReg(&regX86EAX,&typeI32i));
		leaArgs = strX86AddrModeAppendItem(leaArgs, X86AddrModeClone(outMode));
		leaArgs[1]->valueType=NULL;
		assembleInst("LEA", leaArgs);

		pushReg(&regX86EAX);
	}
	
	strX86AddrMode callArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
	callArgs = strX86AddrModeAppendItem(callArgs, X86AddrModeClone(funcMode));
	assembleInst("CALL", callArgs);

	if (retType != &typeU0 && outMode) {
		if (!retsStruct) {
			// We point to area we made on the stack for the return value eariler
			struct X86AddressingMode *outMode2 CLEANUP(X86AddrModeDestroy) =
					X86AddrModeIndirSIB(0, NULL, X86AddrModeReg(stackPointer(),getTypeForSize(ptrSize())), X86AddrModeSint(stackSize - 4), outMode->valueType);
			if (objectBaseType(outMode->valueType) != &typeF64) {
					struct X86AddressingMode *eaxMode CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(&regX86EAX,retType);
				// Assign type of eaxMode
					asmTypecastAssign(start,outMode2, eaxMode,0);
				goto end;
			} else {
				struct X86AddressingMode *outMode2 CLEANUP(X86AddrModeDestroy) = X86AddrModeClone(outMode);
				struct X86AddressingMode *st0Mode CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(&regX86ST0,&typeF64);
				asmAssign(start,outMode2, st0Mode, 8,ASM_ASSIGN_X87FPU_POP);
				goto end;
			}
		} else {				
				struct X86AddressingMode *outMode2 CLEANUP(X86AddrModeDestroy) = X86AddrModeClone(outMode);
				struct X86AddressingMode *indirEaxMode CLEANUP(X86AddrModeDestroy) = X86AddrModeIndirReg(&regX86EAX, outMode2->valueType);
				asmAssign(start,outMode2, indirEaxMode, objectSize(outMode2->valueType, NULL),0);
				goto end;
		}
		assert(0);
	}
end:;
	
	strX86AddrMode stackSubArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
	stackSubArgs = strX86AddrModeAppendItem(stackSubArgs, X86AddrModeReg(stackPointer(),objectPtrCreate(&typeU0)));
	stackSubArgs = strX86AddrModeAppendItem(stackSubArgs, X86AddrModeSint(stackSize - stackSizeBeforeArgs));
	assembleInst("ADD", stackSubArgs); 
	stackSize = stackSizeBeforeArgs;

	// Pop all the clobered
	for (long p = strRegPSize(clobbered) - 1; p >= 0; p--) {
		if (clobbered[p] == &regX86EAX)
			eaxOffset = stackSize;

		strX86AddrMode r CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeReg(clobbered[p],getTypeForSize(clobbered[p]->size)));
		popMode(r[0]);
		stackSize -= clobbered[p]->size;
	}
	
	// Now we are left with the spot we made eariler for return values,let's pop it(if not a structure,all int non-structs get stuffed in EAX)
	if (!retsStruct && !retsFloat && retType != &typeU0) {
			if(outMode) {
					strX86AddrMode outArgs CLEANUP(strX86AddrModeDestroy2) = strX86AddrModeAppendItem(NULL, X86AddrModeClone(outMode));
					if(objectSize(outMode->valueType, NULL)<4)
							outMode->valueType=&typeI32i;
					popMode(outArgs[0]);
					stackSize -= 4;
			} else {
					strX86AddrMode addArgs CLEANUP(strX86AddrModeDestroy2)=NULL; 
					addArgs=strX86AddrModeAppendItem(addArgs, X86AddrModeReg(stackPointer(),getTypeForSize(ptrSize())));
					addArgs=strX86AddrModeAppendItem(addArgs, X86AddrModeSint(4));
					assembleInst("ADD", addArgs);
					stackSize -= 4;
			}
	}
	assert(stackSize == 0);
}
#define VA_ARG_LIST_ARGV -1
static struct X86AddressingMode *abiI386AddrModeNode(struct objectFunction *func, long argI) {
	int returnsStruct = func->retType->type == TYPE_CLASS || func->retType->type == TYPE_UNION;
	long offset = returnsStruct ? -12 : -8;
	if (returnsStruct&&argI==0) {
			return X86AddrModeIndirSIB(0, NULL, X86AddrModeReg(basePointer(), objectPtrCreate(&typeU0)), X86AddrModeSint(8),  objectPtrCreate(&typeU0));
	}
	for (long i = 0; i != strFuncArgSize(func->args); i++) {
		long size = objectSize(func->args[i].type, NULL);
		if (i+returnsStruct == argI)
				return X86AddrModeIndirSIB(0, NULL, X86AddrModeReg(basePointer(), objectPtrCreate(&typeU0)), X86AddrModeSint(-offset),  func->args[i].type);
		offset -= size / 4 * 4 + ((size % 4) ? 4 : 0);
	}
	if(argI==VA_ARG_LIST_ARGV) {
			return X86AddrModeIndirSIB(0, NULL, X86AddrModeReg(basePointer(), objectPtrCreate(&typeU0)), X86AddrModeSint(-offset), objectPtrCreate(&typeU0));
	}
	return NULL;
}
static void transparentKillNode(graphNodeIR node) {
		strGraphEdgeIRP in CLEANUP(strGraphEdgeIRPDestroy)=graphNodeIRIncoming(node);
		strGraphEdgeIRP out CLEANUP(strGraphEdgeIRPDestroy)=graphNodeIROutgoing(node);
		for(long i=0;i!=strGraphEdgeIRPSize(in);i++) {
				for(long o=0;o!=strGraphEdgeIRPSize(out);o++)
						graphNodeIRConnect(graphEdgeIRIncoming(in[i]), graphEdgeIROutgoing(out[o]), *graphEdgeIRValuePtr(in[i]));
		}
		graphNodeIRKill(&node ,(void(*)(void*))IRNodeDestroy, NULL);
}
static strVar IR_ABI_I386_SYS_InsertLoadArgs(graphNodeIR start) {
	calledInsertArgs = 0;

	strGraphNodeIRP nodes CLEANUP(strGraphNodeIRPDestroy) = graphNodeIRAllNodes(start);
	struct IRNodeFuncStart *funcStart = (void *)graphNodeIRValuePtr(start);
	struct objectFunction *fType = (void *)funcStart->func->type;

	// Returning a structure returns adds a hidden first argument pointing to the return location
	int returnsStruct = 0;
	if (fType->retType->type == TYPE_CLASS || fType->retType->type == TYPE_UNION)
		returnsStruct = 1;
	for (long v = 0; v < strFuncArgSize(fType->args); v++) {
			struct X86AddressingMode *arg CLEANUP(X86AddrModeDestroy) = abiI386AddrModeNode(fType, v+returnsStruct);
		__auto_type var = fType->args[v].var;
		struct X86AddressingMode *varMode CLEANUP(X86AddrModeDestroy)=X86AddrModeVar(var, 0);
		asmTypecastAssign(NULL, varMode, arg, ASM_ASSIGN_X87FPU_POP);
	}

	if(fType->hasVarLenArgs) {
			//
			// HolyC specific,ECX contains number of words 
			//
			struct X86AddressingMode * argc CLEANUP(X86AddrModeDestroy)=X86AddrModeReg(&regX86ECX, &typeI32i);
			__auto_type varC=fType->argcVar;
			struct X86AddressingMode *varCMode CLEANUP(X86AddrModeDestroy)=X86AddrModeVar(varC, 0);
			asmTypecastAssign(NULL, varCMode, argc, ASM_ASSIGN_X87FPU_POP);
			
			struct X86AddressingMode * argv CLEANUP(X86AddrModeDestroy)=abiI386AddrModeNode(fType,VA_ARG_LIST_ARGV);
			__auto_type varV=fType->argvVar;
			struct X86AddressingMode *varMode CLEANUP(X86AddrModeDestroy)=X86AddrModeVar(varV, 0);
			strX86AddrMode leaArgs CLEANUP(strX86AddrModeDestroy)=NULL;
			leaArgs=strX86AddrModeAppendItem(leaArgs, varMode);
			leaArgs=strX86AddrModeAppendItem(leaArgs, argv);
			assembleOpcode(NULL, "LEA", leaArgs);
	}
	
	for (long n = 0; n != strGraphNodeIRPSize(nodes); n++) {
		struct IRNodeFuncArg *arg = (void *)graphNodeIRValuePtr(nodes[n]);
		if (arg->base.type == IR_FUNC_ARG) {
				transparentKillNode(nodes[n]);
		} else if(arg->base.type==IR_FUNC_VAARG_ARGC) {
				transparentKillNode(nodes[n]);
		} else if(arg->base.type==IR_FUNC_VAARG_ARGV) {
				transparentKillNode(nodes[n]);
		}
	}
	
	return NULL;
}
static void abiI386LoadPreservedRegs(long frameSize) {
		struct X86AddressingMode *ebpMode CLEANUP(X86AddrModeDestroy)=X86AddrModeReg(basePointer(),objectPtrCreate(&typeU0));
		struct X86AddressingMode *espMode CLEANUP(X86AddrModeDestroy)=X86AddrModeReg(stackPointer(),objectPtrCreate(&typeU0));
		asmAssign(NULL,espMode, ebpMode, ptrSize(), 0);

		strX86AddrMode subArgs CLEANUP(strX86AddrModeDestroy2)=NULL;
		subArgs=strX86AddrModeAppendItem(subArgs, X86AddrModeReg(stackPointer(),getTypeForSize(ptrSize())));
		subArgs=strX86AddrModeAppendItem(subArgs, X86AddrModeSint(frameSize+4+4+4));
		assembleInst("SUB", subArgs);

		popReg(&regX86EDI);
		popReg(&regX86ESI);
		popReg(&regX86EBX);
}
static void IR_ABI_I386_SYSV_Return(graphNodeIR start,long frameSize) {
		if(!start) {
				abiI386LoadPreservedRegs(frameSize);
				assembleInst("LEAVE", NULL);
				assembleInst("RET", NULL);
				return;
		}
	strGraphEdgeIRP in CLEANUP(strGraphEdgeIRPDestroy) = graphNodeIRIncoming(start);
	strGraphEdgeIRP inSource CLEANUP(strGraphEdgeIRPDestroy) = IRGetConnsOfType(in, IR_CONN_SOURCE_A);
	if (strGraphEdgeIRPSize(inSource) != 0) {
		__auto_type source = graphEdgeIRIncoming(inSource[0]);
		struct X86AddressingMode *mode CLEANUP(X86AddrModeDestroy) = IRNode2AddrMode(source);
		__auto_type baseType = objectBaseType(mode->valueType);
		if (baseType == &typeF64) {
			if (mode->type == X86ADDRMODE_REG) {
				// X87 fpu stack must contain return ST(0) upon return
				assert(mode->value.reg == &regX86ST0);
				abiI386LoadPreservedRegs(frameSize);
				goto loadBasePtr;
			} else {
					struct X86AddressingMode *st0 CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(&regX86ST0,&typeF64);
				st0->valueType=&typeF64;
				asmTypecastAssign(start,st0, mode,0);
				abiI386LoadPreservedRegs(frameSize);
				goto loadBasePtr;
			}
		} else if (baseType->type == TYPE_CLASS || baseType->type == TYPE_UNION) {

			/* SystemV ABI expects both struct return addess and func return address to be popped
			 * So let's exchange the  struct pointer and func return addr,pop into EAX then reutrn
			 */
			struct X86AddressingMode *retPtr CLEANUP(X86AddrModeDestroy) =
					X86AddrModeIndirSIB(0, NULL, X86AddrModeReg(basePointer(),objectPtrCreate(&typeU0)), X86AddrModeSint(4), objectPtrCreate(baseType));
			struct X86AddressingMode *eaxNode CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(&regX86EAX,&typeI32i);
			asmAssign(start,eaxNode, retPtr, ptrSize(),0);
			
			strX86AddrMode xchgArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
			__auto_type structPtr = X86AddrModeIndirSIB(0, NULL, X86AddrModeReg(basePointer(),objectPtrCreate(&typeU0)), X86AddrModeSint(8), objectPtrCreate(baseType));
			xchgArgs = strX86AddrModeAppendItem(xchgArgs, structPtr);
			xchgArgs = strX86AddrModeAppendItem(xchgArgs, X86AddrModeReg(&regX86EAX,&typeI32i));
			assembleInst("XCHG", xchgArgs);

			struct X86AddressingMode *indirEaxMode CLEANUP(X86AddrModeDestroy) = X86AddrModeIndirReg(&regX86EAX, baseType);
			asmAssign(start,indirEaxMode, mode, objectSize(baseType, NULL),0);
			
			abiI386LoadPreservedRegs(frameSize);
			// Pop the extra 4 bytes from the return address,then return
			assembleInst("LEAVE", NULL);
			strX86AddrMode addArgs CLEANUP(strX86AddrModeDestroy2) = NULL;
			addArgs=strX86AddrModeAppendItem(addArgs, X86AddrModeReg(stackPointer(),objectPtrCreate(&typeU0)));
			addArgs=strX86AddrModeAppendItem(addArgs, X86AddrModeSint(4));
			assembleInst("ADD", addArgs);
			
			goto ret;
		} else if (baseType == &typeU0) {
				abiI386LoadPreservedRegs(frameSize);
				goto loadBasePtr;
		} else {
			// Isn't a struct/union/F64,so is an int/ptr
				struct X86AddressingMode *eaxMode CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(&regX86EAX, &typeI32i);
			asmTypecastAssign(start,eaxMode, mode,0);
			
			abiI386LoadPreservedRegs(frameSize);
			goto loadBasePtr;
		}
	} else {
			abiI386LoadPreservedRegs(frameSize);
	}
loadBasePtr : { assembleInst("LEAVE", NULL); }
ret:
	assembleInst("RET", NULL);
}
strVar IRABIInsertLoadArgs(graphNodeIR start,long frameSize) {
	calledInsertArgs = 1;
	switch (getCurrentArch()) {
	case ARCH_TEST_SYSV:
	case ARCH_X86_SYSV: {
		return IR_ABI_I386_SYS_InsertLoadArgs(start);
	}
	case ARCH_X64_SYSV:
			IR_ABI_SYSV_X64_LoadArgs(start,frameSize);
			return NULL;
	}
}

void IRABIAsmPrologue(long frameSize) {
	switch (getCurrentArch()) {
	case ARCH_TEST_SYSV:
	case ARCH_X86_SYSV: {
			struct X86AddressingMode *esp CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(stackPointer(),objectPtrCreate(&typeU0));
		struct X86AddressingMode *ebp CLEANUP(X86AddrModeDestroy) = X86AddrModeReg(basePointer(),objectPtrCreate(&typeU0));
		pushReg(basePointer());
		asmAssign(NULL,ebp, esp, ptrSize(),0);

		strX86AddrMode subArgs CLEANUP(strX86AddrModeDestroy2)=NULL;
		subArgs=strX86AddrModeAppendItem(subArgs, X86AddrModeReg(stackPointer(),objectPtrCreate(&typeU0)));
		subArgs=strX86AddrModeAppendItem(subArgs, X86AddrModeSint(frameSize));
		assembleInst("SUB", subArgs);

		pushReg(&regX86EBX);
		pushReg(&regX86ESI);
		pushReg(&regX86EDI);
		return;
	}
	case ARCH_X64_SYSV:
			IR_ABI_SYSV_X64_Prologue(frameSize);
			return;
	}
}
void IRABIReturn2Asm(graphNodeIR start,long frameSize) {
	switch (getCurrentArch()) {
	case ARCH_TEST_SYSV:
	case ARCH_X86_SYSV: {
			return IR_ABI_I386_SYSV_Return(start,frameSize);
	}
	case ARCH_X64_SYSV:
			return IR_ABI_SYSV_X64_Return(start,frameSize);
	}
}
void IRABICall2Asm(graphNodeIR start ,struct X86AddressingMode *funcMode,strX86AddrMode args,struct X86AddressingMode *outMode) {
	switch (getCurrentArch()) {
	case ARCH_TEST_SYSV:
	case ARCH_X86_SYSV: {
			return IR_ABI_I386_SYSV_2Asm(start,funcMode,args,outMode);
	}
	case ARCH_X64_SYSV:
			return IR_ABI_SYSV_X64_Call(start,funcMode,args,outMode);
	}
}
void IRABIFuncNode2Asm(graphNodeIR start) {
		struct IRNodeFuncCall *call=(void*)graphNodeIRValuePtr(start);
		strGraphEdgeIRP incoming CLEANUP(strGraphEdgeIRPDestroy)=graphNodeIRIncoming(start);
		strGraphEdgeIRP inFunc CLEANUP(strGraphEdgeIRPDestroy)=IRGetConnsOfType(incoming, IR_CONN_FUNC);
		strGraphNodeIRP funcArgs CLEANUP(strGraphNodeIRPDestroy)=getFuncArgs(start);
		strGraphEdgeIRP outgoing CLEANUP(strGraphEdgeIRPDestroy)=graphNodeIROutgoing(start);
		strGraphEdgeIRP outDst CLEANUP(strGraphEdgeIRPDestroy)=IRGetConnsOfType(outgoing, IR_CONN_DEST);
		strGraphEdgeIRP outDst2 CLEANUP(strGraphEdgeIRPDestroy)=IRGetConnsOfType(outgoing, IR_CONN_ASSIGN_FROM_PTR);
		
		
		graphNodeIR outNode=NULL;
		if(strGraphEdgeIRPSize(outDst)==1) {
				outNode=graphEdgeIROutgoing(outDst[0]);
		}
		if(strGraphEdgeIRPSize(outDst2)==1) {
				outNode=graphEdgeIROutgoing(outDst2[0]);
		}
		
		strX86AddrMode args CLEANUP(strX86AddrModeDestroy2)=strX86AddrModeResize(NULL, strGraphNodeIRPSize(funcArgs));
		for(long a=0;a!=strGraphNodeIRPSize(funcArgs);a++)
				args[a]=IRNode2AddrMode(funcArgs[a]);

		struct X86AddressingMode *oMode CLEANUP(X86AddrModeDestroy)=NULL;
		if(outNode)
				oMode=IRNode2AddrMode(outNode);
		struct X86AddressingMode *funcMode CLEANUP(X86AddrModeDestroy)=IRNode2AddrMode(graphEdgeIRIncoming(inFunc[0]));
		return IRABICall2Asm(start, funcMode, args, oMode);
}
