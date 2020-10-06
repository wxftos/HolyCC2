#include <assert.h>
#include <cacheingLexer.h>
#include <ctype.h>
#include <hashTable.h>
#include <preprocessor.h>
#include <stdio.h>
#include <str.h>
#include <string.h>
#include <stringParser.h>
#include <unistd.h>
// TODO implement exe,if,ifdef,ifndef
static __thread strSourceMapping sourceMappings = NULL;
static __thread FILE *preprocessedSource = NULL;

static void __vecDestroy2(struct __vec **vec) { __vecDestroy(*vec); }
static void fileDestroy(FILE **file) { fclose(*file); }
MAP_TYPE_DEF(struct defineMacro, DefineMacro);
MAP_TYPE_FUNCS(struct defineMacro, DefineMacro);
static struct __vec *createNullTerminated(struct __vec *vec) {
	__auto_type nameLen = __vecSize(vec);
	__auto_type retVal = __vecResize(NULL, nameLen + 1);
	memcpy(retVal, vec, nameLen);
	nameLen[(char *)retVal] = '\0';
	return retVal;
}
static void *findEndOfLine(const struct __vec *text, long pos) {
	void *retVal = (void *)text + pos;
	for (; retVal < (void *)text + __vecSize(text); retVal++) {
		__auto_type chr = *(char *)retVal;
		if (chr == '\r' || chr == '\n')
			break;
	}
	if (retVal == (void *)text + __vecSize(text))
		return retVal - 1;
	
	return retVal;
}
void *findNextLine(const struct __vec *text, long pos) {
	void *retVal = findEndOfLine(text, pos);
	for (; retVal < (void *)text + __vecSize(text); retVal++) {
		__auto_type chr = *(char *)retVal;
		if (chr != '\r' || chr != '\n')
			break;
	}
	return retVal;
}
static void *findSkipStringAndComments(
    const struct __vec *text, long pos, const void *data,
    void *(*findFunc)(const void *, const struct __vec *text, long pos)) {
	__auto_type end = (void *)text + __vecSize(text);
loop:;
	void *find = findFunc(data, text, pos);

	void *findCommentS = strstr((void *)text + pos, "//");
	void *findCommentM = strstr((void *)text + pos, "/*");
	findCommentS = (findCommentS == NULL) ? end : findCommentS;
	findCommentM = (findCommentM == NULL) ? end : findCommentM;
	__auto_type lesserComment =
	    (findCommentS < findCommentM) ? findCommentS : findCommentM;

	void *findStr1 = strchr((void *)text + pos, '\'');
	void *findStr2 = strchr((void *)text + pos, '"');
	findStr1 = (findStr1 == NULL) ? end : findStr1;
	findStr2 = (findStr2 == NULL) ? end : findStr2;
	__auto_type lesserStr = (findStr1 < findStr2) ? findStr1 : findStr2;

	if (find < lesserStr && find < lesserComment)
		return find;

	if (lesserComment != end && lesserComment < lesserStr) {
		// Skip comments
		if (lesserComment == findCommentS) {
			// Single-line
			pos = findCommentS - (void *)text;
			pos = findNextLine(text, pos) - (void *)text;
		} else if (lesserComment == findCommentM) {
			// Multiline
			void *otherSide = strstr((void *)text, "*/");
			if (otherSide == NULL)
				return NULL;
			else
				return otherSide + strlen("*/");
		}
	} else if (lesserStr != end) {
		// Skip strings
		pos = findStr1 - (void *)text;
		long endOfString;
		int err;
		stringParse(text, pos, &endOfString, NULL, &err);
		if (err)
			return NULL;
	} else {
		return NULL; // Will never reach here
	}
	goto loop;
	return NULL;
}
static void *chrFind(const void *a, const struct __vec *b, long pos) {
	return strstr((void *)b + pos, a);
}
static void *skipNonMacro(const struct __vec *text, long pos) {
	return findSkipStringAndComments(text, pos, "#", chrFind);
}
static void *replacementFindPred(const void *a, const struct __vec *text,
                                 long pos) {
	const mapDefineMacro map = (const mapDefineMacro)a;
	__auto_type b = (void *)text + pos;
	__auto_type textEnd = b + __vecSize(text);

	while (b != textEnd) {
		// Check if charactor before is a word charactor,if so quit search
		if (b != text) // Check if a start
			if (isalnum(-1 [(char *)b]))
				return NULL;
		if (isalpha(*(char *)b)) {
			__auto_type original = b;
			__auto_type end = original;
			while (isalnum(*(char *)end) && end < textEnd)
				end++;

			char buffer[end - original + 1];
			memcpy(buffer, original, end - original);
			buffer[end - original] = '\0';
			__auto_type macro = mapDefineMacroGet(map, buffer);

			if (macro == NULL)
				goto next;

			return b;
		}

	next:
		// Skip word then look for next word
		while (b < textEnd)
			if (isalnum(*(char *)b))
				b++;
			else
				break;
		while (b < textEnd)
			if (!isalnum(*(char *)b))
				b++;
			else
				break;

		continue;
	}
	return NULL;
}
static void *findNextReplacement(const struct __vec *text,
                                 const mapDefineMacro defines, long pos) {
	return findSkipStringAndComments(text, pos, defines, replacementFindPred);
}
static void *skipWhitespace(struct __vec *text, long pos) {
	__auto_type len = __vecSize(text);
	void *where;
	for (where = (void *)text + pos; where < (void *)text + len; where++) {
		__auto_type chr = *(char *)where;
		if (isblank(chr))
			continue;
		if (chr == '\n')
			continue;
		if (chr == '\r')
			continue;
		break;
	}
	return where;
}
static int expectMacroAndSkip(const char *macroText, struct __vec *text,
                              long pos, long *end) {
	const char *text2 = (const char *)text;
	if (*text2 != '#')
		return 0;

	pos = skipWhitespace(text, pos + 1) - (const void *)text;

	__auto_type len = strlen(macroText);
	if (__vecSize(text) - pos < len)
		return 0;

	if (pos + len < __vecSize(text))
		if (isalnum((pos + len)[(const char *)text]))
			return 0;

	if (end != NULL)
		*end = pos + len;
	return 0 == strncmp((void *)text + pos, macroText, len);
}
static int includeMacroLex(struct __vec *text, long pos, long *end,
                           struct includeMacro *result, int *err) {
	if (err != NULL)
		*err = 0;

	if (!expectMacroAndSkip("include", text, pos, &pos))
		return 0;

	pos = skipWhitespace(text, pos) - (void *)text;

	struct parsedString filename;
	if (!stringParse(text, pos, end, &filename, err))
		goto malformed;

	struct includeMacro retVal;
	retVal.fileName =
	    __vecAppendItem(NULL, (char *)filename.text, __vecSize(filename.text));
	if (result != NULL)
		*result = retVal;
	return 1;
malformed : {
	*err = 1;
	return 0;
}
}
static void defineMacroDestroy(void *macro) {
	struct defineMacro *macro2 = macro;
	__vecDestroy(macro2->name);
	__vecDestroy(macro2->text);
}
static void defineMacroDestroyLexerItem(struct __lexerItem *item) {
	defineMacroDestroy(lexerItemValuePtr(item));
}
static int defineMacroLex(struct __vec *text, long pos, long *end,
                          struct defineMacro *result, int *err) {
	if (!expectMacroAndSkip("define", text, pos, &pos))
		return 0;

	pos = skipWhitespace(text, pos) - (void *)text;

	int len = 0;
	while (isalnum((pos + len)[(char *)text]))
		len++;

	if (isdigit(pos[(char *)text]))
		goto malformed;

	__auto_type name = __vecAppendItem(NULL, &pos[(char *)text], len);
	name = __vecAppendItem(name, "\0", 1);

	pos += len;
	__auto_type end2 = findEndOfLine(text, pos) - (void *)text;
	// Ignore whitespace from end2
	for (end2--; isblank(end2[(char *)text]); end2--)
		;
	struct __vec *replacement =
	    __vecAppendItem(NULL, &pos[(char *)text], end2 - pos);
	if (end != NULL)
		*end = findEndOfLine(text, pos) - (void *)text;
	replacement = __vecAppendItem(replacement, "\0", 1);

	struct defineMacro retVal;
	retVal.name = name;
	retVal.text = replacement;
	if (result != NULL)
		*result = retVal;
	return 0;
malformed : {
	*err = 1;
	return 0;
}
}
void includeMacroDestroy(struct includeMacro *macro) {
	__vecDestroy(macro->fileName);
}
/**
 * This takes a processed text(result of preprocessor),then maps it to source
 */
