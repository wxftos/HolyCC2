#pragma once
#include <graph.h>
#include <hashTable.h>
#include <lexer.h>
#include <linkedList.h>
#include <parserA.h>
#include <registers.h>
#include <str.h>
#include <ptrMap.h>
enum IRFlag {
	IR_FLAG_EQZ,
	IR_FLAG_NEQZ,
	IR_FLAG_GE,
	IR_FLAG_LE,
	IR_FLAG_GT,
	IR_FLAG_LT,
	IR_FLAG_EQ,
	IR_FLAG_NEQ,
	IR_FLAG_B,
	IR_FLAG_AE,
	IR_FLAG_BE,
	IR_FLAG_ABOVE,
	IR_FLAG_BELOW,
};
enum IRConnType {
	IR_CONN_SOURCE_A,
	IR_CONN_SOURCE_B,
	IR_CONN_DEST,
	IR_CONN_FLOW,
	IR_CONN_NEVER_FLOW,
	IR_CONN_COND_TRUE,
	IR_CONN_COND_FALSE,
	IR_CONN_COND,
	IR_CONN_CASE,
	IR_CONN_DFT,
	IR_CONN_FUNC_ARG,
	IR_CONN_SIMD_ARG,
	IR_CONN_FUNC,
};
enum IRNodeType {
	IR_TYPECAST,
	//
	IR_STATEMENT_START,
	IR_STATEMENT_END,
	//
	IR_INC,
	IR_DEC,
	//
	IR_ADD,
	IR_SUB,
	//
	IR_POS,
	IR_NEG,
	//
	IR_MULT,
	IR_MOD,
	IR_DIV,
	IR_POW,
	//
	IR_LAND,
	IR_LXOR,
	IR_LOR,
	IR_LNOT,
	//
	IR_BNOT,
	IR_BAND,
	IR_BXOR,
	IR_BOR,
	IR_LSHIFT,
	IR_RSHIFT,
	//
	IR_ARRAY_ACCESS,
	//
	IR_SIMD,
	//
	IR_GT,
	IR_LT,
	IR_GE,
	IR_LE,
	IR_EQ,
	IR_NE,
	IR_ASSIGN,
	//
	IR_CHOOSE,
	//
	IR_COND_JUMP,
	IR_JUMP_TAB,
	//
	IR_VALUE,
	IR_LABEL,
	//
	IR_FUNC_ARG,
	IR_FUNC_CALL,
	IR_FUNC_RETURN,
	IR_FUNC_START,
	IR_FUNC_END,
	//
	IR_SUB_SWITCH_START_LABEL,
	//
	IR_ADDR_OF,
	IR_DERREF,
	//
	IR_SPILL,
	IR_LOAD,
};
struct IRNode;
struct IRAttr {
	void *name;
};
LL_TYPE_DEF(struct IRAttr, IRAttr);
LL_TYPE_FUNCS(struct IRAttr, IRAttr);
int IRAttrInsertPred(const struct IRAttr *a, const struct IRAttr *b);
int IRAttrGetPred(const void *key, const struct IRAttr *b);
struct IRNode {
	enum IRNodeType type;
	llIRAttr attrs; // NULL by defualt
};
GRAPH_TYPE_DEF(struct IRNode, enum IRConnType, IR);
GRAPH_TYPE_FUNCS(struct IRNode, enum IRConnType, IR);
enum IRValueType {
	__IR_VAL_MEM_FRAME,
	__IR_VAL_MEM_GLOBAL,
	__IR_VAL_LABEL,
	IR_VAL_REG,
	IR_VAL_VAR_REF,
	IR_VAL_FUNC,
	IR_VAL_INT_LIT,
	IR_VAL_STR_LIT,
};
struct IRValue;
struct IRValOpResult {
	graphNodeIR node;
};
struct IRValIndirect {
	graphNodeIR base;
	graphNodeIR index;
	long scale;
};
struct IRValMemFrame {
	long offset;
	struct object *type;
};
struct IRValMemGlobal {
	struct variable *symbol;
};
enum IRVarType {
	IR_VAR_VAR,
	IR_VAR_MEMBER,
};
struct IRVar {
	enum IRVarType type;
	union {
		struct variable *var;
			struct {
					graphNodeIR base;
					struct objectMember *mem;
			} member;
	} value;
	long SSANum;
};
struct IRValue {
	enum IRValueType type;
	union {
		struct regSlice reg;
		struct IRVar var;
		struct IRValMemFrame __frame;
		struct IRValMemGlobal __global;
		graphNodeIR __label;
		struct IRValIndirect indir;
		struct IRValOpResult opRes;
		struct function *func;
		struct lexerInt intLit;
		const char *strLit;
		graphNodeIR memLabel;
	} value;
};
struct IRNodeAssign {
	struct IRNode base;
};
struct IRNodeBinop {
	struct IRNode base;
};
struct IRNodeUnop {
	struct IRNode base;
};
// Out-going connection is to where
struct IRNodeJump {
	struct IRNode base;
	int forward;
};
// Out-going connection is to where
struct IRNodeCondJump {
	struct IRNode base;
	enum IRFlag cond;
};
struct IRNodeSpill {
	struct IRNode base;
	struct IRValue item;
};
struct IRNodeLoad {
	struct IRNode base;
	struct IRValue item;
};
struct IRNodeSIMD {
	struct IRNode base;
	enum IRNodeType opType;
};
struct IRNodeValue {
	struct IRNode base;
	struct IRValue val;
};
struct IRNodeInc {
	struct IRNode base;
	long isSuffix;
};
struct IRNodeDec {
	struct IRNode base;
	long isSuffix;
};
//"a"
//"b"
struct IRNodeArrayAccess {
	struct IRNode base;
	long scale;
};
struct IRNodeLabel {
	struct IRNode base;
};
struct IRNodeFuncCall {
	struct IRNode base;
	strGraphNodeIRP incomingArgs;
};
struct IRJumpTableRange {
		graphNodeIR to;
		long start,end;
};
STR_TYPE_DEF(struct IRJumpTableRange,IRTableRange);
STR_TYPE_FUNCS(struct IRJumpTableRange,IRTableRange);
struct IRNodeJumpTable {
		struct IRNode base;
		long startIndex;
		long count;
		strIRTableRange labels;
};
struct IRNodeSubSwit {
	struct IRNode base;
		graphNodeIR startCode;
		
};
struct IRNodeTypeCast {
	struct IRNode base;
	struct object *in;
	struct object *out;
};
struct IRNodeStatementStart {
	struct IRNode base;
	graphNodeIR end;
};
struct IRNodeStatementEnd {
	struct IRNode base;
	graphNodeIR start;
};
struct IRNodeFuncStart {
		struct IRNode base;
		graphNodeIR end;
		struct function *func;
};
struct IRNodeFuncArg {
		struct IRNode base;
		struct object *type;
		long argIndex;
};
struct IRNodeFuncEnd {
	struct IRNode base;
};
struct IRNodeFuncReturn {
	struct IRNode base;
	graphNodeIR exp;
};
struct IRNodeChoose {
	struct IRNode base;
	strGraphNodeIRP canidates;
};

