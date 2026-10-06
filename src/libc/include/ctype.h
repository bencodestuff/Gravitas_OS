#ifndef _CTYPE_H
#define _CTYPE_H 1

#include <sys/cdefs.h>

__BEGIN_DECLS

int isdigit(int);
int isspace(int);
int isalpha(int);
int isalnum(int);
int isupper(int);
int islower(int);
int isxdigit(int);
int isprint(int);
int isgraph(int);
int ispunct(int);
int iscntrl(int);

int toupper(int);
int tolower(int);

__END_DECLS

#endif
