#include <assert.h>
#include <cleanup.h>
#include <hashTable.h>
#include <object.h>
#include <parserA.h>
#include <registers.h>
#include <stdio.h>
#include <string.h>
#include <diagMsg.h>
MAP_TYPE_DEF(struct object *, Object);
MAP_TYPE_FUNCS(struct object *, Object);
STR_TYPE_DEF(char, Char);
STR_TYPE_FUNCS(char, Char);
static mapObject objectRegistry = NULL;
/**
	* This is uses to treat arrays as pointers when addressing array size outside of a class/union
	* We use this treat arrays as pointers outside of a class/union as arrays are actually pointers in functions.
	*/
static __thread int dontTreatArraysAsPtrs=0;
struct object *objectBaseType(const struct object *obj) {
	if (obj->type == TYPE_CLASS) {
		struct objectClass *cls = (void *)obj;
		if (!cls->baseType)
			return (struct object *)obj;

		return objectBaseType(cls->baseType);
	} else if (obj->type == TYPE_UNION) {
		struct objectUnion *un = (void *)obj;
		if (!un->baseType)
			return (struct object *)obj;

		return objectBaseType(un->baseType);
	}

	return (struct object *)obj;
}
/* This function clones a string. */
static char *strClone(const char *str) {
	__auto_type len = strlen(str);
	char *retVal = calloc(len + 1,1);
	strcpy(retVal, str);
	return retVal;
}
static char *ptr2Str(const void *a) {
	long len =snprintf(NULL, 0, "%p", a);
	char buffer[len+1];
	sprintf(buffer, "%p", a);
	return strClone(buffer);
}
static int objectIsReplacable(struct object *obj) {
		switch(obj->type) {
		case TYPE_ARRAY: {
				struct objectArray *ptr=(void*)obj;
				return objectIsReplacable(ptr->type);
		}
		case TYPE_FORWARD:
				return 1;
		case TYPE_PTR: {
				struct objectPtr *ptr=(void*)obj;
				return objectIsReplacable(ptr->type);
		}
default:
		return 0;
		}
}
/**
 * This function hashes an object,*it also assigns the hash to the object if it
 * doesn't exit.*
 */
