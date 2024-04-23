#pragma once
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#define HCRT_LOC_LOCAL "E:/Projects/os/templeos/TINE/HolyCC2/HolyCRT/"
#define HCRT_LOC_INSTALLED "C:/Program Files (x86)/holycc2/share/HolyCRT"
__attribute__((always_inline)) inline char* HCRTFile(const char* file) {
	//long len = snprintf(NULL, 0, "%s/%s", HCRT_LOC_LOCAL, file);
	#define len 256
	char local[len + 1];
	sprintf(local, "%s/%s", HCRT_LOC_LOCAL, file);

	//len = snprintf(NULL, 0, "%s/%s", HCRT_LOC_INSTALLED, file);
	char installed[len + 1];
	sprintf(installed, "%s/%s", HCRT_LOC_INSTALLED, file);

	if (0 == access(local, F_OK))
		return strcpy(calloc(strlen(local) + 1, 1), local);

	if (0 == access(installed, F_OK))
		return strcpy(calloc(strlen(installed) + 1, 1), installed);

	fprintf(stderr, "HCRT file\"%s\" not found\n", file);
	abort();
#undef len
}
