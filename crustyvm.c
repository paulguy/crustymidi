/*
 * Copyright 2020 paulguy <paulguy119@gmail.com>
 *
 * This file is part of crustymidi.
 *
 * crustymidi is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * crustymidi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with crustymidi.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stddef.h>

#ifdef CRUSTY_TEST
#include <stdarg.h>
#endif

#include "crustyvm.h"

#define MAX_SYMBOL_LEN (32)
#define MACRO_STACK_SIZE (32)
#define MAX_PASSES (16)
#define MAX_INCLUDE_DEPTH (16)
#define DEFAULT_CALLSTACK_SIZE (256)

#define ALIGNMENT (sizeof(int))

#define LOG_PRINTF(CVM, FMT, ...) \
    (CVM)->log_cb((CVM)->log_priv, "%s: " FMT, (CVM)->stage, ##__VA_ARGS__)
#define LOG_PRINTF_LINE(CVM, FMT, ...) \
    (CVM)->log_cb((CVM)->log_priv, "%s:%s:%u: " FMT, \
        (CVM)->stage, \
        (CVM)->line[(CVM)->logline].module, \
        (CVM)->line[(CVM)->logline].line, \
        ##__VA_ARGS__)
#define LOG_PRINTF_BARE(CVM, FMT, ...) \
    (CVM)->log_cb((CVM)->log_priv, FMT, ##__VA_ARGS__)

typedef struct {
    unsigned int ip;
    unsigned int proc;
} CrustyCallStackArg;

typedef struct {
    unsigned int flags;
    unsigned int val;
    unsigned int index;
    unsigned int ptr;
} CrustyStackArg;

typedef struct {
    unsigned long *offset;
    char **token;
    unsigned int tokencount;

    long moduleOffset;
    const char *module;
    unsigned int line;

    unsigned int instruction;
} CrustyLine;

typedef struct CrustyProcedure_s CrustyProcedure;

typedef struct {
    long nameOffset;
    const char *name;
    CrustyType type;
    struct CrustyProcedure_s *proc;
    unsigned int length; /* array length
                            0  if reference (local argument)
                            1  if a single value
                            >1 if array */
    unsigned int offset; /* offset in to stack
                            position of value in local stack (from stack pointer) if local
                            position of reference in local stack if reference
                            position in global stack (from 0) if global */
    /* runtime and function call initializers, only 1 must be not-NULL */
    char *chrinit;
    int *intinit;
    double *floatinit;

    /* callbacks for IO, both NULL if not IO */
    CRUSTY_IO_READ_FUNC_DECL(read);
    void *readpriv;
    CRUSTY_IO_WRITE_FUNC_DECL(write);
    void *writepriv;
} CrustyVariable;

typedef struct {
    long nameOffset;
    const char *name;
    unsigned int line;
} CrustyLabel;

typedef struct CrustyProcedure_s {
    long nameOffset;
    const char *name;
    unsigned int start;
    unsigned int length;
    unsigned int args;
    unsigned int stackneeded;
    unsigned int instruction;

    int *varIndex;
    CrustyVariable **var;
    unsigned int vars;

    CrustyLabel *label;
    unsigned int labels;
} CrustyProcedure;

typedef enum {
    CRUSTY_INSTRUCTION_TYPE_MOVE,
    CRUSTY_INSTRUCTION_TYPE_ADD,
    CRUSTY_INSTRUCTION_TYPE_SUB,
    CRUSTY_INSTRUCTION_TYPE_MUL,
    CRUSTY_INSTRUCTION_TYPE_DIV,
    CRUSTY_INSTRUCTION_TYPE_AND,
    CRUSTY_INSTRUCTION_TYPE_OR,
    CRUSTY_INSTRUCTION_TYPE_XOR,
    CRUSTY_INSTRUCTION_TYPE_SHR,
    CRUSTY_INSTRUCTION_TYPE_SHL,
    CRUSTY_INSTRUCTION_TYPE_CMP,
    CRUSTY_INSTRUCTION_TYPE_JUMP,
    CRUSTY_INSTRUCTION_TYPE_JUMPN,
    CRUSTY_INSTRUCTION_TYPE_JUMPZ,
    CRUSTY_INSTRUCTION_TYPE_JUMPL,
    CRUSTY_INSTRUCTION_TYPE_JUMPG,
    CRUSTY_INSTRUCTION_TYPE_CALL,
    CRUSTY_INSTRUCTION_TYPE_RET
} CrustyInstructionType;

#define MOVE_DEST_FLAGS (1)
#define MOVE_DEST_VAL   (2)
#define MOVE_DEST_INDEX (3)
#define MOVE_SRC_FLAGS  (4)
#define MOVE_SRC_VAL    (5)
#define MOVE_SRC_INDEX  (6)
#define MOVE_ARGS MOVE_SRC_INDEX

#define MOVE_FLAG_TYPE_MASK (3)
#define MOVE_FLAG_IMMEDIATE (0)
#define MOVE_FLAG_VAR       (1)
#define MOVE_FLAG_LENGTH    (2)
#define MOVE_FLAG_INVALID   (3)

#define MOVE_FLAG_INDEX_TYPE_MASK (1 << 2)
#define MOVE_FLAG_INDEX_IMMEDIATE (0 << 2)
#define MOVE_FLAG_INDEX_VAR (1 << 2)

#define JUMP_LOCATION (1)
#define JUMP_ARGS JUMP_LOCATION

#define CALL_PROCEDURE (1)
#define CALL_START_ARGS (2)
#define CALL_ARG_FLAGS (0)
#define CALL_ARG_VAL (1)
#define CALL_ARG_INDEX (2)
#define CALL_ARG_SIZE (CALL_ARG_INDEX + 1)

#define RET_ARGS (0)

typedef struct CrustyVM_s {
    void (*log_cb)(void *priv, const char *fmt, ...);
    void *log_priv;

    unsigned int flags;

/* compile-time data */

/* logging things */
    unsigned int logline;
    const char *stage;

    CrustyLine *line;
    unsigned int lines;

    char *tokenmem;
    int tokenmemlen;

    CrustyVariable *var;
    unsigned int vars;

    CrustyProcedure *proc;
    unsigned int procs;

    int *inst;
    unsigned int insts;

    unsigned int stacksize;
    unsigned int initialstack;

    unsigned int callstacksize;

    /* runtime data */
    char *stack; /* runtime stack */
    CrustyCallStackArg *cstack; /* call stack */
    unsigned int sp; /* stack pointer */
    unsigned int csp; /* callstack pointer */
    unsigned int ip; /* instruction pointer */
    /* result of last operation, for conditional jumps */
    CrustyType resulttype;
    double floatresult;
    int intresult;
    CrustyStatus status;
} CrustyVM;

/* compile-time stuff */

typedef struct {
    long nameOffset;
    unsigned int start;

    long *argOffset;
    unsigned int argcount;
} CrustyMacro;

typedef enum {
    CRUSTY_EXPR_NUMBER,
    CRUSTY_EXPR_LPAREN,
    CRUSTY_EXPR_RPAREN,
    CRUSTY_EXPR_PLUS,
    CRUSTY_EXPR_MINUS,
    CRUSTY_EXPR_MULTIPLY,
    CRUSTY_EXPR_DIVIDE,
    CRUSTY_EXPR_MODULO,
    CRUSTY_EXPR_EQUALS,
    CRUSTY_EXPR_NEQUALS,
    CRUSTY_EXPR_LESS,
    CRUSTY_EXPR_GREATER,
    CRUSTY_EXPR_LEQUALS,
    CRUSTY_EXPR_GEQUALS,
    CRUSTY_EXPR_AND,
    CRUSTY_EXPR_OR,
    CRUSTY_EXPR_XOR,
    CRUSTY_EXPR_NAND,
    CRUSTY_EXPR_NOR,
    CRUSTY_EXPR_XNOR,
    CRUSTY_EXPR_LSHIFT,
    CRUSTY_EXPR_RSHIFT
} CrustyExprOp;

typedef struct CrustyExpr_s {
    CrustyExprOp op;

    int number;

    struct CrustyExpr_s *prev;
    struct CrustyExpr_s *next;
} CrustyExpr;

const char *CRUSTY_STATUSES[] = {
    "Ready",
    "Active",
    "Internal error/VM bug",
    "Array access out of range",
    "Invalid instruction",
    "Stack overflow",
    "Callback returned failure",
    "Float used as index",
    "Invalid status code"
};

static CrustyVM *init() {
    CrustyVM *cvm;

    cvm = malloc(sizeof(CrustyVM));
    if(cvm == NULL) {
        return(NULL);
    }

    cvm->log_cb = NULL;
    cvm->log_priv = NULL;
    cvm->stage = NULL;
    cvm->line = NULL;
    cvm->lines = 0;
    cvm->tokenmem = NULL;
    cvm->tokenmemlen = 0;
    cvm->var = NULL;
    cvm->vars = 0;
    cvm->proc = NULL;
    cvm->procs = 0;
    cvm->inst = NULL;
    cvm->insts = 0;
    cvm->stack = NULL;
    cvm->cstack = NULL;

    return(cvm);
}

void crustyvm_free(CrustyVM *cvm) {
    unsigned int i;

    if(cvm->line != NULL) {
        for(i = 0; i < cvm->lines; i++) {
            if(cvm->line[i].offset != NULL) {
                free(cvm->line[i].offset);
            }
            if(cvm->line[i].token != NULL) {
                free(cvm->line[i].token);
            }
        }
        free(cvm->line);
    }

    if(cvm->tokenmem != NULL) {
        free(cvm->tokenmem);
    }

    if(cvm->proc != NULL) {
        for(i = 0; i < cvm->procs; i++) {
            if(cvm->proc[i].varIndex != NULL) {
                free(cvm->proc[i].varIndex);
            }
            if(cvm->proc[i].var != NULL) {
                free(cvm->proc[i].var);
            }
            if(cvm->proc[i].label != NULL) {
                free(cvm->proc[i].label);
            }
        }
        free(cvm->proc);
    }

    if(cvm->var != NULL) {
        for(i = 0; i < cvm->vars; i++) {
            if(cvm->var[i].chrinit != NULL) {
                free(cvm->var[i].chrinit);
            }
            if(cvm->var[i].intinit != NULL) {
                free(cvm->var[i].intinit);
            }
            if(cvm->var[i].floatinit != NULL) {
                free(cvm->var[i].floatinit);
            }
        }
        free(cvm->var);
    }

    if(cvm->inst != NULL) {
        free(cvm->inst);
    }

    if(cvm->stack != NULL) {
        free(cvm->stack);
    }

    if(cvm->cstack != NULL) {
        free(cvm->cstack);
    }

    free(cvm);
}

static long add_token(CrustyVM *cvm,
                     const char *token,
                     unsigned long len,
                     int quoted,
                     unsigned int *line) {
    char *temp;
    unsigned long oldlen;
    unsigned long srcpos, destpos;

    /* existing length + token + null terminator.  Resulting quoted strings can
       only ever be equal to or shorter than the input length. */
    temp = realloc(cvm->tokenmem, cvm->tokenmemlen + len + 1);
    if(temp == NULL) {
        return(-1);
    }
    cvm->tokenmem = temp;
    temp = &(cvm->tokenmem[cvm->tokenmemlen]);

    if(token == NULL) { /* just allocate the space and return it */
        temp[len] = '\0';
        oldlen = cvm->tokenmemlen;
        cvm->tokenmemlen += len + 1;
    } else {
        if(quoted) { /* much slower method and uncommonly used */
            srcpos = 0;
            destpos = 0;
            for(srcpos = 0; srcpos < len; srcpos++) {
                if(token[srcpos] == '\\') {
                    if(srcpos + 1 == len) {
                        LOG_PRINTF(cvm, "Lone escape char at end of string.\n");
                        return(-1);
                    }
                    srcpos++;
                    switch(token[srcpos]) {
                        case 'r':
                            temp[destpos] = '\r';
                            destpos++;
                            break;
                        case 'n':
                            temp[destpos] = '\n';
                            destpos++;
                            break;
                        case '\n':
                            /* escape a newline to eliminate it */
                            /* including multichar newlines */
                            if(srcpos + 1 < len &&
                               token[srcpos + 1] == '\r') {
                                srcpos++;
                            }
                            /* increment line count passed in */
                            (*line)++;
                            break;
                        case '\r':
                            /* same as above */
                            if(srcpos + 1 < len &&
                               token[srcpos + 1] == '\n') {
                                srcpos++;
                            }
                            break;
                        case '\\':
                            temp[destpos] = '\\';
                            destpos++;
                            break;
                        default:
                            LOG_PRINTF(cvm, "Invalid escape sequence: \\%c.\n", token[srcpos]);
                            return(-1);
                    }
                } else {
                    if(token[srcpos] == '\n') {
                        /* same */
                        (*line)++;
                    }
                    temp[destpos] = token[srcpos];
                    destpos++;
                }
            }
            if(destpos < len) {
                temp = realloc(cvm->tokenmem, cvm->tokenmemlen + destpos + 1);
                if(temp == NULL) {
                    LOG_PRINTF(cvm, "Failed to trim tokenmem.\n");
                    return(-1);
                }
                cvm->tokenmem = temp;
            }
            cvm->tokenmem[cvm->tokenmemlen + destpos] = '\0';
            oldlen = cvm->tokenmemlen;
            cvm->tokenmemlen += destpos + 1;
        } else {
            memcpy(temp, token, len);
            temp[len] = '\0';
            oldlen = cvm->tokenmemlen;
            cvm->tokenmemlen += len + 1;
        }
    }

    return((long)oldlen);
}

#define ISJUNK(X) ((X) == ' ' || \
                   (X) == '\t' || \
                   (X) == '\r' || \
                   (X) == '\n' || \
                   (X) == ';')

#define PROGRAM (includestack[includestackptr])
#define LEN (includesize[includestackptr])
#define MODULE (includemodule[includestackptr])
#define LINE (includeline[includestackptr])
#define POS (includepos[includestackptr])

/* this is before a bunch of stuff is guaranteed to be set up, so just fetch
   the same information from local state */
