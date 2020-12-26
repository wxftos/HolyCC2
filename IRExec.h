#include <IR.h>
#include <stdint.h>
enum IREvalValType {
	IREVAL_VAL_INT,
	IREVAL_VAL_PTR,
	IREVAL_VAL_DFT,
	IREVAL_VAL_FLT,
	IREVAL_VAL_VAR,
	IREVAL_VAL_REG,
	IREVAL_VAL_CLASS,
};
MAP_TYPE_DEF(struct IREvalVal,IREvalMembers);
struct IREvalVal {
		enum IREvalValType type;
		union {
			double flt;
			int64_t i;
			struct reg *reg;
			struct IREvalVal *ptr;
				mapIREvalMembers class;
	} value;
		struct IREvalVal *valueStoredAt;
};
MAP_TYPE_FUNCS(struct IREvalVal,IREvalMembers);;
struct IREvalVal IREvalNode(graphNodeIR node, int *success);
void IREvalSetVarVal(const struct variable *var, struct IREvalVal value);
struct IREvalVal IREvalValFltCreate(double f);
struct IREvalVal IREValValIntCreate(long i);
void IREvalInit();
struct IREvalVal IREvalPath(graphNodeIR start, int *success);
void IREvalValDestroy(struct IREvalVal *val);