static const char * /* Dont free me*/
hashObject(struct object *obj, int *alreadyExists) {
	if (obj->name) {
		if (alreadyExists)
			*alreadyExists = 1;
		return obj->name;
	}

	char *retVal = NULL;
	switch (obj->type) {
	case TYPE_U8i:
		retVal = strClone("U8i");
		goto end;
	case TYPE_U16i:
		retVal = strClone("U16i");
		goto end;
	case TYPE_U32i:
		retVal = strClone("U32i");
		goto end;
	case TYPE_U64i:
		retVal = strClone("U64i");
		goto end;
	case TYPE_I8i:
		retVal = strClone("I8i");
		goto end;
	case TYPE_I16i:
		retVal = strClone("I16i");
		goto end;
	case TYPE_I32i:
		retVal = strClone("I32i");
		goto end;
	case TYPE_I64i:
		retVal = strClone("I64i");
		goto end;
	case TYPE_ARRAY: {
		struct objectArray *arr = (void *)obj;
		const char *baseH = hashObject(arr->type, NULL);
		// If integer dim
		if (arr->dim) {
				if(arr->dim->type == NODE_LIT_INT) {
			struct parserNodeLitInt *lint = (void *)arr->dim;
			long len = snprintf(NULL, 0, "%s[%lli]", baseH, (long long)lint->value.value.sLong);
			char buffer[len + 1];
			sprintf(buffer, "%s[%lli]", baseH, (long long)lint->value.value.sLong);

			retVal = strClone(buffer);
			goto end;
				}
		}
		// Isn't an int-dim
		__auto_type dimStr = ptr2Str(arr->dim);
		long len = snprintf(NULL, 0, "%s[%p(%p)]", baseH, arr->dim,arr->dimIR);
		char buffer[len + 1];
		sprintf(buffer, "%s[%p(%p)]", baseH, arr->dim,arr->dimIR);
		
		retVal = strClone(buffer);
		goto end;
	}
	case TYPE_Bool: {
			retVal = strClone("Bool");
		goto end;
	}
	case TYPE_FORWARD: {
		struct objectForwardDeclaration *fwd = (void *)obj;
		struct parserNodeName *name = (void *)fwd->name;
		long len = snprintf(NULL, 0, "%s", name->text);
		char buffer[len + 1];
		sprintf(buffer, "%s", name->text);

		retVal = strClone(buffer);
		goto end;
	}
	case TYPE_CLASS: {
			struct objectClass *cls=(void*)obj;
			if(!cls->name)
					retVal = ptr2Str(obj);
			else
					retVal=object2Str(obj);
			goto end;
	}
	case TYPE_F64:
		retVal = strClone("F64");
		goto end;
	case TYPE_FUNCTION: {
		struct objectFunction *func = (void *)obj;
		long len=snprintf(NULL, 0, "%p", func);
		char buffer[len+1];
		sprintf(buffer, "%p", func);
		retVal = strClone(buffer);
		goto end;
	}
	case TYPE_PTR: {
		struct objectPtr *ptr = (void *)obj;
		const char *base = hashObject(ptr->type, NULL);

		long len = snprintf(NULL, 0, "%s*", base);
		char buffer[len + 1];
		sprintf(buffer, "%s*", base);

		retVal = strClone(buffer);
		goto end;
	}
	case TYPE_U0:
		retVal = strClone("U0");
		goto end;
	case TYPE_UNION:{
			struct objectUnion *cls=(void*)obj;
			if(!cls->name)
					retVal = ptr2Str(obj);
			else
					retVal=object2Str(obj);
			goto end;
	}
	}
end:
	if (alreadyExists)
		*alreadyExists = 0;
	if (NULL == mapObjectGet(objectRegistry, retVal)) {
		mapObjectInsert(objectRegistry, retVal, obj);
	} else  {
			__auto_type find=*mapObjectGet(objectRegistry, retVal);
			//Check if a forward declaration.
			if(!objectIsReplacable(find)) {
					if (alreadyExists)
							*alreadyExists = 1;
			} else {
					mapObjectRemove(objectRegistry, retVal,NULL);
					goto end;
			}
	}

	obj->name = retVal;
	return obj->name;
}
/**
 * This conmputes the align of a type.
 */
long /*Align of the object,think padding. */
objectAlign(const struct object *type, int *success) {
	if (success != NULL)
		*success = 1;

	switch (type->type) {
	case TYPE_ARRAY: {
			struct objectArray *arr=(void*)type; 
			return objectAlign(arr->type, success);
	}
case TYPE_FUNCTION:
		return ptrSize();
	case TYPE_FORWARD:
	case TYPE_U0: {
		return 0;
	}
	case TYPE_PTR: {
		return ptrSize();
	}
	case TYPE_UNION: {
		__auto_type ptr = (struct objectUnion *)type;
		return ptr->size;
	}
	case TYPE_CLASS: {
		__auto_type ptr = (struct objectClass *)type;
		return ptr->size;
	}
	case TYPE_Bool:
	case TYPE_I8i:
	case TYPE_U8i: {
		return 1;
	}
	case TYPE_I16i:
	case TYPE_U16i: {
		return 2;
	}
	case TYPE_I32i:
	case TYPE_U32i: {
		return 4;
	}
	case TYPE_F64:
	case TYPE_I64i:
	case TYPE_U64i: {
		return 8;
	}
	}
}
/**
 * This gets the size of an object.
 */
