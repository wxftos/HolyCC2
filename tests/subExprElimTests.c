#include <assert.h>
#include <subExprElim.h>
//Assumes "a" is 3,only "+" is implemented
static int evalIRNode(graphNodeIR node) {
		struct IRNode *ir=graphNodeIRValuePtr(node);
		switch(ir->type) {
		case IR_VALUE: {
				struct IRValue *value=(void*)ir;
				switch(value-> type) {
				case IR_VAL_VAR_REF: {
						assert(value->value.var.var.type==IR_VAR_VAR);
						if(0==strcmp(value->value.var.var.value.var->name,"a"))
								return 3; 
				}
				case IR_VAL_INT_LIT: {
						return value->value.intLit.value.sInt;
				}
						default:
								assert(0);
				} 
		}
		case IR_ADD: {
				__auto_type incoming =graphNodeIRIncomingNodes(node);
				int sum=0;
				for(int i=0;i!=2;i++)
						sum+=evalIRNode(incoming[i]);
				
				return sum;
		}
				default:
						assert(0);
		}
		return 0;
}
void subExprElimTests() {
		//a+1+a+1+a+1
		{
				initIR();
		__auto_type a=createVirtVar(&typeI64i);
		__auto_type one1=createIntLit(1);
		__auto_type one2=createIntLit(1);
		__auto_type one3=createIntLit(1);
		
		__auto_type a1=createVarRef(a);
		__auto_type a2=createVarRef(a);
		__auto_type a3=createVarRef(a);

		__auto_type binop1=createBinop(a1, one1, IR_ADD);
		__auto_type binop2=createBinop(a2, one2, IR_ADD);
		__auto_type binop3=createBinop(a3, one3, IR_ADD);
		
		__auto_type sum1= createBinop(binop1,binop2 ,  IR_ADD);
		__auto_type sum2= createBinop(sum1,binop3 ,  IR_ADD);

		__auto_type start=createStmtStart();
		__auto_type end=createStmtEnd(start);
		graphNodeIRConnect(sum2, end, IR_CONN_FLOW);

		graphNodeIR connectToStart[]={
				one1,one2,one3,a1,a2,a3
		};
		for(long i=0;i!=sizeof(connectToStart)/sizeof(*connectToStart);i++)
				graphNodeIRConnect(start, connectToStart[i], IR_CONN_FLOW);

		findSubExprs(start);
		removeSubExprs();

		__auto_type tail=graphNodeIRIncomingNodes(end);
		assert(strGraphNodeIRPSize(tail)==1);
		assert(4+4+4==evalIRNode(tail[0]));
		}
}
