// safectype.h - make the <ctype.h> classification/conversion functions safe to
// call with raw (signed) char arguments.
//
// Legacy Comic Chat code calls isspace()/isdigit()/isalpha()/tolower()/... with
// plain `char` values all over the place. On a signed-char compiler any byte
// >= 0x80 (extended ASCII, UTF-8/DBCS lead bytes, etc.) becomes a NEGATIVE int,
// which is undefined behaviour for these functions and trips the modern debug
// CRT assertion "c >= -1 && c <= 255" (ucrt isctype.cpp). Wrapping each call so
// the argument is passed as unsigned char fixes the entire class of crash at
// once, without editing thousands of call sites.
//
// Use explicitly named helpers instead of redefining the CRT names. Global
// ctype macros corrupt modern standard-library overloads such as
// std::tolower(value, locale) and no longer compile with current MSVC.

#ifndef _SAFECTYPE_H_
#define _SAFECTYPE_H_

#include <ctype.h>		// pull in the real declarations BEFORE we define macros

static __inline int cc_isalnum(int c)  { return (isalnum)((unsigned char)c); }
static __inline int cc_isalpha(int c)  { return (isalpha)((unsigned char)c); }
static __inline int cc_iscntrl(int c)  { return (iscntrl)((unsigned char)c); }
static __inline int cc_isdigit(int c)  { return (isdigit)((unsigned char)c); }
static __inline int cc_isgraph(int c)  { return (isgraph)((unsigned char)c); }
static __inline int cc_islower(int c)  { return (islower)((unsigned char)c); }
static __inline int cc_isprint(int c)  { return (isprint)((unsigned char)c); }
static __inline int cc_ispunct(int c)  { return (ispunct)((unsigned char)c); }
static __inline int cc_isspace(int c)  { return (isspace)((unsigned char)c); }
static __inline int cc_isupper(int c)  { return (isupper)((unsigned char)c); }
static __inline int cc_isxdigit(int c) { return (isxdigit)((unsigned char)c); }
static __inline int cc_tolower(int c)  { return (tolower)((unsigned char)c); }
static __inline int cc_toupper(int c)  { return (toupper)((unsigned char)c); }

#endif // _SAFECTYPE_H_