long /*Size of the object.*/
objectSize(const struct object *type, int *success) {
	if (success != NULL)
		*success = 1;

	switch (type->type) {
	case TYPE_ARRAY: {
			if (success != NULL)
			*success = 1;

			if(!dontTreatArraysAsPtrs)
					return ptrSize();
			
			
			//
			// If cant detirmine size and is a variable length array,just return pointer size;
			//
			struct objectArray *arr=(void*)type;
			long currentSize=1;
			for(;arr->base.type==TYPE_ARRAY;arr=(struct objectArray*)arr->type) {
					if(!arr->dim) {
					arrayAmbig:
							if(success)
									*success=0;
							return -1;
					}
					if(arr->dim->type!=NODE_LIT_INT)
							goto arrayAmbig;
					struct parserNodeLitInt *i=(void*)arr->dim;
					currentSize*=(i->value.type==INT_SLONG)?i->value.value.sLong:i->value.value.uLong;
			}
			struct object *base=(void*)arr;
			return objectSize(base, success)*currentSize;
	}
	case TYPE_U0: {
		return 0;
	}
	case TYPE_PTR: {
		return ptrSize();
	}
	case TYPE_UNION: {
		__auto_type ptr = (struct objectUnion *)type;
		return ptr->size;
	}
	case TYPE_CLASS: {
		__auto_type ptr = (struct objectClass *)type;
		return ptr->size;
	}
	case TYPE_Bool:
	case TYPE_I8i:
	case TYPE_U8i: {
		return 1;
	}
	case TYPE_I16i:
	case TYPE_U16i: {
		return 2;
	}
	case TYPE_I32i:
	case TYPE_U32i: {
		return 4;
	}
	case TYPE_F64:
	case TYPE_I64i:
	case TYPE_U64i: {
		return 8;
	}
	case TYPE_FUNCTION: {
			return ptrSize();
	}
	}
	assert(0);
	return 0;
}
/**
 * Makes a class,See `struct objectMember`. This also registers said class.
 */
struct object * /*This created class.*/
objectClassCreate(const struct parserNode *name, const struct objectMember *members, long count) {
		dontTreatArraysAsPtrs++;

		struct objectClass *newClass = calloc(sizeof(struct objectClass),1);
	newClass->name = (struct parserNode *)name;
	newClass->base.type = TYPE_CLASS;
	newClass->base.name = NULL;
	newClass->methods = NULL;
	newClass->members = NULL;
	newClass->baseType = NULL;
	
	long largestMemberAlign = 0;
	int success;
	for (long i = 0; i != count; i++) {
		__auto_type align = objectAlign(members[i].type, &success);
		if (!success)
			goto fail;

		if (align > largestMemberAlign)
			largestMemberAlign = align;
	}

	newClass->align = largestMemberAlign;

	newClass->members = NULL;
	long offset = 0;
	for (long i = 0; i != count; i++) {
		offset += offset % objectAlign(members[i].type, &success);
		if (!success)
			goto fail;

		newClass->members = strObjectMemberAppendItem(newClass->members, members[i]);
		newClass->members[strObjectMemberSize(newClass->members) - 1].offset = offset;
		offset += objectSize(members[i].type, &success);
		if (!success)
			goto fail;
	}
	newClass->size = offset + offset % largestMemberAlign;

	int alreadyExists;
	hashObject((void *)newClass, &alreadyExists);
	if(alreadyExists) {
			diagErrorStart(name->pos.start, name->pos.end);
			diagPushText("Object ");
			diagPushQoutedText(name->pos.start, name->pos.end);
			diagPushText(" already exists.");
			diagEndMsg();
	}
	
	dontTreatArraysAsPtrs--;
	return (struct object *)newClass;
fail:
	dontTreatArraysAsPtrs--;
	return NULL;
}
/**
 * This creates a union and registers it too.
 */