#define LOG_PRINTF_TOK(CVM, FMT, ...) \
    (CVM)->log_cb((CVM)->log_priv, "%s:%s:%u: " FMT, \
        (CVM)->stage, \
        &((CVM)->tokenmem[MODULE]), \
        LINE, \
        ##__VA_ARGS__)

#define GET_TOKEN(LINE, TOKEN) (&(cvm->tokenmem[cvm->line[LINE].offset[TOKEN]]))

static int tokenize(CrustyVM *cvm,
                    const char *modulename,
                    const char *programdata,
                    unsigned long programdatalen) {
    unsigned int i;
    CrustyLine *temp;
    unsigned long linelen, lineend;
    unsigned long cursor;
    int scanningjunk;
    int quotedstring;
    long tokenstart;
    unsigned int linesmem;
    long filelen;

    /* data to read from */
    const char *includestack[MAX_INCLUDE_DEPTH];
    /* length of module */
    unsigned long includesize[MAX_INCLUDE_DEPTH];
    /* name of module */
    long includemodule[MAX_INCLUDE_DEPTH];
    /* line within current module, for line metadata */
    unsigned int includeline[MAX_INCLUDE_DEPTH];
    /* byte in current module */
    unsigned long includepos[MAX_INCLUDE_DEPTH];
    unsigned int includestackptr = 0;

    PROGRAM = programdata;
    LEN = programdatalen;
    tokenstart = add_token(cvm, modulename, strlen(modulename), 0, NULL);
    if(tokenstart < 0) {
        LOG_PRINTF(cvm, "Failed to allocate memory for module name.\n");
        return(-1);
    }
    MODULE = tokenstart;
    LINE = 0;
    POS = 0;

    cvm->lines = 0; /* current line */
    linesmem = 0; /* actual size of array */
    for(;;) {
        LINE++;
        /* find the end of meaningful line contents and total size of line up to
           the start of the next line */
        lineend = 0;
        for(linelen = 0; linelen < LEN - POS; linelen++) {
            if(PROGRAM[POS + linelen] == '\r') {
                linelen++;
                /* mark this character as the end of the line, unless a comment
                   was previously found, then that is the real line end, so
                   don't overwrite it */
                if(lineend == 0 && PROGRAM[POS] != ';') {
                    lineend = linelen;
                }
                if(POS + linelen < LEN - 1 &&
                   PROGRAM[POS + linelen] == '\n') {
                    linelen++;
                }
                break;
            } else if(PROGRAM[POS + linelen] == '\n') {
                linelen++;
                /* same as above */
                if(lineend == 0 && PROGRAM[POS] != ';') {
                    lineend = linelen;
                }
                if(POS + linelen < LEN - 1 &&
                   PROGRAM[POS + linelen] == '\r') {
                    linelen++;
                }
                break;
            } else if(PROGRAM[POS + linelen] == '"') {
                /* allow quoted strings to span lines by scanning until the next
                   quote (or end of file) is found */

                /* ignore quoted strings in comments */
                if(lineend == 0) {
                    if(POS + linelen < LEN - 1 &&
                       (PROGRAM[POS + linelen] == '\r' ||
                        PROGRAM[POS + linelen] == '\n')) {
                        LOG_PRINTF_TOK(cvm, "Quoted string opened at end of line.\n");
                        return(-1);
                    }
                    linelen++;
                    while(linelen < LEN - POS) {
                        linelen++;
                        if(PROGRAM[POS + linelen] == '"') {
                            break;
                        }
                    }
                }
            } else if(PROGRAM[POS + linelen] == ';') { /* comments */
                /* only count the first found comment */
                if(lineend == 0) {
                    lineend = linelen;
                }
            }
        }
        /* scanning reached the end of file without hitting a newline, so the
           line length is just the rest of the file */
        if(POS + linelen == LEN) {
            lineend = linelen;
        }

        /* allocate memory for a new line if needed */
        if(cvm->lines + 1 > linesmem) {
            temp = realloc(cvm->line, sizeof(CrustyLine) * (linesmem + 1));
            if(temp == NULL) {
                LOG_PRINTF_TOK(cvm, "Failed to allocate memory for lines list.\n");
                return(-1);
            }
            cvm->line = temp;
            linesmem++;
        }

        cvm->line[cvm->lines].tokencount = 0;
        cvm->line[cvm->lines].offset = NULL;
        cvm->line[cvm->lines].moduleOffset = MODULE;
        cvm->line[cvm->lines].line = LINE;

        /* find starts and ends of tokens and insert them in to the line entry

           assume we'll start with junk so if there is no junk at the start
           of the line, the first token will be marked at 0 */
        scanningjunk = 1;
        quotedstring = 0;
        for(cursor = 0; cursor < lineend; cursor++) {
            if(!quotedstring) {
                if(scanningjunk) {
                    /* if we're scanning for junk and there's still junk,
                       nothing more to do. */
                    if(ISJUNK(PROGRAM[POS + cursor])) {
                        continue;
                    }

                    /* no longer junk, so create a space for the offset in to
                       token memory. */
                    cvm->line[cvm->lines].offset =
                        realloc(cvm->line[cvm->lines].offset,
                                sizeof(unsigned long) *
                                (cvm->line[cvm->lines].tokencount + 1));
                    if(cvm->line[cvm->lines].offset == NULL) {
                        LOG_PRINTF_TOK(cvm, "Couldn't allocate memory for offsets.\n");
                        return(-1);
                    }

                    /* check if at the start of a quoted string */
                    if(PROGRAM[POS + cursor] == '"') {
                        /* point the start to the next character, which will
                           for sure exist because a quote at the end of the line
                           is previously checked for */
                        cursor++;
                        tokenstart = POS + cursor;
                        quotedstring = 1;
                        /* don't reset junk scanning because there could be junk
                           directly following a quoted string */
                        continue;
                    }

                    /* start scanning non-junk */
                    tokenstart = POS + cursor;
                    scanningjunk = 0;

                    continue;
                }

                /* if junk wasn't found, continue scanning */
                if(!ISJUNK(PROGRAM[POS + cursor])) {
                    continue;
                }

                /* transition from not junk to junk by adding the token to token
                   memory, then pointing the current offset to the token */
                tokenstart = add_token(cvm,
                                       &(PROGRAM[tokenstart]),
                                       POS + cursor - tokenstart,
                                       0,
                                       NULL);
                if(tokenstart < 0) {
                    LOG_PRINTF_TOK(cvm, "Couldn't allocate memory for token.\n");
                    return(-1);
                }
                cvm->line[cvm->lines].offset[cvm->line[cvm->lines].tokencount] =
                    tokenstart;
                cvm->line[cvm->lines].tokencount++;

                scanningjunk = 1;
            } else {
                /* this check is safe because cursor will have been incremented
                   at least once to get to this point. */
                if(PROGRAM[POS + cursor] == '"') {
                    /* transition from quoted string to junk. Same as above. */
                    tokenstart = add_token(cvm,
                                           &(PROGRAM[tokenstart]),
                                           POS + cursor - tokenstart,
                                           1, /* quoted */
                                           &(LINE));
                    if(tokenstart < 0) {
                        LOG_PRINTF_TOK(cvm, "Couldn't allocate memory for token.\n");
                        return(-1);
                    }
                    cvm->line[cvm->lines].offset[cvm->line[cvm->lines].tokencount] =
                        tokenstart;
                    cvm->line[cvm->lines].tokencount++;

                    scanningjunk = 1;
                    quotedstring = 0;
                }
            }
        }

        /* check for includes */
        if(cvm->line[cvm->lines].tokencount > 0) {
            if(strcmp(GET_TOKEN(cvm->lines, 0), "include") == 0) {
                if(cvm->line[cvm->lines].tokencount != 2) {
                    LOG_PRINTF_TOK(cvm, "include takes a single filename");
                    return(-1);
                }

                if(includestackptr == MAX_INCLUDE_DEPTH) {
                    LOG_PRINTF_TOK(cvm, "Includes too deep.\n");
                    return(-1);
                }

                /* make sure the same file isn't included from cyclicly */
                for(i = 0; i < includestackptr; i++) {
                    if(strcmp(GET_TOKEN(cvm->lines, 1), includestack[i]) == 0) {
                        LOG_PRINTF_TOK(cvm, "Circular includes.\n");
                        return(-1);
                    }
                }

                /* load the file in to program memory.  Can't use any of the
                   convenience macros though so log messages point to the right
                   line/module */
                FILE *in;
                in = fopen(GET_TOKEN(cvm->lines, 1), "rb");
                if(in == NULL) {
                    LOG_PRINTF_TOK(cvm, "Failed to open include file %s.\n",
                                   GET_TOKEN(cvm->lines, 1));
                    return(-1);
                }

                if(fseek(in, 0, SEEK_END) < 0) {
                    LOG_PRINTF_TOK(cvm, "Failed to seek include file.\n");
                    return(-1);
                }

                filelen = ftell(in);
                if(filelen < 0) {
                    LOG_PRINTF_TOK(cvm, "Failed to get include file size.\n");
                    return(-1);
                }
                includesize[includestackptr+1] = (unsigned long)filelen;

                includestack[includestackptr+1] = malloc(includesize[includestackptr+1]);
                if(includestack[includestackptr+1] == NULL) {
                    LOG_PRINTF_TOK(cvm, "Failed to allocate memory for include.\n");
                    return(-1);
                }

                /* read the contents in to memory */
                rewind(in);
                /* needs to be made non-const so this buffer can be read in to,
                   but the array is of const char ** because the 0th entry is
                   always the const char passed in to the function, which is
                   never modified. */
                if(fread((char *)(includestack[includestackptr+1]),
                         1,
                         includesize[includestackptr+1],
                         in) < includesize[includestackptr+1]) {
                    LOG_PRINTF_TOK(cvm, "Failed to read include file.\n");
                    return(-1);
                }
                fclose(in);

                /* go past the include line */
                POS += linelen;

                /* add the module name */
                includestackptr++;
                MODULE = cvm->line[cvm->lines].offset[1];
                LINE = 0;
                POS = 0;

                /* we're done with this line and it won't end up in the line
                   list so free its offsets and it will be reused, tokencount
                   will be reset later */
                free(cvm->lines[cvm->line].offset);
                cvm->lines[cvm->line].offset = NULL;

                /* don't advance line count */
                continue;
            } else { /* no include, so just advance things normal */
                POS += linelen;
                cvm->lines++;
            }
        } else { /* don't have lines increment if it's a blank line */
            /* no need to free offset because no token was ever found */
            POS += linelen;
        }

        /* reached the end, so pop it off, if already at the bottom, tokenizing
           is done */
        if(POS == LEN) {
            if(includestackptr == 0) {
                break;
            }
            /* see comment above before fread() */
            free((char *)PROGRAM);
            includestackptr--;
        }
    }

    return(0);
}

#undef LOG_PRINTF_TOK
#undef LINE
#undef MODULE
#undef LEN
#undef PROGRAM
#undef ISJUNK

/* next block of functions related to preprocessing also need a special version
   of LOG_PRINTF_LINE as well by fetching module names from the token memory
   offset because the char pointers aren't set up in cvm->line yet. */
#define LOG_PRINTF_TOK(CVM, FMT, ...) \
    (CVM)->log_cb((CVM)->log_priv, "%s:%s:%u: " FMT, \
        (CVM)->stage, \
        &((CVM)->tokenmem[(CVM)->line[(CVM)->logline].moduleOffset]), \
        (CVM)->line[(CVM)->logline].line, \
        ##__VA_ARGS__)

static CrustyMacro *find_macro(CrustyVM *cvm,
                               CrustyMacro *macro,
                               unsigned int count,
                               const char *name) {
    unsigned int i;

    for(i = 0; i < count; i++) {
        if(strcmp(&(cvm->tokenmem[macro[i].nameOffset]), name) == 0) {
            return(&(macro[i]));
        }
    }

    return(NULL);
}

static long string_replace(CrustyVM *cvm,
                            long tokenOffset,
                            long macroOffset,
                            long replaceOffset) {
    char *token = &(cvm->tokenmem[tokenOffset]);
    char *macro = &(cvm->tokenmem[macroOffset]);
    char *replace = &(cvm->tokenmem[replaceOffset]);
    int macrofound = 0;
    char *macroInToken;
    int tokenlen = strlen(token);
    int macrolen = strlen(macro);
    int replacelen = strlen(replace);
    int betweenlen;
    char *temp;
    int newlen;
    int i;
    int srcpos, dstpos;
    long tokenstart;

    /* scan the token to find the number of instances of the macro */
    macroInToken = strstr(token, macro);
    while(macroInToken != NULL) {
        macrofound++;
        /* crusty pointer arithmetic that might not be
           safe to make sure there are more characters
           to search within or if the last result is
           at the end of the string anyway. */
        if(tokenlen - (macroInToken - token) > macrolen) {
            /* start checking 1 character in so the same
               string isn't found again, safe because
               there is at least an additional character
               after this */
            macroInToken = strstr(macroInToken + 1, macro);
        } else { /* already at end of token */
            break;
        }
    }

    /* if nothing, just return the token so it can be assigned to whatever
       needed a potentially processed token */
    if(macrofound == 0) {
        return(tokenOffset);
    }

    /* token take away the macros, add replacements */
    newlen = tokenlen - (macrolen * macrofound) + (strlen(replace) * macrofound);

    /* will return an index in to a null terminated buffer long enough for newlen */
    tokenstart = add_token(cvm, NULL, newlen, 0, NULL);
    if(tokenstart < 0) {
        LOG_PRINTF(cvm, "Failed to add string replace memory to extra memory.\n");
        return(-1);
    }
    temp = &(cvm->tokenmem[tokenstart]);
    /* update these because they may have moved */
    token = &(cvm->tokenmem[tokenOffset]);
    macro = &(cvm->tokenmem[macroOffset]);
    replace = &(cvm->tokenmem[replaceOffset]);

    /* alternate scanning for macros, copying from the token in to the
       destination up until the found macro, then copy the replacement in to the
       destination instead of the macro. */
    srcpos = 0;
    dstpos = 0;
    for(i = 0; i < macrofound; i++) {
        /* find macro */
        macroInToken = strstr(&(token[srcpos]), macro);
        /* more funky pointer arithmetic */
        betweenlen = macroInToken - &(token[srcpos]);
        /* copy from last found macro (or beginning) up until the beginning of
           the found macro */
        memcpy(&(temp[dstpos]), &(token[srcpos]), betweenlen);
        dstpos += betweenlen;
        srcpos += betweenlen;
        /* copy the replacement */
        memcpy(&(temp[dstpos]), replace, replacelen);
        dstpos += replacelen;
        srcpos += macrolen;
    }

    /* see if there's anything after the last macro replacement to copy over */
    memcpy(&(temp[dstpos]), &(token[srcpos]), newlen - dstpos);

    return(tokenstart);
}

/* I wrote this kinda crappily, and when adding a bunch more operators it got
   extra crappy.  I probably won't rewrite it because i'd have to basically
   either figure out what I did or just rewrite it from scratch to be less silly
   but it probably doesn't really matter.  It won't be noticeably slow for any
   program written in this, and it doesn't affect runtime speed whatsoever. */
static CrustyExpr *do_expression(CrustyVM *cvm, CrustyExpr *expr) {
    CrustyExpr *cursor;
    CrustyExpr *innercursor;
    CrustyExpr *eval;
    int level;

    /* scan for parentheses, this function assumes matched parentheses */
    cursor = expr;
    while(cursor != NULL) {
        if(cursor->op == CRUSTY_EXPR_LPAREN) {
            level = 1;
            /* this won't crash because a previous test determined that every
               left parenthesis will always have a matching right parenthesis */
            innercursor = cursor;
            while(level > 0) {
                innercursor = innercursor->next;
                if(innercursor->op == CRUSTY_EXPR_LPAREN) {
                    level++;
                } else if(innercursor->op == CRUSTY_EXPR_RPAREN) {
                    level--;
                }
            }
            if(cursor->next == innercursor) {
                LOG_PRINTF_TOK(cvm, "Empty parentheses in evaluation.\n");
                return(NULL);
            }
            /* evaluate the inner expression in isolation */
            innercursor->prev->next = NULL;
            cursor->next->prev = NULL;
            eval = do_expression(cvm, cursor->next);
            if(eval == NULL) {
                /* don't log this so we don't get repeated reports of evaluation
                   failing all the way down the stack. */
                return(NULL);
            }
            /* at this point, the entire expression within the parentheses 
               should be collapsed in to a single value.  Make eval point to the
               items before the left parenthesis and after the right parenthesis
               and if there's anything actually there, make sure those point to
               eval, effectively cutting out the parentheses and any expression
               inside and inserting just the single value which the inner
               expression evaluated to. */
            eval->prev = cursor->prev;
            if(eval->prev != NULL) {
                eval->prev->next = eval;
            } else { /* cursor was a leading parenthesis, so make the start of
                        expression point to this value instead */
                expr = eval;
            }
            eval->next = innercursor->next;
            if(eval->next != NULL) {
                eval->next->prev = eval;
            } /* nothing to do because there's no tail pointer */

            cursor = eval;
        }

        cursor = cursor->next;
    }

    /* at this point, there should be no more parentheses, if there were any in
       the first place and eval should start with a number, alternate between an
       operation and a number until terminating on a number */

    /* operation precedence based on C, described from:
       https://en.cppreference.com/w/c/language/operator_precedence */

    /* multiplication and division */
    cursor = expr;
    while(cursor != NULL) {
        if(cursor->op == CRUSTY_EXPR_MULTIPLY ||
           cursor->op == CRUSTY_EXPR_DIVIDE ||
           cursor->op == CRUSTY_EXPR_MODULO) {
            if(cursor->prev == NULL) {
                LOG_PRINTF_TOK(cvm, "Operator with nothing before.\n");
                return(NULL);
            }
            if(cursor->prev->op != CRUSTY_EXPR_NUMBER) {
                LOG_PRINTF_TOK(cvm, "Operator with not a number before.\n");
                return(NULL);
            }
            if(cursor->next == NULL) {
                LOG_PRINTF_TOK(cvm, "Operator with nothing after.\n");
                return(NULL);
            }
            if(cursor->next->op != CRUSTY_EXPR_NUMBER) {
                LOG_PRINTF_TOK(cvm, "Operator with not a number after.\n");
                return(NULL);
            }

            if(cursor->op == CRUSTY_EXPR_MULTIPLY) {
                cursor->prev->number = cursor->prev->number * cursor->next->number;
            } else if(cursor->op == CRUSTY_EXPR_DIVIDE) {
                cursor->prev->number = cursor->prev->number / cursor->next->number;
            } else {
                cursor->prev->number = cursor->prev->number % cursor->next->number;
            }

            /* the operator and second operand are unimportant now so move the
               context to the result value and repoint it to the value following
               the second operand */
            cursor = cursor->prev;
            cursor->next = cursor->next->next->next;
            if(cursor->next != NULL) {
                cursor->next->prev = cursor;
            }
        }
        cursor = cursor->next;
    }

    /* addition and subtraction */
    cursor = expr;
    while(cursor != NULL) {
        if(cursor->op == CRUSTY_EXPR_PLUS ||
           cursor->op == CRUSTY_EXPR_MINUS) {
            if(cursor->prev == NULL) {
                LOG_PRINTF_TOK(cvm, "Operator with nothing before.\n");
                return(NULL);
            }
            if(cursor->prev->op != CRUSTY_EXPR_NUMBER) {
                LOG_PRINTF_TOK(cvm, "Operator with not a number before.\n");
                return(NULL);
            }
            if(cursor->next == NULL) {
                LOG_PRINTF_TOK(cvm, "Operator with nothing after.\n");
                return(NULL);
            }
            if(cursor->next->op != CRUSTY_EXPR_NUMBER) {
                LOG_PRINTF_TOK(cvm, "Operator with not a number after.\n");
                return(NULL);
            }

            if(cursor->op == CRUSTY_EXPR_PLUS) {
                cursor->prev->number = cursor->prev->number + cursor->next->number;
            } else {
                cursor->prev->number = cursor->prev->number - cursor->next->number;
            }

            /* the operator and second operand are unimportant now so move the
               context to the result value and repoint it to the value following
               the second operand */
            cursor = cursor->prev;
            cursor->next = cursor->next->next->next;
            if(cursor->next != NULL) {
                cursor->next->prev = cursor;
            }
        }
        cursor = cursor->next;
    }

    /* bit shift */
    cursor = expr;
    while(cursor != NULL) {
        if(cursor->op == CRUSTY_EXPR_LSHIFT ||
           cursor->op == CRUSTY_EXPR_RSHIFT) {
            if(cursor->prev == NULL) {
                LOG_PRINTF_TOK(cvm, "Operator with nothing before.\n");
                return(NULL);
            }
            if(cursor->prev->op != CRUSTY_EXPR_NUMBER) {
                LOG_PRINTF_TOK(cvm, "Operator with not a number before.\n");
                return(NULL);
            }
            if(cursor->next == NULL) {
                LOG_PRINTF_TOK(cvm, "Operator with nothing after.\n");
                return(NULL);
            }
            if(cursor->next->op != CRUSTY_EXPR_NUMBER) {
                LOG_PRINTF_TOK(cvm, "Operator with not a number after.\n");
                return(NULL);
            }

            if(cursor->op == CRUSTY_EXPR_LSHIFT) {
                cursor->prev->number = cursor->prev->number << cursor->next->number;
            } else {
                cursor->prev->number = cursor->prev->number >> cursor->next->number;
            }

            /* the operator and second operand are unimportant now so move the
               context to the result value and repoint it to the value following
               the second operand */
            cursor = cursor->prev;
            cursor->next = cursor->next->next->next;
            if(cursor->next != NULL) {
                cursor->next->prev = cursor;
            }
        }
        cursor = cursor->next;
    }

    /* less than (or equal) and greater than (or equal) */
    cursor = expr;
    while(cursor != NULL) {
        if(cursor->op == CRUSTY_EXPR_LESS ||
           cursor->op == CRUSTY_EXPR_LEQUALS ||
           cursor->op == CRUSTY_EXPR_GREATER ||
           cursor->op == CRUSTY_EXPR_GEQUALS) {
            if(cursor->prev == NULL) {
                LOG_PRINTF_TOK(cvm, "Operator with nothing before.\n");
                return(NULL);
            }
            if(cursor->prev->op != CRUSTY_EXPR_NUMBER) {
                LOG_PRINTF_TOK(cvm, "Operator with not a number before.\n");
                return(NULL);
            }
            if(cursor->next == NULL) {
                LOG_PRINTF_TOK(cvm, "Operator with nothing after.\n");
                return(NULL);
            }
            if(cursor->next->op != CRUSTY_EXPR_NUMBER) {
                LOG_PRINTF_TOK(cvm, "Operator with not a number after.\n");
                return(NULL);
            }

            if(cursor->op == CRUSTY_EXPR_LESS) {
                cursor->prev->number = (cursor->prev->number < cursor->next->number);
            } else if(cursor->op == CRUSTY_EXPR_LEQUALS) {
                cursor->prev->number = (cursor->prev->number <= cursor->next->number);
            } else if(cursor->op == CRUSTY_EXPR_GREATER) {
                cursor->prev->number = (cursor->prev->number > cursor->next->number);
            } else {
                cursor->prev->number = (cursor->prev->number >= cursor->next->number);
            }

            /* the operator and second operand are unimportant now so move the
               context to the result value and repoint it to the value following
               the second operand */
            cursor = cursor->prev;
            cursor->next = cursor->next->next->next;
            if(cursor->next != NULL) {
                cursor->next->prev = cursor;
            }
        }
        cursor = cursor->next;
    }

    /* equal to and not equal to */
    cursor = expr;
    while(cursor != NULL) {
        if(cursor->op == CRUSTY_EXPR_EQUALS ||
           cursor->op == CRUSTY_EXPR_NEQUALS) {
            if(cursor->prev == NULL) {
                LOG_PRINTF_TOK(cvm, "Operator with nothing before.\n");
                return(NULL);
            }
            if(cursor->prev->op != CRUSTY_EXPR_NUMBER) {
                LOG_PRINTF_TOK(cvm, "Operator with not a number before.\n");
                return(NULL);
            }
            if(cursor->next == NULL) {
                LOG_PRINTF_TOK(cvm, "Operator with nothing after.\n");
                return(NULL);
            }
            if(cursor->next->op != CRUSTY_EXPR_NUMBER) {
                LOG_PRINTF_TOK(cvm, "Operator with not a number after.\n");
                return(NULL);
            }

            if(cursor->op == CRUSTY_EXPR_EQUALS) {
                cursor->prev->number = (cursor->prev->number == cursor->next->number);
            } else {
                cursor->prev->number = (cursor->prev->number != cursor->next->number);
            }

            /* the operator and second operand are unimportant now so move the
               context to the result value and repoint it to the value following
               the second operand */
            cursor = cursor->prev;
            cursor->next = cursor->next->next->next;
            if(cursor->next != NULL) {
                cursor->next->prev = cursor;
            }
        }
        cursor = cursor->next;
    }

    /* and and nand */
    cursor = expr;
    while(cursor != NULL) {
        if(cursor->op == CRUSTY_EXPR_AND ||
           cursor->op == CRUSTY_EXPR_NAND) {
            if(cursor->prev == NULL) {
                LOG_PRINTF_TOK(cvm, "Operator with nothing before.\n");
                return(NULL);
            }
            if(cursor->prev->op != CRUSTY_EXPR_NUMBER) {
                LOG_PRINTF_TOK(cvm, "Operator with not a number before.\n");
                return(NULL);
            }
            if(cursor->next == NULL) {
                LOG_PRINTF_TOK(cvm, "Operator with nothing after.\n");
                return(NULL);
            }
            if(cursor->next->op != CRUSTY_EXPR_NUMBER) {
                LOG_PRINTF_TOK(cvm, "Operator with not a number after.\n");
                return(NULL);
            }

            if(cursor->op == CRUSTY_EXPR_AND) {
                cursor->prev->number = cursor->prev->number & cursor->next->number;
            } else {
                cursor->prev->number = ~(cursor->prev->number & cursor->next->number);
            }

            /* the operator and second operand are unimportant now so move the
               context to the result value and repoint it to the value following
               the second operand */
            cursor = cursor->prev;
            cursor->next = cursor->next->next->next;
            if(cursor->next != NULL) {
                cursor->next->prev = cursor;
            }
        }
        cursor = cursor->next;
    }

    /* or and nor */
    cursor = expr;
    while(cursor != NULL) {
        if(cursor->op == CRUSTY_EXPR_OR ||
           cursor->op == CRUSTY_EXPR_NOR) {
            if(cursor->prev == NULL) {
                LOG_PRINTF_TOK(cvm, "Operator with nothing before.\n");
                return(NULL);
            }
            if(cursor->prev->op != CRUSTY_EXPR_NUMBER) {
                LOG_PRINTF_TOK(cvm, "Operator with not a number before.\n");
                return(NULL);
            }
            if(cursor->next == NULL) {
                LOG_PRINTF_TOK(cvm, "Operator with nothing after.\n");
                return(NULL);
            }
            if(cursor->next->op != CRUSTY_EXPR_NUMBER) {
                LOG_PRINTF_TOK(cvm, "Operator with not a number after.\n");
                return(NULL);
            }

            if(cursor->op == CRUSTY_EXPR_OR) {
                cursor->prev->number = cursor->prev->number | cursor->next->number;
            } else {
                cursor->prev->number = ~(cursor->prev->number | cursor->next->number);
            }

            /* the operator and second operand are unimportant now so move the
               context to the result value and repoint it to the value following
               the second operand */
            cursor = cursor->prev;
            cursor->next = cursor->next->next->next;
            if(cursor->next != NULL) {
                cursor->next->prev = cursor;
            }
        }
        cursor = cursor->next;
    }

    /* xor and xnor */
    cursor = expr;
    while(cursor != NULL) {
        if(cursor->op == CRUSTY_EXPR_XOR ||
           cursor->op == CRUSTY_EXPR_XNOR) {
            if(cursor->prev == NULL) {
                LOG_PRINTF_TOK(cvm, "Operator with nothing before.\n");
                return(NULL);
            }
            if(cursor->prev->op != CRUSTY_EXPR_NUMBER) {
                LOG_PRINTF_TOK(cvm, "Operator with not a number before.\n");
                return(NULL);
            }
            if(cursor->next == NULL) {
                LOG_PRINTF_TOK(cvm, "Operator with nothing after.\n");
                return(NULL);
            }
            if(cursor->next->op != CRUSTY_EXPR_NUMBER) {
                LOG_PRINTF_TOK(cvm, "Operator with not a number after.\n");
                return(NULL);
            }

            if(cursor->op == CRUSTY_EXPR_XOR) {
                cursor->prev->number = cursor->prev->number ^ cursor->next->number;
            } else {
                cursor->prev->number = ~(cursor->prev->number ^ cursor->next->number);
            }

            /* the operator and second operand are unimportant now so move the
               context to the result value and repoint it to the value following
               the second operand */
            cursor = cursor->prev;
            cursor->next = cursor->next->next->next;
            if(cursor->next != NULL) {
                cursor->next->prev = cursor;
            }
        }
        cursor = cursor->next;
    }

    /* at this point, everything should be collapsed in to one lone value */
    if(expr->prev != NULL || expr->next != NULL) {
        LOG_PRINTF_TOK(cvm, "Expression didn't evaluate down to a single number.\n");
        return(NULL);
    }

    return(expr);
}

static CrustyExpr *add_expr(CrustyVM *cvm,
                            CrustyExprOp op,
                            int number,
                            CrustyExpr *buffer,
                            int *len,
                            CrustyExpr **new) {
    CrustyExpr *expr;
    expr = realloc(buffer, sizeof(CrustyExpr) * (*len + 1));
    if(expr == NULL) {
        LOG_PRINTF_TOK(cvm, "Failed to allocate memory for CrustyExpr.\n");
        return(NULL);
    }

    *new = &(expr[*len]);
    (*new)->op = op;
    (*new)->number = number;
    (*len)++;

    return(expr);
}

#define ISJUNK(X) ((X) == ' ' || (X) == '\t')

/* this is all awful and probably a horrible, inefficient way to do this but I don't know a better way */
static long evaluate_expr(CrustyVM *cvm, const char *expression) {
    CrustyExpr *expr = NULL;
    int exprmem = 0;
    CrustyExpr *temp;
    CrustyExpr *new = NULL;
    int parens = 0;
    int valsize;

    char *end;
    long num;

    int i;
    int exprlen = strlen(expression);
    long tokenstart;

    for(i = 0; i < exprlen; i++) {
        if(ISJUNK(expression[i])) {
            continue;
        } else if(expression[i] == '(') {
            temp = add_expr(cvm, CRUSTY_EXPR_LPAREN, 0, expr, &exprmem, &new);
            if(temp == NULL) {
                goto error;
            }
            expr = temp;
            parens++;
        } else if(expression[i] == ')') {
            temp = add_expr(cvm, CRUSTY_EXPR_RPAREN, 0, expr, &exprmem, &new);
            if(temp == NULL) {
                goto error;
            }
            expr = temp;
            parens--;
        } else if(expression[i] == '+' &&
                  new != NULL &&
                  (new->op == CRUSTY_EXPR_NUMBER ||
                   new->op == CRUSTY_EXPR_RPAREN)) {
            /* Only assume we want to add a + or - if we're following a
               point where it'd be clearly valid to do so, otherwise one
               wouldn't be able to for example add or subtract a negative
               number, because the evaluation of something like 2 - -2 would
               become 2 subtract subtract 2 which can't work.  This will
               allow it to be 2 subtract -2 because this'll fall through and
               the -2 will be evaluated by strtol. */
            temp = add_expr(cvm, CRUSTY_EXPR_PLUS, 0, expr, &exprmem, &new);
            if(temp == NULL) {
                goto error;
            }
            expr = temp;
        } else if(expression[i] == '-' &&
                  new != NULL &&
                  (new->op == CRUSTY_EXPR_NUMBER ||
                   new->op == CRUSTY_EXPR_RPAREN)) {
            /* see above */
            temp = add_expr(cvm, CRUSTY_EXPR_MINUS, 0, expr, &exprmem, &new);
            if(temp == NULL) {
                goto error;
            }
            expr = temp;
        } else if(expression[i] == '*') {
            temp = add_expr(cvm, CRUSTY_EXPR_MULTIPLY, 0, expr, &exprmem, &new);
            if(temp == NULL) {
                goto error;
            }
            expr = temp;
        } else if(expression[i] == '/') {
            temp = add_expr(cvm, CRUSTY_EXPR_DIVIDE, 0, expr, &exprmem, &new);
            if(temp == NULL) {
                goto error;
            }
            expr = temp;
        } else if(expression[i] == '%') {
            temp = add_expr(cvm, CRUSTY_EXPR_MODULO, 0, expr, &exprmem, &new);
            if(temp == NULL) {
                goto error;
            }
            expr = temp;
        } else if(expression[i] == '=') {
            if(i + 1 < exprlen) {
                if(expression[i + 1] == '=') {
                    temp = add_expr(cvm, CRUSTY_EXPR_EQUALS, 0, expr, &exprmem, &new);
                    if(temp == NULL) {
                        goto error;
                    }
                    expr = temp;
                    i++;
                } else {
                    LOG_PRINTF_TOK(cvm, "Invalid operator: =%c\n", expression[i + 1]);
                    goto error;
                }
            } else {
                LOG_PRINTF_TOK(cvm, "Invalid operator: =\n");
                goto error;
            }
        } else if(expression[i] == '<') {
            if(i + 1 < exprlen) {
                if(expression[i + 1] == '=') {
                    temp = add_expr(cvm, CRUSTY_EXPR_LEQUALS, 0, expr, &exprmem, &new);
                    if(temp == NULL) {
                        goto error;
                    }
                    expr = temp;
                    i++;
                } else if(expression[i + 1] == '<') {
                    temp = add_expr(cvm, CRUSTY_EXPR_LSHIFT, 0, expr, &exprmem, &new);
                    if(temp == NULL) {
                        goto error;
                    }
                    expr = temp;
                    i++;
                } else if(ISJUNK(expression[i + 1])) {
                    temp = add_expr(cvm, CRUSTY_EXPR_LESS, 0, expr, &exprmem, &new);
                    if(temp == NULL) {
                        goto error;
                    }
                    expr = temp;
                } else {
                    LOG_PRINTF_TOK(cvm, "Invalid operator: <%c\n", expression[i + 1]);
                    goto error;
                }
            } else {
                LOG_PRINTF_TOK(cvm, "Operator at end of expression: <\n");
                goto error;
            }            
        } else if(expression[i] == '>') {
            if(i + 1 < exprlen) {
                if(expression[i + 1] == '=') {
                    temp = add_expr(cvm, CRUSTY_EXPR_GEQUALS, 0, expr, &exprmem, &new);
                    if(temp == NULL) {
                        goto error;
                    }
                    expr = temp;
                    i++;
                } else if(expression[i + 1] == '>') {
                    temp = add_expr(cvm, CRUSTY_EXPR_RSHIFT, 0, expr, &exprmem, &new);
                    if(temp == NULL) {
                        goto error;
                    }
                    expr = temp;
                    i++;
                } else if(ISJUNK(expression[i + 1])) {
                    temp = add_expr(cvm, CRUSTY_EXPR_GREATER, 0, expr, &exprmem, &new);
                    if(temp == NULL) {
                        goto error;
                    }
                    expr = temp;
                } else {
                    LOG_PRINTF_TOK(cvm, "Invalid operator: >%c\n", expression[i + 1]);
                    goto error;
                }
            } else {
                LOG_PRINTF_TOK(cvm, "Operator at end of expression: >\n");
                goto error;
            }            
        } else if(expression[i] == '!') {
            if(i + 1 < exprlen) {
                if(expression[i + 1] == '=') {
                    temp = add_expr(cvm, CRUSTY_EXPR_NEQUALS, 0, expr, &exprmem, &new);
                    if(temp == NULL) {
                        goto error;
                    }
                    expr = temp;
                    i++;
                } else if(expression[i + 1] == '&') {
                    temp = add_expr(cvm, CRUSTY_EXPR_NAND, 0, expr, &exprmem, &new);
                    if(temp == NULL) {
                        goto error;
                    }
                    expr = temp;
                    i++;
                } else if(expression[i + 1] == '|') {
                    temp = add_expr(cvm, CRUSTY_EXPR_NOR, 0, expr, &exprmem, &new);
                    if(temp == NULL) {
                        goto error;
                    }
                    expr = temp;
                    i++;
                } else if(expression[i + 1] == '^') {
                    temp = add_expr(cvm, CRUSTY_EXPR_XNOR, 0, expr, &exprmem, &new);
                    if(temp == NULL) {
                        goto error;
                    }
                    expr = temp;
                    i++;
                } else if(ISJUNK(expression[i + 1])) {
                    LOG_PRINTF_TOK(cvm, "Invalid operator: !\n");
                    goto error;
                } else {
                    LOG_PRINTF_TOK(cvm, "Invalid operator: !%c\n", expression[i + 1]);
                    goto error;
                }
            } else {
                LOG_PRINTF_TOK(cvm, "Invalid operator: !\n");
                goto error;
            }            
        } else if(expression[i] == '&') {
            temp = add_expr(cvm, CRUSTY_EXPR_AND, 0, expr, &exprmem, &new);
            if(temp == NULL) {
                goto error;
            }
            expr = temp;
        } else if(expression[i] == '|') {
            temp = add_expr(cvm, CRUSTY_EXPR_OR, 0, expr, &exprmem, &new);
            if(temp == NULL) {
                goto error;
            }
            expr = temp;
        } else if(expression[i] == '^') {
            temp = add_expr(cvm, CRUSTY_EXPR_NOR, 0, expr, &exprmem, &new);
            if(temp == NULL) {
                goto error;
            }
            expr = temp;
        } else {
            num = strtol(&(expression[i]), &end, 0);
            if(&(expression[i]) != end) {
                temp = add_expr(cvm, CRUSTY_EXPR_NUMBER, num, expr, &exprmem, &new);
                if(temp == NULL) {
                    goto error;
                }
                expr = temp;

                i += (int)(end - &(expression[i]) - 1);
            } else {
                /* insert a 0 for an undefined variable or whatever the user
                   might have put in that can't be interpreted as anything. This
                   will make user errors harder to find but whichever. */
                temp = add_expr(cvm, CRUSTY_EXPR_NUMBER, 0, expr, &exprmem, &new);

                if(temp == NULL) {
                    goto error;
                }
                expr = temp;
                /* find the next "junk" char */
                while(i < exprlen) {
                    if(ISJUNK(expression[i])) {
                        break;
                    }
                    i++;
                }
            }
        }

        /* PRINT_EXPR(*new) */
    }
    /* fprintf(stderr, "\n"); */
    if(parens != 0) {
        LOG_PRINTF_TOK(cvm, "Unmatched parentheses.\n");
        goto error;
    }

    if(exprmem == 0) {
        LOG_PRINTF_TOK(cvm, "No expression tokens found.\n");
        goto error;
    }

    /* point everything */
    expr[0].prev = NULL;
    if(exprmem > 1) {
        expr[0].next = &(expr[1]);
        for(i = 1; i < exprmem - 1; i++) {
            expr[i].prev = &(expr[i - 1]);
            expr[i].next = &(expr[i + 1]);
        }
        expr[exprmem - 1].next = NULL;
        expr[exprmem - 1].prev = &(expr[exprmem - 2]);
    } else {
        expr[0].next = NULL;
    }

    /* pass in the expression, any errors will result in NULL being returned */
    temp = do_expression(cvm, expr);
    if(temp == NULL) {
        goto error;
    }
    /* temp should now be a lone CRUSTY_EXPR_NUMBER */

    /* create the string containing the evaluated value */
    valsize = snprintf(NULL, 0, "%d", temp->number);
    /* returned buffer is already null terminated and large enough to fit valsize */
    tokenstart = add_token(cvm, NULL, valsize, 0, NULL);
    if(tokenstart < 0) {
        LOG_PRINTF_TOK(cvm, "Failed to allocate memory for expression value string.\n");
        goto error;
    }
    if(snprintf(&(cvm->tokenmem[tokenstart]),
                valsize + 1,
                "%d",
                temp->number) != valsize) {
        LOG_PRINTF_TOK(cvm, "Failed to write expression value in to string.\n");
        goto error;
    }
    free(expr);

    return(tokenstart);
error:
    if(expr != NULL) {
        free(expr);
    }

    return(-1);
}

#undef ISJUNK

#define INSTRUCTION_COUNT (24)

static int valid_instruction(const char *name) {
    int i;

    const char *INSTRUCTION_LIST[] = {
        "stack",
        "proc",
        "export",
        "ret",
        "label",
        "static",
        "local",
        "move",
        "add",
        "sub",
        "mul",
        "div",
        "and",
        "or",
        "xor",
        "shl",
        "shr",
        "cmp",
        "call",
        "jump",
        "jumpn",
        "jumpz",
        "jumpl",
        "jumpg"
    };  

    for(i = 0; i < INSTRUCTION_COUNT; i++) {
        if(strcmp(name, INSTRUCTION_LIST[i]) == 0) {
            return(1);
        }
    }

    return(0);
}

#undef INSTRUCTION_COUNT

#define GET_ACTIVE(TOKEN) (&(cvm->tokenmem[active.offset[TOKEN]]))
#define GET_VAR(VAR) (&(cvm->tokenmem[vars[VAR]]))
#define GET_VALUE(VAL) (&(cvm->tokenmem[values[VAL]]))
#define GET_MACRO_STACK_NAME(PTR) (&(cvm->tokenmem[macrostack[PTR]->nameOffset]))

static int preprocess(CrustyVM *cvm,
                      const unsigned long *inVar,
                      const unsigned long *inValue,
                      unsigned int inVars) {
    unsigned int i, j;
    CrustyLine *new = NULL;
    unsigned int mem;
    unsigned int lines;
    char *temp;

    CrustyLine active;
    active.offset = NULL;

    CrustyMacro *macro = NULL;
    unsigned int macrocount = 0;
    CrustyMacro *curmacro = NULL;

    unsigned int returnstack[MACRO_STACK_SIZE];
    CrustyMacro *macrostack[MACRO_STACK_SIZE];
    long *macroargs[MACRO_STACK_SIZE];
    int macrostackptr = -1;

    long *vars = NULL;
    long *values = NULL;
    unsigned int varcount = 0;

    int foundmacro = 0;

    long tokenstart;

    mem = 0; /* actual memory allocated for line */
    lines = 0; /* size of initialized array */

    cvm->logline = 0; /* line being evaluated */
    while(cvm->logline < cvm->lines) {
        /* no need to check if tokencount > 0 because those lines were filtered
           out previously */

        if(active.offset != NULL) {
            free(active.offset);
        }
        active.offset = malloc(sizeof(long) * cvm->line[cvm->logline].tokencount);
        if(active.offset == NULL) {
            LOG_PRINTF_TOK(cvm, "Failed to allocate memory for active token arguments.");
            goto failure;
        }
        /* make mutable active line */
        active.tokencount = cvm->line[cvm->logline].tokencount;
        active.moduleOffset = cvm->line[cvm->logline].moduleOffset;
        active.line = cvm->line[cvm->logline].line;
        /* don't mutate the endmacro line while processing a macro, since it
           could replace substrings causing the macro to never end */
        if(curmacro != NULL &&
           strcmp(GET_TOKEN(cvm->logline, 0), "endmacro") == 0 &&
           strcmp(GET_TOKEN(cvm->logline, 1), &(cvm->tokenmem[curmacro->nameOffset])) == 0) {
            for(i = 0; i < active.tokencount; i++) {
                active.offset[i] = cvm->line[cvm->logline].offset[i];
            }
        } else {
            /* replace any tokens with tokens containing any possible macro
               replacement values */
            for(i = 0; i < active.tokencount; i++) {
                active.offset[i] = cvm->line[cvm->logline].offset[i];
                for(j = 0; j < inVars; j++) {
                    tokenstart = string_replace(cvm,
                                                active.offset[i],
                                                inVar[j],
                                                inValue[j]);
                    if(tokenstart < 0) {
                        /* reason will have already been printed */
                        goto failure;
                    }
                    active.offset[i] = tokenstart;
                }
                if((macrostackptr >= 0 && macrostack[macrostackptr]->argcount > 0)) {
                    for(j = 0; j < macrostack[macrostackptr]->argcount; j++) {
                        /* function will just pass back the token passed to
                           it in the case there's nothing to be done,
                           otherwise it'll create the new string in extramem
                           and update the length and return it. */
                        tokenstart =
                            string_replace(cvm,
                                           active.offset[i],
                                           macrostack[macrostackptr]->argOffset[j],
                                           macroargs[macrostackptr][j]);
                        if(tokenstart < 0) {
                            /* reason will have already been printed */
                            goto failure;
                        }
                        active.offset[i] = tokenstart;
                    }
                }
                for(j = 0; j < varcount; j++) {
                    tokenstart = string_replace(cvm, active.offset[i], vars[j], values[j]);
                    if(tokenstart < 0) {
                        /* reason will have already been printed */
                        goto failure;
                    }
                    active.offset[i] = tokenstart;
                }
            }
        }

        if(strcmp(GET_ACTIVE(0), "macro") == 0) {
            if(curmacro == NULL) { /* don't evaluate any macros which may be
                                      within other macros, which will be
                                      evaluated on subsequent passes. */
                if(active.tokencount < 2) {
                    LOG_PRINTF_TOK(cvm, "Macros must at least be defined with a name.\n");
                    goto failure;
                }

                curmacro = find_macro(cvm, macro, macrocount, GET_ACTIVE(1));

                /* if the macro wasn't found, allocate space for it, otherwise
                   override previous declaration */
                if(curmacro == NULL) {
                    curmacro = realloc(macro, sizeof(CrustyMacro) * (macrocount + 1));
                    if(curmacro == NULL) {
                        LOG_PRINTF_TOK(cvm, "Failed to allocate memory for macro.\n");
                        goto failure;
                    }
                    macro = curmacro;
                    curmacro = &(macro[macrocount]);
                    macrocount++;
                }
                curmacro->nameOffset = active.offset[1];
                curmacro->argcount = active.tokencount - 2;
                curmacro->argOffset = malloc(sizeof(long) * curmacro->argcount);
                if(curmacro->argOffset == NULL) {
                    LOG_PRINTF_TOK(cvm, "Failed to allocate memory for macro args list.\n");
                    free(curmacro);
                    goto failure;
                }
                for(i = 2; i < active.tokencount; i++) {
                    curmacro->argOffset[i - 2] = active.offset[i];
                }
                curmacro->start = cvm->logline + 1; /* may not be defined now but a
                                                  valid program will have it
                                                  defined eventually as a macro
                                                  at least needs a matching
                                                  endmacro */

                /* suppress copying evaluated macro in to destination */
                goto skip_copy;
            } else {
                foundmacro = 1;
            }
        } else if(strcmp(GET_ACTIVE(0), "endmacro") == 0) {
            if(active.tokencount != 2) {
                LOG_PRINTF_TOK(cvm, "endmacro takes a name.\n");
                goto failure;
            }

            /* if a macro is being read in and the end of that macro has
               been reached, another macro can start being read in again */
            if(curmacro != NULL &&
               strcmp(GET_ACTIVE(1), &(cvm->tokenmem[curmacro->nameOffset])) == 0) {
                curmacro = NULL;

                /* suppress copying evaluated endmacro in to destination */
                goto skip_copy;
            }

            /* if a macro is being output and the end of the macro currently
               being output is reached, then pop it off the stack. */
            if(macrostackptr >= 0 &&
               strcmp(GET_MACRO_STACK_NAME(macrostackptr),
                      GET_ACTIVE(1)) == 0) {
                free(macroargs[macrostackptr]);
                cvm->logline = returnstack[macrostackptr];
                macrostackptr--;

                /* suppress copying evaluated endmacro in to destination */
                goto skip_copy;
            }
        } else if(strcmp(GET_ACTIVE(0), "if") == 0) {
            /* don't evaluate macro calls while reading in a macro, only
               while writing out */
            if(curmacro == NULL) {
                /* at this point, a defined variable will already have
                   replaced the first argument so we just need to determine
                   whather it's a number and whether it's not 0 */
                if(active.tokencount < 3) {
                    LOG_PRINTF_TOK(cvm,
                        "if takes a variable and at least 1 more argument.\n");
                    goto failure;
                }

                char *endchar;
                int num;
                num = strtol(GET_ACTIVE(1), &endchar, 0);
                /* check that the entire string was valid and that the
                   result was not zero */
                if(GET_ACTIVE(1)[0] != '\0' && *endchar == '\0' && num != 0) {
                    /* move everything over 2 */
                    for(j = 2; j < active.tokencount; j++) {
                        cvm->line[cvm->logline].offset[j - 2] =
                            cvm->line[cvm->logline].offset[j];
                    }
                    cvm->line[cvm->logline].tokencount -= 2;

                    continue; /* don't copy but reevaluate */
                }

                goto skip_copy; /* don't copy and don't reevaluate */
            }
        } else if(strcmp(GET_ACTIVE(0), "expr") == 0) {
            if(curmacro == NULL) { /* don't evaluate any expressions which
                                      may be within macros, which will be
                                      evaluated on subsequent passes. */
                if(active.tokencount != 3) {
                    LOG_PRINTF_TOK(cvm,
                        "expr takes a variable name and an expression.\n");
                    goto failure;
                }

                temp = realloc(vars, sizeof(long) * (varcount + 1));
                if(temp == NULL) {
                    LOG_PRINTF_TOK(cvm, "Failed to allocate memory for expr var.\n");
                    goto failure;
                }
                vars = (long *)temp;

                temp = realloc(values, sizeof(long) * (varcount + 1));
                if(temp == NULL) {
                    LOG_PRINTF_TOK(cvm, "Failed to allocate memory for expr value.\n");
                    goto failure;
                }
                values = (long *)temp;

                vars[varcount] = active.offset[1];
                values[varcount] = evaluate_expr(cvm, GET_ACTIVE(2));
                if(values[varcount] < 0) {
                    LOG_PRINTF_TOK(cvm, "Expression evaluation failed.\n");
                    goto failure;
                }
                varcount++;

                goto skip_copy;
            } else {
                foundmacro = 1;
            }
        } else if(!valid_instruction(GET_ACTIVE(0))) {
            /* don't evaluate macro calls while reading in a macro, only
               while writing out */
            if(curmacro == NULL) {
                if(macrostackptr == MACRO_STACK_SIZE - 1) {
                    LOG_PRINTF_TOK(cvm, "Macro stack filled.\n");
                }

                macrostack[macrostackptr + 1] = find_macro(cvm,
                                                           macro,
                                                           macrocount,
                                                           GET_ACTIVE(0));
                if(macrostack[macrostackptr + 1] == NULL) {
                    LOG_PRINTF_TOK(cvm, "Invalid keyword or macro not found: %s.\n",
                                    GET_ACTIVE(0));
                    goto failure;
                }

                if(macrostack[macrostackptr + 1] == curmacro) {
                    LOG_PRINTF_TOK(cvm, "Macro called recursively: %s.\n",
                                    &(cvm->tokenmem[curmacro->nameOffset]));
                    goto failure;
                }
                if(active.tokencount - 1 !=
                   macrostack[macrostackptr + 1]->argcount) {
                    LOG_PRINTF_TOK(cvm, "Wrong number of arguments to macro: "
                                         "got %d, expected %d.\n",
                               active.tokencount - 1,
                               macrostack[macrostackptr + 1]->argcount);
                    goto failure;
                }

                macrostackptr++;
                macroargs[macrostackptr] =
                    malloc(sizeof(long) * macrostack[macrostackptr]->argcount);
                if(macroargs[macrostackptr] == NULL) {
                    LOG_PRINTF_TOK(cvm, "Failed to allocate memory for macro args.\n");
                    goto failure;
                }
                for(i = 0; i < macrostack[macrostackptr]->argcount; i++) {
                    macroargs[macrostackptr][i] = active.offset[i + 1];
                }
                returnstack[macrostackptr] = cvm->logline;
                cvm->logline = macrostack[macrostackptr]->start;

                /* don't copy the next line but make sure it's still evaluated */
                continue;
            }
        }

        /* don't actually output a macro being read in */
        if(curmacro == NULL) {
            if(lines == mem) {
                temp = realloc(new, sizeof(CrustyLine) * (mem + 1));
                if(temp == NULL) {
                    LOG_PRINTF_TOK(cvm, "Failed to allocate memory for line copy.\n");
                    goto failure;
                }
                new = (CrustyLine *)temp;
                new[lines].offset = NULL;
                mem++;
            }

            new[lines].tokencount = active.tokencount;
            new[lines].moduleOffset = active.moduleOffset;
            new[lines].line = active.line;
            new[lines].offset = malloc(sizeof(long) * new[lines].tokencount);
            if(new[lines].offset == NULL) {
                LOG_PRINTF_TOK(cvm, "Failed to allocate memory for line offsets copy.\n");
                goto failure;
            }
            for(i = 0; i < new[lines].tokencount; i++) {
                new[lines].offset[i] = active.offset[i];
            }

            lines++;
        }

skip_copy:
        cvm->logline++;
    }

    for(i = 0; i < cvm->lines; i++) {
        free(cvm->line[i].offset);
    }
    free(cvm->line);
    cvm->line = new;
    cvm->lines = lines;

    if(macro != NULL) {
        for(i = 0; i < macrocount; i++) {
            if(macro[i].argOffset != NULL) {
                free(macro[i].argOffset);
            }
        }
        free(macro);
    }

    if(active.offset != NULL) {
        free(active.offset);
    }

    if(vars != NULL) {
        free(vars);
    }
    if(values != NULL) {
        free(values);
    }

    if(curmacro != NULL) {
        LOG_PRINTF_TOK(cvm, "Macro without endmacro: %s.\n",
                        &(cvm->tokenmem[curmacro->nameOffset]));
        return(-1);
    }

    return(foundmacro);

failure:
    if(new != NULL) {
        for(i = 0; i < lines; i++) {
            if(new[i].offset != NULL) {
                free(new[i].offset);
            }
        }
        free(new);
    }

    if(macro != NULL) {
        for(i = 0; i < macrocount; i++) {
            if(macro[i].argOffset != NULL) {
                free(macro[i].argOffset);
            }
        }
        free(macro);
    }

    if(active.offset != NULL) {
        free(active.offset);
    }

    if(vars != NULL) {
        free(vars);
    }
    if(values != NULL) {
        free(values);
    }

    return(-1);
}

#undef GET_VALUE
#undef GET_VAR
#undef GET_ACTIVE

static int find_procedure(CrustyVM *cvm,
                          const char *name) {
    unsigned int i;

    for(i = 0; i < cvm->procs; i++) {
        if(strcmp(&(cvm->tokenmem[cvm->proc[i].nameOffset]), name) == 0) {
            return(i);
        }
    }

    return(-1);
}

static int variable_is_global(CrustyVariable *var) {
    return(var->proc == NULL);
}

static int variable_is_argument(CrustyVariable *var) {
    return(var->length == 0);
}

static int variable_is_callback(CrustyVariable *var) {
    return(var->read != NULL || var->write != NULL);
}

/* this function is used while proc->var and var->proc are invalid and the only
   associations between variables and their procedures is a list of indexes in
   to the variable list within each procedure */
static int find_variable(CrustyVM *cvm,
                         CrustyProcedure *proc,
                         const char *name) {
    unsigned int i;

    if(proc != NULL) {
        /* scan local */
        for(i = 0; i < proc->vars; i++) {
            if(strcmp(&(cvm->tokenmem[cvm->var[proc->varIndex[i]].nameOffset]),
                      name) == 0) {
                return(proc->varIndex[i]);
            }
        }
    }
    /* scan global */
    for(i = 0; i < cvm->vars; i++) {
        if(variable_is_global(&(cvm->var[i]))) {
            if(strcmp(&(cvm->tokenmem[cvm->var[i].nameOffset]), name) == 0) {
                return(i);
            }
        }
    }

    return(-1);
}

static int new_variable(CrustyVM *cvm,
                        long nameOffset,
                        CrustyType type,
                        unsigned int length,
                        char *chrinit,
                        int *intinit,
                        double *floatinit,
                        const CrustyCallback *cb,
                        CrustyProcedure *proc) {
    char *temp;

    if(find_variable(cvm, proc, &(cvm->tokenmem[nameOffset])) >= 0) {
        if(cb != NULL) {
            LOG_PRINTF(cvm, "Redeclaration of callback variable.\n");
        } else if(proc == NULL) {
            LOG_PRINTF(cvm, "Redeclaration of static variable.\n");
        } else {
            LOG_PRINTF(cvm, "Redeclaration of local variable.\n");
        }
        return(-1);
    }

    temp = realloc(cvm->var, sizeof(CrustyVariable) * (cvm->vars + 1));
    if(temp == NULL) {
        LOG_PRINTF(cvm, "Failed to allocate memory for variable.\n");
        return(-1);
    }
    cvm->var = (CrustyVariable *)temp;
    cvm->var[cvm->vars].nameOffset = nameOffset;
    cvm->var[cvm->vars].length = length;
    cvm->var[cvm->vars].type = type;
    cvm->var[cvm->vars].chrinit = chrinit;
    cvm->var[cvm->vars].intinit = intinit;
    cvm->var[cvm->vars].floatinit = floatinit;
    if(cb == NULL) {
        cvm->var[cvm->vars].read = NULL;
        cvm->var[cvm->vars].write = NULL;
    } else {
        cvm->var[cvm->vars].read = cb->read;
        cvm->var[cvm->vars].write = cb->write;
        cvm->var[cvm->vars].readpriv = cb->readpriv;
        cvm->var[cvm->vars].writepriv = cb->writepriv;
    }        
    cvm->var[cvm->vars].proc = proc; /* this value will likely become invalid
                                        as the procedure list grows, but it will
                                        be updated again once everything is its
                                        final size.  All that matters right now
                                        is whether it's NULL or not. */

    if(proc != NULL) {
        temp = realloc(proc->varIndex, sizeof(int) * (proc->vars + 1));
        if(temp == NULL) {
            LOG_PRINTF(cvm, "Failed to allocate memory for local variable list.\n");
            return(-1);
        }
        proc->varIndex = (int *)temp;
        proc->varIndex[proc->vars] = cvm->vars;
        proc->vars++;
    }

    cvm->vars++;

    return(0);
}

#define ISJUNK(X) ((X) == ' ' || \
                   (X) == '\t')

static int number_list_ints(const char *list, int **buffer) {
    int count = 0;
    int cur;
    unsigned int len = strlen(list);
    unsigned int i;
    char *end;

    /* count numbers found in list */
    for(i = 0; i < len; i++) {
        if(!ISJUNK(list[i])) {
            strtol(&(list[i]), &end, 0);

            if(end == &(list[i]) ||
               !ISJUNK(*end)) {
                if(*end == '\0') {
                    /* number found at end of string, nothing more to do */
                    count++;
                    break;
                }
                /* separator ended with something not a number and number ended on
                   something not a separator */
                return(0);
            }

            /* more gross pointery stuff */
            i += (end - &(list[i]));
            count++;
        }
    }

    *buffer = malloc(sizeof(int) * count);
    if(*buffer == NULL) {
        return(-1);
    }

    /* scan for numbers a second time, but populate the array */
    cur = 0;
    for(i = 0; i < len; i++) {
        if(!ISJUNK(list[i])) {
            (*buffer)[cur] = strtol(&(list[i]), &end, 0);

            /* don't bother with sanity checks because they were already done */

            /* more gross pointery stuff */
            i += (end - &(list[i]));
            cur++;
        }
    }

    return(count);
}

static int number_list_floats(const char *list, double **buffer) {
    int count = 0;
    int cur;
    unsigned int len = strlen(list);
    unsigned int i;
    char *end;

    /* count numbers found in list */
    for(i = 0; i < len; i++) {
        if(!ISJUNK(list[i])) {
            strtod(&(list[i]), &end);

            if(end == &(list[i]) ||
               !ISJUNK(*end)) {
                if(*end == '\0') {
                    /* number found at end of string, nothing more to do */
                    count++;
                    break;
                }
                /* separator ended with something not a number and number ended on
                   something not a separator */
                return(0);
            }

            /* more gross pointery stuff */
            i += (end - &(list[i]));
            count++;
        }
    }

    *buffer = malloc(sizeof(double) * count);
    if(*buffer == NULL) {
        return(-1);
    }

    /* scan for numbers a second time, but populate the array */
    cur = 0;
    for(i = 0; i < len; i++) {
        if(!ISJUNK(list[i])) {
            (*buffer)[cur] = strtod(&(list[i]), &end);

            /* don't bother with sanity checks because they were already done */

            /* more gross pointery stuff */
            i += (end - &(list[i]));
            cur++;
        }
    }

    return(count);
}

#undef ISJUNK

static int variable_declaration(CrustyVM *cvm,
                                CrustyLine *line,
                                CrustyProcedure *proc) {
    int length;
    int *intinit = NULL;
    char *chrinit = NULL;
    double *floatinit = NULL;
    CrustyType type;

    if(line->tokencount == 2) { /* no initializer, allocated to 0 */
        type = CRUSTY_TYPE_INT;
        length = 1;

        intinit = malloc(sizeof(int));
        if(intinit == NULL) {
            LOG_PRINTF_TOK(cvm, "Failed to allocate memory for initializer.\n");
            return(-1);
        }

        intinit[0] = 0;
    } else if(line->tokencount == 3) { /* with initializer */
        int num;
        char *end;
        type = CRUSTY_TYPE_INT;
        length = 1;

        num = strtol(&(cvm->tokenmem[line->offset[2]]), &end, 0);
        if(end != &(cvm->tokenmem[line->offset[2]]) && *end == '\0') {
            intinit = malloc(sizeof(int));
            if(intinit == NULL) {
                LOG_PRINTF_TOK(cvm, "Failed to allocate memory for initializer.\n");
                return(-1);
            }

            intinit[0] = num;
        } else {
            LOG_PRINTF_TOK(cvm, "Initializer wasn't a number.\n");
            return(-1);
        }
    } else if(line->tokencount == 4) {
        if(strcmp(&(cvm->tokenmem[line->offset[2]]), "ints") == 0) {
            type = CRUSTY_TYPE_INT;
            length = number_list_ints(&(cvm->tokenmem[line->offset[3]]), &intinit);
            if(length < 0) {
                LOG_PRINTF_TOK(cvm, "Failed to allocate memory for initializer.\n");
                return(-1);
            } else if(length == 0) {
                LOG_PRINTF_TOK(cvm, "Initializer must be a space separated list of numbers.\n");
                return(-1);
            } else if(length == 1) { /* array without initializer, so fill with zero */
                length = intinit[0];

                free(intinit);
                intinit = malloc(sizeof(int) * length);
                if(intinit == NULL) {
                    LOG_PRINTF_TOK(cvm, "Failed to allocate memory for initializer.\n");
                    return(-1);
                }

                memset(intinit, 0, sizeof(int) * length);
            }
            /* array with initializer, nothing to do, since length and intinit
               are already what they should be */
        } else if(strcmp(&(cvm->tokenmem[line->offset[2]]), "floats") == 0) {
            type = CRUSTY_TYPE_FLOAT;
            length = number_list_floats(&(cvm->tokenmem[line->offset[3]]), &floatinit);
            if(length < 0) {
                LOG_PRINTF_TOK(cvm, "Failed to allocate memory for initializer.\n");
                return(-1);
            } else if(length == 0) {
                LOG_PRINTF_TOK(cvm, "Initializer must be a space separated list of numbers.\n");
                return(-1);
            }
            /* array with initializer */
        } else if(strcmp(&(cvm->tokenmem[line->offset[2]]), "string") == 0) {
            type = CRUSTY_TYPE_CHAR;
            length = strlen(&(cvm->tokenmem[line->offset[3]]));

            chrinit = malloc(length + 1);
            if(chrinit == NULL) {
                LOG_PRINTF_TOK(cvm, "Failed to allocate memory for initializer.\n");
                return(-1);
            }

            /* This line throws a warning in gcc 9.3.0 about the length argument
               depending on the source argument but the same variable is used to
               allocate the destination argument anyway. */
            strncpy(chrinit, &(cvm->tokenmem[line->offset[3]]), length + 1);
        } else {
            LOG_PRINTF_TOK(cvm, "variable declaration can be array or string.\n");
            return(-1);
        }
    } else {
        LOG_PRINTF_TOK(cvm, "static can be declared as string or "
                             "array and is followed by an initializer "
                             "or array may be followed by a numeric "
                             "size.\n");
        return(-1);
    }

    if(new_variable(cvm,
                    line->offset[1],
                    type,
                    length,
                    chrinit, intinit, floatinit,
                    NULL,
                    proc) < 0) {
        /* print an error so the user can get a line number. */
        LOG_PRINTF_TOK(cvm, "Error from new_variable().\n");
        return(-1);
    }

    return(0);
}

static int symbols_scan(CrustyVM *cvm) {
    unsigned int i, j;
    CrustyProcedure *curProc = NULL;

    CrustyLine *new = NULL;
    char *temp;
    unsigned int lines;

    new = malloc(sizeof(CrustyLine) * cvm->lines);
    if(new == NULL) {
        LOG_PRINTF(cvm, "Failed to allocate memory for lines\n");
        return(-1);
    }

    cvm->stacksize = 0;

    lines = 0;
    for(cvm->logline = 0; cvm->logline < cvm->lines; cvm->logline++) {
        if(curProc != NULL) {
            curProc->length++;
        }
        /* no need to check if tokencount > 0 because those lines were filtered
           out previously */
        if(strcmp(GET_TOKEN(cvm->logline, 0), "proc") == 0) {
            if(cvm->line[cvm->logline].tokencount < 2) {
                LOG_PRINTF_TOK(cvm, "proc takes a name as argument.\n");
                goto failure;
            }

            if(curProc != NULL) {
                LOG_PRINTF_TOK(cvm, "proc within proc.\n");
                goto failure;
            }

            if(find_procedure(cvm, GET_TOKEN(cvm->logline, 1)) >= 0) {
                LOG_PRINTF_TOK(cvm, "Redeclaration of procedure.\n");
                goto failure;
            }

            curProc = realloc(cvm->proc, sizeof(CrustyProcedure) * (cvm->procs + 1));
            if(curProc == NULL) {
                LOG_PRINTF_TOK(cvm, "Couldn't allocate memory for procedure.\n");
                goto failure;
            }
            cvm->proc = curProc;
            curProc = &(cvm->proc[cvm->procs]);
            cvm->procs++;

            curProc->nameOffset = cvm->line[cvm->logline].offset[1];
            curProc->start = lines;
            curProc->length = 0;
            curProc->stackneeded = 0;
            curProc->args = cvm->line[cvm->logline].tokencount - 2; 
            curProc->var = NULL;
            curProc->varIndex = NULL;
            curProc->vars = 0;
            curProc->label = NULL;
            curProc->labels = 0;

            /* add arguments as local variables */
            for(i = 0; i < curProc->args; i++) {
                /* argument variables have 0 length and no initializers and no
                   read or write functions but obviously is a local variable */
                /* make stack variables int by default because they may be
                   referenced directly but only can exist as a single int with
                   a length of 1. */
                if(new_variable(cvm,
                                cvm->line[cvm->logline].offset[i + 2],
                                CRUSTY_TYPE_INT,
                                0,
                                NULL, NULL, NULL,
                                NULL,
                                curProc) < 0) {
                    /* reason will have already been printed */
                    goto failure;
                }
            }                    

            continue; /* don't copy in to new list */
        } else if(strcmp(GET_TOKEN(cvm->logline, 0), "ret") == 0) {
            if(curProc == NULL) {
                LOG_PRINTF_TOK(cvm, "ret without proc.\n");
                goto failure;
            }

            curProc = NULL;

            /* this is a real instruction, so it should be copied over */
        } else if(strcmp(GET_TOKEN(cvm->logline, 0), "static") == 0) {
            if(cvm->line[cvm->logline].tokencount < 2) {
                LOG_PRINTF_TOK(cvm, "static takes a name as argument.\n");
                goto failure;
            }

            if(variable_declaration(cvm, &(cvm->line[cvm->logline]), NULL) < 0) {
                goto failure;
            }

            continue; /* don't copy in to new list */
        } else if(strcmp(GET_TOKEN(cvm->logline, 0), "local") == 0) {
            if(cvm->line[cvm->logline].tokencount < 2) {
                LOG_PRINTF_TOK(cvm, "local takes a name as argument.\n");
                goto failure;
            }

            if(curProc == NULL) {
                LOG_PRINTF_TOK(cvm, "local declared outside of procedure.\n");
                goto failure;
            }

            if(variable_declaration(cvm, &(cvm->line[cvm->logline]), curProc) < 0) {
                goto failure;
            }

            continue; /* don't copy in to new list */
        } else if(strcmp(GET_TOKEN(cvm->logline, 0), "stack") == 0) {
            if(cvm->line[cvm->logline].tokencount != 2) {
                LOG_PRINTF_TOK(cvm, "stack takes a number as argument.\n");
                goto failure;
            }

            long stack = strtol(GET_TOKEN(cvm->logline, 1), &temp, 0);
            if(*temp == '\0' && temp != GET_TOKEN(cvm->logline, 1)) {
                cvm->stacksize += stack;
            } else {
                LOG_PRINTF_TOK(cvm, "stack takes a number as argument.\n");
                goto failure;
            }

            continue; /* don't copy in to new list */
        } else if(strcmp(GET_TOKEN(cvm->logline, 0), "label") == 0) {
            if(cvm->line[cvm->logline].tokencount != 2) {
                LOG_PRINTF_TOK(cvm, "label takes a name as argument.\n");
                goto failure;
            }

            if(curProc == NULL) {
                LOG_PRINTF_TOK(cvm, "label not in a procedure.\n");
                goto failure;
            }

            temp = realloc(curProc->label, sizeof(CrustyLabel) * (curProc->labels + 1));
            if(temp == NULL) {
                LOG_PRINTF_TOK(cvm, "Failed to allocate memory for labels list.\n");
                goto failure;
            }
            curProc->label = (CrustyLabel *)temp;
            curProc->label[curProc->labels].nameOffset =
                cvm->line[cvm->logline].offset[1];
            curProc->label[curProc->labels].line = lines;
            curProc->labels++;

            continue; /* don't copy in to new list */
        }

        new[lines].tokencount = cvm->line[cvm->logline].tokencount;
        new[lines].moduleOffset = cvm->line[cvm->logline].moduleOffset;
        new[lines].line = cvm->line[cvm->logline].line;
        new[lines].offset = malloc(sizeof(long) * new[lines].tokencount);
        if(new[lines].offset == NULL) {
            LOG_PRINTF_TOK(cvm, "Failed to allocate memory for line copy.\n");
            goto failure;
        }
        for(i = 0; i < new[lines].tokencount; i++) {
            new[lines].offset[i] = cvm->line[cvm->logline].offset[i];
        }
        lines++;
    }

    if(curProc != NULL) {
        LOG_PRINTF(cvm, "Procedure without return.\n");
        goto failure;
    }

    /* memory allocation stuff */

    cvm->initialstack = 0;
    for(i = 0; i < cvm->vars; i++) {
        if(variable_is_global(&(cvm->var[i])) &&
           !variable_is_callback(&(cvm->var[i]))) {
            cvm->var[i].offset = cvm->initialstack;
            if(cvm->var[i].type == CRUSTY_TYPE_INT) {
                cvm->initialstack += (cvm->var[i].length * sizeof(int));
            } else if(cvm->var[i].type == CRUSTY_TYPE_FLOAT) {
                cvm->initialstack += (cvm->var[i].length * sizeof(double));
            } else { /* CHAR */
                cvm->initialstack += cvm->var[i].length;
            }
            /* make things aligned */
            if(cvm->initialstack % ALIGNMENT != 0) {
                cvm->initialstack += (ALIGNMENT -
                                      (cvm->initialstack % ALIGNMENT));
            }
        }
    }

    cvm->stacksize = cvm->initialstack;
    for(i = 0; i < cvm->procs; i++) {
        cvm->proc[i].var = malloc(sizeof(CrustyVariable *) * cvm->proc[i].vars);
        if(cvm->proc[i].var == NULL) {
            LOG_PRINTF(cvm, "Failed to allocate memory for procedure variable pointer list.\n");
            goto failure;
        }

        cvm->proc[i].stackneeded = 0;
        for(j = 0; j < cvm->proc[i].vars; j++) {
            /* point CrustyVariables in CrustyProcedures and vise versa */
            cvm->proc[i].var[j] = &(cvm->var[cvm->proc[i].varIndex[j]]);
            cvm->proc[i].var[j]->proc = &(cvm->proc[i]);

            /* set up offsets in to local stack for each variable in a procedure */

            if(variable_is_argument(cvm->proc[i].var[j])) {
                cvm->proc[i].var[j]->offset = j + 1; /* store argument number */
                cvm->proc[i].stackneeded += sizeof(CrustyStackArg);
            } else if(cvm->proc[i].var[j]->type == CRUSTY_TYPE_INT) {
                cvm->proc[i].stackneeded += (cvm->proc[i].var[j]->length * sizeof(int));
                cvm->proc[i].var[j]->offset = cvm->proc[i].stackneeded;
            } else if(cvm->proc[i].var[j]->type == CRUSTY_TYPE_FLOAT) {
                cvm->proc[i].stackneeded += (cvm->proc[i].var[j]->length * sizeof(double));
                cvm->proc[i].var[j]->offset = cvm->proc[i].stackneeded;
            } else { /* CHAR */
                cvm->proc[i].stackneeded += cvm->proc[i].var[j]->length;
                /* make things aligned */
                if(cvm->proc[i].stackneeded % ALIGNMENT != 0) {
                    cvm->proc[i].stackneeded += (ALIGNMENT - 
                                                 (cvm->proc[i].stackneeded % ALIGNMENT));
                }
                cvm->proc[i].var[j]->offset = cvm->proc[i].stackneeded;
            }
        }
        cvm->stacksize += cvm->proc[i].stackneeded;
    }

    /* allow at least enough stack for each function to be called once as a
       (hopefully) harmless convenience to the user, also set up offsets in the
       global stack */

    temp = realloc(new, sizeof(CrustyLine) * lines);
    if(temp == NULL) {
        LOG_PRINTF(cvm, "Failed to shrink line array.\n");
        goto failure;
    }

    for(i = 0; i < cvm->lines; i++) {
        free(cvm->line[i].offset);
    }
    free(cvm->line);
    cvm->line = (CrustyLine *)temp;
    cvm->lines = lines;

    return(0);

failure:
    if(new != NULL) {
        for(i = 0; i < lines; i++) {
            if(new[i].offset != NULL) {
                free(new[i].offset);
            }
        }
        free(new);
    }

    return(-1);
}

#undef LOG_PRINTF_TOK
#undef GET_TOKEN

/* get a bunch of messy checks out of the way.  Many of these are wacky and
   should be impossible but may as well get as much out of the way as possible. */
static int symbols_verify(CrustyVM *cvm) {
    unsigned int i, j, k;
    unsigned int leni, lenj, offi, offj;
    int ret = 0;

    for(i = 0; i < cvm->vars; i++) {
        if(variable_is_global(&(cvm->var[i]))) {
            if(cvm->var[i].length == 0) {
                LOG_PRINTF(cvm, "Global variable %s has 0 length.\n", cvm->var[i].name);
                ret = -1;
            }

            if(cvm->var[i].read == NULL && cvm->var[i].write == NULL) {
                if(cvm->var[i].intinit == NULL &&
                   cvm->var[i].floatinit == NULL &&
                   cvm->var[i].chrinit == NULL) {
                    LOG_PRINTF(cvm, "Non-callback variable %s has no initializer.\n",
                               cvm->var[i].name);
                    ret = -1;
                }

                if((cvm->var[i].intinit != NULL && cvm->var[i].chrinit != NULL) ||
                   (cvm->var[i].chrinit != NULL && cvm->var[i].floatinit != NULL) ||
                   (cvm->var[i].floatinit != NULL && cvm->var[i].intinit != NULL)) {
                    LOG_PRINTF(cvm, "Variable %s has multiple initializers set.\n",
                               cvm->var[i].name);
                    ret = -1;
                }

                if((cvm->var[i].type == CRUSTY_TYPE_INT && cvm->var[i].intinit == NULL) ||
                   (cvm->var[i].type == CRUSTY_TYPE_FLOAT && cvm->var[i].floatinit == NULL) ||
                   (cvm->var[i].type == CRUSTY_TYPE_CHAR && cvm->var[i].chrinit == NULL)) {
                   LOG_PRINTF(cvm, "Variable %s has no initializer for type.\n",
                              cvm->var[i].name);
                }

                if(cvm->var[i].intinit != NULL) {
                    leni = cvm->var[i].length * sizeof(int);
                } else if(cvm->var[i].floatinit != NULL) {
                    leni = cvm->var[i].length * sizeof(double);
                } else {
                    leni = cvm->var[i].length;
                }
                if(cvm->var[i].offset + leni > cvm->initialstack) {
                    LOG_PRINTF(cvm, "Global variable %s exceeds initial stack: "
                                    "%u + %u = %u > %u\n",
                                    cvm->var[i].name,
                                    cvm->var[i].offset,
                                    leni,
                                    cvm->var[i].offset + leni,
                                    cvm->initialstack);
                    ret = -1;
                }
                for(j = i + 1; j < cvm->vars; j++) {
                    if(variable_is_global(&(cvm->var[j]))) {
                        if(cvm->var[i].intinit != NULL) {
                            lenj = cvm->var[j].length * sizeof(int);
                        } else {
                            lenj = cvm->var[j].length;
                        }
                        if((cvm->var[j].offset > cvm->var[i].offset &&
                            cvm->var[j].offset < cvm->var[i].offset + leni - 1) ||
                           (cvm->var[j].offset + lenj - 1 > cvm->var[i].offset &&
                            cvm->var[j].offset + lenj - 1 < cvm->var[i].offset + leni - 1)) {
                            LOG_PRINTF(cvm, "Global variables %s and %s overlap: "
                                            "(%u -> %u) (%u -> %u)\n",
                                       cvm->var[i].name, cvm->var[j].name,
                                       cvm->var[i].offset,
                                       cvm->var[i].offset + leni - 1,
                                       cvm->var[j].offset,
                                       cvm->var[j].offset + lenj - 1);
                            ret = -1;
                        }
                    }
                }
            }
        } else { /* locals */
            if(variable_is_callback(&(cvm->var[i]))) {
                LOG_PRINTF(cvm, "Local variable %s with callback.\n", cvm->var[i].name);
                ret = -1;
            }

            if(cvm->var[i].length > 0 &&
               cvm->var[i].chrinit != NULL &&
               cvm->var[i].intinit != NULL &&
               cvm->var[i].floatinit != NULL) {
                LOG_PRINTF(cvm, "Local variable %s with length but NULL initializers.\n",
                           cvm->var[i].name);
                ret = -1;
            }

            for(j = 0; j < cvm->var[i].proc->vars; j++) {
                if(cvm->var[i].proc == cvm->var[i].proc->var[j]->proc) {
                    break;
                }
            }
            if(j == cvm->var[i].proc->vars) {
                LOG_PRINTF(cvm, "Couldn't find variable in procedure %s "
                                "referenced by variable %s.\n",
                           cvm->var[i].proc->name, cvm->var[i].name);
                ret = -1;
            }

            if(variable_is_argument(&(cvm->var[i]))) {
                if(cvm->var[i].intinit != NULL ||
                   cvm->var[i].chrinit != NULL ||
                   cvm->var[i].floatinit != NULL) {
                    LOG_PRINTF(cvm, "Local argument variable %s with initializer set.\n",
                               cvm->var[i].name);
                }
            } else {
                if((cvm->var[i].intinit != NULL && cvm->var[i].chrinit != NULL) ||
                   (cvm->var[i].chrinit != NULL && cvm->var[i].floatinit != NULL) ||
                   (cvm->var[i].floatinit != NULL && cvm->var[i].intinit != NULL)) {
                    LOG_PRINTF(cvm, "Local variable %s has multiple initializers set.\n",
                               cvm->var[i].name);
                    ret = -1;
                }

                if((cvm->var[i].type == CRUSTY_TYPE_INT && cvm->var[i].intinit == NULL) ||
                   (cvm->var[i].type == CRUSTY_TYPE_FLOAT && cvm->var[i].floatinit == NULL) ||
                   (cvm->var[i].type == CRUSTY_TYPE_CHAR && cvm->var[i].chrinit == NULL)) {
                   LOG_PRINTF(cvm, "Local variable %s has no initializer for type.\n",
                              cvm->var[i].name);
                }
            }
        }
    }

    for(i = 0; i < cvm->procs; i++) {
        for(j = 0; j < cvm->proc[i].vars; j++) {
            if(cvm->proc[i].var[j]->proc != &(cvm->proc[i])) {
                LOG_PRINTF(cvm, "Mispointed variable %s points to procedure %s "
                                "but pointed to by procedure %s.\n",
                           cvm->proc[i].var[j]->name,
                           cvm->proc[i].var[j]->proc->name,
                           cvm->proc[i].name);
                ret = -1;
            }

            if(cvm->proc[i].var[j]->length == 0) {
                if(j > cvm->proc[i].args) {
                    LOG_PRINTF(cvm, "Variable %s in proc %s has 0 length but "
                                    "index greater than args. (%u > %u)\n",
                               cvm->proc[i].var[j]->name,
                               cvm->proc[i].name,
                               j, cvm->proc[i].args);
                    ret = -1;
                }
                if(cvm->proc[i].var[j]->offset > cvm->proc[i].args) {
                    LOG_PRINTF(cvm, "Variable %s in proc %s is argument but "
                                    "stack offset greater than args. (%u > %u)\n",
                               cvm->proc[i].var[j]->name,
                               cvm->proc[i].name,
                               cvm->proc[i].var[j]->offset,
                               cvm->proc[i].args);
                    ret = -1;
                }
            }

            if(!variable_is_argument(cvm->proc[i].var[j])) {
                if(cvm->proc[i].var[j]->type == CRUSTY_TYPE_INT) {
                    lenj = cvm->proc[i].var[j]->length * sizeof(int);
                    offj = cvm->proc[i].var[j]->offset;
                    offj -= lenj; /* stack is indexed from top */
                } else if(cvm->proc[i].var[j]->type == CRUSTY_TYPE_FLOAT) {
                    lenj = cvm->proc[i].var[j]->length * sizeof(double);
                    offj = cvm->proc[i].var[j]->offset;
                    offj -= lenj; /* stack is indexed from top */
                } else { /* CHAR */
                    lenj = cvm->proc[i].var[j]->length;
                    offj = cvm->proc[i].var[j]->offset;
                    offj -= lenj;
                }
            } else {
                lenj = sizeof(CrustyStackArg);
                offj = cvm->proc[i].var[j]->offset * sizeof(CrustyStackArg);
                offj -= lenj;
            }
            if(offj > cvm->proc[i].stackneeded) {
                LOG_PRINTF(cvm, "Variable %s from procedure %s exceeds "
                                "needed stack: %u > %u\n",
                                cvm->proc[i].var[j]->name,
                                cvm->proc[i].name,
                                offj,
                                cvm->proc[i].stackneeded);
                ret = -1;
            }
            for(k = j + 1; k < cvm->proc[i].vars; k++) {
                if(!variable_is_argument(cvm->proc[i].var[k])) {
                    if(cvm->proc[i].var[k]->type == CRUSTY_TYPE_INT) {
                        leni = cvm->proc[i].var[k]->length * sizeof(int);
                        offi = cvm->proc[i].var[k]->offset;
                        offi -= leni; /* stack is indexed from top */
                    } else if(cvm->proc[i].var[k]->type == CRUSTY_TYPE_FLOAT) {
                        leni = cvm->proc[i].var[k]->length * sizeof(double);
                        offi = cvm->proc[i].var[k]->offset;
                        offi -= leni; /* stack is indexed from top */
                    } else { /* CHAR */
                        leni = cvm->proc[i].var[k]->length;
                        offi = cvm->proc[i].var[k]->offset;
                        offi -= leni;
                    }
                } else {
                    leni = sizeof(CrustyStackArg);
                    offi = cvm->proc[i].var[k]->offset * sizeof(CrustyStackArg);
                    offi -= leni;
                }
                if((offi > offj &&
                    offi < offj + lenj - 1) ||
                   (offi + leni - 1 > offj &&
                    offi + leni - 1 < offj + lenj - 1)) {
                    LOG_PRINTF(cvm, "Variables %s and %s from procedure %s "
                                    "overlap: (%u -> %u) (%u -> %u)\n",
                               cvm->proc[i].var[j]->name,
                               cvm->proc[i].var[k]->name,
                               cvm->proc[i].name,
                               offj, offj + lenj - 1,
                               offi, offi + leni - 1);
                    ret = -1;
                }
            }
        }
    }

    return(ret);
}

static int populate_var(CrustyVM *cvm,
                        char *name,
                        CrustyProcedure *proc,
                        int readable,
                        int writable,
                        int *flags,
                        int *var,
                        int *index) {
    CrustyVariable *varObj, *indexObj;
    char *colon;
    char *vararray = NULL;
    char *end;

    *flags = 0;
    *index = 0;

    /* test to see if we have an immediate value */
    *var = strtol(name, &end, 0);
    if(name != end && *end == '\0') {
        /* immediate value */
        if(writable) {
            LOG_PRINTF_LINE(cvm, "Immediate values aren't writable.\n");
            return(-1);
        }

        *flags |= MOVE_FLAG_IMMEDIATE;
        return(0);
    }

    /* find colon */
    colon = strrchr(name, ':');

    if(colon != NULL) {
        /* replace : with 0 and start at beginning of offset, if any */
        *colon = '\0';
        vararray = &(colon[1]);
    }

    *var = find_variable(cvm, proc, name);
    if(*var == -1) {
        LOG_PRINTF_LINE(cvm, "Variable %s not found.\n", name);
        goto failure;
    }
    varObj = &(cvm->var[*var]);

    if(writable) {
        if(varObj->read != NULL && varObj->write == NULL) {
            LOG_PRINTF_LINE(cvm, "%s isn't a writable callback.\n", name);
            goto failure;
        }
    }
    if(readable) {
        if(varObj->write != NULL && varObj->read == NULL) {
            LOG_PRINTF_LINE(cvm, "%s isn't a readable callback.\n", name);
            goto failure;
        }
    }

    if(vararray != NULL) {
        if(*vararray == '\0') {
            /* array length */
            if(writable) {
                LOG_PRINTF_LINE(cvm, "Array length isn't writable.\n");
                goto failure;
            }

            *flags |= MOVE_FLAG_LENGTH;
        } else {
            *flags |= MOVE_FLAG_VAR;
            *index = strtol(vararray, &end, 0);
            if(end != vararray && *end == '\0') {
                /* array with immediate index */
                if(*index < 0 || *index > (int)(varObj->length) - 1) {
                    LOG_PRINTF_LINE(cvm, "Immediate index out of array size.\n");
                    goto failure;
                }
                *flags |= MOVE_FLAG_INDEX_IMMEDIATE;
            } else {
                /* array with variable index */
                *index = find_variable(cvm, proc, vararray);
                if(*index == -1) {
                    LOG_PRINTF_LINE(cvm, "Array index variable %s not found.\n", vararray);
                    goto failure;
                }
                indexObj = &(cvm->var[*index]);

                if(indexObj->write != NULL &&
                   indexObj->read == NULL) {
                    LOG_PRINTF_LINE(cvm, "%s isn't a readable callback.\n", vararray);
                    goto failure;
                }

                *flags |= MOVE_FLAG_INDEX_VAR;
            }
        }
    } else {
        /* variable */
        *flags |= MOVE_FLAG_VAR | MOVE_FLAG_INDEX_IMMEDIATE;
        *index = 0;
    }

    if(colon != NULL) {
        *colon = ':';
    }
    return(0);

failure:
    if(colon != NULL) {
        *colon = ':';
    }
    return(-1);
}

static int *new_instruction(CrustyVM *cvm, unsigned int args) {
    int *temp;

    temp = realloc(cvm->inst, sizeof(int) * (cvm->insts + args + 1));
    if(temp == NULL) {
        LOG_PRINTF_LINE(cvm, "Failed to allocate memory for instructions.\n");
        return(NULL);
    }
    cvm->inst = temp;
    temp = &(cvm->inst[cvm->insts]);
    cvm->insts += (args + 1);

    return(temp);
}

static int find_label(CrustyProcedure *proc, const char *name) {
    unsigned int i;

    for(i = 0; i < proc->labels; i++) {
        if(strcmp(proc->label[i].name, name) == 0) {
            return(proc->label[i].line);
        }
    }

    return(-1);
}

#define MATH_INSTRUCTION(NAME, ENUM) \
    else if(strcmp(cvm->line[cvm->logline].token[0], NAME) == 0) { \
        if(cvm->line[cvm->logline].tokencount != 3) { \
            LOG_PRINTF_LINE(cvm, NAME " takes two operands.\n"); \
            return(-1); \
        } \
    \
        inst = new_instruction(cvm, MOVE_ARGS); \
        if(inst == NULL) { \
            return(-1); \
        } \
    \
        inst[0] = ENUM; \
    \
        if(populate_var(cvm, \
                        cvm->line[cvm->logline].token[1], \
                        curproc, \
                        1, 1, \
                        &(inst[MOVE_DEST_FLAGS]), \
                        &(inst[MOVE_DEST_VAL]), \
                        &(inst[MOVE_DEST_INDEX])) < 0) { \
            return(-1); \
        } \
    \
        if(populate_var(cvm, \
                        cvm->line[cvm->logline].token[2], \
                        curproc, \
                        1, 0, \
                        &(inst[MOVE_SRC_FLAGS]), \
                        &(inst[MOVE_SRC_VAL]), \
                        &(inst[MOVE_SRC_INDEX])) < 0) { \
            return(-1); \
        }

#define JUMP_INSTRUCTION(NAME, ENUM) \
    else if(strcmp(cvm->line[cvm->logline].token[0], NAME) == 0) { \
        if(cvm->line[cvm->logline].tokencount != 2) { \
            LOG_PRINTF_LINE(cvm, NAME " takes a label.\n"); \
            return(-1); \
        } \
    \
        inst = new_instruction(cvm, JUMP_ARGS); \
        if(inst == NULL) { \
            return(-1); \
        } \
    \
        inst[0] = ENUM; \
    \
        inst[JUMP_LOCATION] = find_label(curproc, cvm->line[cvm->logline].token[1]); \
        if(inst[JUMP_LOCATION] == -1) { \
            LOG_PRINTF_LINE(cvm, "Couldn't find label %s.\n", \
                            cvm->line[cvm->logline].token[1]); \
            return(-1); \
        }

static int codegen(CrustyVM *cvm) {
    CrustyProcedure *curproc = NULL;
    int procnum = 0;
    int *inst;
    unsigned int j;

    for(cvm->logline = 0; cvm->logline < cvm->lines; cvm->logline++) {
        if(curproc == NULL) {
            if(cvm->logline == cvm->proc[procnum].start) {
                curproc = &(cvm->proc[procnum]);
                curproc->instruction = cvm->insts;
            }
        }

        if(curproc == NULL) {
            LOG_PRINTF_LINE(cvm, "BUG: code line not in a procedure.\n");
            return(-1);
        }

        cvm->line[cvm->logline].instruction = cvm->insts;

        if(strcmp(cvm->line[cvm->logline].token[0], "move") == 0) {
            if(cvm->line[cvm->logline].tokencount != 3) {
                LOG_PRINTF_LINE(cvm, "move takes a destination and source.\n");
                return(-1);
            }

            inst = new_instruction(cvm, MOVE_ARGS);
            if(inst == NULL) {
                return(-1);
            }

            inst[0] = CRUSTY_INSTRUCTION_TYPE_MOVE;

            if(populate_var(cvm,
                            cvm->line[cvm->logline].token[1],
                            curproc,
                            0, 1,
                            &(inst[MOVE_DEST_FLAGS]),
                            &(inst[MOVE_DEST_VAL]),
                            &(inst[MOVE_DEST_INDEX])) < 0) {
                return(-1);
            }

            if(populate_var(cvm,
                            cvm->line[cvm->logline].token[2],
                            curproc,
                            1, 0,
                            &(inst[MOVE_SRC_FLAGS]),
                            &(inst[MOVE_SRC_VAL]),
                            &(inst[MOVE_SRC_INDEX])) < 0) {
                return(-1);
            }
        } MATH_INSTRUCTION("add", CRUSTY_INSTRUCTION_TYPE_ADD)
        } MATH_INSTRUCTION("sub", CRUSTY_INSTRUCTION_TYPE_SUB)
        } MATH_INSTRUCTION("mul", CRUSTY_INSTRUCTION_TYPE_MUL)
        } MATH_INSTRUCTION("div", CRUSTY_INSTRUCTION_TYPE_DIV)
        } MATH_INSTRUCTION("and", CRUSTY_INSTRUCTION_TYPE_AND)
        } MATH_INSTRUCTION("or",  CRUSTY_INSTRUCTION_TYPE_OR )
        } MATH_INSTRUCTION("xor", CRUSTY_INSTRUCTION_TYPE_XOR)
        } MATH_INSTRUCTION("shr", CRUSTY_INSTRUCTION_TYPE_SHR)
        } MATH_INSTRUCTION("shl", CRUSTY_INSTRUCTION_TYPE_SHL)
        } else if(strcmp(cvm->line[cvm->logline].token[0], "cmp") == 0) {
            if(cvm->line[cvm->logline].tokencount != 3) {
                LOG_PRINTF_LINE(cvm, "cmp takes two operands.\n");
                return(-1);
            }

            inst = new_instruction(cvm, MOVE_ARGS);
            if(inst == NULL) {
                return(-1);
            }

            inst[0] = CRUSTY_INSTRUCTION_TYPE_CMP;

            if(populate_var(cvm,
                            cvm->line[cvm->logline].token[1],
                            curproc,
                            1, 0,
                            &(inst[MOVE_DEST_FLAGS]),
                            &(inst[MOVE_DEST_VAL]),
                            &(inst[MOVE_DEST_INDEX])) < 0) {
                return(-1);
            }

            if(populate_var(cvm,
                            cvm->line[cvm->logline].token[2],
                            curproc,
                            1, 0,
                            &(inst[MOVE_SRC_FLAGS]),
                            &(inst[MOVE_SRC_VAL]),
                            &(inst[MOVE_SRC_INDEX])) < 0) {
                return(-1);
            }
        } JUMP_INSTRUCTION("jump",  CRUSTY_INSTRUCTION_TYPE_JUMP )
        } JUMP_INSTRUCTION("jumpn", CRUSTY_INSTRUCTION_TYPE_JUMPN)
        } JUMP_INSTRUCTION("jumpz", CRUSTY_INSTRUCTION_TYPE_JUMPZ)
        } JUMP_INSTRUCTION("jumpl", CRUSTY_INSTRUCTION_TYPE_JUMPL)
        } JUMP_INSTRUCTION("jumpg", CRUSTY_INSTRUCTION_TYPE_JUMPG)
        } else if(strcmp(cvm->line[cvm->logline].token[0], "call") == 0) {
            if(cvm->line[cvm->logline].tokencount < 2) {
                LOG_PRINTF_LINE(cvm, "call takes a procedure and possible arguments.\n");
                return(-1);
            }

            unsigned int args = cvm->line[cvm->logline].tokencount - 2;
            unsigned int i;

            inst = new_instruction(cvm, (args * 3) + 1);
            if(inst == NULL) {
                return(-1);
            }

            inst[0] = CRUSTY_INSTRUCTION_TYPE_CALL;

            inst[1] = find_procedure(cvm, cvm->line[cvm->logline].token[1]);
            if(inst[1] == -1) {
                LOG_PRINTF_LINE(cvm, "Couldn't find procedure %s.\n",
                                cvm->line[cvm->logline].token[1]);
                return(-1);
            }

            if(args != cvm->proc[inst[1]].args) {
                LOG_PRINTF_LINE(cvm, "Procedure %s takes %u args, %u given.\n",
                                cvm->proc[inst[1]].name,
                                cvm->proc[inst[1]].args,
                                args);
                return(-1);
            }

            for(i = 0; i < args; i++) {
                if(populate_var(cvm,
                            cvm->line[cvm->logline].token[i + 2],
                            curproc,
                            0, 0,
                            &(inst[CALL_START_ARGS + (i * CALL_ARG_SIZE) + CALL_ARG_FLAGS]),
                            &(inst[CALL_START_ARGS + (i * CALL_ARG_SIZE) + CALL_ARG_VAL]),
                            &(inst[CALL_START_ARGS + (i * CALL_ARG_SIZE) + CALL_ARG_INDEX]))
                            < 0 ) {
                    return(-1);
                }
            }
        } else if(strcmp(cvm->line[cvm->logline].token[0], "ret") == 0) {
            if(cvm->line[cvm->logline].tokencount != 1) {
                LOG_PRINTF_LINE(cvm, "ret takes no arguments.\n");
                return(-1);
            }

            inst = new_instruction(cvm, 0);
            if(inst == NULL) {
                return(-1);
            }

            inst[0] = CRUSTY_INSTRUCTION_TYPE_RET;

            procnum++;
            curproc = NULL;
        } else {
            LOG_PRINTF_LINE(cvm, "Invalid instruction mnemonic: %s\n",
                            cvm->line[cvm->logline].token[0]);
            return(-1);
        }
    }

    /* convert jump arguments from line to ip */
    for(j = 0; j < cvm->lines; j++) {
        switch(cvm->inst[cvm->line[j].instruction]) {
            case CRUSTY_INSTRUCTION_TYPE_JUMP:
            case CRUSTY_INSTRUCTION_TYPE_JUMPN:
            case CRUSTY_INSTRUCTION_TYPE_JUMPZ:
            case CRUSTY_INSTRUCTION_TYPE_JUMPL:
            case CRUSTY_INSTRUCTION_TYPE_JUMPG:
                cvm->inst[cvm->line[j].instruction + JUMP_LOCATION] =
                cvm->line[cvm->inst[cvm->line[j].instruction + JUMP_LOCATION]].instruction;
            default:
                break;
        }
    }

    return(0);
}