long mappedPosition(const strSourceMapping mapping, long processedPos) {
	long processedStart = 0;
	long sourcePrevEnd = 0;
	for (long i = 0; i != strSourceMappingSize(mapping); i++) {
		__auto_type sourceLen = mapping[i].processedEnd - mapping[i].processedStart;

		/**
		 * Check if between gap
		 * Source:
		 * #define x ["123"]
		 * ["a"] x "[b"]
		 *
		 * Processed:
		 * ["a"] ["123"] ["b"]
		 *
		 * Gap is where "123" is in processed text( "123" comes from x)
		 */
		__auto_type oldProcessedStart = processedStart;
		processedStart += mapping[i].processedStart - mapping[i].processedEnd;
		if (processedStart > processedPos && processedPos >= oldProcessedStart)
			return sourcePrevEnd;

		oldProcessedStart = processedStart;
		processedStart += sourceLen;
		if (processedStart > processedPos && processedPos >= oldProcessedStart)
			return processedPos - oldProcessedStart;

		sourcePrevEnd += sourceLen;
	}
	return -1;
}
static int sourceMappingCmp(const void *a, const void *b) {
	const struct sourceMapping *A = a;
	const struct sourceMapping *B = b;
	if (A->processedStart > B->processedStart)
		return 1;
	else if (A->processedStart == B->processedStart)
		return 0;
	else
		return -1;
}
static void insertMacroText(struct __vec **text, const struct __vec *toInsert,
                            long insertAt, long deleteCount,
                            strSourceMapping *sourceMappings) {
	__auto_type insertLen = __vecSize(toInsert);
	__auto_type sourceLen = __vecSize(*text);
	*text = __vecResize(*text, insertLen + sourceLen - deleteCount);

	assert(deleteCount <= sourceLen);

	// Insert the text and delete requested amount of chars
	memmove((void *)text + insertAt + insertLen,
	        (void *)text + insertAt + deleteCount,
	        sourceLen - insertAt - deleteCount);

	// Check if insert is in source mapping
	long moveAheadFrom;
	for (int i = 0; i != strSourceMappingSize(*sourceMappings); i++) {
		__auto_type start = sourceMappings[0][i].processedStart;
		__auto_type end = sourceMappings[0][i].processedEnd;
		if (insertAt >= start && insertAt < end) {

			__auto_type oldEnd = sourceMappings[0][i].processedEnd;
			sourceMappings[0][i].processedEnd = insertAt;

			struct sourceMapping splitRight;
			splitRight.processedStart = insertAt + insertLen - deleteCount;
			splitRight.processedEnd = oldEnd + insertLen - deleteCount;
			*sourceMappings = strSourceMappingSortedInsert(
			    *sourceMappings, splitRight, sourceMappingCmp);

			// Move ahead mappings forward
			__auto_type splitRightPtr = strSourceMappingSortedFind(
			    *sourceMappings, splitRight, sourceMappingCmp);
			assert(splitRightPtr != NULL);
			long splitRightIndex =
			    (splitRightPtr - *sourceMappings) / sizeof(struct sourceMapping);
			moveAheadFrom = splitRightIndex + 1;
		moveAhead:;
			for (__auto_type i2 = moveAheadFrom;
			     i2 < strSourceMappingSize(*sourceMappings); i2++) {
				sourceMappings[0][i2].processedStart += insertLen - deleteCount;
				sourceMappings[0][i2].processedEnd += insertLen - deleteCount;
			}
			return;
		} else if (insertAt < start) {
			moveAheadFrom = i;
			goto moveAhead;
		}
	}
}
static long fstreamSeekEndOfLine(FILE *stream) {
	char buffer[1025];
	buffer[1024] = '\0';
	long traveled = 0;
	__auto_type oldPos = ftell(stream);
loop:;
	__auto_type count = fread(buffer, 1, 1024, stream);
	traveled += count;

	if (count == 0) { // EOF
		// Seek to original pos
		fseek(stream, SEEK_CUR, -traveled);

		return ftell(stream);
	}

	char *finds[] = {
	    strchr(buffer, '\n'),
	    strchr(buffer, '\r'),
	};
	for (int i = 0; i != 2; i++) {
		if (finds[i] != NULL) {
			__auto_type at = ftell(stream);
			__auto_type offset = finds[i] - buffer;
			if (offset >= count)
				continue;
			// Seek to original pos
			fseek(stream, SEEK_CUR, -traveled);

			return at - oldPos + offset;
		}
	}
	goto loop;
}
static long fstreamGoPastEndOfLine(FILE *stream) {
	__auto_type res = fstreamSeekEndOfLine(stream);
	fseek(stream, SEEK_CUR, res - ftell(stream));
	do {
		__auto_type chr = fgetc(stream);
		if (chr == EOF)
			return ftell(stream);

		if (chr == '\n' || chr == '\r')
			continue;

		// rewind before current char
		fseek(stream, SEEK_CUR, -1);
		return ftell(stream);
		break;
	} while (1);
}
static void createPreprocessedFileLine(long processedPos,
                                       strSourceMapping *sourceMappings,
                                       mapDefineMacro defines,
                                       struct __vec *text_, FILE *writeTo,
                                       int *err) {
	struct __vec *retVal = __vecAppendItem(NULL, text_, __vecSize(text_));

	// Substitute macros
	for (long where = 0; where != __vecSize(retVal);) {
		__auto_type prev = (void *)retVal + where;
		void *nextReplacement = findNextReplacement(prev, defines, where);

		if (nextReplacement == NULL) {
			break;
		} else {
			retVal = __vecAppendItem(retVal, prev, nextReplacement - prev);

			// Get replacement text
			__auto_type alnumCount = 0;
			for (__auto_type i = nextReplacement; isalnum(*(char *)i);
			     i++, alnumCount++)
				;
			__auto_type slice = __vecAppendItem(NULL, nextReplacement, alnumCount);
			__auto_type replacement = mapDefineMacroGet(defines, (void *)slice);
			assert(NULL != replacement);

			// Add source mapping
			long insertAt = nextReplacement - (void *)retVal;
			insertMacroText(&retVal, replacement->text, insertAt + processedPos,
			                alnumCount, sourceMappings);

			__vecDestroy(slice);
		}
	}

	for (long where = 0; where != __vecSize(retVal);) {
		void *nextMacro = skipNonMacro(retVal, where);

		if (nextMacro == NULL)
			break;

		long endPos;
		struct defineMacro define;
		struct includeMacro include;

		if (defineMacroLex(retVal, nextMacro - (void *)retVal, &endPos, &define,
		                   err)) {
			// Make slice with null ending
			__auto_type nameStr = createNullTerminated(define.name);
			// Insert new macro
			mapDefineMacroInsert(defines, (char *)nameStr, define);
			__vecDestroy(nameStr);

			// Remove macro text from source
			__auto_type at = nextMacro - (void *)retVal;
			insertMacroText(&retVal, NULL, at + processedPos, endPos - at,
			                sourceMappings);
		} else if (includeMacroLex(retVal, nextMacro - (void *)retVal, &endPos,
		                           &include, err)) {
			struct includeMacro includeClone
			    __attribute((cleanup(includeMacroDestroy)));
			includeClone = include;

			struct __vec *fn __attribute__((cleanup(__vecDestroy2)));

			fn = createNullTerminated(include.fileName);
			assert(fn != NULL); // TODO whine about file not found

			FILE *file __attribute__((cleanup(fileDestroy)));
			file = fopen((char *)fn, "r");

			long lineStart = ftell(file);
			for (__auto_type lineEnd = fstreamSeekEndOfLine(file);
			     lineStart != lineEnd;) {
				fseek(file, SEEK_CUR, lineStart - ftell(file));
				struct __vec *lineText __attribute__((cleanup(__vecDestroy2)));
				lineText = __vecResize(NULL, lineEnd - lineStart + 1);
				fread((void *)lineText, 1, lineEnd - lineStart, file);
				((char *)lineText)[lineEnd - lineStart] = '\0';

				createPreprocessedFileLine(processedPos, sourceMappings, defines,
				                           lineText, writeTo, err);
				if (err != NULL)
					if (*err)
						goto returnLabel;

				// Append newline to preprocessed source
				char newLineBuffer[32];
				__auto_type newLine = fstreamGoPastEndOfLine(file);
				fseek(file, SEEK_CUR, lineEnd - ftell(file));
				fread(newLineBuffer, 1, newLine - lineEnd, file);
				fwrite(newLineBuffer, 1, newLine - lineEnd, preprocessedSource);

				lineStart = newLine;
				fseek(file, SEEK_CUR, newLine - ftell(file));
			}
		}
		if (err != NULL)
			if (*err)
				goto returnLabel;

		where = endPos;
	}
returnLabel:
	return;
}