struct object * /*The union being returned. */
objectUnionCreate(const struct parserNode *name /*Can be `NULL` for empty union.*/, const struct objectMember *members, long count) {
	int success;
	dontTreatArraysAsPtrs++;
	
	struct objectUnion *newUnion = calloc(sizeof(struct objectUnion),1);
	newUnion->name = (struct parserNode *)name;
	newUnion->base.type = TYPE_UNION;
	newUnion->baseType=NULL;
	newUnion->members = NULL;

	long largestMemberAlign = 0;
	long largestSize = 0;
	for (long i = 0; i != count; i++) {
		struct objectMember clone = members[i];
		clone.offset = 0;

		__auto_type align = objectAlign(members[i].type, &success);
		if (!success)
			goto fail;
		__auto_type size = objectSize(members[i].type, &success);
		if (!success)
			goto fail;

		if (align > largestMemberAlign)
			largestMemberAlign = align;
		if (size > largestSize)
			largestSize = size;

		clone.name = strClone(clone.name);
		newUnion->members = strObjectMemberAppendItem(newUnion->members, clone);
	}
	largestSize += largestSize % largestMemberAlign;
	newUnion->size = largestSize;
	newUnion->align = largestMemberAlign;

	int alreadyExists;
	hashObject((void *)newUnion, &alreadyExists);
	if(alreadyExists) {
			diagErrorStart(name->pos.start, name->pos.end);
			diagPushText("Object ");
			diagPushQoutedText(name->pos.start, name->pos.end);
			diagPushText(" already exists.");
			diagEndMsg();
	}

	dontTreatArraysAsPtrs--;
	return (struct object *)newUnion;
fail:
	dontTreatArraysAsPtrs--;
	return NULL;
}
/**
 * Creates a pointer type.
 */
struct object * /* The newly created type.*/
objectPtrCreate(struct object *baseType) {
	// Check if item is in registry prior to making a new one

		struct objectPtr *ptr = calloc(sizeof(struct objectPtr),1);
	ptr->base.type = TYPE_PTR;
	ptr->type = baseType;
	ptr->base.name = NULL;

	int alreadyExists;
	__auto_type hash = hashObject((void *)ptr, &alreadyExists);

	return *mapObjectGet(objectRegistry, hash);
}
/**
 * This creates an array type. parserA.h defines `struct parserNode`.
 */
struct object * /*Array type.*/
objectArrayCreate(struct object *baseType, struct parserNode *dim,void *dimIR) {
		struct objectArray *array = calloc(sizeof(struct objectArray),1);
	array->base.type = TYPE_ARRAY;
	array->base.name = NULL;
	array->dim = dim;
	array->dimIR=dimIR;
	array->type = baseType;

	int alreadyExists;
	__auto_type key = hashObject((void *)array, &alreadyExists);

	return *mapObjectGet(objectRegistry, key);
}
/**
 * This function takes `TYPE_CLASS`/`TYPE_UNION` for forward declarations.
 */
struct object * /*This returns a forward declaration.*/
objectForwardDeclarationCreate(const struct parserNode *name, enum holyCTypeKind type /* See `TYPE_CLASS`/`TYPE_UNION`.*/) {
		struct objectForwardDeclaration *retVal = calloc(sizeof(struct objectForwardDeclaration),1);
	retVal->base.type = TYPE_FORWARD;
	retVal->base.name = NULL;
	retVal->name = (struct parserNode *)name;
	retVal->type = type;

	int alreadyExists;
	__auto_type hash = hashObject((void *)retVal, &alreadyExists);

	return *mapObjectGet(objectRegistry, hash);
}
struct object typeBool = {TYPE_Bool};
struct object typeU0 = {TYPE_U0};
struct object typeU8i = {TYPE_U8i};
struct object typeU16i = {TYPE_U16i};
struct object typeU32i = {TYPE_U32i};
struct object typeU64i = {TYPE_U64i};
struct object typeI8i = {TYPE_I8i};
struct object typeI16i = {TYPE_I16i};
struct object typeI32i = {TYPE_I32i};
struct object typeI64i = {TYPE_I64i};
struct object typeF64 = {TYPE_F64};
void initObjectRegistry();
void initObjectRegistry() {
	objectRegistry = mapObjectCreate();

	// hashObject assigns name to type
	typeBool.type = TYPE_Bool;
	typeBool.name = NULL;
	hashObject(&typeBool, NULL);

	typeU0.type = TYPE_U0;
	typeU0.name = NULL;
	hashObject(&typeU0, NULL);

	typeU8i.type = TYPE_U8i;
	typeU8i.name = NULL;
	hashObject(&typeU8i, NULL);

	typeU16i.type = TYPE_U16i;
	typeU16i.name = NULL;
	hashObject(&typeU16i, NULL);

	typeU32i.type = TYPE_U32i;
	typeU32i.name = NULL;
	hashObject(&typeU32i, NULL);

	typeU64i.type = TYPE_U64i;
	typeU64i.name = NULL;
	hashObject(&typeU64i, NULL);

	typeI8i.type = TYPE_I8i;
	typeI8i.name = NULL;
	hashObject(&typeI8i, NULL);

	typeI16i.type = TYPE_I16i;
	typeI16i.name = NULL;
	hashObject(&typeI16i, NULL);

	typeI32i.type = TYPE_I32i;
	typeI32i.name = NULL;
	hashObject(&typeI32i, NULL);

	typeI64i.type = TYPE_I64i;
	typeI64i.name = NULL;
	hashObject(&typeI64i, NULL);

	typeF64.type = TYPE_F64;
	typeF64.name = NULL;
	hashObject(&typeF64, NULL);
}
/**
 * Takes a name and gets an object by said name.
 */
