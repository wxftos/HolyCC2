#include <IR.h>
#include <parserA.h>
#include <linkedList.h>
#include <hashTable.h>
#include <assert.h>
#include <parse2IR.h>
#include <stdio.h>
#include <exprParser.h>
#include <stdarg.h>
MAP_TYPE_DEF(enum IRNodeType,IRNodeType);
MAP_TYPE_FUNCS(enum IRNodeType,IRNodeType);
#define ALLOCATE(x) ({typeof(&x) ptr=malloc(sizeof(x));memcpy(ptr,&x,sizeof(x));ptr;})
#define GRAPHN_ALLOCATE(x) ({__graphNodeCreate(&x,sizeof(x),0);})
struct IRVarRefs {
		struct IRVar *var;
		long refs;
};
MAP_TYPE_DEF(struct IRVarRefs,IRVarRefs);
MAP_TYPE_FUNCS(struct IRVarRefs,IRVarRefs);
MAP_TYPE_DEF(struct parserNode*,ParserNode);
MAP_TYPE_FUNCS(struct parserNode*,ParserNode);
enum scopeType {
		SCOPE_TYPE_FUNC,
		SCOPE_TYPE_SCOPE,
		SCOPE_TYPE_LOOP,
		SCOPE_TYPE_SWIT,
};
struct IRGenScopeStack {
		enum scopeType type;
		union {
				struct {
						graphNodeIR next;
						graphNodeIR exit;
				} loop;
				graphNodeIR switExitLab;
		} value;
};
STR_TYPE_DEF(struct IRGenScopeStack, ScopeStack);
STR_TYPE_FUNCS(struct IRGenScopeStack, ScopeStack);
struct IRGenInst {
		mapIRVarRefs vars;
		mapParserNode cases;
		mapParserNode labels;
		strGraphNodeIRP currentNodes;
		strScopeStack scopes;
};
static void setCurrentNodes(struct IRGenInst *inst,...) {
		if(inst->currentNodes)
				strGraphNodeIRPDestroy(&inst->currentNodes);

		inst->currentNodes=NULL;
		va_list list;
		va_start(list, inst);
		for(;;) {
				__auto_type node=va_arg(list, graphNodeIR);
				if(!node)
						break;

				inst->currentNodes=strGraphNodePAppendItem(inst->currentNodes, node);
		}
		va_end(list);
}
static struct IRGenInst *currentGen=NULL;
static void connectCurrentsToNode(graphNodeIR node) {
		for(long i=0;i!=strGraphNodeIRPSize(currentGen-> currentNodes);i++) {
				graphNodeIRConnect(currentGen-> currentNodes[i], node, IR_CONN_FLOW);
		}
}
STR_TYPE_DEF(char , Char);
STR_TYPE_FUNCS(char , Char);
static strChar strClone(const char *str) {
		strChar buffer=strCharResize(NULL,strlen(str)+1);
		strcpy(buffer, str);

		return buffer;
}
static char *IRAssignHash(struct parserNode *node) {
		if(node->type==NODE_VAR) {
				struct parserNodeVar *var=(void*)node;
				
				long len=sprintf(NULL, "V%p", var->var);
				char buffer[len+1];
				sprintf(NULL, "V%p", var->var);
				return strClone(buffer);
		} else if(node->type==NODE_MEMBER_ACCESS) {
				strChar tail=NULL;

				//Ensure left-most item is var
				//Also make the tail str
				struct parserNodeVar *varNode;
				for(__auto_type node2=node;; ) {
						if(node2->type==NODE_MEMBER_ACCESS) {
								struct parserNodeMemberAccess *access=(void*)node2;
								node2=access->exp;

								struct parserNodeName *name=(void*)access->name;
								assert(name->base.type==NODE_NAME);
								tail=strCharConcat(strCharAppendItem(strClone(name->text), '.'),tail);
						} else if(node2->type==NODE_VAR) {
								varNode=(void*)node2;
								break;
						} else {
								strCharDestroy(&tail);
								return NULL;
						}
				}
				
				long len=sprintf(NULL, "V%p", varNode->var);
				char buffer[len+1];
				sprintf(NULL, "V%p", varNode->var);
					
				return strCharConcat(strClone(buffer),tail);
		}
		
		return NULL;
};
strChar ptr2Str(const void *ptr) {
		long len=sprintf(NULL, "%p", ptr);
		char buffer[len+1];
		sprintf(buffer, "%p", ptr);

		return strCharAppendData(NULL, buffer, strlen(buffer)+1);
}
//
// Assign operations  take 2 inputs "a" and "b","b" is the dest,"a" is the source
//
graphNodeIR IRAssign(struct parserNode *to,struct parserNode *from) {
		struct IRGenInst *gen=currentGen;
		__auto_type fromValue=parserNode2IR(from,0);
		
		//Returns NULL on unhashable,
		char *hash=IRAssignHash(to);
		if(!hash) {
				__auto_type toVar=parserNode2IR(to,0);
				struct IRNodeAssign assign;
				assign.base.type=IR_ASSIGN;
				assign.base.attrs=NULL;
				
				__auto_type retVal=GRAPHN_ALLOCATE(assign);
				graphNodeIRConnect(fromValue, retVal, IR_CONN_SOURCE_A);
				graphNodeIRConnect( retVal,toVar, IR_CONN_DEST);

				setCurrentNodes(currentGen, retVal,NULL);
				return retVal;
		}
		
		__auto_type find=mapIRVarRefsGet(gen->vars, hash);
		if(!find) {
				struct IRVar var;
				if(to->type==NODE_VAR) {
						var.type=IR_VAR_VAR;
						var.value.var=((struct parserNodeVar*)to)->var;
				} else if(to->type==NODE_MEMBER_ACCESS) {
						var.type=IR_VAR_MEMBER;
						var.value.member=(void*)to;
				} else {
						assert(0);
				}

				struct IRVarRefs refs;
				refs.refs=1;
				refs.var=ALLOCATE(var);
				mapIRVarRefsInsert(gen->vars, hash, refs);
		} else {
				find->refs++;
		}
		struct IRValue ref;
		ref.type=IR_VAL_VAR_REF;
		ref.value.var.SSANum=find->refs;
		ref.value.var.var=find->var;
		struct IRNodeValue valNode;
		valNode.base.type=IR_VALUE;
		valNode.base.attrs=NULL;
		valNode.val=ref;
		__auto_type valNode2=GRAPHN_ALLOCATE(valNode); 
		
		struct IRNodeAssign assign;
		assign.base.type=IR_ASSIGN;
		assign.base.attrs=NULL;

		__auto_type assign2=GRAPHN_ALLOCATE(assign);
		graphNodeIRConnect(fromValue,assign2, IR_CONN_SOURCE_A);
		graphNodeIRConnect(assign2,valNode2, IR_CONN_DEST);

		setCurrentNodes(currentGen, assign2,NULL);
		return assign2;
}
static mapIRNodeType unop2IRType;
static mapIRNodeType binop2IRType;
static mapIRNodeType assign2IRType;
static mapIRNodeType unopAssign2IRType;
static void init() __attribute__((constructor));
static void init() {
		//
		//Assign unops
		//
		unopAssign2IRType=mapIRNodeTypeCreate();
		mapIRNodeTypeInsert(unopAssign2IRType, "++", IR_INC);
		mapIRNodeTypeInsert(unopAssign2IRType, "--", IR_DEC);
		//
		// Unops
		//
		unop2IRType=mapIRNodeTypeCreate();
		mapIRNodeTypeInsert(unop2IRType, "~", IR_BNOT);
		mapIRNodeTypeInsert(unop2IRType, "!", IR_LNOT);
		mapIRNodeTypeInsert(unop2IRType, "-", IR_NEG);
		mapIRNodeTypeInsert(unop2IRType, "+", IR_POS);
		//
		//Binops
		//
		binop2IRType=mapIRNodeTypeCreate();
		mapIRNodeTypeInsert(binop2IRType, "+", IR_ADD);
		mapIRNodeTypeInsert(binop2IRType, "-", IR_SUB);
		//
		mapIRNodeTypeInsert(binop2IRType, "*", IR_MULT);
		mapIRNodeTypeInsert(binop2IRType, "/", IR_DIV);
		mapIRNodeTypeInsert(binop2IRType, "%", IR_MOD);
		//
		mapIRNodeTypeInsert(binop2IRType, "<<", IR_LSHIFT);
		mapIRNodeTypeInsert(binop2IRType, ">>", IR_RSHIFT);
		//
		mapIRNodeTypeInsert(binop2IRType, "&", IR_BAND);
		mapIRNodeTypeInsert(binop2IRType, "|", IR_BOR);
		mapIRNodeTypeInsert(binop2IRType, "^", IR_BXOR);
		//
		mapIRNodeTypeInsert(binop2IRType, ">", IR_GT);
		mapIRNodeTypeInsert(binop2IRType, "<", IR_LT);
		mapIRNodeTypeInsert(binop2IRType, ">=", IR_GE);
		mapIRNodeTypeInsert(binop2IRType, "<=", IR_LE);
		mapIRNodeTypeInsert(binop2IRType, "==", IR_EQ);
		mapIRNodeTypeInsert(binop2IRType, "!=", IR_NE);
		//
		mapIRNodeTypeInsert(binop2IRType, "&&", IR_LAND);
		mapIRNodeTypeInsert(binop2IRType, "||", IR_LOR);
		mapIRNodeTypeInsert(binop2IRType, "^^", IR_LXOR);
		
		//
		//Assigment operators
		//
		assign2IRType=mapIRNodeTypeCreate();
		mapIRNodeTypeInsert(assign2IRType, "=", IR_ASSIGN);
		//
		mapIRNodeTypeInsert(assign2IRType, "+=", IR_ADD);
		mapIRNodeTypeInsert(assign2IRType, "-=", IR_SUB);
		//
		mapIRNodeTypeInsert(assign2IRType, "<<=", IR_LSHIFT);
		mapIRNodeTypeInsert(assign2IRType, ">>=", IR_RSHIFT);
		//
		mapIRNodeTypeInsert(assign2IRType, "*=", IR_MULT);
		mapIRNodeTypeInsert(assign2IRType, "/=", IR_DIV);
		mapIRNodeTypeInsert(assign2IRType, "%=", IR_MOD);
		//
		mapIRNodeTypeInsert(assign2IRType, "*=", IR_MULT);
		mapIRNodeTypeInsert(assign2IRType, "/=", IR_DIV);
		mapIRNodeTypeInsert(assign2IRType, "%=", IR_MOD);
		//
		mapIRNodeTypeInsert(assign2IRType, "&=", IR_BAND);
		mapIRNodeTypeInsert(assign2IRType, "^=", IR_BXOR);
		mapIRNodeTypeInsert(assign2IRType, "|=", IR_BOR);
}
static void deinit() __attribute__((destructor));
static void deinit() {
		mapIRNodeTypeDestroy(assign2IRType, NULL);
		mapIRNodeTypeDestroy(binop2IRType, NULL);
}
//TODO optomize
graphNodeIR IRCondJump(graphNodeIR cond,graphNodeIR successLabel,graphNodeIR failLabel) {
		if(!successLabel&&!failLabel)
				return NULL;
		
		struct IRNodeCondJump jump;
		jump.base.type=IR_COND_JUMP;
		jump.base.attrs=NULL;
		
		__auto_type jump2=GRAPHN_ALLOCATE(jump);
		graphNodeIRConnect(cond, jump2, IR_CONN_COND);
		if(successLabel)
				graphNodeIRConnect(jump2, successLabel, IR_CONN_COND_TRUE);
		if(failLabel)
				graphNodeIRConnect(jump2, failLabel, IR_CONN_COND_TRUE);

		if(successLabel&&failLabel)
				setCurrentNodes(currentGen, successLabel,failLabel,NULL);
		else if(failLabel)
				setCurrentNodes(currentGen, failLabel,NULL);
		else if(successLabel)
				setCurrentNodes(currentGen, successLabel,NULL);

		return jump2;
};
graphNodeIR parserNode2IR(const struct parserNode *node,int createStatement) {
		graphNodeIR retVal=NULL;
		switch(node->type) {
		case NODE_BINOP: {
				struct parserNodeBinop *binop=(void*)node;
				struct parserNodeOpTerm *op=(void*)binop->op;

				//If non-assign binop
				__auto_type b=mapIRNodeTypeGet(binop2IRType, op->text);
				if(b) {
						//Compute args
						__auto_type aVal=parserNode2IR(binop->a,0);
						__auto_type bVal=parserNode2IR(binop->b,0);

						struct IRNodeBinop binop2;
						binop2.base.type=*b;
						binop2.base.attrs=NULL;
						
						__auto_type retVal=GRAPHN_ALLOCATE(binop2);
						graphNodeIRConnect(aVal, aVal, IR_CONN_SOURCE_A);
						graphNodeIRConnect(aVal, bVal, IR_CONN_SOURCE_B);

						setCurrentNodes(currentGen, retVal,NULL);
						return retVal;
				}

				assert(0);
		}
		case NODE_UNOP: {
				struct parserNodeUnop *unop=(void*)node;
				struct parserNodeOpTerm *op=(void*)unop->op;
				__auto_type in=parserNode2IR(unop->a,0);
				//
				//Unops and assign unops are seperate
				//

				//Find assign unop
				__auto_type find=mapIRNodeTypeGet(unopAssign2IRType,op->text);
				if(find) {
						graphNodeIR newNode=NULL;
						if(0==strcmp(op->text,"++")) {
								struct IRNodeInc inc;
								inc.base.type=IR_INC;
								inc.base.attrs=NULL;
								newNode=GRAPHN_ALLOCATE(inc);
						} else if(0==strcmp(op->text,"--")) {
								struct IRNodeInc dec;
								dec.base.type=IR_INC;
								dec.base.attrs=NULL;
								newNode=GRAPHN_ALLOCATE(dec);
						} else {
								struct IRNodeUnop unop;
								unop.base.type=*find;
								unop.base.attrs=NULL;
								newNode=GRAPHN_ALLOCATE(unop);
						}
						assert(newNode!=NULL);

						graphNodeIRConnect(in, newNode, IR_CONN_SOURCE_A);
						setCurrentNodes(currentGen,newNode,NULL);
						return newNode;
				}
				case NODE_ARRAY_ACCESS: {
						struct parserNodeArrayAccess *access=(void*)node;
						__auto_type exp= parserNode2IR(access->exp,0);
						__auto_type index= parserNode2IR(access->index,0);

						int success;
						__auto_type scale=objectSize(assignTypeToOp(node),&success);
						assert(success);

						struct IRNodeArrayAccess access2;
						access2.base.type=IR_ARRAY_ACCESS;
						access2.base.attrs=NULL;
						access2.scale=scale;

						__auto_type retVal= GRAPHN_ALLOCATE(access2);
						graphNodeIRConnect(exp, retVal,IR_CONN_SOURCE_A);
						graphNodeIRConnect(index, retVal,IR_CONN_SOURCE_B);

						setCurrentNodes(currentGen,retVal,NULL);
						return retVal;
				}
		}
		case NODE_DEFAULT:
		case NODE_CASE: {
				__auto_type ptrStr=ptr2Str(node);
				mapParserNodeInsert(currentGen->cases, ptrStr, (struct parserNode*)node);
				strCharDestroy(&ptrStr);
				
				struct IRNodeLabel lab;
				lab.base.type=IR_LABEL;
				lab.base.attrs=NULL;;

				setCurrentNodes(currentGen,GRAPHN_ALLOCATE(lab),NULL);
				return currentGen->currentNodes[0];
		};
		case NODE_KW:
		case NODE_FUNC_DEF:
		case NODE_FUNC_FORWARD_DECL:
		case NODE_CLASS_DEF: {
				// Classes dont exist in code
				return NULL;
		}
		case NODE_COMMA_SEQ: {
				struct parserNodeCommaSeq *seq=(void*)node;

				graphNodeIR lastNode;
				for(long i=0;i!=strParserNodeSize(seq->items);i++) {
						__auto_type node=parserNode2IR(seq->items[i],0);
						for(long i=0;i!=strGraphNodePSize(currentGen->currentNodes);i++)
								graphNodeIRConnect(currentGen->currentNodes[i], node, IR_CONN_FLOW);

						lastNode=node;
						setCurrentNodes(currentGen,node,NULL);
				}

				return lastNode;
		}
		case NODE_DO: {
				struct parserNodeDo *doStmt=(void*)node;
				
				//Label
				struct IRNodeLabel lab	;
				lab.base.type=IR_LABEL;
				lab.base.attrs=NULL;
				__auto_type lab2=GRAPHN_ALLOCATE(lab);
				setCurrentNodes(currentGen, lab);
				
				//Body
				__auto_type body=parserNode2IR(doStmt->body, 1);
				
				//Cond
				__auto_type cond=parserNode2IR(doStmt->cond,1);
				__auto_type cJump=IRCondJump(cond, lab2, NULL);

				setCurrentNodes(currentGen, cJump,NULL);
				return lab2;
		}
		case NODE_FOR: {
				struct IRNodeLabel lab;
				lab.base.type=IR_LABEL;
				lab.base.attrs=NULL;
				__auto_type labNext=GRAPHN_ALLOCATE(lab);
				__auto_type labExit=GRAPHN_ALLOCATE(lab);
				__auto_type labCond=GRAPHN_ALLOCATE(lab);
				
				struct parserNodeFor *forStmt=(void*)node;
				__auto_type init=parserNode2IR(forStmt->init,1);
				connectCurrentsToNode(init);
				setCurrentNodes(currentGen, init,NULL);
				connectCurrentsToNode(labCond);
				setCurrentNodes(currentGen, labCond,NULL);
				
				//"cond" label
				connectCurrentsToNode(labCond);
				setCurrentNodes(currentGen,labCond,NULL);
				
				//Cond
				__auto_type cond=parserNode2IR(forStmt->cond,1);
				IRCondJump(cond, NULL,labExit);
				connectCurrentsToNode(cond);
				setCurrentNodes(currentGen,cond,NULL);

				//Make "scope" for body to hold next loop
				struct IRGenScopeStack scope;
				scope.type=SCOPE_TYPE_LOOP;
				scope.value.loop.next=labNext;
				scope.value.loop.exit=labExit;
				currentGen->scopes=strScopeStackAppendItem(currentGen->scopes, scope);

				//Enter body
				__auto_type body=parserNode2IR(forStmt->body,1);
				connectCurrentsToNode(body);

				//"Exit" body by connecting to exit label
				connectCurrentsToNode(labExit);
				setCurrentNodes(currentGen, labExit);
				//Pop "scope"
				currentGen->scopes=strScopeStackResize(currentGen->scopes, strScopeStackSize(currentGen->scopes)-1);

				// "Next" label
				connectCurrentsToNode(labNext);
				setCurrentNodes(currentGen,labNext,NULL);
				//Inc code
				__auto_type incCode=parserNode2IR(forStmt->inc, 1);

				//Goto cond
				struct IRNodeJump jmp;
				jmp.base.attrs=NULL;
				jmp.base.type=IR_JUMP;
				jmp.forward=0;
				__auto_type jmpNode=GRAPHN_ALLOCATE(jmp);
				connectCurrentsToNode(jmpNode);
				graphNodeIRConnect(jmpNode, labCond, IR_CONN_FLOW);

				// Exit node
				setCurrentNodes(currentGen, labExit,NULL);
				
				return labCond;
		}
		case NODE_FUNC_CALL: {
				struct IRNodeFuncCall call;
				call.base.attrs=NULL;
				call.base.type=IR_FUNC_CALL;
				call.incomingArgs=NULL;

				struct parserNodeFuncCall *call2=(void*)node;
				__auto_type func=parserNode2IR(call2->func,0);

				strGraphNodeIRP args=NULL;
				for(long i=0;i!=strParserNodeSize(call2->args) ;i++) {
						graphNodeIR arg=NULL;
						//If provided
						if(call2->args[i]) {
								arg=parserNode2IR(call2->args[i],0);	
						} else {
								//Use dft
								struct objectFunction *funcType= (void*)assignTypeToOp(call2->func);
								assert(funcType->args[i].dftVal);
								arg=parserNode2IR(funcType->args[i].dftVal,0);
						}
						
						args=strGraphNodeIRPAppendItem(args, arg);
				}

				call.incomingArgs=args;
				graphNodeIR callNode=GRAPHN_ALLOCATE(call);
				for(long i=0;i!=strGraphNodeIRPSize(args);i++) {
						graphNodeIRConnect(args[i], callNode, IR_CONN_FUNC_ARG);
				}

				return callNode;
		}
		case NODE_FUNC_REF: {
				struct parserNodeFuncRef *ref=(void*)node;
						
				struct IRNodeValue val;
				val.base.attrs=NULL;
				val.base.type=IR_VALUE;
				val.val.type=IR_VAL_FUNC;
				val.val.value.func=ref->func;

				return GRAPHN_ALLOCATE(val);
		}
		case NODE_IF: {
				struct parserNodeIf *ifNode=(void*)node;

				struct IRNodeLabel lab;
				lab.base.attrs=NULL;
				lab.base.type=IR_LABEL;
				graphNodeIR fBranch,endBranch;
				endBranch=GRAPHN_ALLOCATE(lab);
				//If no else,endBranch if false branch
				if(ifNode->el==NULL) {
						fBranch=endBranch; 
				} else {
						fBranch=GRAPHN_ALLOCATE(lab);
				}
				
				__auto_type cond=parserNode2IR(ifNode->cond,1);
				__auto_type condJ=IRCondJump(cond,NULL,fBranch);

				connectCurrentsToNode(condJ);
				setCurrentNodes(currentGen, condJ,NULL);
				__auto_type body=parserNode2IR(ifNode->body,1);

				if(ifNode->el) {
						//Jump to end on true path
						struct IRNodeJump jmpToEnd;
						jmpToEnd.base.attrs=NULL;
						jmpToEnd.base.type=IR_JUMP;
						jmpToEnd.forward=1;
						__auto_type jteNode= GRAPHN_ALLOCATE(jmpToEnd);
						//Connect
						connectCurrentsToNode(jteNode);

						//Connect cond to fBranch label
						setCurrentNodes(currentGen, condJ,NULL);
						connectCurrentsToNode(fBranch);

						setCurrentNodes(currentGen, fBranch,NULL);
						__auto_type elseBody=parserNode2IR(ifNode->body,1);
						//Connect else to end
						connectCurrentsToNode(endBranch);
				}
				
				setCurrentNodes(currentGen, endBranch,NULL);
				return condJ;
		}
		case NODE_LIT_INT: {
				struct parserNodeLitInt *intLit=(void*)node;
				struct IRNodeValue il;
				il.base.attrs=NULL;
				il.base.type=IR_VALUE;
				il.val.type=IR_VAL_INT_LIT;
				il.val.value.intLit=intLit->value;

				return GRAPHN_ALLOCATE(il); 
		}
		case NODE_LABEL: {
				struct parserNodeLabel *labNode=(void*)node;
				
				struct IRNodeLabel lab;
				lab.base.attrs=NULL;
				lab.base.type=IR_LABEL;

				__auto_type ptrStr=ptr2Str(labNode);
				mapParserNodeInsert(currentGen->labels, ptrStr, (struct parserNode*)node);
				strCharDestroy(&ptrStr);
				
				return GRAPHN_ALLOCATE(lab);
		}
		case NODE_LIT_STR: {
				struct parserNodeLitStr str;
		}
		}
}