char *IR2Str();
graphNodeIR IRCreateIntLit(int64_t lit);
graphNodeIR IRCreateBinop(graphNodeIR a, graphNodeIR b, enum IRNodeType type);
graphNodeIR IRCreateLabel();
graphNodeIR IRCreateVarRef(struct variable *var);
graphNodeIR IRCreateValueFromLabel(graphNodeIR lab);
struct variable *IRCreateVirtVar(struct object *type);
struct IRVarRefs {
	struct IRVar var;
	long refs;
};
MAP_TYPE_DEF(struct IRVarRefs, IRVarRefs);
MAP_TYPE_FUNCS(struct IRVarRefs, IRVarRefs);
extern mapIRVarRefs IRVars;
strGraphNodeP IRStatementNodes(graphNodeIR stmtStart, graphNodeIR stmtEnd);
graphNodeIR IRCreateStmtEnd(graphNodeIR start);
graphNodeIR IRCreateStmtStart();
void initIR();
strGraphEdgeIRP IRGetConnsOfType(strGraphEdgeIRP conns, enum IRConnType type);
void IRInsertBefore(graphNodeIR insertBefore, graphNodeIR entry,
                    graphNodeIR exit, enum IRConnType connType);
void IRInsertAfter(graphNodeIR insertAfter, graphNodeIR entry, graphNodeIR exit,
                   enum IRConnType connType);