struct object * /*This object,`NULL` if not-registerd. Creating objects will
                   register them.*/
objectByName(const char *name) {
	__auto_type find = mapObjectGet(objectRegistry, name);
	if (find == NULL)
		return NULL;

	return *find;
}
/**
 * This creates a function type.
 */
struct object * /* The created function type.*/
objectFuncCreate(struct object *retType, strFuncArg args,int varLenArgs) {
	struct objectFunction func;
	func.base.name = NULL;
	func.base.type = TYPE_FUNCTION;
	func.args = strFuncArgAppendData(NULL, args, strFuncArgSize(args));
	func.retType = retType;
	func.hasVarLenArgs=varLenArgs;

	void *retVal = calloc(sizeof(struct objectFunction),1);
	memcpy(retVal, &func, sizeof(struct objectFunction));

	int alreadyExists;
	__auto_type key = hashObject((void *)retVal, &alreadyExists);

	return *mapObjectGet(objectRegistry, key);
}
/**
 * This turns a object into a readable string, Dont use this for hashing, see
 * `hashObject`. This produces readable representations of objects that exclude
 * defualt arguemnt types for readabilty.
 */
char *object2Str(struct object *obj) {
		switch(obj->type) {
		case TYPE_ARRAY: {
				struct objectPtr *ptr=(void*)obj;
				char *base=object2Str(ptr->type);
				long len=snprintf(NULL, 0, "%s[]", base);
				char buffer[len+1];
				sprintf(buffer, "%s[]", base);
				free(base);
				return strClone(buffer);
		}
		case TYPE_Bool:
				return strClone("Bool");
		case TYPE_CLASS: {
				struct objectClass *cls=(void*)obj;
				if(cls->name) {
						struct parserNodeName *nm=(void*)cls->name;
						long len=snprintf(NULL, 0, "%s", nm->text);
						char buffer[len+1];
						sprintf(buffer, "%s", nm->text);
						return strClone(buffer);
				} else {
						return ptr2Str(obj);
				}
		}
		case TYPE_F64:
				return strClone("F64");
		case TYPE_FORWARD: {
				return strClone("FWD");
		}
		case TYPE_FUNCTION: {
				struct objectFunction *func=(void*)obj;
				char *retType=object2Str(func->retType);
				long argc=strFuncArgSize(func-> args);
				strChar argsStr CLEANUP(strCharDestroy)=NULL;
				for(long a=0;a!=argc;a++) {
						if(a)
								argsStr=strCharAppendItem(argsStr, ',');
						
						char *typeStr=object2Str(func->args[a].type);
						argsStr=strCharAppendData(argsStr, typeStr, strlen(typeStr));
						free(typeStr);
				}
				argsStr=strCharAppendItem(argsStr, '\0');
				
				long len=snprintf(NULL, 0, "%s(%s)",retType,argsStr);
				char buffer[len+1];
				sprintf(buffer,"%s(%s)",retType,argsStr);
				free(retType);
				return strClone(buffer);
		}
		case TYPE_I8i:
				return strClone("I8i");
		case TYPE_I16i:
				return strClone("I16i");
		case TYPE_I32i:
				return strClone("I32i");
		case TYPE_I64i:
				return strClone("I64i");
		case TYPE_U8i:
				return strClone("U8i");
		case TYPE_U16i:
				return strClone("U16i");
		case TYPE_U32i:
				return strClone("U32i");
		case TYPE_U64i:
				return strClone("U64i");
		case TYPE_UNION: {
				struct objectUnion *cls=(void*)obj;
				if(cls->name) {
						struct parserNodeName *nm=(void*)cls->name;
						long len=snprintf(NULL, 0, "%s", nm->text);
						char buffer[len+1];
						sprintf(buffer, "%s", nm->text);
						return strClone(buffer);
				} else {
						return ptr2Str(obj);
				}
		}
		case TYPE_U0:
				return strClone("U0");
		case TYPE_PTR: {
				struct objectPtr *ptr=(void*)obj;
				char *base=object2Str(ptr->type);
				long len=snprintf(NULL, 0, "%s*", base);
				char buffer[len+1];
				sprintf(buffer, "%s*", base);
				free(base);
				return strClone(buffer);
		}
		}
		return NULL;
}
/**
 * This compares if objects are equal.
 */
