#include <assert.h>
#include <holyCParser.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
// TODO replace repeats with struct parserNodeRepeat
#define ALLOCATE(x)                                                            \
	({                                                                           \
		void *r = malloc(sizeof(x));                                               \
		memcpy(r, &x, sizeof(x));                                                  \
		r;                                                                         \
	})
enum assoc {
	DIR_LEFT,
	DIR_RIGHT,
};
static void parserNodeDestroy(struct parserNode **node) { free(*node); }
static int isExpression(const struct parserNode *node) {
	switch (node->type) {
	case NODE_LIT_CHAR:
	case NODE_LIT_FLOATING:
	case NODE_LIT_INT:
	case NODE_LIT_STRING:
	case NODE_EXPR_BINOP:
	case NODE_EXPR_UNOP:
	case NODE_NAME_TOKEN:
		return 1;
	default:
		return 0;
	}
}
static struct __lexerItemTemplate nameTemplate;
static struct __lexerItemTemplate intTemplate;
static struct __lexerItemTemplate stringTemplate;
static struct __lexerItemTemplate floatingTemplate;
static struct __lexerItemTemplate operatorTemplate;
static strLexerItemTemplate templates;
static void initTemplates() __attribute__((constructor));
static void initTemplates() {
	intTemplate = intTemplateCreate();
	stringTemplate = stringTemplateCreate();
	// floatingTemplate = floatingTemplateCreate();

	const char *operators[] = {
	    "~",
	    "!",
	    //
	    ".",
	    "->"
	    //
	    "`",
	    "<<", ">>",
	    //
	    "*", "/", "%",
	    //
	    "+", "-",
	    //
	    "++", "--",
	    //
	    "&&", "||", "^^",
	    //
	    "&", "|", "^",
	    //
	    "(", ")", "[", "]", ",",
	    //
	    "=", "+=", "-=", "*=", "/=", "%=", "|=", "&=", "^=", ">>=", "<<=="};
	__auto_type operCount = sizeof(operators) / sizeof(*operators);
	operatorTemplate = keywordTemplateCreate(operators, operCount);

	const char *keywords[] = {
	    "if",
	    "else",
	    //
	    "while",
	    "do",
	    "for",
	    "break",
	    //
	    "class",
	    "union",
	    //
	    "switch",
	    "case",
	    "default",
	    //
	    "reg",
	    "noreg",
	    //
	    "asm",
	    //
	    "static",
	    "extern",
	    "_extern",
	    "import",
	    "_import",
	    "public",
	};
	__auto_type keywordCount = sizeof(keywords) / sizeof(*keywords);

	nameTemplate = nameTemplateCreate(keywords, keywordCount);

	const struct __lexerItemTemplate *templates2[] = {
	    &intTemplate,
	    &operatorTemplate,
	    //&floatingTemplate,
	    &stringTemplate,
	    &nameTemplate,
	};
	long templateCount = sizeof(templates2) / sizeof(*templates2);
	templates = strLexerItemTemplateAppendData(NULL, templates2, templateCount);
}
const strLexerItemTemplate holyCLexerTemplates() { return templates; }
struct parserNode **findOtherParen(struct parserNode **start,
                                   struct parserNode **end, int index) {
	const char *lefts[] = {"(", "["};
	const char *rights[] = {")", "]"};

	int depth = 0;
	int dir = 0;
	for (int firstRun = 1; depth != 0 || firstRun; firstRun = 0) {
		if (index < 0 || index >= end - start)
			goto fail;

		struct parserNodeOpTerm *node = (void *)start[index];
		if (node->base.type == NODE_OP_TERM) {
			for (long i = 0; i != 2; i++)
				if (0 == strcmp(node->text, lefts[i]))
					depth++;
			for (long i = 0; i != 2; i++)
				if (0 == strcmp(node->text, rights[i]))
					depth--;

			if (firstRun == 1) {
				dir = (depth == 1) ? 1 : -1;
				if (depth == 0)
					goto fail;
			}

			if (depth == 0)
				break;
		}
		if (firstRun && depth == 0)
			goto fail;

		index += dir;
		if (index < 0 || (end - start) / sizeof(*start) > index)
			goto fail;
	}

	return &start[index];
fail : { return NULL; }
}
static void *floatingLiteral(const struct __lexerItem *item) {
	if (item->template == &floatingTemplate) {
		struct parserNodeFloatingLiteral lit;
		lit.pos.start = item->start;
		lit.pos.end = item->end;
		lit.base.type = NODE_LIT_FLOATING;
		lit.value = *(struct lexerFloating *)lexerItemValuePtr(item);

		return ALLOCATE(lit);
	}
	return NULL;
}
static void *nameToken(const struct __lexerItem *item) {
	if (item->template == &nameTemplate) {
		const char *text = lexerItemValuePtr(item);
		struct parserNodeNameToken retVal;
		retVal.base.type = NODE_NAME_TOKEN;
		retVal.pos.start = item->start;
		retVal.pos.end = item->end;
		retVal.text = malloc(strlen(text) + 1);
		strcpy(retVal.text, text);

		return ALLOCATE(retVal);
	}

	return NULL;
}
static void *intLiteral(const struct __lexerItem *item) {
	if (item->template == &intTemplate) {
		struct parserNodeIntLiteral lit;
		lit.pos.start = item->start;
		lit.pos.end = item->end;
		lit.base.type = NODE_LIT_INT;
		lit.value = *(struct lexerInt *)lexerItemValuePtr(item);

		struct parserNode *node = ALLOCATE(lit);
		return node;
	}
	return NULL;
};
static void *stringLiteral(const struct __lexerItem *item) {
	if (item->template == &stringTemplate) {
		struct parserNodeStringLiteral lit;
		lit.pos.start = item->start;
		lit.pos.end = item->end;
		lit.base.type = NODE_LIT_STRING;
		lit.value = *(struct parsedString *)lexerItemValuePtr(item);

		return ALLOCATE(lit);
	}
	return NULL;
};
struct parserNode *parserNodeUnopCreate(struct parserNode *exp,
                                        struct parserNode *op) {
	struct parserNodeUnop unop;
	unop.a = exp;
	unop.op = op;
	unop.base.type = NODE_EXPR_UNOP;

	return ALLOCATE(unop);
}
struct parserNode *parserNodeCommaSequenceAppend(struct parserNode *start,
                                                 struct parserNode *comma,
                                                 struct parserNode *next) {
	if (start == NULL) {
		struct parserNodeCommaSequence seq;
		seq.base.type = NODE_COMMA_SEQ;
		seq.commas = NULL;
		seq.nodes = strParserNodeAppendItem(NULL, next);
		return ALLOCATE(seq);
	}
	struct parserNodeCommaSequence *seq = (void *)start;
	seq->commas = strParserNodeAppendItem(seq->commas, comma);
	seq->nodes = strParserNodeAppendItem(seq->nodes, next);

	return (void *)seq;
}
static void replaceNodeReferences(struct parserNode **start,
                                  struct parserNode **end, int index,
                                  struct parserNode *replaceWith) {
	__auto_type toReplace = start[index];
	start[index] = replaceWith;

	for (long i = index - 1; i >= 0; i--)
		if (start[i] == toReplace)
			start[i] = replaceWith;
		else
			break;

	for (long i = index + 1; i < (end - start); i++)
		if (start[i] == toReplace)
			start[i] = replaceWith;
		else
			break;
}
static struct parserNode *operator(const char **names, long count,
                                   enum assoc dir, int grabsLeft,
                                   int grabsRight, struct parserNode **start,
                                   struct parserNode **end) {
	__auto_type originalStart = start;
	__auto_type originalEnd = end;
	if (dir == DIR_LEFT) {
	} else if (dir == DIR_RIGHT) {
		__auto_type clone = start;
		start = end - 1;
		end = clone;
	}

	for (__auto_type node = start; node != end;
	     node = (dir == DIR_LEFT) ? node + 1 : node - 1) {
		__auto_type node2 =
		    findOtherParen(originalStart, originalEnd, node - originalStart);
		if (node2 != NULL)
			node = node2;

		if (node == NULL)
			break;
		if (node == originalStart && grabsLeft)
			continue;
		if (node == originalEnd - 1 && grabsRight)
			continue;
		if (grabsLeft && !isExpression(node[-1]))
			continue;
		if (grabsRight && !isExpression(node[1]))
			continue;
		//If unop,ensure next item isn't expression if consumes left
		if(grabsLeft&&!grabsRight) {
		 if(node+1<originalEnd)
			if(isExpression(node[1]))
			 continue;
		}
		//If unop,ensure prev item isn't expression if consumes right
		if(!grabsLeft&&grabsRight) {
		 if(node-1>=originalStart)
			if(isExpression(node[-1]))
			 continue;
		}

		__auto_type item = *node;
		if (item->type != NODE_OP_TERM)
			continue;

		__auto_type text = ((struct parserNodeOpTerm *)item)->text;
		for (long i = 0; i != count; i++) {
			if (0 == strcmp(text, names[i])) {
				// Create new node

				struct parserNode *newNode = NULL;
				if (grabsLeft && grabsRight) {
					struct parserNodeBinop bin;
					bin.base.type = NODE_EXPR_BINOP;
					bin.a = node[-1];
					bin.op = node[0];
					bin.b = node[1];

					newNode = ALLOCATE(bin);
				} else if (grabsLeft || grabsRight) {
					struct parserNodeUnop un;
					un.a = node[grabsLeft ? -1 : 1];
					un.op = node[0];
					un.base.type = NODE_EXPR_UNOP;

					newNode = ALLOCATE(un);
				} else {
					assert(0);
				}

				// Replace consumed nodes with newNode
				if (grabsLeft)
					replaceNodeReferences(originalStart, originalEnd,
					                      (node - originalStart) - 1, newNode);
				if (grabsRight)
					replaceNodeReferences(originalStart, originalEnd,
					                      (node - originalStart) + 1, newNode);
				replaceNodeReferences(originalStart, originalEnd,
				                      (node - originalStart), newNode);

				return newNode;
			}
		}
	}
	return NULL;
}
static struct parserNode *lexerItem2ParserNode(struct __lexerItem *item) {
	if (item->template == &operatorTemplate) {
		struct parserNodeOpTerm term;
		term.base.type = NODE_OP_TERM;
		term.pos.start = item->start;
		term.pos.end = item->end;
		term.text = *(const char **)lexerItemValuePtr(item);

		return ALLOCATE(term);
	} else if (item->template == &nameTemplate) {
		struct parserNodeNameToken token;
		token.base.type = NODE_NAME_TOKEN;
		token.pos.start = item->start;
		token.pos.end = item->end;

		const char *text = lexerItemValuePtr(item);
		token.text = malloc(strlen(text) + 1);
		strcpy(token.text, text);

		return ALLOCATE(token);
	} else if (item->template == &stringTemplate) {
		struct parsedString *string = lexerItemValuePtr(item);
		struct parserNodeStringLiteral str;
		str.base.type = NODE_LIT_STRING;
		str.pos.start = item->start;
		str.pos.end = item->end;
		str.value = *(struct parsedString *)lexerItemValuePtr(item);

		return ALLOCATE(str);
	} else if (item->template == &intTemplate) {
		struct parserNodeIntLiteral intLit;
		intLit.base.type = NODE_LIT_INT;
		intLit.pos.start = item->start;
		intLit.pos.end = item->end;
		intLit.value = *(struct lexerInt *)lexerItemValuePtr(item);

		return ALLOCATE(intLit);
	} else {
		assert(0);
	}
}
static struct parserNode *__parseExpression(struct parserNode **start,
                                            struct parserNode **end,
                                            int includeCommas, int *success);