graphNodeIR IRCreateAssign(graphNodeIR in, graphNodeIR dst);
graphNodeIR IRCreateCondJmp(graphNodeIR cond, graphNodeIR t, graphNodeIR f);
void IRStmtBlockFromTailNode(graphNodeIR tail, graphNodeIR *enter,
                             graphNodeIR *exit);
graphNodeIR IRStmtStart(graphNodeIR node);
int IRVarCmp(const struct IRVar *a, const struct IRVar *b);
char *graphEdgeIR2Str(struct __graphEdge *edge);
graphNodeIR IRCreateReturn(graphNodeIR exp, graphNodeIR func);
void IRGraphMap2GraphViz(
    graphNodeMapping graph, const char *title, const char *fn,
    char *(*nodeLabelOverride)(graphNodeIR node, mapGraphVizAttr *attrs,
                               const void *data),
    char *(*edgeLabelOverride)(graphEdgeIR node, mapGraphVizAttr *attrs,
                               const void *data),
    const void *dataNodes, const void *dataEdge);
graphNodeIR IRCreateStrLit(const char *text);
graphNodeIR IRCreateUnop(graphNodeIR a, enum IRNodeType type);
graphNodeIR IRCreateFuncCall(graphNodeIR func, ...);
graphNodeIR IRCreateTypecast(graphNodeIR in, struct object *inType,
                           struct object *outType);
enum IRCloneMode {
	IR_CLONE_NODE,
	IR_CLONE_EXPR,
	IR_CLONE_EXPR_UNTIL_ASSIGN,
	IR_CLONE_UP_TO,
};
PTR_MAP_FUNCS(struct __graphNode *, struct __graphNode *, GraphNode);
graphNodeIR IRCloneNode(graphNodeIR node, enum IRCloneMode mode,
                      ptrMapGraphNode *mappings);
int IRVarCmpIgnoreVersion(const struct IRVar *a, const struct IRVar *b);
int IRIsExprEdge(enum IRConnType type);
struct object *IRValueGetType(struct IRValue *node);
graphNodeIR IRCreateLoad(struct IRVar *var);
graphNodeIR IRCreateSpill(struct IRVar *var);
graphNodeIR IRCreateRegRef(const struct regSlice *slice);
graphNodeIR IREndOfExpr(graphNodeIR node);
strGraphNodeIRP IRStmtNodes(graphNodeIR end);
void IRRemoveDeadExpression(graphNodeIR end, strGraphNodeP *removed);
int IRIsDeadExpression(graphNodeIR end);
struct object *IRNodeType(graphNodeIR node);
void IRNodeDestroy(struct IRNode *node);
graphNodeIR IRCloneUpTo(graphNodeIR node, strGraphNodeIRP to,
                        ptrMapGraphNode *mappings);
int IRIsOperator(graphNodeIR node);
graphNodeIR IRCreateFuncArg(struct object *type,long funcIndex);
graphNodeIR IRCreateMemberAccess(graphNodeIR input,const char *name);
void IRRemoveNeedlessLabels(graphNodeIR start) ;
void IRInsertNodesBetweenExprs(graphNodeIR expr);
void IRPrintMappedGraph(graphNodeMapping map);