#undef JUMP_INSTRUCTION
#undef MATH_INSTRUCTION

/* do a lot of checking now so a lot can be skipped later when actually
   executing. */
static int check_move_arg(CrustyVM *cvm,
                          int dest,
                          int flags,
                          int val,
                          int index) {
    if((flags & MOVE_FLAG_TYPE_MASK) == MOVE_FLAG_IMMEDIATE) {
        if(dest) {
            LOG_PRINTF_LINE(cvm, "Destination flagged as immediate.\n");
            return(-1);
        }

#ifdef CRUSTY_TEST
        LOG_PRINTF_BARE(cvm, "%d", val);
#endif
    } else if((flags & MOVE_FLAG_TYPE_MASK) == MOVE_FLAG_LENGTH) {
        if(dest) {
            LOG_PRINTF_LINE(cvm, "Destination flagged as array length.\n");
            return(-1);
        }

        if(val < 0 || val > (int)(cvm->vars) - 1) {
            LOG_PRINTF_LINE(cvm, "Var out of range (%d).\n", val);
            return(-1);
        }

#ifdef CRUSTY_TEST
        LOG_PRINTF_BARE(cvm, "%d(%s):", val, cvm->var[val].name);
#endif
    } else if((flags & MOVE_FLAG_TYPE_MASK) == MOVE_FLAG_VAR) {
        if(val < 0 || val > (int)(cvm->vars) - 1) {
            LOG_PRINTF_LINE(cvm, "Var out of range (%d).\n", val);
            return(-1);
        }

        if(index < 0) {
            LOG_PRINTF_LINE(cvm, "Negative index %d.\n", index);
            return(-1);
        }

        if(dest) {
            if(cvm->var[val].read != NULL && cvm->var[val].write == NULL) {
                LOG_PRINTF_LINE(cvm, "Read only callback variable as "
                                     "destination (%s).\n", cvm->var[val].name);
                return(-1);
            }
        } else {
            if(cvm->var[val].write != NULL && cvm->var[val].read == NULL) {
                LOG_PRINTF_LINE(cvm, "Write only callback variable as "
                                     "source (%s).\n", cvm->var[val].name);
                return(-1);
            }
        }

        if((flags & MOVE_FLAG_INDEX_TYPE_MASK) == MOVE_FLAG_INDEX_VAR) {
            if(index < 0 || index > (int)(cvm->vars) - 1) {
                LOG_PRINTF_LINE(cvm, "Index var out of range (%d).\n", index);
                return(-1);
            }

            if(cvm->var[index].write != NULL && cvm->var[index].read == NULL) {
                LOG_PRINTF_LINE(cvm, "Write only callback variable "
                                     "as index (%s).\n", cvm->var[val].name);
                return(-1);
            }

#ifdef CRUSTY_TEST
            LOG_PRINTF_BARE(cvm, "%d(%s):%d(%s)",
                            val, cvm->var[val].name,
                            index, cvm->var[index].name);
#endif
        } else if((flags & MOVE_FLAG_INDEX_TYPE_MASK) == MOVE_FLAG_INDEX_IMMEDIATE) {
            if(index < 0 ||
               (cvm->var[val].length > 0 &&
                index > (int)(cvm->var[val].length) - 1)) {
                LOG_PRINTF_LINE(cvm, "Index out of range %d.\n", index);
                return(-1);
            }

#ifdef CRUSTY_TEST
            LOG_PRINTF_BARE(cvm, "%d(%s):%d", val, cvm->var[val].name, index);
#endif
        }
    } else {
        LOG_PRINTF_LINE(cvm, "Invalid variable type.\n");
        return(-1);
    }

    return(0);
}