static struct parserNode *
pairOperator(const char *left, int grabsLeft, struct parserNode **start,
             struct parserNode **end,
             struct parserNode *(*callBack)(struct parserNode *left,
                                            struct parserNode *node),
             int *success) {
	for (long i = 0; i < (end - start); i++) {
		if (start[i]->type != NODE_OP_TERM)
			goto next;
		if (i == 0 && grabsLeft)
			goto next;
		if (grabsLeft && !isExpression(start[i - 1]))
			goto next;

		struct parserNodeOpTerm *term = (void *)start[i];
		if (0 == strcmp(left, term->text)) {
			__auto_type next = findOtherParen(start, end, i);
			if (next == NULL)
				goto fail;

			int success2;
			__auto_type retVal = __parseExpression(start + i + 1, next, 1, &success2);
			if (!success2)
				goto fail;

			replaceNodeReferences(start, end, i, retVal);
			replaceNodeReferences(start, end, next - start, retVal);
			for (long i2 = i; i2 != next - start; i2++)
				start[i2] = retVal;

			return retVal;
		} else {
		}
	next:;
		__auto_type next = findOtherParen(start, end, i);
		if (next != NULL) {
			// i will be incremented,so go back one
			i = next - start;
			continue;
		}
	}
fail : {
	if (success != NULL)
		*success = 0;

	return NULL;
}
}
// TODO implement me
static struct parserNode *parseTypeName(struct parserNode **start,
                                        struct parserNode **end,
                                        char **itemName) {
	return NULL;
}
static struct parserNode *parseTypeCast(struct parserNode **start,
                                        struct parserNode **end) {
	for (long i = 0; i != (end - start); i++) {
		if (i == 0)
			continue;
		if (!isExpression(start[i - 1]))
			continue;
		if (start[i]->type != NODE_OP_TERM)
			continue;

		struct parserNodeOpTerm *term = (void *)start[i];
		if (0 != strcmp(term->text, "("))
			continue;

		__auto_type endItem = findOtherParen(start, end, i);
		if (endItem == NULL)
			return NULL;

		long endI = (endItem - start);
		__auto_type type = parseTypeName(&start[i], endItem, NULL);

		if (type != NULL) {
			struct parserNodeTypeCast retVal;
			retVal.base.type = NODE_TYPECAST;
			retVal.exp = start[i - 1];
			retVal.toType = type;
			__auto_type alloced = ALLOCATE(retVal);

			for (__auto_type p = &start[i - 1]; p != endItem + 1; p++)
				*p = alloced;

			return alloced;
		}
	}
	return NULL;
}
static struct parserNode *__functionCallback(struct parserNode *left,
                                             struct parserNode *node) {
	struct parserNodeFunctionCall retVal;
	retVal.base.type = NODE_FUNC_CALL;
	retVal.args = NULL;
	retVal.func = left;
	if (node->type == NODE_COMMA_SEQ) {
		struct parserNodeCommaSequence *seq = (void *)node;
		for (long i = 0; i != strParserNodeSize(seq->nodes); i++)
			retVal.args = strParserNodeAppendItem(retVal.args, seq->nodes[i]);

		parserNodeDestroy(&node);
	} else if (isExpression(node)) {
		retVal.args = strParserNodeAppendItem(retVal.args, node);
	} else {
		return NULL;
	}

	return ALLOCATE(retVal);
}
static struct parserNode *__arrayAccessCallback(struct parserNode *left,
                                                struct parserNode *node) {
	struct parserNodeArrayAccess retVal;
	retVal.base.type = NODE_ARRAY_ACCESS;
	retVal.exp = left;
	retVal.index = node;

	return ALLOCATE(retVal);
}
STR_TYPE_DEF(void *, Ptr);
STR_TYPE_FUNCS(void *, Ptr);
/**
 * Returns regular parse on no comma
 */