int /*Returns 0 if not equal.*/ objectEqual(const struct object *a, const struct object *b) {
	if (a == b)
		return 1;

	if (a->type != b->type)
		return 0;
	if (a->type == TYPE_PTR) {
		struct objectPtr *aBase = (void *)a, *bBase = (void *)b;
		return objectEqual(aBase->type, bBase->type);
	} else if (a->type == TYPE_CLASS) {
		return a == b;
	} else if (a->type == TYPE_UNION) {
		return a == b;
	} else if (a->type == TYPE_UNION) {
		return a == b;
	} else if (a->type == TYPE_FUNCTION) {
		struct objectFunction *aFunc = (void *)a, *bFunc = (void *)b;
		if (strFuncArgSize(aFunc->args) != strFuncArgSize(bFunc->args))
			return 0;

		for (long i = 0; i != strFuncArgSize(aFunc->args); i++) {
			if (!objectEqual(aFunc->args[i].type, bFunc->args[i].type))
				return 0;

			return 1;
		}
	} else if (a->type == TYPE_ARRAY) {
		struct objectArray *aArr = (void *)a, *bArr = (void *)b;

		// Only checks constant dim
		if (aArr->dim->type == NODE_LIT_INT) {
			if (bArr->dim->type == NODE_LIT_INT) {
				struct parserNodeLitInt *aInt = (void *)aArr->dim;
				struct parserNodeLitInt *bInt = (void *)bArr->dim;
				if (aInt->value.value.sLong == bInt->value.value.sLong)
					return 1;
			}
		}

		return 0;
	}

	return 1;
}
/**
 * If the said type can be used with arithmetic expressions,it returns non-0.
 * Pointer arithmetic counts as arithmetic
 */
static int /*Non-0 if arithmetic type*/
isArith(const struct object *type) {
	if (type == &typeU8i || type == &typeU16i || type == &typeU32i || type == &typeU64i || type == &typeI8i || type == &typeI16i || type == &typeI32i ||
	    type == &typeI64i || type == &typeF64 || type->type == TYPE_PTR || type->type == TYPE_ARRAY) {
		return 1;
	}
	return 0;
}
/**
 * This compares if objects are compatable with each other.
 */
int /*Non-0 if types are compatible. */ objectIsCompat(const struct object *a, const struct object *b) {
	if (objectEqual(a, b))
		return 1;
	return isArith(a) && isArith(b);
}
struct objectMember *objectMemberGet(struct object *aType,struct parserNodeName *nm) {
				struct objectMember *member = NULL;
			if (aType->type == TYPE_CLASS) {
				struct objectClass *cls = (void *)aType;
				for (long m = 0; m != strObjectMemberSize(cls->members); m++) {
					if (0 == strcmp(cls->members[m].name, nm->text)) {
						member = &cls->members[m];
						break;
					}
				}
			} else if (aType->type == TYPE_UNION) {
				struct objectUnion *un = (void *)aType;
				for (long m = 0; m != strObjectMemberSize(un->members); m++) {
					if (0 == strcmp(un->members[m].name, nm->text)) {
						member = &un->members[m];
						break;
					}
				}
			}
			return member;
}