static int check_math_instruction(CrustyVM *cvm,
                                  const char *name,
                                  unsigned int i,
                                  unsigned int notcmp) {
    if(i + MOVE_ARGS > cvm->insts - 1) {
        LOG_PRINTF_LINE(cvm, "Instruction memory ends before end "
                             "of %s instruction.\n", name);
        return(-1);
    }

#ifdef CRUSTY_TEST
    LOG_PRINTF_BARE(cvm, "%s ", name);
#endif
    if(check_move_arg(cvm,
                      notcmp,
                      cvm->inst[i+MOVE_DEST_FLAGS],
                      cvm->inst[i+MOVE_DEST_VAL],
                      cvm->inst[i+MOVE_DEST_INDEX]) < 0) {
        return(-1);
    }
#ifdef CRUSTY_TEST
    LOG_PRINTF_BARE(cvm, " ");
#endif
    if(check_move_arg(cvm,
                      0,
                      cvm->inst[i+MOVE_SRC_FLAGS],
                      cvm->inst[i+MOVE_SRC_VAL],
                      cvm->inst[i+MOVE_SRC_INDEX]) < 0) {
        return(-1);
    }
#ifdef CRUSTY_TEST
    LOG_PRINTF_BARE(cvm, "\n");
#endif
    return(0);
}