struct parserNode *commaSequence(struct parserNode **start,
                                 struct parserNode **end, int *success) {
	strPtr commaPositions = NULL;
	for (__auto_type p = start; p != end; p++) {
		if (p[0]->type == NODE_OP_TERM) {
			struct parserNodeOpTerm *op = (void *)(p[0]);
			if (0 == strcmp(op->text, ","))
				commaPositions = strPtrAppendItem(commaPositions, p);
		}
	}

	strParserNode nodes = NULL;

	int anyFailed = 0;
	for (long i = 0; i != strPtrSize(commaPositions) + 1; i++) {
		int success2;

		struct parserNode **end2 =
		    (i < strPtrSize(commaPositions)) ? commaPositions[i] : end;
		struct parserNode **start2 = (i > 0) ? (struct parserNode**)commaPositions[i-1] + 1 : start;

		if (start2 == end2) {
			nodes = strParserNodeAppendItem(nodes, NULL);
			continue;
		}

		nodes = strParserNodeAppendItem(
		    nodes, __parseExpression(start2, end2, 0, &success2));
		anyFailed |= !success2;
	}

	if (success != NULL)
		*success = !anyFailed;

	struct parserNode *retVal = NULL;
	if (strParserNodeSize(nodes) == 1) {
		retVal = nodes[0];
	} else if (strParserNodeSize(nodes) > 0) {
		struct parserNodeCommaSequence seq;
		seq.base.type = NODE_COMMA_SEQ;
		seq.commas = NULL;
		for (long i = 0; i != strPtrSize(commaPositions); i++)
			seq.commas =
			    strParserNodeAppendItem(seq.commas, *(void **)commaPositions[i]);
		seq.nodes = NULL;
		for (long i = 0; i != strParserNodeSize(nodes); i++)
			seq.nodes = strParserNodeAppendItem(seq.nodes, nodes[i]);

		retVal = ALLOCATE(seq);
	}

	strPtrDestroy(&commaPositions);
	strParserNodeDestroy(&nodes);

	return retVal;
}
static struct parserNode *__parseExpression(struct parserNode **start,
                                            struct parserNode **end,
                                            int includeCommas, int *success) {
	if (includeCommas)
		return commaSequence(start, end, success);

	for (long prec = 0; prec != 14 + 1; prec++) {
		for (int found = 1; found;) {
			found = 0;
			struct parserNode *currentFind = NULL;

			// Typecast
			if (prec == 0) {
				currentFind = parseTypeCast(start, end);
				if (currentFind) {

					found = 1;
					continue;
				}
			}
			// Function call
			if (prec == 0) {
				currentFind =
				    pairOperator("(", 0, start, end, __functionCallback, success);
				if (currentFind) {
					found = 1;
					continue;
				}
			}
			if (prec == 0) {
				currentFind =
				    pairOperator("[", 1, start, end, __arrayAccessCallback, success);
				if (currentFind) {
					found = 1;
					continue;
				}
			}
#define OPERATORS(prec2, dir, left, right, ...)                                \
	if (prec == prec2) {                                                         \
		const char *names[] = {__VA_ARGS__};                                       \
		__auto_type count = sizeof(names) / sizeof(*names);                        \
		currentFind = operator(names, count, dir, left, right, start, end);        \
		if (currentFind != NULL) {                                                 \
			found = 1;                                                               \
			continue;                                                                \
		}                                                                          \
	}
			OPERATORS(0, DIR_LEFT, 1, 0, "++", "--");
			OPERATORS(0, DIR_LEFT, 1, 1, ".", "->");

			OPERATORS(1, DIR_RIGHT, 0, 1, "++", "--", "~", "!", "&", "*");
			OPERATORS(2, DIR_LEFT, 1, 1, "`", "<<", ">>");
			OPERATORS(3, DIR_LEFT, 1, 1, "*", "/", "%");
			OPERATORS(4, DIR_LEFT, 1, 1, "&");
			OPERATORS(5, DIR_LEFT, 1, 1, "^");
			OPERATORS(6, DIR_LEFT, 1, 1, "|");
			OPERATORS(7, DIR_LEFT, 1, 1, "+", "-");
			OPERATORS(8, DIR_LEFT, 1, 1, "<", ">", "<=", ">=");
			OPERATORS(9, DIR_LEFT, 1, 1, "!=", "==");
			OPERATORS(10, DIR_LEFT, 1, 1, "&&");
			OPERATORS(11, DIR_LEFT, 1, 1, "^^");
			OPERATORS(12, DIR_LEFT, 1, 1, "||");
			OPERATORS(13, DIR_RIGHT, 1, 1, "=",
			          "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=");
		}
	}

	__auto_type item = *start;
	for (__auto_type p = start + 1; p < end; p++)
		if (*p != item)
			goto fail;

	if (success != NULL)
		*success = 1;

	return item;
fail : {
	if (success != NULL)
		*success = 0;

	for (__auto_type p = start; p != end; p++)
		parserNodeDestroy(p);
	return NULL;
}
}
// TODO convert at nodes at once
struct parserNode *parseExpression(llLexerItem start, llLexerItem *end,
                                   int includeCommas, int *success) {
	strParserNode items = NULL;

	int parenDepth = 0;
	__auto_type node = start;
	for (; node != NULL; node = llLexerItemNext(node)) {
		__auto_type item = llLexerItemValuePtr(node);

		items = strParserNodeAppendItem(items, lexerItem2ParserNode(item));
	}
	if (end != NULL)
		*end = node;

	__auto_type retVal = __parseExpression(
	    items, items + strParserNodeSize(items), includeCommas, success);
	strParserNodeDestroy(&items);
	return retVal;
}