FILE *createPreprocessedFile(struct __vec *text, int *err) {
	strSourceMappingDestroy(&sourceMappings);

	if (preprocessedSource != NULL)
		fclose(preprocessedSource);
	preprocessedSource = tmpfile();

	mapDefineMacro defines = mapDefineMacroCreate();

	__auto_type processedPos = 0;

	long lineStart = 0;
	for (long lineEnd = findEndOfLine(text, lineStart) - (void *)text;;) {
		__auto_type oldFPos = ftell(preprocessedSource);

		struct __vec *lineText __attribute__((cleanup(__vecDestroy2)));
		lineText = __vecResize(NULL, lineEnd - lineStart + 1);
		memcpy(lineText, (void *)text + lineStart, lineEnd - lineStart);
		(lineEnd - lineStart)[(char *)lineText] = '\0';

		createPreprocessedFileLine(processedPos, &sourceMappings, defines, lineText,
		                           preprocessedSource, err);
		if (err != NULL)
			if (*err)
				goto returnLabel;

		// Append newline to preprocessed source
		__auto_type nextLine = findNextLine(text, lineStart) - (void *)text;
		__auto_type lineEnd2 = findEndOfLine(text, lineStart) - (void *)text;
		fwrite((void *)text + lineEnd2, 1, nextLine - lineEnd2, preprocessedSource);

		// Update the start of the processed source for next line
		__auto_type added = ftell(preprocessedSource) - oldFPos;
		processedPos += added;

		lineStart = nextLine;
	}

returnLabel:
	mapDefineMacroDestroy(defines, defineMacroDestroy);

	return preprocessedSource;
}