#define MATH_INSTRUCTION(NAME, NOTCMP) \
    if(check_math_instruction(cvm, NAME, i, (NOTCMP)) < 0) { \
        return(-1); \
    }

static int check_jump_instruction(CrustyVM *cvm,
                                  const char *name,
                                  CrustyProcedure *proc,
                                  unsigned int i) {
    unsigned int j;
    unsigned int line;
    unsigned int found;

    if(i + JUMP_ARGS > cvm->insts - 1) {
        LOG_PRINTF_LINE(cvm, "Instruction memory ends before end "
                             "of %s instruction.\n", name);
        return(-1);
    }

    found = 0;
    for(j = 0; j < cvm->lines; j++) {
        if(cvm->inst[i+JUMP_LOCATION] < 0 ) {
            LOG_PRINTF_LINE(cvm, "Negative jump pointer?\n");
            return(-1);
        }

        if((unsigned int)(cvm->inst[i+JUMP_LOCATION]) == cvm->line[j].instruction) {
            line = j;
            found = 1;
            break;
        }
    }

    if(found == 0) {
        LOG_PRINTF_LINE(cvm, "Jump argument doesn't land on an instruction.\n");
        return(-1);
    }

    if(proc != NULL) {
        if(line < proc->start || line > proc->start + proc->length) {
            LOG_PRINTF_LINE(cvm, "Jump outside of procedure.\n");
            return(-1);
        }
    }

#ifdef CRUSTY_TEST
    LOG_PRINTF_BARE(cvm, "%s %d\n", name, cvm->inst[i+JUMP_LOCATION]);
#endif
    return(0);
}

#define JUMP_INSTRUCTION(NAME) \
    if(proc == NULL) { \
        if(check_jump_instruction(cvm, NAME, NULL, i) < 0) { \
            return(-1); \
        } \
    } else { \
        if(check_jump_instruction(cvm, NAME, *proc, i) < 0) { \
            return(-1); \
        } \
    }

static int check_instruction(CrustyVM *cvm,
                      CrustyProcedure **proc,
                      unsigned int i) {
#ifdef CRUSTY_TEST
    LOG_PRINTF(cvm, "%d: ", i);
#endif
    switch(cvm->inst[i]) {
        case CRUSTY_INSTRUCTION_TYPE_MOVE:
            MATH_INSTRUCTION("move", 1)
            return(MOVE_ARGS + 1);
        case CRUSTY_INSTRUCTION_TYPE_ADD:
            MATH_INSTRUCTION("add", 1)
            return(MOVE_ARGS + 1);
        case CRUSTY_INSTRUCTION_TYPE_SUB:
            MATH_INSTRUCTION("sub", 1)
            return(MOVE_ARGS + 1);
        case CRUSTY_INSTRUCTION_TYPE_MUL:
            MATH_INSTRUCTION("mul", 1)
            return(MOVE_ARGS + 1);
        case CRUSTY_INSTRUCTION_TYPE_DIV:
            MATH_INSTRUCTION("div", 1)
            return(MOVE_ARGS + 1);
        case CRUSTY_INSTRUCTION_TYPE_AND:
            MATH_INSTRUCTION("and", 1)
            return(MOVE_ARGS + 1);
        case CRUSTY_INSTRUCTION_TYPE_OR:
            MATH_INSTRUCTION("or", 1)
            return(MOVE_ARGS + 1);
        case CRUSTY_INSTRUCTION_TYPE_XOR:
            MATH_INSTRUCTION("xor", 1)
            return(MOVE_ARGS + 1);
        case CRUSTY_INSTRUCTION_TYPE_SHR:
            MATH_INSTRUCTION("shr", 1)
            return(MOVE_ARGS + 1);
        case CRUSTY_INSTRUCTION_TYPE_SHL:
            MATH_INSTRUCTION("shl", 1)
            return(MOVE_ARGS + 1);
        case CRUSTY_INSTRUCTION_TYPE_CMP:
            MATH_INSTRUCTION("cmp", 0)
            return(MOVE_ARGS + 1);
        case CRUSTY_INSTRUCTION_TYPE_JUMP:
            JUMP_INSTRUCTION("jump")
            return(JUMP_ARGS + 1);
        case CRUSTY_INSTRUCTION_TYPE_JUMPN:
            JUMP_INSTRUCTION("jumpn")
            return(JUMP_ARGS + 1);
        case CRUSTY_INSTRUCTION_TYPE_JUMPZ:
            JUMP_INSTRUCTION("jumpz")
            return(JUMP_ARGS + 1);
        case CRUSTY_INSTRUCTION_TYPE_JUMPL:
            JUMP_INSTRUCTION("jumpl")
            return(JUMP_ARGS + 1);
        case CRUSTY_INSTRUCTION_TYPE_JUMPG:
            JUMP_INSTRUCTION("jumpg")
            return(JUMP_ARGS + 1);
        case CRUSTY_INSTRUCTION_TYPE_CALL:
            if(i + JUMP_ARGS > cvm->insts - 1) {
                LOG_PRINTF_LINE(cvm, "Instruction memory ends before end "
                                     "of call instruction.\n");
                return(-1);
            }

            if(cvm->inst[i+CALL_PROCEDURE] < 0 ||
               cvm->inst[i+CALL_PROCEDURE] > (int)(cvm->procs) - 1) {
                LOG_PRINTF_LINE(cvm, "Call to procedure out of range.\n");
                return(-1);
            }

            CrustyProcedure *callProc = &(cvm->proc[cvm->inst[i+CALL_PROCEDURE]]);
            unsigned int j;

#ifdef CRUSTY_TEST
            LOG_PRINTF_BARE(cvm, "call %d(%s)", cvm->inst[i+CALL_PROCEDURE],
                            callProc->name);
#endif

            for(j = 0; j < callProc->args; j++) {
#ifdef CRUSTY_TEST
                LOG_PRINTF_BARE(cvm, " ");
#endif
                if(check_move_arg(cvm,
                                  0,
                                  cvm->inst[i + CALL_START_ARGS + (j * CALL_ARG_SIZE) + CALL_ARG_FLAGS],
                                  cvm->inst[i + CALL_START_ARGS + (j * CALL_ARG_SIZE) + CALL_ARG_VAL],
                                  cvm->inst[i + CALL_START_ARGS + (j * CALL_ARG_SIZE) + CALL_ARG_INDEX]) < 0) {
                    return(-1);
                }
            }

#ifdef CRUSTY_TEST
            LOG_PRINTF_BARE(cvm, "\n");
#endif

            return(CALL_PROCEDURE + (callProc->args * 3) + 1);
        case CRUSTY_INSTRUCTION_TYPE_RET:
            /* takes no arguments so it can't end early */
#ifdef CRUSTY_TEST
            LOG_PRINTF_BARE(cvm, "ret\n");
#endif
            if(proc != NULL) {
                *proc = NULL;
            }

            return(RET_ARGS + 1);
        default:
            LOG_PRINTF_LINE(cvm, "Invalid instruction %u.\n", cvm->inst[i]);
            return(-1);
    }
}

