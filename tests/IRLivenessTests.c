#include <IRLiveness.h>
#include <assert.h>
#include <debugPrint.h>
void LivenessTests() {
		initIR();
		//https://lambda.uta.edu/cse5317/spring01/notes/node37.html
		__auto_type u=createVirtVar(&typeI64i);
		__auto_type v=createVirtVar(&typeI64i);
		__auto_type w=createVirtVar(&typeI64i);
		__auto_type x=createVirtVar(&typeI64i);
		__auto_type y=createVirtVar(&typeI64i);
		__auto_type z=createVirtVar(&typeI64i);

		graphNodeIR exit=NULL;
		__auto_type entry=createLabel();
		//v=1
		{
				__auto_type one1=createIntLit(1);
				__auto_type vRef1=createVarRef(v);
				graphNodeIRConnect(entry, one1, IR_CONN_FLOW);
				graphNodeIRConnect(one1, vRef1, IR_CONN_DEST);
				
				debugAddPtrName(one1, "1.1");
				debugAddPtrName(vRef1, "v.1");
				
				exit=vRef1;
		}
		
		//z=v+1
		{
				__auto_type one2=createIntLit(1);
				__auto_type vRef2=createVarRef(v);
				__auto_type zRef1=createVarRef(z);
				graphNodeIRConnect(createBinop(one2, vRef2, IR_ADD), zRef1, IR_CONN_DEST);
				graphNodeIRConnect(exit, one2, IR_CONN_FLOW);
				graphNodeIRConnect(exit, vRef2, IR_CONN_FLOW);
				exit=zRef1;
				
				debugAddPtrName(one2, "1.2");
				debugAddPtrName(zRef1, "z.2");
				debugAddPtrName(vRef2, "v.2");
		}
		
		//y=x+2
		{
				__auto_type xRef1=createVarRef(x);
				__auto_type two1=createIntLit(2);
				__auto_type yRef1=createVarRef(y);
				graphNodeIRConnect(exit, two1, IR_CONN_FLOW);
				graphNodeIRConnect(exit, xRef1, IR_CONN_FLOW);
				graphNodeIRConnect(createBinop(two1, xRef1, IR_ADD), yRef1, IR_CONN_DEST);
				exit=yRef1;

				debugAddPtrName(xRef1, "x.3");
				debugAddPtrName(two1, "2.3");
		}

		//w=x+y*z
		{
				__auto_type xRef2=createVarRef(x);
				__auto_type yRef2=createVarRef(y);
				__auto_type zRef2=createVarRef(z);
				__auto_type wRef1=createVarRef(w);
				graphNodeIRConnect(exit, xRef2, IR_CONN_FLOW);
				graphNodeIRConnect(exit, yRef2, IR_CONN_FLOW);
				graphNodeIRConnect(exit, zRef2, IR_CONN_FLOW);
				graphNodeIRConnect(createBinop(xRef2,createBinop(yRef2, zRef2, IR_MULT),IR_ADD), wRef1, IR_CONN_DEST);
				exit=wRef1;
				debugAddPtrName(xRef2, "x.4");
				debugAddPtrName(yRef2, "y.4");
				debugAddPtrName(zRef2, "z.4");
				debugAddPtrName(wRef1, "w.4");
		}
		
		//u=z+2
		{
				__auto_type two2=createIntLit(2);
				__auto_type zRef3=createVarRef(z);
				__auto_type uRef1=createVarRef(u);
				graphNodeIRConnect(exit, two2, IR_CONN_FLOW);
				graphNodeIRConnect(exit, zRef3, IR_CONN_FLOW);
				graphNodeIRConnect(createBinop(two2, zRef3, IR_ADD), uRef1, IR_CONN_DEST);
				exit=uRef1;
				
				debugAddPtrName(two2, "2.5");
				debugAddPtrName(zRef3, "z.5");
				debugAddPtrName(uRef1, "u.5");
		}

		//v=u+w+y
		{
				__auto_type uRef=createVarRef(u);
				__auto_type wRef=createVarRef(w);
				__auto_type yRef=createVarRef(y);
				__auto_type vRef=createVarRef(v);
				graphNodeIRConnect(exit, uRef, IR_CONN_FLOW);
				graphNodeIRConnect(exit, wRef, IR_CONN_FLOW);
				graphNodeIRConnect(exit, yRef, IR_CONN_FLOW);
				graphNodeIRConnect(createBinop(uRef,createBinop(yRef, wRef, IR_ADD), IR_ADD),vRef, IR_CONN_DEST);
				exit=vRef;
				
				debugAddPtrName(uRef, "u.6");
				debugAddPtrName(wRef, "w.6");
				debugAddPtrName(yRef, "y.6");
				debugAddPtrName(vRef, "v.6");
		}

		//u*v
		{
				__auto_type uRef=createVarRef(u);
				__auto_type vRef=createVarRef(v);
				createBinop(uRef, vRef, IR_MULT);
				graphNodeIRConnect(exit, uRef, IR_CONN_FLOW);
				graphNodeIRConnect(exit, vRef, IR_CONN_FLOW);
				debugAddPtrName(uRef, "u.7");
				debugAddPtrName(vRef, "v.7");
		}

		if(1)
				graphPrint(entry, (char*(*)(struct __graphNode*))debugGetPtrName);
}