#undef JUMP_INSTRUCTION
#undef MATH_INSTRUCTION

static int codeverify(CrustyVM *cvm) {
    CrustyProcedure *curproc = NULL;
    int procnum = 0;
    int instsize;
    unsigned int i = 0;
    cvm->logline = 0;

    while(i < cvm->insts) {
        if(curproc == NULL) {
            if(cvm->logline == cvm->proc[procnum].start) {
                curproc = &(cvm->proc[procnum]);
                procnum++;
#ifdef CRUSTY_TEST
                LOG_PRINTF(cvm, "proc %s\n", curproc->name);
#endif
            } else {
                LOG_PRINTF_LINE(cvm, "BUG: code line not in a procedure.\n");
                check_instruction(cvm, NULL, i);
                return(-1);
            }
        }

        instsize = check_instruction(cvm, &curproc, i);
        if(instsize < 0) {
            return(-1);
        }

        i += instsize;
        cvm->logline++;
    }

    if(curproc != NULL) {
        LOG_PRINTF_LINE(cvm, "Procedure without ret?\n");
        return(-1);
    }

    return(0);
}

int crustyvm_reset(CrustyVM *cvm) {
    unsigned int i;
    const char *temp = cvm->stage;

    cvm->stage = "reset";
#ifdef CRUSTY_TEST
    LOG_PRINTF(cvm, "Start\n");
#endif

    for(i = 0; i < cvm->vars; i++) {
        if(variable_is_global(&(cvm->var[i])) &&
           !variable_is_callback(&(cvm->var[i]))) {
            /* one of these cases has been assured by this point */
            if(cvm->var[i].type == CRUSTY_TYPE_INT) {
                memcpy(&(cvm->stack[cvm->var[i].offset]),
                       cvm->var[i].intinit,
                       cvm->var[i].length * sizeof(int));
            } else if(cvm->var[i].type == CRUSTY_TYPE_FLOAT) {
                memcpy(&(cvm->stack[cvm->var[i].offset]),
                       cvm->var[i].floatinit,
                       cvm->var[i].length * sizeof(double));
            } else { /* CHAR */
                memcpy(&(cvm->stack[cvm->var[i].offset]),
                       cvm->var[i].chrinit,
                       cvm->var[i].length);
            }
        }
    }

    cvm->status = CRUSTY_STATUS_READY;
    cvm->stage = temp;

    return(0);
}

#ifdef CRUSTY_TEST
static int write_lines(CrustyVM *cvm, const char *name, int byOffset) {
    FILE *out;
    unsigned int i, j;
    char *temp;

    out = fopen(name, "wb");
    if(out == NULL) {
        LOG_PRINTF(cvm, "Couldn't open file %s for writing.\n", name);
        return(-1);
    }

    for(i = 0; i < cvm->lines; i++) {
        for(j = 0; j < cvm->line[i].tokencount; j++) {
            if(byOffset) {
                temp = &(cvm->tokenmem[cvm->line[i].offset[j]]);
            } else {
                temp = cvm->line[i].token[j];
            }
            if(fprintf(out, "%s", temp) < 0) {
                LOG_PRINTF(cvm, "Couldn't write to file.\n");
                return(-1);
            }
            if(j < cvm->line[i].tokencount - 1) {
                if(fprintf(out, " ") < 0) {
                    LOG_PRINTF(cvm, "Couldn't write to file.\n");
                    return(-1);
                }
            }
        }
        if(fprintf(out, "\n") < 0) {
            LOG_PRINTF(cvm, "Couldn't write to file.\n");
            return(-1);
        }
    }

    fclose(out);

    return(0);
}
#endif

CrustyVM *crustyvm_new(const char *name,
                       const char *program,
                       long len,
                       unsigned int flags,
                       unsigned int callstacksize,
                       const CrustyCallback *cb,
                       unsigned int cbcount,
                       const char **var,
                       const char **value,
                       unsigned int vars,
                       void (*log_cb)(void *priv, const char *fmt, ...),
                       void *log_priv) {
    CrustyVM *cvm;
    int foundmacro;
    unsigned int i, j;
    char namebuffer[] = "preprocess###.cvm";
    long tokenstart;
    unsigned long *varOffset;
    unsigned long *valueOffset;

#ifdef CRUSTY_TEST
    unsigned int k;
#endif

    if(name == NULL) {
        /* nothing is defined yet so don't use the macro */
        log_cb(log_priv, "NULL passed as program name.\n");
        return(NULL);
    }

    cvm = init();
    if(cvm == NULL) {
        return(NULL);
    }

    cvm->flags = flags;

    cvm->log_cb = log_cb;
    cvm->log_priv = log_priv;

    cvm->stage = "tokenize";
#ifdef CRUSTY_TEST
    LOG_PRINTF(cvm, "Start\n");
#endif

    if(tokenize(cvm, name, program, len) < 0) {
        crustyvm_free(cvm);
        return(NULL);
    }

#ifdef CRUSTY_TEST
    if(cvm->flags & CRUSTY_FLAG_OUTPUT_PASSES) {
    /* this outputs ugly nonsense that's full of nulls and other crap and not
       really useful outside of debugging the program itself. */
        FILE *out;
        out = fopen("tokens.bin", "wb");
        if(out == NULL) {
            LOG_PRINTF(cvm, "Failed to open tokenizer output file for writing.\n");
            crustyvm_free(cvm);
            return(NULL);
        }

        if(fwrite(cvm->tokenmem, 1, cvm->tokenmemlen, out) <
           (unsigned long)cvm->tokenmemlen) {
            LOG_PRINTF(cvm, "Failed to write tokenizer output.\n");
            crustyvm_free(cvm);
            return(NULL);
        }
        fclose(out);
    }
#endif

    if(cvm->lines == 0) {
        LOG_PRINTF(cvm, "No lines remain after pass.\n");
        crustyvm_free(cvm);
        return(NULL);
    }

#ifdef CRUSTY_TEST
    if(cvm->flags & CRUSTY_FLAG_OUTPUT_PASSES) {
        FILE *out;
        out = fopen("tokens_meta.txt", "wb");
        if(out == NULL) {
            LOG_PRINTF(cvm, "Failed to open tokenizer metadata output file for writing.\n");
            crustyvm_free(cvm);
            return(NULL);
        }

        for(i = 0; i < cvm->lines; i++) {
            fprintf(out, "%s %04u ",
                    &(cvm->tokenmem[cvm->line[i].moduleOffset]),
                    cvm->line[i].line);
            for(j = 0; j < cvm->line[i].tokencount; j++) {
                fprintf(out, "%s", &(cvm->tokenmem[cvm->line[i].offset[j]]));
                if(j < cvm->line[i].tokencount - 1) {
                    fprintf(out, " ");
                }
            }
            fprintf(out, "\n");
        }
        fclose(out);
    }

    if(cvm->flags & CRUSTY_FLAG_OUTPUT_PASSES) {
        if(write_lines(cvm, "tokenize.cvm", 1) < 0) {
            LOG_PRINTF(cvm, "Failed to write tokenize pass.\n");
            crustyvm_free(cvm);
            return(NULL);
        }
    }
#endif

    cvm->stage = "input variables";
#ifdef CRUSTY_TEST
    LOG_PRINTF(cvm, "Start\n");
#endif

    varOffset = malloc(sizeof(long) * vars);
    if(varOffset == NULL) {
        LOG_PRINTF(cvm, "Failed to allocate memory for input variables list.\n");
        crustyvm_free(cvm);
        return(NULL);
    }
    valueOffset = malloc(sizeof(long) * vars);
    if(valueOffset == NULL) {
        LOG_PRINTF(cvm, "Failed to allocate memory for input values list.\n");
        crustyvm_free(cvm);
        return(NULL);
    }

    for(i = 0; i < vars; i++) {
        tokenstart = add_token(cvm, var[i], strlen(var[i]), 0, NULL);
        if(tokenstart < 0) {
            LOG_PRINTF(cvm, "Failed to allocate memory for input variable.\n");
            crustyvm_free(cvm);
            return(NULL);
        }
        varOffset[i] = tokenstart;
        tokenstart = add_token(cvm, value[i], strlen(value[i]), 0, NULL);
        if(tokenstart < 0) {
            LOG_PRINTF(cvm, "Failed to allocate memory for input value.\n");
            crustyvm_free(cvm);
            return(NULL);
        }
        valueOffset[i] = tokenstart;
    }

#ifdef CRUSTY_TEST
    for(i = 0; i < vars; i++) {
        LOG_PRINTF(cvm, "%s = %s\n",
                   &(cvm->tokenmem[varOffset[i]]),
                   &(cvm->tokenmem[valueOffset[i]]));
    }
#endif

    for(i = 0; i < MAX_PASSES; i++) {
        snprintf(namebuffer, sizeof(namebuffer), "preprocess %d", i + 1);
        cvm->stage = namebuffer;
#ifdef CRUSTY_TEST
        LOG_PRINTF(cvm, "Start\n", i);
#endif

        foundmacro = preprocess(cvm, varOffset, valueOffset, vars);
        if(foundmacro < 0) {
            LOG_PRINTF(cvm, "Failed preprocess at pass %d.\n", i + 1);
            crustyvm_free(cvm);
            return(NULL);
        }

        if(cvm->lines == 0) {
            LOG_PRINTF(cvm, "No lines remain after pass.\n");
            crustyvm_free(cvm);
            return(NULL);
        }

#ifdef CRUSTY_TEST
        if(cvm->flags & CRUSTY_FLAG_OUTPUT_PASSES) {
            snprintf(namebuffer, sizeof(namebuffer), "preprocess%03d.cvm", i + 1);
            if(write_lines(cvm, namebuffer, 1) < 0) {
                LOG_PRINTF(cvm, "Failed to write pass %d.\n", i + 1);
                crustyvm_free(cvm);
                return(NULL);
            }
        }
#endif

        if(foundmacro == 0) {
            break;
        }
    }
    free(varOffset);
    free(valueOffset);
    if(i == MAX_PASSES) {
        LOG_PRINTF(cvm, "Preprocess passes exceeded.\n");
        crustyvm_free(cvm);
        return(NULL);
    }

    cvm->stage = "adding callbacks";
#ifdef CRUSTY_TEST
    LOG_PRINTF(cvm, "Start\n");
#endif

    for(i = 0; i < cbcount; i++) {
        if(cb[i].read == NULL && cb[i].write == NULL) {
            LOG_PRINTF(cvm, "Callback variables must have a non-NULL read and/or write function.\n");
            crustyvm_free(cvm);
            return(NULL);
        }

        tokenstart = add_token(cvm, cb[i].name, strlen(cb[i].name), 0, NULL);
        if(tokenstart < 0) {
            LOG_PRINTF(cvm, "Failed to allocate memory for callback name.\n");
            crustyvm_free(cvm);
            return(NULL);
        }
        /* callback variables have 0 length and read or write must be non-NULL */
        if(new_variable(cvm,
                        tokenstart,
                        cb[i].type,
                        cb[i].length,
                        NULL, NULL, NULL,
                        &(cb[i]),
                        NULL) < 0) {
            /* reason will have already been printed */
            crustyvm_free(cvm);
            return(NULL);
        }
    }

    cvm->stage = "symbols scan";
#ifdef CRUSTY_TEST
    LOG_PRINTF(cvm, "Start\n");
#endif

    if(symbols_scan(cvm) < 0) {
        LOG_PRINTF(cvm, "Symbols scan failed.\n");
        crustyvm_free(cvm);
        return(NULL);
    }

    if(cvm->lines == 0) {
        LOG_PRINTF(cvm, "No lines remain after pass.\n");
        crustyvm_free(cvm);
        return(NULL);
    }

    /* all tokens have been loaded in/created, so the pointers in to tokenmem
       won't change, so token offsets can be turned in to normal char * pointers
       and the memory used to store the offsets can be freed up */
    for(i = 0; i < cvm->lines; i++) {
        cvm->line[i].module = &(cvm->tokenmem[cvm->line[i].moduleOffset]);
        cvm->line[i].token = malloc(sizeof(char *) * cvm->line[i].tokencount);
        if(cvm->line[i].token == NULL) {
            LOG_PRINTF(cvm, "Failed to allocate memory for token pointer list.\n");
            crustyvm_free(cvm);
            return(NULL);
        }
        for(j = 0; j < cvm->line[i].tokencount; j++) {
            cvm->line[i].token[j] = &(cvm->tokenmem[cvm->line[i].offset[j]]);
        }
        free(cvm->line[i].offset);
        cvm->line[i].offset = NULL;
    }

    for(i = 0; i < cvm->procs; i++) {
        cvm->proc[i].name = &(cvm->tokenmem[cvm->proc[i].nameOffset]);
        for(j = 0; j < cvm->proc[i].labels; j++) {
            cvm->proc[i].label[j].name =
                &(cvm->tokenmem[cvm->proc[i].label[j].nameOffset]);
        }
    }

    for(i = 0; i < cvm->vars; i++) {
        cvm->var[i].name = &(cvm->tokenmem[cvm->var[i].nameOffset]);
    }

#ifdef CRUSTY_TEST
    /* output a text file because it is no longer a valid cvm source file */
    if(cvm->flags & CRUSTY_FLAG_OUTPUT_PASSES) {
        if(write_lines(cvm, "symbols scan.txt", 0) < 0) {
            LOG_PRINTF(cvm, "Failed to write tokenize pass.\n");
            crustyvm_free(cvm);
            return(NULL);
        }
    }

    cvm->stage = "symbols list";
    LOG_PRINTF(cvm, "Global Variables:\n");
    for(i = 0; i < cvm->vars; i++) {
        if(variable_is_global(&(cvm->var[i]))) {
            LOG_PRINTF(cvm, " %s", cvm->var[i].name);
            if(cvm->var[i].read != NULL) {
                LOG_PRINTF_BARE(cvm, " r");
            }
            if(cvm->var[i].write != NULL) {
                LOG_PRINTF_BARE(cvm, " w");
            }
            LOG_PRINTF_BARE(cvm, "\n");
            if(cvm->var[i].length > 0) {
                if(cvm->var[i].chrinit != NULL) {
                    LOG_PRINTF(cvm, "  String initializer: \"");
                    for(j = 0; j < cvm->var[i].length; j++) {
                        LOG_PRINTF_BARE(cvm, "%c", cvm->var[i].chrinit[j]);
                    }
                    LOG_PRINTF_BARE(cvm, "\"");
                }
                if(cvm->var[i].intinit != NULL) {
                    LOG_PRINTF(cvm, "  Integer initializer:");
                    for(j = 0; j < cvm->var[i].length; j++) {
                        LOG_PRINTF_BARE(cvm, " %d", cvm->var[i].intinit[j]);
                    }
                }
                if(cvm->var[i].floatinit != NULL) {
                    LOG_PRINTF(cvm, "  Float initializer:");
                    for(j = 0; j < cvm->var[i].length; j++) {
                        LOG_PRINTF_BARE(cvm, " %g", cvm->var[i].floatinit[j]);
                    }
                }
                LOG_PRINTF_BARE(cvm, "\n");
            }
        }
    }
    for(i = 0; i < cvm->procs; i++) {
        LOG_PRINTF(cvm, "Procedure: %s @%u, %u, args: %u\n", cvm->proc[i].name,
                                                             cvm->proc[i].start,
                                                             cvm->proc[i].length,
                                                             cvm->proc[i].args);
        if(cvm->proc[i].vars > 0) {
            LOG_PRINTF(cvm, " Variables:\n");
            for(j = 0; j < cvm->proc[i].vars; j++) {
                LOG_PRINTF(cvm, "  %s", cvm->proc[i].var[j]->name);
                if(j < cvm->proc[i].args) {
                    LOG_PRINTF_BARE(cvm, " arg %u\n", j);
                } else {
                    LOG_PRINTF_BARE(cvm, "\n");
                }
                if(cvm->proc[i].var[j]->length > 0) {
                    if(cvm->proc[i].var[j]->chrinit != NULL) {
                        LOG_PRINTF(cvm, "   String initializer: \"");
                        for(k = 0; k < cvm->proc[i].var[j]->length; k++) {
                            LOG_PRINTF_BARE(cvm, "%c", cvm->proc[i].var[j]->chrinit[k]);
                        }
                        LOG_PRINTF_BARE(cvm, "\"");
                    }
                    if(cvm->proc[i].var[j]->intinit != NULL) {
                        LOG_PRINTF(cvm, "   Integer initializer:");
                        for(k = 0; k < cvm->proc[i].var[j]->length; k++) {
                            LOG_PRINTF_BARE(cvm, " %d", cvm->proc[i].var[j]->intinit[k]);
                        }
                    }
                    if(cvm->proc[i].var[j]->floatinit != NULL) {
                        LOG_PRINTF(cvm, "   Float initializer:");
                        for(k = 0; k < cvm->proc[i].var[j]->length; k++) {
                            LOG_PRINTF_BARE(cvm, " %g", cvm->proc[i].var[j]->floatinit[k]);
                        }
                    }
                    LOG_PRINTF_BARE(cvm, "\n");
                }
                if(cvm->proc[i].var[j]->proc != &(cvm->proc[i])) {
                    LOG_PRINTF(cvm, "   Improperly pointed procedure!\n");
                }
            }
        }
        if(cvm->proc[i].labels > 0) {
            LOG_PRINTF(cvm, " Labels:\n");
            for(j = 0; j < cvm->proc[i].labels; j++) {
                LOG_PRINTF(cvm, "  %s @%u\n",
                    cvm->proc[i].label[j].name,
                    cvm->proc[i].label[j].line);
            }
        }
    }
#endif

    cvm->stage = "symbols verification";
#ifdef CRUSTY_TEST
    LOG_PRINTF(cvm, "Start\n");
#endif

    if(symbols_verify(cvm) < 0) {
        LOG_PRINTF(cvm, "Symbols verification failed.\n");
        crustyvm_free(cvm);
        return(NULL);
    }

    cvm->stage = "code generation";
#ifdef CRUSTY_TEST
    LOG_PRINTF(cvm, "Start\n");
#endif

    if(codegen(cvm) < 0) {
        LOG_PRINTF(cvm, "Code generation failed.\n");
        crustyvm_free(cvm);
        return(NULL);
    }

    cvm->stage = "code verification";
#ifdef CRUSTY_TEST
    LOG_PRINTF(cvm, "Start\n");
#endif

    if(codeverify(cvm) < 0) {
        LOG_PRINTF(cvm, "Code verification failed.\n");
        crustyvm_free(cvm);
        return(NULL);
    }

    cvm->stage = "memory allocation";
#ifdef CRUSTY_TEST
    LOG_PRINTF(cvm, "Start\n");
#endif

    cvm->stack = malloc(cvm->stacksize);
    if(cvm->stack == NULL) {
        LOG_PRINTF(cvm, "Failed to allocate stack memory.\n");
        crustyvm_free(cvm);
        return(NULL);
    }

    if(callstacksize == 0) {
        cvm->callstacksize = DEFAULT_CALLSTACK_SIZE;
    } else {
        cvm->callstacksize = callstacksize;
    }

    cvm->cstack = malloc(sizeof(CrustyCallStackArg) * cvm->callstacksize);
    if(cvm->cstack == NULL) {
        LOG_PRINTF(cvm, "Failed to allocate callstack memory.\n");
        crustyvm_free(cvm);
        return(NULL);
    }

    if(crustyvm_reset(cvm) < 0) {
        crustyvm_free(cvm);
        return(NULL);
    }

    return(cvm);
}

static int read_var(CrustyVM *cvm,
                    int *intval,
                    double *floatval,
                    int ptr,
                    CrustyVariable *var,
                    unsigned int index) {
    if(var->read != NULL) {
        if(var->type == CRUSTY_TYPE_CHAR) {
            return(var->read(var->readpriv, intval, index));
        } else if(var->type == CRUSTY_TYPE_FLOAT) {
            return(var->read(var->readpriv, floatval, index));
        } else { /* INT */
            return(var->read(var->readpriv, intval, index));
        }
    }

    if(var->type == CRUSTY_TYPE_CHAR) {
        *intval = (int)(cvm->stack[ptr + index]);
    } else if(var->type == CRUSTY_TYPE_FLOAT) {
        *floatval = *((double *)(&(cvm->stack[ptr + (index * sizeof(double))])));
    } else { /* INT */
        *intval = *((int *)(&(cvm->stack[ptr + (index * sizeof(int))])));
    }

    return(0);
}

static int write_var(CrustyVM *cvm,
                     int intval,
                     double floatval,
                     int ptr,
                     CrustyVariable *var,
                     unsigned int index) {
    if(var->write != NULL) {
        if(var->type == CRUSTY_TYPE_CHAR) {
            return(var->write(var->writepriv, &intval, index));
        } else if(var->type == CRUSTY_TYPE_FLOAT) {
            return(var->write(var->writepriv, &floatval, index));
        } else { /* INT */
            return(var->write(var->writepriv, &intval, index));
        }
    }

    if(var->type == CRUSTY_TYPE_CHAR) {
        cvm->stack[ptr + index] = ((char)intval);
    } else if(var->type == CRUSTY_TYPE_FLOAT) {
        *((double *)(&(cvm->stack[ptr + (index * sizeof(double))]))) = floatval;
    } else { /* INT */
        *((int *)(&(cvm->stack[ptr + (index * sizeof(int))]))) = intval;
    }

    return(0);
}

#define STACK_ARG(STACKSTART, IDX) \
    ((CrustyStackArg *)(&(cvm->stack[(STACKSTART) - \
                                     (sizeof(CrustyStackArg) * (IDX))])))

#define GET_PTR(VAR, SP) \
    (variable_is_global(&(cvm->var[VAR])) ? \
        (cvm->var[VAR].offset) : \
        ((SP) - cvm->var[VAR].offset))

/* see variable permutations.txt for more information on what these do */

/* only returns an index to another variable (which may contain a float) or an
   integer */
int update_src_ref(CrustyVM *cvm,
                   int *flags,
                   int *val,
                   int *index,
                   int *ptr) {
    if((*flags & MOVE_FLAG_TYPE_MASK) == MOVE_FLAG_VAR) {
        if(variable_is_argument(&(cvm->var[*val]))) {
            if((STACK_ARG(cvm->sp, cvm->var[*val].offset)->flags &
                MOVE_FLAG_TYPE_MASK) ==
               MOVE_FLAG_VAR) {
                if((*flags & MOVE_FLAG_INDEX_TYPE_MASK) == MOVE_FLAG_INDEX_VAR) {
                    if(variable_is_argument(&(cvm->var[*index]))) {
                        if((STACK_ARG(cvm->sp, cvm->var[*index].offset)->flags &
                           MOVE_FLAG_TYPE_MASK) == MOVE_FLAG_VAR) {
                            if(cvm->var[STACK_ARG(cvm->sp,
                                                  cvm->var[*index].offset)->val].type ==
                               CRUSTY_TYPE_FLOAT) {
                                cvm->status = CRUSTY_STATUS_FLOAT_INDEX;
                                return(-1);
                            }

                            if(read_var(cvm,
                                        index,
                                        NULL,
                                        STACK_ARG(cvm->sp, cvm->var[*index].offset)->ptr,
                                        &(cvm->var[STACK_ARG(cvm->sp,
                                                             cvm->var[*index].offset)->val]),
                                        STACK_ARG(cvm->sp,
                                                  cvm->var[*index].offset)->index) < 0) {
                                cvm->status = CRUSTY_STATUS_CALLBACK_FAILED;
                                return(-1);
                            }
                        } else {
                            *index = STACK_ARG(cvm->sp, cvm->var[*index].offset)->val;
                        }
                    } else {
                        if(cvm->var[*index].type == CRUSTY_TYPE_FLOAT) {
                            cvm->status = CRUSTY_STATUS_FLOAT_INDEX;
                            return(-1);
                        }

                        if(read_var(cvm,
                                    index,
                                    NULL,
                                    GET_PTR(*index, *ptr),
                                    &(cvm->var[*index]),
                                    0) < 0) {
                            cvm->status = CRUSTY_STATUS_CALLBACK_FAILED;
                            return(-1);
                        }
                    }
                } /* else {
                    do nothing
                } */
                if(*index < 0) {
                    cvm->status = CRUSTY_STATUS_OUT_OF_RANGE;
                    return(-1);
                }

                *index += STACK_ARG(cvm->sp, cvm->var[*val].offset)->index;
                if(*index >
                   (int)(cvm->var[STACK_ARG(cvm->sp, 
                                            cvm->var[*val].offset)->val].length -
                   1)) {
                    cvm->status = CRUSTY_STATUS_OUT_OF_RANGE;
                    return(-1);
                }

                *flags = MOVE_FLAG_VAR;
                *ptr = STACK_ARG(cvm->sp, cvm->var[*val].offset)->ptr;
                *val = STACK_ARG(cvm->sp, cvm->var[*val].offset)->val;
                /* index is already updated */
            } else {
                if((*flags & MOVE_FLAG_INDEX_TYPE_MASK) == MOVE_FLAG_INDEX_VAR) {
                    if(variable_is_argument(&(cvm->var[*index]))) {
                        if((STACK_ARG(cvm->sp, cvm->var[*index].offset)->flags &
                            MOVE_FLAG_TYPE_MASK) ==
                           MOVE_FLAG_VAR) {
                            if(cvm->var[STACK_ARG(cvm->sp,
                                                  cvm->var[*index].offset)->val].type ==
                               CRUSTY_TYPE_FLOAT) {
                                cvm->status = CRUSTY_STATUS_FLOAT_INDEX;
                                return(-1);
                            }

                            if(read_var(cvm,
                                        index,
                                        NULL,
                                        STACK_ARG(cvm->sp, cvm->var[*index].offset)->ptr,
                                        &(cvm->var[STACK_ARG(cvm->sp,
                                                             cvm->var[*index].offset)->val]),
                                        STACK_ARG(cvm->sp,
                                                  cvm->var[*index].offset)->index) < 0) {
                                cvm->status = CRUSTY_STATUS_CALLBACK_FAILED;
                                return(-1);
                            }
                        } else {
                            *index = STACK_ARG(cvm->sp, cvm->var[*index].offset)->val;
                        }
                    } else {
                        if(cvm->var[*index].type == CRUSTY_TYPE_FLOAT) {
                            cvm->status = CRUSTY_STATUS_FLOAT_INDEX;
                            return(-1);
                        }

                        if(read_var(cvm,
                                    index,
                                    NULL,
                                    GET_PTR(*index, *ptr),
                                    &(cvm->var[*index]),
                                    0) < 0) {
                            cvm->status = CRUSTY_STATUS_CALLBACK_FAILED;
                            return(-1);
                        }
                    }
                } /* else {
                    do nothing
                } */
                if(*index < 0 || *index > 0) {
                    cvm->status = CRUSTY_STATUS_OUT_OF_RANGE;
                    return(-1);
                }

                *flags = MOVE_FLAG_IMMEDIATE;
                *val = STACK_ARG(cvm->sp, cvm->var[*val].offset)->val;
            }
        } else {
            if((*flags & MOVE_FLAG_INDEX_TYPE_MASK) == MOVE_FLAG_INDEX_VAR) {
                if(variable_is_argument(&(cvm->var[*index]))) {
                    if((STACK_ARG(cvm->sp, cvm->var[*index].offset)->flags &
                       MOVE_FLAG_TYPE_MASK) == MOVE_FLAG_VAR) {
                        if(cvm->var[STACK_ARG(cvm->sp,
                                              cvm->var[*index].offset)->val].type ==
                           CRUSTY_TYPE_FLOAT) {
                            cvm->status = CRUSTY_STATUS_FLOAT_INDEX;
                            return(-1);
                        }

                        if(read_var(cvm,
                                    index,
                                    NULL,
                                    STACK_ARG(cvm->sp, cvm->var[*index].offset)->ptr,
                                    &(cvm->var[STACK_ARG(cvm->sp,
                                                         cvm->var[*index].offset)->val]),
                                    STACK_ARG(cvm->sp,
                                              cvm->var[*index].offset)->index) < 0) {
                            cvm->status = CRUSTY_STATUS_CALLBACK_FAILED;
                            return(-1);
                        }
                    } else {
                        *index = STACK_ARG(cvm->sp, cvm->var[*index].offset)->val;
                    }
                } else {
                    if(cvm->var[*index].type == CRUSTY_TYPE_FLOAT) {
                        cvm->status = CRUSTY_STATUS_FLOAT_INDEX;
                        return(-1);
                    }

                    if(read_var(cvm,
                                index,
                                NULL,
                                GET_PTR(*index, *ptr),
                                &(cvm->var[*index]),
                                0) < 0) {
                        cvm->status = CRUSTY_STATUS_CALLBACK_FAILED;
                        return(-1);
                    }
                }
            } /* else {
                do nothing
            } */
            if(*index < 0 || *index > (int)(cvm->var[*val].length - 1)) {
                cvm->status = CRUSTY_STATUS_OUT_OF_RANGE;
                return(-1);
            }

            *flags = MOVE_FLAG_VAR;
            /* val and index are already correct */
            *ptr = GET_PTR(*val, *ptr);
        }
    } else if((*flags & MOVE_FLAG_TYPE_MASK) == MOVE_FLAG_LENGTH) {
        if(variable_is_argument(&(cvm->var[*val]))) {
            if((STACK_ARG(cvm->sp, cvm->var[*val].offset)->flags &
                MOVE_FLAG_TYPE_MASK) ==
               MOVE_FLAG_VAR) {
                *flags = MOVE_FLAG_IMMEDIATE;
                *index = STACK_ARG(cvm->sp, cvm->var[*val].offset)->index;
                *val = cvm->var[STACK_ARG(cvm->sp,
                                          cvm->var[*val].offset)->val].length -
                       *index;
            } else {
                *flags = MOVE_FLAG_IMMEDIATE;
                *val = 1;
            }
        } else {
            *flags = MOVE_FLAG_IMMEDIATE;
            *val = cvm->var[*val].length;
        }
    } else if((*flags & MOVE_FLAG_TYPE_MASK) == MOVE_FLAG_IMMEDIATE) {
        *flags = MOVE_FLAG_IMMEDIATE;
        /* val is already val */
    } else {
        cvm->status = CRUSTY_STATUS_INTERNAL_ERROR;
        return(-1);
    }

    return(0);
}

static int call(CrustyVM *cvm, unsigned int procindex, unsigned int argsindex) {
    unsigned int i;
    unsigned int newsp;
    CrustyProcedure *callee;
    int flags, val, index, ptr;

    if(cvm->csp == cvm->callstacksize) {
        cvm->status = CRUSTY_STATUS_STACK_OVERFLOW;
        return(-1);
    }

    if(cvm->sp + cvm->proc[procindex].stackneeded > cvm->stacksize) {
        cvm->status = CRUSTY_STATUS_STACK_OVERFLOW;
        return(-1);
    }

    callee = &(cvm->proc[procindex]);

    /* push return and procedure to be called on to the stack */
    cvm->csp++;
    /* make the return ip start at the next instruction */
    cvm->cstack[cvm->csp - 1].ip =
        argsindex + (cvm->proc[procindex].args * CALL_ARG_SIZE);
    cvm->cstack[cvm->csp - 1].proc = procindex;

    newsp = cvm->sp + cvm->proc[procindex].stackneeded;

    /* set up procedure arguments */
    for(i = 0; i < callee->args; i++) {
        flags = cvm->inst[argsindex + (i * CALL_ARG_SIZE) + CALL_ARG_FLAGS];
        val = cvm->inst[argsindex + (i * CALL_ARG_SIZE) + CALL_ARG_VAL];
        index = cvm->inst[argsindex + (i * CALL_ARG_SIZE) + CALL_ARG_INDEX];
        ptr = cvm->sp;

        if(update_src_ref(cvm, &flags, &val, &index, &ptr) < 0) {
            return(-1);
        }

        STACK_ARG(newsp, i + 1)->flags = flags;
        STACK_ARG(newsp, i + 1)->val = val;
        STACK_ARG(newsp, i + 1)->index = index;
        STACK_ARG(newsp, i + 1)->ptr = ptr;
    }

    /* initialize local variables */
    for(i = callee->args; i < callee->vars; i++) {
        if(callee->var[i]->type == CRUSTY_TYPE_CHAR) {
            memcpy(&(cvm->stack[newsp - callee->var[i]->offset]),
                   callee->var[i]->chrinit,
                   callee->var[i]->length);
        } else if(callee->var[i]->type == CRUSTY_TYPE_FLOAT) {
            memcpy(&(cvm->stack[newsp - callee->var[i]->offset]),
                   callee->var[i]->floatinit,
                   callee->var[i]->length * sizeof(double));
        } else { /* INT */
            memcpy(&(cvm->stack[newsp - callee->var[i]->offset]),
                   callee->var[i]->intinit,
                   callee->var[i]->length * sizeof(int));
        } /* other possibilities will have already been checked before */
    }

    cvm->sp = newsp;
    cvm->ip = callee->instruction;

    return(0);
}

int update_dest_ref(CrustyVM *cvm,
                    int *flags,
                    int *val,
                    int *index,
                    int *ptr) {
    if((*flags & MOVE_FLAG_TYPE_MASK) == MOVE_FLAG_VAR) {
        if(variable_is_argument(&(cvm->var[*val]))) {
            if((STACK_ARG(cvm->sp, cvm->var[*val].offset)->flags &
                MOVE_FLAG_TYPE_MASK) ==
               MOVE_FLAG_VAR) {
                if((*flags & MOVE_FLAG_INDEX_TYPE_MASK) == MOVE_FLAG_INDEX_VAR) {
                    if(variable_is_argument(&(cvm->var[*index]))) {
                        if((STACK_ARG(cvm->sp, cvm->var[*index].offset)->flags &
                           MOVE_FLAG_TYPE_MASK) == MOVE_FLAG_VAR) {
                            if(cvm->var[STACK_ARG(cvm->sp,
                                                  cvm->var[*index].offset)->val].type ==
                               CRUSTY_TYPE_FLOAT) {
                                cvm->status = CRUSTY_STATUS_FLOAT_INDEX;
                                return(-1);
                            }

                            if(read_var(cvm,
                                        index,
                                        NULL,
                                        STACK_ARG(cvm->sp, cvm->var[*index].offset)->ptr,
                                        &(cvm->var[STACK_ARG(cvm->sp,
                                                             cvm->var[*index].offset)->val]),
                                        STACK_ARG(cvm->sp,
                                                  cvm->var[*index].offset)->index) < 0) {
                                cvm->status = CRUSTY_STATUS_CALLBACK_FAILED;
                                return(-1);
                            }
                        } else {
                            *index = STACK_ARG(cvm->sp, cvm->var[*index].offset)->val;
                        }
                    } else {
                        if(cvm->var[*index].type == CRUSTY_TYPE_FLOAT) {
                            cvm->status = CRUSTY_STATUS_FLOAT_INDEX;
                            return(-1);
                        }

                        if(read_var(cvm,
                                    index,
                                    NULL,
                                    GET_PTR(*index, *ptr),
                                    &(cvm->var[*index]),
                                    0) < 0) {
                            cvm->status = CRUSTY_STATUS_CALLBACK_FAILED;
                            return(-1);
                        }
                    }
                } /* else {
                    do nothing
                } */
                if(*index < 0) {
                    cvm->status = CRUSTY_STATUS_OUT_OF_RANGE;
                    return(-1);
                }

                *index += STACK_ARG(cvm->sp, cvm->var[*val].offset)->index;
                if(*index >
                   (int)(cvm->var[STACK_ARG(cvm->sp, 
                                            cvm->var[*val].offset)->val].length -
                   1)) {
                    cvm->status = CRUSTY_STATUS_OUT_OF_RANGE;
                    return(-1);
                }

                *ptr = STACK_ARG(cvm->sp, cvm->var[*val].offset)->ptr;
                *val = STACK_ARG(cvm->sp, cvm->var[*val].offset)->val;
                /* index is already updated */
            } else {
                if((*flags & MOVE_FLAG_INDEX_TYPE_MASK) == MOVE_FLAG_INDEX_VAR) {
                    if(variable_is_argument(&(cvm->var[*index]))) {
                        if((STACK_ARG(cvm->sp, cvm->var[*index].offset)->flags &
                            MOVE_FLAG_TYPE_MASK) ==
                           MOVE_FLAG_VAR) {
                            if(cvm->var[STACK_ARG(cvm->sp,
                                                  cvm->var[*index].offset)->val].type ==
                               CRUSTY_TYPE_FLOAT) {
                                cvm->status = CRUSTY_STATUS_FLOAT_INDEX;
                                return(-1);
                            }

                            if(read_var(cvm,
                                        index,
                                        NULL,
                                        STACK_ARG(cvm->sp, cvm->var[*index].offset)->ptr,
                                        &(cvm->var[STACK_ARG(cvm->sp,
                                                             cvm->var[*index].offset)->val]),
                                        STACK_ARG(cvm->sp,
                                                  cvm->var[*index].offset)->index) < 0) {
                                cvm->status = CRUSTY_STATUS_CALLBACK_FAILED;
                                return(-1);
                            }
                        } else {
                            *index = STACK_ARG(cvm->sp, cvm->var[*index].offset)->val;
                        }
                    } else {
                        if(cvm->var[*index].type == CRUSTY_TYPE_FLOAT) {
                            cvm->status = CRUSTY_STATUS_FLOAT_INDEX;
                            return(-1);
                        }

                        if(read_var(cvm,
                                    index,
                                    NULL,
                                    GET_PTR(*index, *ptr),
                                    &(cvm->var[*index]),
                                    0) < 0) {
                            cvm->status = CRUSTY_STATUS_CALLBACK_FAILED;
                            return(-1);
                        }
                    }
                } /* else {
                    do nothing
                } */
                if(*index < 0 || *index > 0) {
                    cvm->status = CRUSTY_STATUS_OUT_OF_RANGE;
                    return(-1);
                }

                /* special case but should be safe since the ptr will point to
                   the immediate value on the stack and index will always be + 0
                   and the write_var function will just see the local var which
                   is not a callback and just treat it as a pointer with 0 index
                   and chrinit will be NULL which will cause write_var to treat
                   it as an integer.
                 */
                /* do some goofy nonsense to get the pointer (in VM memory) in
                   to the stack of the value referenced by val */
                *ptr = cvm->sp -
                       (cvm->var[*val].offset * sizeof(CrustyStackArg)) +
                       offsetof(CrustyStackArg, val);
            }
        } else {
            if((*flags & MOVE_FLAG_INDEX_TYPE_MASK) == MOVE_FLAG_INDEX_VAR) {
                if(variable_is_argument(&(cvm->var[*index]))) {
                    if((STACK_ARG(cvm->sp, cvm->var[*index].offset)->flags &
                       MOVE_FLAG_TYPE_MASK) == MOVE_FLAG_VAR) {
                        if(cvm->var[STACK_ARG(cvm->sp,
                                              cvm->var[*index].offset)->val].type ==
                           CRUSTY_TYPE_FLOAT) {
                            cvm->status = CRUSTY_STATUS_FLOAT_INDEX;
                            return(-1);
                        }

                        if(read_var(cvm,
                                    index,
                                    NULL,
                                    STACK_ARG(cvm->sp, cvm->var[*index].offset)->ptr,
                                    &(cvm->var[STACK_ARG(cvm->sp,
                                                         cvm->var[*index].offset)->val]),
                                    STACK_ARG(cvm->sp,
                                              cvm->var[*index].offset)->index) < 0) {
                            cvm->status = CRUSTY_STATUS_CALLBACK_FAILED;
                            return(-1);
                        }
                    } else {
                        *index = STACK_ARG(cvm->sp, cvm->var[*index].offset)->val;
                    }
                } else {
                    if(cvm->var[*index].type == CRUSTY_TYPE_FLOAT) {
                        cvm->status = CRUSTY_STATUS_FLOAT_INDEX;
                        return(-1);
                    }

                    if(read_var(cvm,
                                index,
                                NULL,
                                GET_PTR(*index, *ptr),
                                &(cvm->var[*index]),
                                0) < 0) {
                        cvm->status = CRUSTY_STATUS_CALLBACK_FAILED;
                        return(-1);
                    }
                }
            } /* else {
                do nothing
            } */
            if(*index < 0 || *index > (int)(cvm->var[*val].length - 1)) {
                cvm->status = CRUSTY_STATUS_OUT_OF_RANGE;
                return(-1);
            }

            /* val and index are already correct */
            *ptr = GET_PTR(*val, *ptr);
        }
    } else {
        cvm->status = CRUSTY_STATUS_INTERNAL_ERROR;
        return(-1);
    }

    /* on success, this function will have always set flags to be a VAR */
    *flags = MOVE_FLAG_VAR;
    return(0);
}

int fetch_val(CrustyVM *cvm,
              int flags,
              int val,
              int index,
              int *intval,
              double *floatval,
              int ptr) {
    if((flags & MOVE_FLAG_TYPE_MASK) == MOVE_FLAG_VAR) {
        if(read_var(cvm,
                    intval,
                    floatval,
                    ptr,
                    &(cvm->var[val]),
                    index) < 0) {
            cvm->status = CRUSTY_STATUS_CALLBACK_FAILED;
            return(-1);
        }
    } else {
        *intval = val;
    }

    return(0);
}

/* if flags isn't VAR, calling this is invalid */
int store_result(CrustyVM *cvm,
                 int val,
                 int index,
                 int ptr) {
    if(write_var(cvm,
                 cvm->intresult,
                 cvm->floatresult,
                 ptr,
                 &(cvm->var[val]),
                 index) < 0) {
        cvm->status = CRUSTY_STATUS_CALLBACK_FAILED;
        return(-1);
    }

    return(0);
}

#define POPULATE_ARGS \
    destflags = cvm->inst[cvm->ip + MOVE_DEST_FLAGS]; \
    destval = cvm->inst[cvm->ip + MOVE_DEST_VAL]; \
    destindex = cvm->inst[cvm->ip + MOVE_DEST_INDEX]; \
    destptr = cvm->sp; \
    srcflags = cvm->inst[cvm->ip + MOVE_SRC_FLAGS]; \
    srcval = cvm->inst[cvm->ip + MOVE_SRC_VAL]; \
    srcindex = cvm->inst[cvm->ip + MOVE_SRC_INDEX]; \
    srcptr = cvm->sp;

#define FETCH_VALS \
    if(update_dest_ref(cvm, &destflags, &destval, &destindex, &destptr) < 0) { \
        break; \
    } \
    if(update_src_ref(cvm, &srcflags, &srcval, &srcindex, &srcptr) < 0) { \
        break; \
    } \
    if(fetch_val(cvm, \
                 srcflags, \
                 srcval, \
                 srcindex, \
                 &intoperand, \
                 &floatoperand, \
                 srcptr) < 0) { \
        break; \
    } \
    if(fetch_val(cvm, \
                 destflags, \
                 destval, \
                 destindex, \
                 &(cvm->intresult), \
                 &(cvm->floatresult), \
                 destptr) < 0) { \
        break; \
    }

#define MATH_INSTRUCTION(OP) \
    POPULATE_ARGS \
    \
    FETCH_VALS \
    \
    if(srcflags == MOVE_FLAG_VAR) { \
        if(cvm->var[srcval].type == CRUSTY_TYPE_FLOAT && \
           cvm->var[destval].type != CRUSTY_TYPE_FLOAT) { \
            cvm->intresult = ((double)(cvm->intresult)) OP floatoperand; \
            cvm->resulttype = CRUSTY_TYPE_INT; \
        } else if(cvm->var[srcval].type != CRUSTY_TYPE_FLOAT && \
                  cvm->var[destval].type == CRUSTY_TYPE_FLOAT) { \
            cvm->floatresult = cvm->floatresult OP ((double)intoperand); \
            cvm->resulttype = CRUSTY_TYPE_FLOAT; \
        } else if(cvm->var[srcval].type == CRUSTY_TYPE_FLOAT && \
                  cvm->var[destval].type == CRUSTY_TYPE_FLOAT) { \
            cvm->floatresult = cvm->floatresult OP floatoperand; \
            cvm->resulttype = CRUSTY_TYPE_FLOAT; \
        } else { /* both not float */ \
            cvm->intresult = cvm->intresult OP intoperand; \
            cvm->resulttype = CRUSTY_TYPE_INT; \
        } \
    } else { \
        /* immediates can only be ints */ \
        if(cvm->var[destval].type == CRUSTY_TYPE_FLOAT) { \
            cvm->floatresult = cvm->floatresult OP ((double)intoperand); \
            cvm->resulttype = CRUSTY_TYPE_FLOAT; \
        } else { \
            cvm->intresult = cvm->intresult OP intoperand; \
            cvm->resulttype = CRUSTY_TYPE_INT; \
        } \
    } \
    \
    if(store_result(cvm, destval, destindex, destptr) < 0) { \
        break; \
    } \
    \
    cvm->ip += MOVE_ARGS + 1;

#define LOGIC_INSTRUCTION(OP) \
    POPULATE_ARGS \
    \
    FETCH_VALS \
    \
    if(srcflags == MOVE_FLAG_VAR) { \
        if(cvm->var[srcval].type == CRUSTY_TYPE_FLOAT || \
           cvm->var[destval].type == CRUSTY_TYPE_FLOAT) { \
            cvm->status = CRUSTY_STATUS_INVALID_INSTRUCTION; \
            break; \
        } \
    } \
    \
    cvm->intresult = cvm->intresult OP intoperand; \
    cvm->resulttype = CRUSTY_TYPE_INT; \
    \
    if(store_result(cvm, destval, destindex, destptr) < 0) { \
        break; \
    } \
    \
    cvm->ip += MOVE_ARGS + 1;

#define JUMP_INSTRUCTION(CMP) \
    if(cvm->resulttype == CRUSTY_TYPE_INT) { \
        if(cvm->intresult CMP 0) { \
            if(cvm->ip == (unsigned int)(cvm->inst[cvm->ip + JUMP_LOCATION])) { \
                cvm->status = CRUSTY_STATUS_READY; \
                break; \
            } \
            cvm->ip = (unsigned int)(cvm->inst[cvm->ip + JUMP_LOCATION]); \
        } else { \
            cvm->ip += JUMP_ARGS + 1; \
        } \
    } else { \
        if(cvm->floatresult CMP 0.0) { \
            if(cvm->ip == (unsigned int)(cvm->inst[cvm->ip + JUMP_LOCATION])) { \
                cvm->status = CRUSTY_STATUS_READY; \
                break; \
            } \
            cvm->ip = (unsigned int)(cvm->inst[cvm->ip + JUMP_LOCATION]); \
        } else { \
            cvm->ip += JUMP_ARGS + 1; \
        } \
    }

CrustyStatus crustyvm_step(CrustyVM *cvm) {
    int destflags, destval, destindex, destptr;
    int srcflags, srcval, srcindex, srcptr;
    double floatoperand;
    int intoperand;

    if(cvm->status != CRUSTY_STATUS_ACTIVE) {
        return(cvm->status);
    }

#ifdef CRUSTY_TEST
    if(cvm->flags & CRUSTY_FLAG_TRACE) {
        if(check_instruction(cvm, NULL, cvm->ip) < 0) {
            LOG_PRINTF(cvm, "Invalid instruction at %u.\n", cvm->ip);
            cvm->status = CRUSTY_STATUS_INVALID_INSTRUCTION;
            return(cvm->status);
        }
    }
#endif

    switch(cvm->inst[cvm->ip]) {
        case CRUSTY_INSTRUCTION_TYPE_MOVE:
            POPULATE_ARGS

            if(update_dest_ref(cvm, &destflags, &destval, &destindex, &destptr) < 0) {
                break;
            }
            if(update_src_ref(cvm, &srcflags, &srcval, &srcindex, &srcptr) < 0) {
                break;
            }

            if(fetch_val(cvm,
                         srcflags,
                         srcval,
                         srcindex,
                         &(cvm->intresult),
                         &(cvm->floatresult),
                         srcptr) < 0) {
                break;
            }

            if(srcflags == MOVE_FLAG_VAR) {
                if(cvm->var[srcval].type == CRUSTY_TYPE_FLOAT &&
                   cvm->var[destval].type != CRUSTY_TYPE_FLOAT) {
                    cvm->intresult = cvm->floatresult;
                    cvm->resulttype = CRUSTY_TYPE_INT;
                } else if(cvm->var[srcval].type != CRUSTY_TYPE_FLOAT &&
                          cvm->var[destval].type == CRUSTY_TYPE_FLOAT) {
                    cvm->floatresult = cvm->intresult;
                    cvm->resulttype = CRUSTY_TYPE_FLOAT;
                }
                /* if src and dest are the same type, no conversion necessary */
            } else {
                /* immediates can only be ints */
                if(cvm->var[destval].type == CRUSTY_TYPE_FLOAT) {
                    cvm->floatresult = cvm->intresult;
                    cvm->resulttype = CRUSTY_TYPE_FLOAT;
                }
            }

            if(store_result(cvm, destval, destindex, destptr) < 0) {
                break;
            }

            cvm->ip += MOVE_ARGS + 1;
            break;
        case CRUSTY_INSTRUCTION_TYPE_ADD:
            MATH_INSTRUCTION(+)
            break;
        case CRUSTY_INSTRUCTION_TYPE_SUB:
            MATH_INSTRUCTION(-)
            break;
        case CRUSTY_INSTRUCTION_TYPE_MUL:
            MATH_INSTRUCTION(*)
            break;
        case CRUSTY_INSTRUCTION_TYPE_DIV:
            MATH_INSTRUCTION(/)
            break;
        case CRUSTY_INSTRUCTION_TYPE_AND:
            LOGIC_INSTRUCTION(&)
            break;
        case CRUSTY_INSTRUCTION_TYPE_OR:
            LOGIC_INSTRUCTION(|)
            break;
        case CRUSTY_INSTRUCTION_TYPE_XOR:
            LOGIC_INSTRUCTION(^)
            break;
        case CRUSTY_INSTRUCTION_TYPE_SHR:
            POPULATE_ARGS

            FETCH_VALS

            /* make sure we're shifting by an integer, so just truncate the float
               value to an integer */
            if(srcflags == MOVE_FLAG_VAR &&
               cvm->var[srcval].type == CRUSTY_TYPE_FLOAT) {
                if(cvm->var[destval].type == CRUSTY_TYPE_FLOAT) {
                    cvm->status = CRUSTY_STATUS_INVALID_INSTRUCTION;
                    break;
                } else {
                    cvm->intresult = cvm->intresult >> intoperand;
                    cvm->resulttype = CRUSTY_TYPE_INT;
                }
            } else {
                if(cvm->var[destval].type == CRUSTY_TYPE_FLOAT) {
                    cvm->status = CRUSTY_STATUS_INVALID_INSTRUCTION;
                    break;
                } else {
                    cvm->intresult = cvm->intresult >> intoperand;
                    cvm->resulttype = CRUSTY_TYPE_INT;
                }
            }

            if(store_result(cvm, destval, destindex, destptr) < 0) {
                break;
            }

            cvm->ip += MOVE_ARGS + 1;
            break;
        case CRUSTY_INSTRUCTION_TYPE_SHL:
            POPULATE_ARGS

            FETCH_VALS

            /* make sure we're shifting by an integer, so just truncate the float
               value to an integer */
            if(srcflags == MOVE_FLAG_VAR &&
               cvm->var[srcval].type == CRUSTY_TYPE_FLOAT) {
                if(cvm->var[destval].type == CRUSTY_TYPE_FLOAT) {
                    cvm->status = CRUSTY_STATUS_INVALID_INSTRUCTION;
                    break;
                } else {
                    cvm->intresult = cvm->intresult << intoperand;
                    cvm->resulttype = CRUSTY_TYPE_INT;
                }
            } else {
                if(cvm->var[destval].type == CRUSTY_TYPE_FLOAT) {
                    cvm->status = CRUSTY_STATUS_INVALID_INSTRUCTION;
                    break;
                } else {
                    cvm->intresult = cvm->intresult << intoperand;
                    cvm->resulttype = CRUSTY_TYPE_INT;
                }
            }

            if(store_result(cvm, destval, destindex, destptr) < 0) {
                break;
            }

            cvm->ip += MOVE_ARGS + 1;
            break;
        case CRUSTY_INSTRUCTION_TYPE_CMP:
            POPULATE_ARGS

            /* this one is a bit special because destination never needs to be
               written to, so treat both as src references */
            if(update_src_ref(cvm, &destflags, &destval, &destindex, &destptr) < 0) {
                break;
            }
            if(update_src_ref(cvm, &srcflags, &srcval, &srcindex, &srcptr) < 0) {
                break;
            }
            if(fetch_val(cvm,
                         srcflags,
                         srcval,
                         srcindex,
                         &intoperand,
                         &floatoperand,
                         srcptr) < 0) {
                break;
            }
            if(fetch_val(cvm,
                         destflags,
                         destval,
                         destindex,
                         &(cvm->intresult),
                         &(cvm->floatresult),
                         destptr) < 0) {
                break;
            }

            if(srcflags == MOVE_FLAG_VAR) {
                if(cvm->var[srcval].type == CRUSTY_TYPE_FLOAT &&
                   cvm->var[destval].type != CRUSTY_TYPE_FLOAT) {
                    cvm->intresult = ((double)(cvm->intresult)) - floatoperand;
                    cvm->resulttype = CRUSTY_TYPE_INT;
                } else if(cvm->var[srcval].type != CRUSTY_TYPE_FLOAT &&
                          cvm->var[destval].type == CRUSTY_TYPE_FLOAT) {
                    cvm->floatresult = cvm->floatresult - ((double)intoperand);
                    cvm->resulttype = CRUSTY_TYPE_FLOAT;
                } else if(cvm->var[srcval].type == CRUSTY_TYPE_FLOAT &&
                          cvm->var[destval].type == CRUSTY_TYPE_FLOAT) {
                    cvm->floatresult -= floatoperand;
                    cvm->resulttype = CRUSTY_TYPE_FLOAT;
                } else { /* both not float */
                    cvm->intresult -= intoperand;
                    cvm->resulttype = CRUSTY_TYPE_INT;
                }
            } else {
                /* immediates can only be ints */
                if(cvm->var[destval].type == CRUSTY_TYPE_FLOAT) {
                    cvm->floatresult -= ((double)intoperand);
                    cvm->resulttype = CRUSTY_TYPE_FLOAT;
                } else {
                    cvm->intresult -= intoperand;
                    cvm->resulttype = CRUSTY_TYPE_INT;
                }
            }

            cvm->ip += MOVE_ARGS + 1;
            break;
        case CRUSTY_INSTRUCTION_TYPE_JUMP:
            /* jump to self means nothing more can happen, so end execution. */
            if(cvm->ip == (unsigned int)(cvm->inst[cvm->ip + JUMP_LOCATION])) {
                cvm->status = CRUSTY_STATUS_READY;
                break;
            }
            cvm->ip = (unsigned int)(cvm->inst[cvm->ip + JUMP_LOCATION]);
            break;
        case CRUSTY_INSTRUCTION_TYPE_JUMPN:
            JUMP_INSTRUCTION(!=)
            break;
        case CRUSTY_INSTRUCTION_TYPE_JUMPZ:
            JUMP_INSTRUCTION(==)
            break;
        case CRUSTY_INSTRUCTION_TYPE_JUMPL:
            JUMP_INSTRUCTION(<)
            break;
        case CRUSTY_INSTRUCTION_TYPE_JUMPG:
            JUMP_INSTRUCTION(>)
            break;
        case CRUSTY_INSTRUCTION_TYPE_CALL:
            if(call(cvm,
                    cvm->inst[cvm->ip + CALL_PROCEDURE],
                    cvm->ip + CALL_START_ARGS) < 0) {
                break;
            }

            /* no need to update ip */
            break;
        case CRUSTY_INSTRUCTION_TYPE_RET:
            /* going to return from initial call */
            if(cvm->csp == 1) {
                cvm->status = CRUSTY_STATUS_READY;
                break;
            }

            cvm->ip = cvm->cstack[cvm->csp - 1].ip;
            cvm->sp -= cvm->proc[cvm->cstack[cvm->csp - 1].proc].stackneeded;

            cvm->csp--;
            break;
        default:
            cvm->status = CRUSTY_STATUS_INVALID_INSTRUCTION;
    }

    return(cvm->status);
}

#undef JUMP_INSTRUCTION
#undef MATH_INSTRUCTION

CrustyStatus crustyvm_get_status(CrustyVM *cvm) {
    return(cvm->status);
}

const char *crustyvm_statusstr(CrustyStatus status) {
    if(status < 0 || status >= CRUSTY_STATUS_INVALID) {
        return(CRUSTY_STATUSES[CRUSTY_STATUS_INVALID]);
    }

    return(CRUSTY_STATUSES[status]);
}

int crustyvm_begin(CrustyVM *cvm, const char *procname) {
    int procnum;

    if(cvm->status != CRUSTY_STATUS_READY) {
        LOG_PRINTF(cvm, "Cannot start running, status is not active.\n");
        return(-1);
    }

    cvm->stage = "runtime init";
#ifdef CRUSTY_TEST
    LOG_PRINTF(cvm, "Start\n");
#endif

    procnum = find_procedure(cvm, procname);
    if(procnum == -1) {
        LOG_PRINTF(cvm, "Couldn't find procedure: %s\n", procname);
        return(-1);
    }
    if(cvm->proc[procnum].args > 0) {
        LOG_PRINTF(cvm, "Can't enter from procedure with arguments.\n");
        return(-1);
    }

    /* just some nonsense value so the call stack has something reasonable on it
       even though this will never be used */
    cvm->ip = 0;

    cvm->sp = cvm->initialstack;
    cvm->csp = 0;
    cvm->intresult = 0;
    cvm->floatresult = 0.0;
    cvm->resulttype = CRUSTY_TYPE_INT;

    if(call(cvm, procnum, 0)) {
        LOG_PRINTF(cvm, "Failed to call procedure %s: %s\n", procname,
                   crustyvm_statusstr(crustyvm_get_status(cvm)));
        return(-1);
    }

    cvm->status = CRUSTY_STATUS_ACTIVE;

    return(0);
}

int crustyvm_run(CrustyVM *cvm, const char *procname) {
    if(crustyvm_begin(cvm, procname) < 0) {
        return(-1);
    }

    cvm->stage = "running";
#ifdef CRUSTY_TEST
    LOG_PRINTF(cvm, "Start\n");
#endif

    while(crustyvm_step(cvm) == CRUSTY_STATUS_ACTIVE);

    if(cvm->status != CRUSTY_STATUS_READY) {
        LOG_PRINTF(cvm, "Execution stopped with error: %s\n",
                   crustyvm_statusstr(crustyvm_get_status(cvm)));
        return(-1);
    }

    return(0);
}

static CrustyLine *inst_to_line(CrustyVM *cvm, unsigned int inst) {
    unsigned int i;

    for(i = 0; i < cvm->lines; i++) {
        if(cvm->line[i].instruction == inst) {
            return(&(cvm->line[i]));
        }
    }

    return(NULL);
}

void crustyvm_debugtrace(CrustyVM *cvm, int full) {
    unsigned int csp, sp, ip;
    unsigned int flags, val, index, ptr;
    unsigned int i, j;
    CrustyProcedure *proc;
    const char *temp;
    CrustyLine *line;

    temp = cvm->stage;
    cvm->stage = "debug trace";
#ifdef CRUSTY_TEST
    LOG_PRINTF(cvm, "Start\n");
#endif

    csp = cvm->csp;
    sp = cvm->sp;
    ip = cvm->ip;

    while(csp > 0) {
        proc = &(cvm->proc[cvm->cstack[csp - 1].proc]);
        LOG_PRINTF(cvm, "%u: %s@", csp, proc->name);
        line = inst_to_line(cvm, ip);
        if(line == NULL) {
            LOG_PRINTF_BARE(cvm, "invalid");
        } else {
            LOG_PRINTF_BARE(cvm, "%s:%u", line->module, line->line);
        }
        for(i = 0; i < proc->args; i++) {
            LOG_PRINTF_BARE(cvm, " %s", proc->var[i]->name);
        }
        LOG_PRINTF_BARE(cvm, "\n");

        for(i = 0; i < proc->args; i++) {
            flags = STACK_ARG(sp, i + 1)->flags;
            val = STACK_ARG(sp, i + 1)->val;
            index = STACK_ARG(sp, i + 1)->index;
            ptr = STACK_ARG(sp, i + 1)->ptr;
            switch(flags & MOVE_FLAG_TYPE_MASK) {
                case MOVE_FLAG_VAR:
                    LOG_PRINTF(cvm, " %u: %s -> %s.%s@%u[%u]:%u %X\n",
                               i,
                               proc->var[i]->name,
                               cvm->var[val].proc == NULL ? "Global" : cvm->var[val].proc->name,
                               cvm->var[val].name,
                               ptr,
                               cvm->var[val].length,
                               index,
                               flags);
                    break;
                case MOVE_FLAG_IMMEDIATE:
                    LOG_PRINTF(cvm, " %u: %s -> %u %X\n",
                               i,
                               proc->var[i]->name,
                               val,
                               flags);
                    break;
                default:
                    LOG_PRINTF(cvm, " %u: Invalid flags %X\n", i, flags);
                    break;
            }
        }
        for(i = proc->args; i < proc->vars; i++) {
            LOG_PRINTF(cvm, " %u: %s@%u[%u]",
                       i,
                       proc->var[i]->name,
                       sp - proc->var[i]->offset,
                       proc->var[i]->length);
            if(full) {
                if(proc->var[i]->type == CRUSTY_TYPE_INT) {
                    for(j = 0; j < proc->var[i]->length; j++) {
                        LOG_PRINTF_BARE(cvm, " %d",
                            *((int *)(&(cvm->stack[sp -
                                                   proc->var[i]->offset +
                                                   (j * sizeof(int))]))));
                    }
                } else if(proc->var[i]->type == CRUSTY_TYPE_FLOAT) {
                    for(j = 0; j < proc->var[i]->length; j++) {
                        LOG_PRINTF_BARE(cvm, " %g",
                            *((int *)(&(cvm->stack[sp -
                                                   proc->var[i]->offset +
                                                   (j * sizeof(double))]))));
                    }
                } else { /* chrinit */
                    LOG_PRINTF_BARE(cvm, " \"");
                    for(j = 0; j < proc->var[i]->length; j++) {
                        LOG_PRINTF_BARE(cvm, "%c",
                            *((int *)(&(cvm->stack[sp - proc->var[i]->offset + j]))));
                    }
                    LOG_PRINTF_BARE(cvm, "\"");
                }
            }
            LOG_PRINTF_BARE(cvm, "\n");
        }

        sp -= proc->stackneeded;
        ip = cvm->cstack[csp - 1].ip;
        csp--;
    }
    LOG_PRINTF(cvm, "Global:\n");
    for(i = 0; i < cvm->vars; i++) {
        if(variable_is_global(&(cvm->var[i]))) {
            if(variable_is_callback(&(cvm->var[i]))) {
                LOG_PRINTF(cvm, " %u: %s[%u] CB\n",
                           i,
                           cvm->var[i].name,
                           cvm->var[i].length);
            } else {
                LOG_PRINTF(cvm, " %u: %s@%u[%u]",
                           i,
                           cvm->var[i].name,
                           cvm->var[i].offset,
                           cvm->var[i].length);
                if(full) {
                    if(cvm->var[i].type == CRUSTY_TYPE_INT) {
                        for(j = 0; j < cvm->var[i].length; j++) {
                            LOG_PRINTF_BARE(cvm, " %d",
                                *((int *)(&(cvm->stack[cvm->var[i].offset +
                                                       (j * sizeof(int))]))));
                        }
                    } else if(cvm->var[i].type == CRUSTY_TYPE_FLOAT) {
                        for(j = 0; j < cvm->var[i].length; j++) {
                            LOG_PRINTF_BARE(cvm, " %g",
                                *((int *)(&(cvm->stack[cvm->var[i].offset +
                                                       (j * sizeof(double))]))));
                        }
                    } else { /* CHAR */
                        LOG_PRINTF_BARE(cvm, " \"");
                        for(j = 0; j < cvm->var[i].length; j++) {
                            LOG_PRINTF_BARE(cvm, "%c",
                                *((int *)(&(cvm->stack[cvm->var[i].offset + j]))));
                        }
                        LOG_PRINTF_BARE(cvm, "\"");
                    }
                }
                LOG_PRINTF_BARE(cvm, "\n");
            }
        }
    }

    cvm->stage = temp;
}

int crustyvm_has_entrypoint(CrustyVM *cvm, const char *name) {
    int procnum;

    procnum = find_procedure(cvm, name);
    if(procnum == -1) {
        return(0);
    }

    if(cvm->proc[procnum].args > 0) {
        return(0);
    }

    return(1);
}

#ifdef CRUSTY_TEST
void vprintf_cb(void *priv, const char *fmt, ...) {
    va_list ap;
    FILE *out = priv;

    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
}

int write_out(void *priv, void *value, unsigned int index) {
    putchar(*(char *)value);
    return(0);
}

int write_err(void *priv, void *value, unsigned int index) {
    fputc(*(char *)value, stderr);
    return(0);
}

int print_int(void *priv, void *value, unsigned int index) {
    fprintf(stderr, "%d", *(int *)value);
    return(0);
}

int print_float(void *priv, void *value, unsigned int index) {
    fprintf(stderr, "%g", *(double *)value);
    return(0);
}

#define CLEAN_ARGS \
    if(vars > 0) { \
        for(i = 0; i < vars; i++) { \
            free(var[i]); \
            free(value[i]); \
        } \
        free(var); \
        free(value); \
    } \
    vars = 0;

int main(int argc, char **argv) {
    const char *filename = NULL;
    unsigned int i;
    unsigned int arglen;
    char *equals;
    char *temp;
    char **tempa;
    char **var = NULL;
    char **value = NULL;
    unsigned int vars = 0;

    FILE *in = NULL;
    CrustyVM *cvm;
    char *program = NULL;
    long len;
    int result;
    CrustyCallback cb[] = {
        {
            .name = "out",
            .length = 1,
            .type = CRUSTY_TYPE_INT,
            .read = NULL,
            .readpriv = NULL,
            .write = write_out,
            .writepriv = NULL
        },
        {
            .name = "err",
            .length = 1,
            .type = CRUSTY_TYPE_INT,
            .read = NULL,
            .readpriv = NULL,
            .write = write_err,
            .writepriv = NULL
        },
        {
            .name = "printint",
            .length = 1,
            .type = CRUSTY_TYPE_INT,
            .read = NULL,
            .readpriv = NULL,
            .write = print_int,
            .writepriv = NULL
        },
        {
            .name = "printfloat",
            .length = 1,
            .type = CRUSTY_TYPE_FLOAT,
            .read = NULL,
            .readpriv = NULL,
            .write = print_float,
            .writepriv = NULL
        }
    };

    for(i = 1; i < (unsigned int)argc; i++) {
        arglen = strlen(argv[i]);
        if(arglen > 0 && argv[i][0] == '-') {
            if(arglen > 1) {
                if(argv[i][1] == '-') {
                    if(filename != NULL) {
                        filename = NULL;
                        break;
                    }
                    if(i + 1 < (unsigned int)argc) {
                        filename = argv[i + 1];
                    }
                    break;
                } else if(argv[i][1] == 'D') {
                    if(argv[i][2] == '=') {
                        filename = NULL;
                        break;
                    }
                    equals = strchr(&(argv[i][2]), '=');
                    if(equals == NULL) {
                        filename = NULL;
                        break;
                    }

                    tempa = realloc(var, sizeof(char *) * (vars + 1));
                    if(tempa == NULL) {
                        fprintf(stderr, "Failed to allocate memory for vars list.\n");
                        goto error;
                    }
                    var = tempa;
                    tempa = realloc(value, sizeof(char *) * (vars + 1));
                    if(tempa == NULL) {
                        fprintf(stderr, "Failed to allocate memory for values list.\n");
                        goto error;
                    }
                    value = tempa;
                    /* difference from start, take away "-D", add space for '\0' */
                    temp = malloc(equals - argv[i] - 2 + 1);
                    if(temp == NULL) {
                        fprintf(stderr, "Failed to allocate memory for var.\n");
                        goto error;
                    }
                    memcpy(temp, &(argv[i][2]), equals - argv[i] - 2);
                    temp[equals - argv[i] - 2] = '\0';
                    var[vars] = temp;
                    /* total length, take away the length of the first part,
                       take away the '=', add the '\0' */
                    temp = malloc(arglen - (equals - argv[i] - 2) - 1 + 1);
                    if(temp == NULL) {
                        fprintf(stderr, "Failed to allocate memory for value.\n");
                        goto error;
                    }
                    memcpy(temp, &(equals[1]), arglen - (equals - argv[i] - 2) - 1);
                    temp[arglen - (equals - argv[i] - 2) - 1] = '\0';
                    value[vars] = temp;
                    vars++;
                } else {
                    filename = NULL;
                    break;
                }
            }
        } else {
            if(filename != NULL) {
                filename = NULL;
                break;
            }
            filename = argv[i];
        }
    }

    if(filename == NULL) {
        fprintf(stderr, "USAGE: %s [(<filename>|-D<var>=<value>) ...] [-- <filename>]\n", argv[0]);
        goto error;
    }

    in = fopen(filename, "rb");
    if(in == NULL) {
        fprintf(stderr, "Failed to open file %s.\n", filename);
        goto error;
    }

    if(fseek(in, 0, SEEK_END) < 0) {
       fprintf(stderr, "Failed to seek to end of file.\n");
       goto error;
    }

    len = ftell(in);
    if(len < 0) {
        fprintf(stderr, "Failed to get file length.\n");
        goto error;
    }
    rewind(in);

    program = malloc(len);
    if(program == NULL) {
        goto error;
    }

    if(fread(program, 1, len, in) < (unsigned long)len) {
        fprintf(stderr, "Failed to read file.\n");
        goto error;
    }

    fclose(in);
    in = NULL;

    cvm = crustyvm_new(filename, program, len,
                       CRUSTY_FLAG_OUTPUT_PASSES
                       /* | CRUSTY_FLAG_TRACE */,
                       0,
                       cb, sizeof(cb) / sizeof(CrustyCallback),
                       (const char **)var, (const char **)value, vars,
                       vprintf_cb, stderr);
    if(cvm == NULL) {
        fprintf(stderr, "Failed to load program.\n");
        goto error;
    }
    free(program);
    program = NULL;
    CLEAN_ARGS
    fprintf(stderr, "Program loaded.\n");

    fprintf(stderr, "Token memory size: %u\n", cvm->tokenmemlen);
    fprintf(stderr, "Stack size: %u\n", cvm->stacksize);

    result = crustyvm_run(cvm, "init");
    fprintf(stderr, "\n");
    if(result < 0) {
        fprintf(stderr, "Program reached an exception while running: %s\n",
                crustyvm_statusstr(crustyvm_get_status(cvm)));
        crustyvm_debugtrace(cvm, 1);
        goto error;
    }

    fprintf(stderr, "Program completed successfully.\n");
    crustyvm_free(cvm);

    exit(EXIT_SUCCESS);

error:
    if(program != NULL) {
        free(program);
    }

    if(in != NULL) {
        fclose(in);
    }

    CLEAN_ARGS

    exit(EXIT_FAILURE);
}
#endif
