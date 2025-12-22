/* proc.c -- proc language tree-walk interpreter
 *
 * XXX: keep in mind this is probably inefficient/broken, this is my first time trying this */

#include <stddef.h>
#include <limits.h>
#include <inttypes.h>
#include <assert.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#define VERSION "2025-12-22 -- https://github.com/danielsource/proc"
#define TOKEN_BUF_SIZE (80) /* 78 meaningful characters at most */
#define PARSE_DATA_SIZE (50*1000*1000)
#define EVAL_DATA_SIZE (50*1000*1000)
#define WORDS_SIZE (500*1000)
#define UNARY_PREC (10)
#define INT64_OB1 ((uint64_t)INT64_MAX + 1) /* off by one */

#define ROUND_UP(n, to) (((n) + ((to) - 1)) & ~((to) - 1))
#define LENGTH(x) ((int)(sizeof(x) / sizeof((x)[0])))
#define DEREF(v) (v.deref = v.deref ? (v.i = words[v.i], 0) : 0) /* lvalue-to-rvalue */

/* assumes ASCII */
#define IS_SPACE(c) (((c) >= 0 && (c) <= 32) || (c) == 127)
#define IS_DIGIT(c) ((c) >= '0' && (c) <= '9')
#define IS_UPPER(c) ((c) >= 'A' && (c) <= 'Z')
#define IS_ALPHA(c) (((c) >= 'A' && (c) <= 'Z') || ((c) >= 'a' && (c) <= 'z'))
#define IS_PRINT(c) ((c) >= ' ' && (c) <= '~')
#define IS_ALNUM(c) (IS_DIGIT(c) || IS_ALPHA(c))
#define IS_IDENT1(c) (IS_ALPHA(c) || (c) == '_') /* A-Za-z_ */
#define IS_IDENT2(c) (IS_ALNUM(c) || (c) == '_') /* A-Za-z_0-9 */

struct bumpalloc {
	char *beg, *top, *end;
};

enum tok_tag {
	TOK_NULL = 0,
	TOK_START,
	TOK_INT,
	TOK_NAME,
	TOK_PAREN_L,
	TOK_PAREN_R,
	TOK_BRACKET_L,
	TOK_BRACKET_R,
	TOK_BRACE_L,
	TOK_BRACE_R,
	TOK_SEMICOLON,
	TOK_COMMA,
	TOK_AMPERSAND,
	TOK_PLUS,
	TOK_MINUS,
	TOK_EXCLAM,
	TOK_TILDE,
	TOK_CARET,
	TOK_PERCENT,
	TOK_SLASH,
	TOK_ASTERISK,
	TOK_ASTERISK2,
	TOK_GREATER,
	TOK_GREATER_EQUALS,
	TOK_LESS,
	TOK_LESS_EQUALS,
	TOK_SHIFT_L,
	TOK_SHIFT_R,
	TOK_NOT_EQUALS,
	TOK_EQUALS,
	TOK_ASSIGN,
	TOK_ASSIGN_ADD,
	TOK_ASSIGN_SUB,
	TOK_ASSIGN_MUL,
	TOK_ASSIGN_DIV,
	TOK_ASSIGN_REM,
	TOK_ASSIGN_BIT_SH_L,
	TOK_ASSIGN_BIT_SH_R,
	TOK_ASSIGN_BIT_AND,
	TOK_ASSIGN_BIT_XOR,
	TOK_ASSIGN_BIT_OR,
	TOK_AND,
	TOK_PIPE,
	TOK_OR,
	TOK_RETURN,
	TOK_INT_KW,
	TOK_IF,
	TOK_ELSE,
	TOK_WHILE,
	TOK_PROC,
	TOK_PERIOD
};

struct token {
	enum tok_tag tag;
	int line, col, col_end;
	union {
		int64_t i;
		const char *name; /* XXX: C strings are a major source of bugs */
	} u;
};

struct ast {
	struct stmt *globals;
	struct proc *procs;
};

struct proc {
	struct token tok;
	struct stmt *params;
	struct stmt *body;
	struct proc *next;
};

enum expr_tag {
	EXPR_INVALID = 0,
	EXPR_INT,
	EXPR_NAME,
	EXPR_ARRIDX,
	EXPR_CALL,
	EXPR_ARGS,
	EXPR_ADDROF,
	EXPR_POS,
	EXPR_NEG,
	EXPR_NOT,
	EXPR_BIT_NOT,
	EXPR_POW,
	EXPR_REM,
	EXPR_DIV,
	EXPR_MUL,
	EXPR_SUB,
	EXPR_ADD,
	EXPR_BIT_SH_R,
	EXPR_BIT_SH_L,
	EXPR_GE,
	EXPR_GT,
	EXPR_LE,
	EXPR_LT,
	EXPR_NE,
	EXPR_EQ,
	EXPR_BIT_AND,
	EXPR_BIT_XOR,
	EXPR_BIT_OR,
	EXPR_AND,
	EXPR_OR
};

struct expr {
	enum expr_tag tag;
	struct token tok;
	struct expr *left, *right;
};

struct stmt_decl {
	struct expr *elems; /* NULL if scalar, otherwise array[elems] */
	struct expr *expr;
};

struct stmt_assign {
	struct expr *lhs, *rhs;
};

struct stmt_select {
	struct expr *cond;
	struct stmt *ifbody, *elsebody;
};

struct stmt_loop {
	struct expr *cond;
	struct stmt *body;
};

enum stmt_tag {
	STMT_INVALID = 0,
	STMT_RETURN,
	STMT_DECL,
	STMT_ASSIGN,
	STMT_CALL,
	STMT_SELECT,
	STMT_LOOP
};

struct stmt {
	enum stmt_tag tag;
	struct token tok;
	struct stmt *next;
	union {
		struct expr *expr;
		struct stmt_decl decl;
		struct stmt_assign ass;
		struct stmt_select sel;
		struct stmt_loop loop;
	} u;
};

struct var {
	struct token *tok;
	int addr;
	int elems; /* 0 if scalar, otherwise array[elems] */
	struct var *next;
};

struct val {
	int64_t i;
	int deref, ret;
};

/* globals */

static int evalcmd = 0;
static int debug = 0;

static char *progname, *script_path;
static FILE *script_file;

static char parse_data[PARSE_DATA_SIZE];
static struct bumpalloc parse_alloc;

static char eval_data[EVAL_DATA_SIZE];
static struct bumpalloc eval_alloc;

static int64_t words[WORDS_SIZE];
static int words_top;

static struct token tok = {TOK_START};
static struct token tok_undo[2];
static int tok_undo_top = 0;

static struct ast ast;

static struct var *globals;

int compiletime_assert1(int [sizeof(int) >= 4             ?1:-1]);
int compiletime_assert2(int [sizeof(char) < sizeof(int)   ?1:-1]);
int compiletime_assert3(int [(int64_t)-1 == ~0            ?1:-1]); /* two's complement */

/* implementation */

static void
debuglog(int ln, const char *msg, ...)
{
	va_list ap;
	char s[TOKEN_BUF_SIZE * 4];

	if (!debug)
		return;
	va_start(ap, msg);
	vsprintf(s, msg, ap);
	va_end(ap);
	fprintf(stderr, __FILE__":%d: DEBUG: %s\n", ln, s);
}

static struct bumpalloc
bumpalloc_new(void *p, int capacity)
{
	struct bumpalloc a;

	assert(p);
	assert(capacity >= 1);
	a.beg = a.top = p;
	a.end = (char *)p + capacity;
	return a;
}

static void *
alloc(struct bumpalloc *a, int n)
{
	char *top, *last;

	assert(n >= 1);
	top = a->top + ROUND_UP(n, sizeof(void *));
	assert(top >= a->beg);
	assert(top >= a->top);
	assert(top <= a->end);
	last = a->top;
	a->top = top;
	return last;
}

static void
parse_die(const char *msg, ...)
{
	va_list ap;
	char s[TOKEN_BUF_SIZE * 4];

	va_start(ap, msg);
	vsprintf(s, msg, ap);
	va_end(ap);
	if (tok.line > 0 && tok.col > 0)
		fprintf(stderr, "%s:%d:%d: ERROR: %s\n", script_path, tok.line, tok.col, s);
	else
		fprintf(stderr, "%s: ERROR: %s\n", script_path, s);
	if (script_file)
		fclose(script_file);
	if (debug)
		abort();
	exit(2);
}

static void
eval_die(struct token *t, const char *msg, ...)
{
	va_list ap;
	char s[TOKEN_BUF_SIZE * 4];

	va_start(ap, msg);
	vsprintf(s, msg, ap);
	va_end(ap);
	if (t && t->line > 0 && t->col > 0)
		fprintf(stderr, "%s:%d:%d: ERROR: %s\n", script_path, t->line, t->col, s);
	else
		fprintf(stderr, "%s: ERROR: %s\n", script_path, s);
	if (script_file)
		fclose(script_file);
	if (debug)
		abort();
	exit(3);
}

static int64_t
str_to_int(const char *digits)
{
	uint64_t n = 0, sign = 1, base = 10, digitval;
	const char *s;

	s = digits;
	if (!*s) {
		goto invalid_int;
	} else if (*s == '+') {
		++s;
	} else if (*s == '-') {
		sign = -1;
		++s;
	}
	if (*s == '0' && !IS_DIGIT(s[1])) { /* 0b..., 0o..., 0x... */
		switch (s[1]) {
		case 'B': case 'b': base = 2; break;
		case 'O': case 'o': base = 8; break;
		case 'X': case 'x': base = 16; break;
		case 0:
			return 0;
		default:
			goto invalid_int;
		}
		s += 2;
		if (!*s)
			goto invalid_int;
	}
	while (*s) {
		if (IS_DIGIT(*s))
			digitval = *s - '0';
		else if (base == 16 && *s >= 'A' && *s <= 'F') /* A-F */
			digitval = *s - 'A' + 10;
		else if (base == 16 && *s >= 'a' && *s <= 'f') /* a-f */
			digitval = *s - 'a' + 10;
		else
			goto invalid_int;
		if (digitval >= base)
			goto invalid_int;
		if (n > INT64_OB1 / base)
			parse_die("integer is too large: `%s` [A]", digits);
		n *= base;
		if (n > INT64_OB1 - digitval)
			parse_die("integer is too large: `%s` [B]", digits);
		n += digitval;
		++s;
	}
	assert(n <= INT64_OB1);
	return n == INT64_OB1 ? INT64_MIN : (int64_t)n * sign;
invalid_int:
	parse_die("invalid integer: `%s`", digits);
	return 0;
}

static const char *
get_token_str(struct token t)
{
	char *s;

	switch (t.tag) {
	case TOK_NULL: return "end-of-file";
	case TOK_INT:
		if (t.line > 0 && t.col > 0) {
			s = alloc(&parse_alloc, TOKEN_BUF_SIZE);
			sprintf(s, "%"PRId64, t.u.i);
			return s;
		}
		return "<integer>";
	case TOK_NAME: return t.u.name ? t.u.name : "<name>";
	case TOK_PAREN_L: return "(";
	case TOK_PAREN_R: return ")";
	case TOK_BRACKET_L: return "[";
	case TOK_BRACKET_R: return "]";
	case TOK_BRACE_L: return "{";
	case TOK_BRACE_R: return "}";
	case TOK_SEMICOLON: return ";";
	case TOK_COMMA: return ",";
	case TOK_AMPERSAND: return "&";
	case TOK_PLUS: return "+";
	case TOK_MINUS: return "-";
	case TOK_EXCLAM: return "!";
	case TOK_TILDE: return "~";
	case TOK_CARET: return "^";
	case TOK_PERCENT: return "%";
	case TOK_SLASH: return "/";
	case TOK_ASTERISK: return "*";
	case TOK_ASTERISK2: return "**";
	case TOK_GREATER: return ">";
	case TOK_GREATER_EQUALS: return ">=";
	case TOK_LESS: return "<";
	case TOK_LESS_EQUALS: return "<=";
	case TOK_SHIFT_L: return "<<";
	case TOK_SHIFT_R: return ">>";
	case TOK_NOT_EQUALS: return "!=";
	case TOK_EQUALS: return "==";
	case TOK_ASSIGN: return "=";
	case TOK_ASSIGN_ADD: return "+=";
	case TOK_ASSIGN_SUB: return "-=";
	case TOK_ASSIGN_MUL: return "*=";
	case TOK_ASSIGN_DIV: return "/=";
	case TOK_ASSIGN_REM: return "%=";
	case TOK_ASSIGN_BIT_SH_L: return "<<=";
	case TOK_ASSIGN_BIT_SH_R: return ">>=";
	case TOK_ASSIGN_BIT_AND: return "&=";
	case TOK_ASSIGN_BIT_XOR: return "^=";
	case TOK_ASSIGN_BIT_OR: return "|=";
	case TOK_AND: return "&&";
	case TOK_PIPE: return "|";
	case TOK_OR: return "||";
	case TOK_RETURN: return "return";
	case TOK_INT_KW: return "int";
	case TOK_IF: return "if";
	case TOK_ELSE: return "else";
	case TOK_WHILE: return "while";
	case TOK_PROC: return "proc";
	case TOK_PERIOD: return ".";
	case TOK_START:
	default:
		assert(!"unexpected state");
		return NULL;
	}
}

/* tokenizer/lexer */
static void
next_token(void)
{
	int c, i, line, col;
	char buf[TOKEN_BUF_SIZE], *name;

	if (ferror(script_file))
		parse_die("reading script failed");
	if (tok.tag == TOK_NULL)
		return;
	if (tok_undo_top) {
		assert(tok_undo[tok_undo_top - 1].tag != TOK_NULL);
		tok = tok_undo[tok_undo_top - 1];
		memset(&tok_undo[tok_undo_top - 1], 0, sizeof(struct token));
		--tok_undo_top;
		return;
	}
	line = tok.line;
	col = tok.col_end;
	tok.tag = TOK_NULL;
	if ((c = fgetc(script_file)) == EOF)
		goto end;
	if (!line)
		line = 1;
	++col;
	while (1) {
		while (IS_SPACE(c)) {
			if (c == '\n') {
				++line;
				col = 0;
			}
			if ((c = fgetc(script_file)) == EOF)
				goto end;
			++col;
		}
		if (c != '#') /* comment is # */
			break;
		while (c != '\n') {
			if ((c = fgetc(script_file)) == EOF)
				goto end;
			++col;
		}
	}
	tok.line = line;
	tok.col = col;
	if (IS_DIGIT(c)) {
		for (i = 0; IS_ALNUM(c) && i < LENGTH(buf) - 1;) {
			buf[i++] = c;
			if ((c = fgetc(script_file)) == '_') { /* _ separator */
				++col;
				if ((c = fgetc(script_file)) == '_' || !IS_DIGIT(c)) {
					buf[i] = 0;
					goto invalid_int_lit;
				}
			}
		}
		buf[i] = 0;
		if (i == LENGTH(buf) - 1)
invalid_int_lit:
			parse_die("invalid integer literal: `%s...`", buf);
		tok.tag = TOK_INT;
		tok.u.i = str_to_int(buf);
		col += i - 1;
		goto end;
	} else if (IS_IDENT1(c)) {
		for (i = 0; IS_IDENT2(c) && i < LENGTH(buf) - 1; ++i) {
			buf[i] = c;
			c = fgetc(script_file);
		}
		buf[i] = 0;
		if (i == LENGTH(buf) - 1)
			parse_die("invalid name: `%s...`", buf);
		if (!strcmp(buf, "return")) {
			tok.tag = TOK_RETURN;
		} else if (!strcmp(buf, "int")) {
			tok.tag = TOK_INT_KW;
		} else if (!strcmp(buf, "if")) {
			tok.tag = TOK_IF;
		} else if (!strcmp(buf, "else")) {
			tok.tag = TOK_ELSE;
		} else if (!strcmp(buf, "while")) {
			tok.tag = TOK_WHILE;
		} else if (!strcmp(buf, "proc")) {
			tok.tag = TOK_PROC;
		} else {
			tok.tag = TOK_NAME;
			name = alloc(&parse_alloc, i + 1);
			memcpy(name, buf, i + 1);
			tok.u.name = name;
		}
		col += i - 1;
		goto end;
	}
	switch (c) {
	case '\'': /* 'c' character literal */
		i = -1;
		c = fgetc(script_file);
		if (c == '\\') { /* \<escape> */
			c = fgetc(script_file);
			switch (c) {
			case '\'': i = '\''; break;
			case '\\': i = '\\'; break;
			case 'a': i = '\a'; break;
			case 'b': i = '\b'; break;
			case 't': i = '\t'; break;
			case 'n': i = '\n'; break;
			case 'r': i = '\r'; break;
			case 'e': i = 27; break; /* \e */
			default:
				goto invalid_char_lit;
			}
			++col;
		} else if (c != '\'' && IS_PRINT(c)) {
			i = c;
		} else {
			goto invalid_char_lit;
		}
		c = fgetc(script_file);
		if (c != '\'')
invalid_char_lit:
			parse_die("invalid character literal");
		assert(i != -1);
		tok.tag = TOK_INT;
		tok.u.i = i;
		col += 2;
		goto end_consuming;
	case '(': tok.tag = TOK_PAREN_L; goto end_consuming;
	case ')': tok.tag = TOK_PAREN_R; goto end_consuming;
	case '[': tok.tag = TOK_BRACKET_L; goto end_consuming;
	case ']': tok.tag = TOK_BRACKET_R; goto end_consuming;
	case '{': tok.tag = TOK_BRACE_L; goto end_consuming;
	case '}': tok.tag = TOK_BRACE_R; goto end_consuming;
	case ';': tok.tag = TOK_SEMICOLON; goto end_consuming;
	case ',': tok.tag = TOK_COMMA; goto end_consuming;
	case '~': tok.tag = TOK_TILDE; goto end_consuming;
	case '&':
		tok.tag = TOK_AMPERSAND;
		if ((c = fgetc(script_file)) == '&') {
			tok.tag = TOK_AND;
			++col;
			goto end_consuming;
		} else if (c == '=') {
			tok.tag = TOK_ASSIGN_BIT_AND;
			++col;
			goto end_consuming;
		}
		goto end;
	case '^':
		tok.tag = TOK_CARET;
		if ((c = fgetc(script_file)) == '=') {
			tok.tag = TOK_ASSIGN_BIT_XOR;
			++col;
			goto end_consuming;
		}
		goto end;
	case '|':
		tok.tag = TOK_PIPE;
		if ((c = fgetc(script_file)) == '|') {
			tok.tag = TOK_OR;
			++col;
			goto end_consuming;
		} else if (c == '=') {
			tok.tag = TOK_ASSIGN_BIT_OR;
			++col;
			goto end_consuming;
		}
		goto end;
	case '+':
		tok.tag = TOK_PLUS;
		if ((c = fgetc(script_file)) == '=') {
			tok.tag = TOK_ASSIGN_ADD;
			++col;
			goto end_consuming;
		}
		goto end;
	case '-':
		tok.tag = TOK_MINUS;
		if ((c = fgetc(script_file)) == '=') {
			tok.tag = TOK_ASSIGN_SUB;
			++col;
			goto end_consuming;
		}
		goto end;
	case '!':
		tok.tag = TOK_EXCLAM;
		if ((c = fgetc(script_file)) == '=') {
			tok.tag = TOK_NOT_EQUALS;
			++col;
			goto end_consuming;
		}
		goto end;
	case '%':
		tok.tag = TOK_PERCENT;
		if ((c = fgetc(script_file)) == '=') {
			tok.tag = TOK_ASSIGN_REM;
			++col;
			goto end_consuming;
		}
		goto end;
	case '/':
		tok.tag = TOK_SLASH;
		if ((c = fgetc(script_file)) == '=') {
			tok.tag = TOK_ASSIGN_DIV;
			++col;
			goto end_consuming;
		}
		goto end;
	case '*':
		tok.tag = TOK_ASTERISK;
		if ((c = fgetc(script_file)) == '=') {
			tok.tag = TOK_ASSIGN_MUL;
			++col;
			goto end_consuming;
		} else if (c == '*') {
			tok.tag = TOK_ASTERISK2;
			++col;
			goto end_consuming;
		}
		goto end;
	case '>':
		tok.tag = TOK_GREATER;
		if ((c = fgetc(script_file)) == '=') {
			tok.tag = TOK_GREATER_EQUALS;
			++col;
			goto end_consuming;
		} else if (c == '>') {
			tok.tag = TOK_SHIFT_R;
			++col;
			if ((c = fgetc(script_file)) == '=') {
				tok.tag = TOK_ASSIGN_BIT_SH_R;
				++col;
				goto end_consuming;
			}
		}
		goto end;
	case '<':
		tok.tag = TOK_LESS;
		if ((c = fgetc(script_file)) == '=') {
			tok.tag = TOK_LESS_EQUALS;
			++col;
			goto end_consuming;
		} else if (c == '<') {
			tok.tag = TOK_SHIFT_L;
			++col;
			if ((c = fgetc(script_file)) == '=') {
				tok.tag = TOK_ASSIGN_BIT_SH_L;
				++col;
				goto end_consuming;
			}
		}
		goto end;
	case '=':
		tok.tag = TOK_ASSIGN;
		if ((c = fgetc(script_file)) == '=') {
			tok.tag = TOK_EQUALS;
			++col;
			goto end_consuming;
		}
		goto end;
	default:
		if (c < 0 || c > 127)
			parse_die("script contains non-ASCII character: %d;\n"
				  "... this can happen when copying text from websites, PDFs, etc.",
				  c);
		else
			parse_die("unexpected character: %c%c%c (%d)",
				  c=='`'?'\'':'`', c, c=='`'?'\'':'`', c);
	}
end:
	if (c != EOF
	    && ungetc(c, script_file) == EOF)
		parse_die("ungetc() failed");
end_consuming:
	tok.col_end = col;
}

static void
undo_token(void)
{
	if (tok.tag == TOK_NULL)
		return;
	assert(tok_undo_top < LENGTH(tok_undo));
	assert(tok_undo[tok_undo_top].tag == TOK_NULL);
	++tok_undo_top;
	tok_undo[tok_undo_top - 1] = tok;
}

static struct expr *parse_expr(int minprec);

static struct expr *
parse_unary_expr(void)
{
	struct expr *e;

	next_token();
	if (tok.tag == TOK_PAREN_L) {
		e = parse_expr(0);
		next_token();
		if (tok.tag != TOK_PAREN_R)
			parse_die("expected `)`, but got `%s`", get_token_str(tok));
		return e;
	}
	e = alloc(&parse_alloc, sizeof(struct expr));
	e->tok = tok;
	switch (tok.tag) {
	case TOK_INT:
		e->tag = EXPR_INT;
		return e;
	case TOK_NAME:
		e->tag = EXPR_NAME;
		return e;
	case TOK_AMPERSAND: e->tag = EXPR_ADDROF; break;
	case TOK_PLUS: e->tag = EXPR_POS; break;
	case TOK_MINUS: e->tag = EXPR_NEG; break;
	case TOK_EXCLAM: e->tag = EXPR_NOT; break;
	case TOK_TILDE: e->tag = EXPR_BIT_NOT; break;
	default:
		parse_die("expected an expression, but got `%s`", get_token_str(tok));
		assert(!"unexpected state");
	}
	e->left = parse_expr(UNARY_PREC);
	return e;
}

static enum expr_tag
get_binary_expr(enum tok_tag tag, int *prec)
{
	switch (tag) {
	case TOK_BRACKET_L:      *prec = 12; return EXPR_ARRIDX;
	case TOK_PAREN_L:        *prec = 12; return EXPR_CALL;
	case TOK_ASTERISK2:      *prec = 11; return EXPR_POW;
	                         /* UNARY_PREC is between these */
	case TOK_PERCENT:        *prec = 10; return EXPR_REM;
	case TOK_SLASH:          *prec = 10; return EXPR_DIV;
	case TOK_ASTERISK:       *prec = 10; return EXPR_MUL;
	case TOK_MINUS:          *prec = 9; return EXPR_SUB;
	case TOK_PLUS:           *prec = 9; return EXPR_ADD;
	case TOK_SHIFT_R:        *prec = 8; return EXPR_BIT_SH_R;
	case TOK_SHIFT_L:        *prec = 8; return EXPR_BIT_SH_L;
	case TOK_GREATER_EQUALS: *prec = 7; return EXPR_GE;
	case TOK_GREATER:        *prec = 7; return EXPR_GT;
	case TOK_LESS_EQUALS:    *prec = 7; return EXPR_LE;
	case TOK_LESS:           *prec = 7; return EXPR_LT;
	case TOK_NOT_EQUALS:     *prec = 6; return EXPR_NE;
	case TOK_EQUALS:         *prec = 6; return EXPR_EQ;
	case TOK_AMPERSAND:      *prec = 5; return EXPR_BIT_AND;
	case TOK_CARET:          *prec = 4; return EXPR_BIT_XOR;
	case TOK_PIPE:           *prec = 3; return EXPR_BIT_OR;
	case TOK_AND:            *prec = 2; return EXPR_AND;
	case TOK_OR:             *prec = 1; return EXPR_OR;
	default:                 *prec = 0; return EXPR_INVALID;
	}
}

static struct expr *
parse_binary_expr(struct expr *left, int minprec)
{
	struct expr *e, *right = NULL, **arg;
	enum expr_tag tag;
	struct token op;
	int prec;

	next_token();
	tag = get_binary_expr(tok.tag, &prec);
	if (tag == EXPR_INVALID || prec <= minprec) {
		undo_token();
		return left;
	}
	op = tok;
	if (tag == EXPR_ARRIDX) {
		right = parse_expr(0);
		next_token();
		if (tok.tag != TOK_BRACKET_R)
			parse_die("expected `]`, but got `%s`", get_token_str(tok));
	} else if (tag == EXPR_CALL) {
		if (left->tag != EXPR_NAME)
			parse_die("expected a procedure name, but got `%s` expression",
				  get_token_str(left->tok));
		arg = &right;
		next_token();
		while (tok.tag != TOK_PAREN_R) {
			undo_token();
			*arg = alloc(&parse_alloc, sizeof(struct expr));
			(*arg)->tag = EXPR_ARGS;
			(*arg)->tok = tok;
			(*arg)->left = parse_expr(0);
			arg = &(*arg)->right;
			next_token();
			if (tok.tag == TOK_COMMA)
				next_token();
			else if (tok.tag != TOK_PAREN_R)
				parse_die("expected `)` or `,`, but got `%s`", get_token_str(tok));
		}
	} else if (tag == EXPR_POW) { /* right-associative exponentiation */
		right = parse_expr(prec - 1);
	} else {
		right = parse_expr(prec);
	}
	e = alloc(&parse_alloc, sizeof(struct expr));
	e->tag = tag;
	e->tok = op;
	e->left = left;
	e->right = right;
	return e;
}

/* https://www.youtube.com/watch?v=fIPO4G42wYE */
static struct expr *
parse_expr(int minprec)
{
	struct expr *e, *left;

	left = parse_unary_expr();
	while (1) {
		e = parse_binary_expr(left, minprec);
		if (e == left)
			return e;
		left = e;
	}
}

static struct stmt *
parse_decl(struct stmt **decl, int depth)
{
	while (1) {
		*decl = alloc(&parse_alloc, sizeof(struct stmt));
		(*decl)->tag = STMT_DECL;
		next_token();
		if (tok.tag != TOK_NAME)
			parse_die("expected a name, but got `%s`", get_token_str(tok));
		(*decl)->tok = tok;
		next_token();
		if (tok.tag == TOK_ASSIGN) {
			(*decl)->u.decl.expr = parse_expr(0);
			next_token();
		} else if (tok.tag == TOK_BRACKET_L) {
			(*decl)->u.decl.elems = parse_expr(0);
			next_token();
			if (tok.tag != TOK_BRACKET_R)
				parse_die("expected `]`, but got `%s`", get_token_str(tok));
			next_token();
		}
		if (tok.tag == TOK_SEMICOLON) {
			if (depth)
				parse_die("declaration is not possible in procedure inner blocks");
			return *decl; /* return last declaration in the list (int a, ..., z) */
		} else if (tok.tag != TOK_COMMA) {
			parse_die("expected `;` or `,`, but got `%s`", get_token_str(tok));
		}
		decl = &(*decl)->next;
		/* handle next declaration */
	}
}

static void
parse_assign(struct stmt **stmt)
{
	(*stmt)->u.ass.lhs = parse_expr(0);
	(*stmt)->tag = STMT_ASSIGN;
	next_token();
	if (tok.tag == TOK_ASSIGN) {
		(*stmt)->u.ass.rhs = parse_expr(0);
	} else {
		(*stmt)->u.ass.rhs = alloc(&parse_alloc, sizeof(struct expr));
		switch (tok.tag) {
		case TOK_ASSIGN:
			break;
		case TOK_ASSIGN_ADD:
			tok.tag = TOK_PLUS;
			(*stmt)->u.ass.rhs->tag = EXPR_ADD;
			goto assign_op;
		case TOK_ASSIGN_SUB:
			tok.tag = TOK_MINUS;
			(*stmt)->u.ass.rhs->tag = EXPR_SUB;
			goto assign_op;
		case TOK_ASSIGN_MUL:
			tok.tag = TOK_ASTERISK;
			(*stmt)->u.ass.rhs->tag = EXPR_MUL;
			goto assign_op;
		case TOK_ASSIGN_DIV:
			tok.tag = TOK_SLASH;
			(*stmt)->u.ass.rhs->tag = EXPR_DIV;
			goto assign_op;
		case TOK_ASSIGN_REM:
			tok.tag = TOK_PERCENT;
			(*stmt)->u.ass.rhs->tag = EXPR_REM;
			goto assign_op;
		case TOK_ASSIGN_BIT_SH_L:
			tok.tag = TOK_SHIFT_L;
			(*stmt)->u.ass.rhs->tag = EXPR_BIT_SH_L;
			goto assign_op;
		case TOK_ASSIGN_BIT_SH_R:
			tok.tag = TOK_SHIFT_R;
			(*stmt)->u.ass.rhs->tag = EXPR_BIT_SH_R;
			goto assign_op;
		case TOK_ASSIGN_BIT_AND:
			tok.tag = TOK_AMPERSAND;
			(*stmt)->u.ass.rhs->tag = EXPR_BIT_AND;
			goto assign_op;
		case TOK_ASSIGN_BIT_XOR:
			tok.tag = TOK_CARET;
			(*stmt)->u.ass.rhs->tag = EXPR_BIT_XOR;
			goto assign_op;
		case TOK_ASSIGN_BIT_OR:
			tok.tag = TOK_PIPE;
			(*stmt)->u.ass.rhs->tag = EXPR_BIT_OR;
			goto assign_op;
assign_op:
			(*stmt)->u.ass.rhs->tok = tok;
			(*stmt)->u.ass.rhs->left = (*stmt)->u.ass.lhs;
			(*stmt)->u.ass.rhs->right = parse_expr(0);
			break;
		default:
			parse_die("expected an assignment, but got `%s`", get_token_str(tok));
		}
	}
	next_token();
	if (tok.tag != TOK_SEMICOLON)
		parse_die("expected `;`, but got `%s`", get_token_str(tok));
}

static struct stmt *parse_stmt(int depth);

static struct stmt *
parse_if(int depth)
{
	struct stmt *root = NULL, **sel;

	sel = &root;
	while (1) {
		*sel = alloc(&parse_alloc, sizeof(struct stmt));
		(*sel)->tag = STMT_SELECT;
		(*sel)->tok = tok;
		(*sel)->u.sel.cond = parse_expr(0);
		next_token();
		if (tok.tag != TOK_BRACE_L)
			parse_die("expected `{`, but got `%s`", get_token_str(tok));
		(*sel)->u.sel.ifbody = parse_stmt(depth + 1);
		next_token();
		if (tok.tag != TOK_BRACE_R)
			parse_die("expected `}`, but got `%s`", get_token_str(tok));
		next_token();
		if (tok.tag != TOK_ELSE) {
			undo_token();
			return root;
		}
		next_token();
		if (tok.tag == TOK_BRACE_L) {
			(*sel)->u.sel.elsebody = parse_stmt(depth + 1);
			next_token();
			if (tok.tag != TOK_BRACE_R)
				parse_die("expected `}`, but got `%s`", get_token_str(tok));
			return root;
		} else if (tok.tag != TOK_IF) {
			parse_die("expected `{` or `if`, but got `%s`", get_token_str(tok));
		}
		sel = &(*sel)->u.sel.elsebody;
		/* handle else-if */
	}
}

static struct stmt *
parse_while(int depth)
{
	struct stmt *root = NULL;

	root = alloc(&parse_alloc, sizeof(struct stmt));
	root->tag = STMT_LOOP;
	root->tok = tok;
	root->u.loop.cond = parse_expr(0);
	next_token();
	if (tok.tag != TOK_BRACE_L)
		parse_die("expected `{`, but got `%s`", get_token_str(tok));
	root->u.loop.body = parse_stmt(depth + 1);
	next_token();
	if (tok.tag != TOK_BRACE_R)
		parse_die("expected `}`, but got `%s`", get_token_str(tok));
	return root;
}

static struct stmt *
parse_stmt(int depth)
{
	struct stmt *root = NULL, **stmt;
	int unclosedbraces = 0;

	stmt = &root;
	while (1) {
		next_token();
		switch (tok.tag) {
		case TOK_BRACE_R:
			undo_token();
			return root;
		case TOK_RETURN:
			*stmt = alloc(&parse_alloc, sizeof(struct stmt));
			(*stmt)->tag = STMT_RETURN;
			(*stmt)->tok = tok;
			next_token();
			if (tok.tag != TOK_SEMICOLON) {
				undo_token();
				(*stmt)->u.expr = parse_expr(0);
				next_token();
				if (tok.tag != TOK_SEMICOLON)
					parse_die("expected `;`, but got `%s`", get_token_str(tok));
			}
			/* ignore code after return: */
			while (tok.tag != TOK_BRACE_R || unclosedbraces) {
				switch (tok.tag) {
				case TOK_BRACE_L:
					++unclosedbraces;
					break;
				case TOK_BRACE_R:
					--unclosedbraces;
					break;
				case TOK_NULL:
					parse_die("missing `}` after `return` statement");
				default:
					;
				}
				next_token();
			}
			undo_token();
			return root;
		case TOK_INT_KW:
			stmt = &parse_decl(stmt, depth)->next;
			break;
		case TOK_IF:
			*stmt = parse_if(depth + 1);
			stmt = &(*stmt)->next;
			break;
		case TOK_WHILE:
			*stmt = parse_while(depth + 1);
			stmt = &(*stmt)->next;
			break;
		case TOK_NAME: /* assignment or call */
			*stmt = alloc(&parse_alloc, sizeof(struct stmt));
			(*stmt)->tok = tok;
			next_token();
			tok_undo[1] = (*stmt)->tok;
			tok_undo[0] = tok;
			tok_undo_top = 2;
			if (tok.tag == TOK_PAREN_L) {
				(*stmt)->tag = STMT_CALL;
				(*stmt)->u.expr = parse_expr(0);
				if ((*stmt)->u.expr->tag != EXPR_CALL)
					parse_die("expected procedure call statement");
				next_token();
				if (tok.tag != TOK_SEMICOLON)
					parse_die("expected `;`, but got `%s`", get_token_str(tok));
			} else {
				parse_assign(stmt);
			}
			stmt = &(*stmt)->next;
			break;
		default:
			parse_die("expected `}` or a statement, but got `%s`",
				  get_token_str(tok));
		}
		/* handle next statement */
	}
}

static struct stmt *
get_decl(struct stmt *decls, const char *name)
{
	struct stmt *decl;

	for (decl = decls; decl; decl = decl->next)
		if (!strcmp(name, decl->tok.u.name))
			return decl;
	return NULL;
}

static struct proc *
get_proc(struct proc *procs, const char *name)
{
	struct proc *proc;

	for (proc = procs; proc; proc = proc->next)
		if (!strcmp(name, proc->tok.u.name))
			return proc;
	return NULL;
}

static void
dummy_main_for_evalcmd(struct token first, struct expr *e, char *procname)
{
	debuglog(__LINE__, "generate \"proc main(argc,argv){%s(...);}\"", procname);
	ast.procs = alloc(&parse_alloc, sizeof(struct proc));
	ast.procs->tok.tag = TOK_NAME;
	ast.procs->tok.u.name = "main";
	ast.procs->params = alloc(&parse_alloc, sizeof(struct stmt));
	ast.procs->params->tag = STMT_DECL;
	ast.procs->params->tok.tag = TOK_NAME;
	ast.procs->params->tok.u.name = "argc";
	ast.procs->params->next = alloc(&parse_alloc, sizeof(struct stmt));
	ast.procs->params->next->tag = STMT_DECL;
	ast.procs->params->next->tok.tag = TOK_NAME;
	ast.procs->params->next->tok.u.name = "argv";
	ast.procs->body = alloc(&parse_alloc, sizeof(struct stmt));
	ast.procs->body->tag = STMT_CALL;
	ast.procs->body->u.expr = alloc(&parse_alloc, sizeof(struct expr));
	ast.procs->body->u.expr->tag = EXPR_CALL;
	ast.procs->body->u.expr->tok.tag = TOK_PAREN_L;
	ast.procs->body->u.expr->left = alloc(&parse_alloc, sizeof(struct expr));
	ast.procs->body->u.expr->left->tag = EXPR_NAME;
	ast.procs->body->u.expr->left->tok.tag = TOK_NAME;
	ast.procs->body->u.expr->left->tok.u.name = procname;
	ast.procs->body->u.expr->right = alloc(&parse_alloc, sizeof(struct expr));
	ast.procs->body->u.expr->right->tag = EXPR_ARGS;
	ast.procs->body->u.expr->right->tok = first;
	ast.procs->body->u.expr->right->left = e;
}

static struct proc *
parse_proc(void)
{
	struct proc *proc;
	struct stmt **par;

	proc = alloc(&parse_alloc, sizeof(struct proc));
	next_token();
	if (tok.tag != TOK_NAME)
		parse_die("expected a name, but got `%s`", get_token_str(tok));
	proc->tok = tok;
	next_token();
	if (tok.tag != TOK_PAREN_L)
		parse_die("expected `(`, but got `%s`", get_token_str(tok));
	par = &proc->params;
	next_token();
	while (tok.tag != TOK_PAREN_R) {
		if (tok.tag != TOK_NAME)
			parse_die("expected `)` or a name, but got `%s`", get_token_str(tok));
		if (get_decl(proc->params, tok.u.name))
			parse_die("parameter redeclaration: `%s` in procedure `%s`",
				  tok.u.name, proc->tok.u.name);
		*par = alloc(&parse_alloc, sizeof(struct stmt));
		(*par)->tag = STMT_DECL;
		(*par)->tok = tok;
		next_token();
		if (tok.tag == TOK_COMMA)
			next_token();
		else if (tok.tag != TOK_PAREN_R)
			parse_die("expected `)` or `,`, but got `%s`", get_token_str(tok));
		par = &(*par)->next;
	}
	next_token();
	if (tok.tag != TOK_BRACE_L)
		parse_die("expected `{`, but got `%s`", get_token_str(tok));
	proc->body = parse_stmt(0);
	next_token();
	if (tok.tag != TOK_BRACE_R)
		parse_die("expected `}`, but got `%s`", get_token_str(tok));
	return proc;
}

static void
parse(void)
{
	struct stmt **decl;
	struct proc *proc;
	struct expr *cmdexpr;
	struct token cmdargtok, conv;

	memset(&ast, 0, sizeof(ast));
	decl = &ast.globals;
	debuglog(__LINE__, "parsing...");
	while (1) {
		next_token();
		switch (tok.tag) {
		case TOK_INT_KW:
			decl = &parse_decl(decl, 0)->next;
			break;
		case TOK_PROC:
			proc = parse_proc();
			if (get_proc(ast.procs, proc->tok.u.name))
				parse_die("procedure redefinition: `%s`",
					  proc->tok.u.name);
			if (ast.procs)
				proc->next = ast.procs;
			ast.procs = proc;
			break;
		case TOK_NULL:
			return;
		default:
			if (evalcmd) { /* try to read an expression instead of a program */
				cmdargtok = tok;
				undo_token();
				debuglog(__LINE__, "trying to parse expression...");
				cmdexpr = parse_expr(0);
				debuglog(__LINE__, "trying to parse expression...done");
				next_token();
				conv = tok;
				if (conv.tag == TOK_NAME && !strcmp(conv.u.name, "to") && (next_token(),1) &&
				    tok.tag == TOK_NAME && !strcmp(tok.u.name, "hex") && (next_token(),1) &&
				    tok.tag == TOK_NULL) {
					dummy_main_for_evalcmd(cmdargtok, cmdexpr, "PutHex");
					return;
				} else if (conv.tag == TOK_NULL) {
					dummy_main_for_evalcmd(cmdargtok, cmdexpr, "PutInt");
					return;
				}
				tok = conv;
				parse_die("unexpected symbol after expression: `%s`", get_token_str(tok));
			}
			parse_die("expected `int` or `proc`, but got `%s`", get_token_str(tok));
		}
	}
	debuglog(__LINE__, "parsing...done");
}

static void
print_expr(struct expr *e, int depth)
{
	struct expr *arg;

	switch (e->tag) {
	case EXPR_INT:
	case EXPR_NAME:
		fputs(get_token_str(e->tok), stdout);
		break;
	case EXPR_ARRIDX:
		if (depth)
			fputs("(", stdout);
		print_expr(e->left, depth + 1);
		fputs("[", stdout);
		print_expr(e->right, 0);
		fputs("]", stdout);
		if (depth)
			fputs(")", stdout);
		break;
	case EXPR_CALL:
		print_expr(e->left, depth + 1);
		fputs("(", stdout);
		if (e->right) {
			for (arg = e->right; arg->right; arg = arg->right) {
				print_expr(arg->left, 0);
				fputs(", ", stdout);
			}
			print_expr(arg->left, 0);
		}
		fputs(")", stdout);
		break;
	case EXPR_ADDROF:
	case EXPR_POS:
	case EXPR_NEG:
	case EXPR_NOT:
	case EXPR_BIT_NOT:
		if (depth)
			fputs("(", stdout);
		fputs(get_token_str(e->tok), stdout);
		fputs("(", stdout);
		print_expr(e->left, 0);
		fputs(")", stdout);
		if (depth)
			fputs(")", stdout);
		break;
	case EXPR_POW:
		if (depth)
			fputs("(", stdout);
		print_expr(e->left, depth + 1);
		fputs(" ", stdout);
		fputs(get_token_str(e->tok), stdout);
		fputs(" ", stdout);
		print_expr(e->right, depth + 1);
		if (depth)
			fputs(")", stdout);
		break;
	case EXPR_REM:
	case EXPR_DIV:
	case EXPR_MUL:
	case EXPR_SUB:
	case EXPR_ADD:
	case EXPR_BIT_SH_R:
	case EXPR_BIT_SH_L:
	case EXPR_GE:
	case EXPR_GT:
	case EXPR_LE:
	case EXPR_LT:
	case EXPR_NE:
	case EXPR_EQ:
	case EXPR_BIT_AND:
	case EXPR_BIT_XOR:
	case EXPR_BIT_OR:
	case EXPR_AND:
	case EXPR_OR:
		if (depth)
			fputs("(", stdout);
		print_expr(e->left, depth + 1);
		fputs(" ", stdout);
		fputs(get_token_str(e->tok), stdout);
		fputs(" ", stdout);
		print_expr(e->right, depth + 1);
		if (depth)
			fputs(")", stdout);
		break;
	default:
		assert(!"unexpected state");
	}
}

static void
print_indent(int indent)
{
	while (indent--)
		fputs("  ", stdout);
}

static void
print_stmt(struct stmt *stmt, int indent)
{
	if (!stmt)
		return;
	print_indent(indent);
	switch (stmt->tag) {
	case STMT_RETURN:
		fputs("return", stdout);
		if (stmt->u.expr) {
			fputs(" ", stdout);
			print_expr(stmt->u.expr, 0);
		}
		printf(";\t# %d:%d:%d\n",
		       stmt->tok.line,
		       stmt->tok.col,
		       stmt->tok.col_end);
		return;
	case STMT_DECL:
		fputs("int ", stdout);
		while (1) {
			fputs(stmt->tok.u.name, stdout);
			if (stmt->u.decl.elems) {
				fputs("[", stdout);
				print_expr(stmt->u.decl.elems, 0);
				fputs("]", stdout);
			} else if (stmt->u.decl.expr) {
				fputs(" = ", stdout);
				print_expr(stmt->u.decl.expr, 0);
			}
			if (!stmt->next || stmt->next->tag != STMT_DECL)
				break;
			printf(",\t# %d:%d:%d\n",
			       stmt->tok.line,
			       stmt->tok.col,
			       stmt->tok.col_end);
			print_indent(indent);
			fputs("    ", stdout);
			stmt = stmt->next;
		}
		printf(";\t# %d:%d:%d\n",
		       stmt->tok.line,
		       stmt->tok.col,
		       stmt->tok.col_end);
		print_stmt(stmt->next, indent);
		return;
	case STMT_ASSIGN:
		print_expr(stmt->u.ass.lhs, 0);
		fputs(" = ", stdout);
		print_expr(stmt->u.ass.rhs, 0);
		printf(";\t# %d:%d:%d\n",
		       stmt->tok.line,
		       stmt->tok.col,
		       stmt->tok.col_end);
		print_stmt(stmt->next, indent);
		return;
	case STMT_CALL:
		print_expr(stmt->u.expr, 0);
		printf(";\t# %d:%d:%d\n",
		       stmt->tok.line,
		       stmt->tok.col,
		       stmt->tok.col_end);
		print_stmt(stmt->next, indent);
		return;
	case STMT_SELECT:
		fputs("if ", stdout);
		print_expr(stmt->u.sel.cond, 0);
		printf(" {\t# %d:%d:%d\n",
		       stmt->tok.line,
		       stmt->tok.col,
		       stmt->tok.col_end);
		print_stmt(stmt->u.sel.ifbody, indent + 1);
		print_indent(indent);
		if (stmt->u.sel.elsebody) {
			fputs("} else {\n", stdout);
			print_stmt(stmt->u.sel.elsebody, indent + 1);
			print_indent(indent);
		}
		fputs("}\n", stdout);
		print_stmt(stmt->next, indent);
		return;
	case STMT_LOOP:
		fputs("while ", stdout);
		print_expr(stmt->u.loop.cond, 0);
		printf(" {\t# %d:%d:%d\n",
		       stmt->tok.line,
		       stmt->tok.col,
		       stmt->tok.col_end);
		print_stmt(stmt->u.loop.body, indent + 1);
		print_indent(indent);
		fputs("}\n", stdout);
		print_stmt(stmt->next, indent);
		return;
	default:
		assert(!"unexpected state");
	}
}

static void
print_program(struct ast a)
{
	struct stmt *gdecl, *par;
	struct proc *proc;

	printf("==> %s <==\n", script_path);
	if (a.globals)
		fputs("# globals\n", stdout);
	for (gdecl = a.globals; gdecl; gdecl = gdecl->next) {
		if (gdecl->u.decl.elems) {
			printf("int %s[", gdecl->tok.u.name);
			print_expr(gdecl->u.decl.elems, 0);
			fputs("];", stdout);
		} else if (gdecl->u.decl.expr) {
			printf("int %s = ", gdecl->tok.u.name);
			print_expr(gdecl->u.decl.expr, 0);
			fputs(";", stdout);
		} else {
			printf("int %s;", gdecl->tok.u.name);
		}
		printf(" # %d:%d:%d\n",
		       gdecl->tok.line,
		       gdecl->tok.col,
		       gdecl->tok.col_end);
	}
	if (a.globals)
		fputs("\n", stdout);
	if (a.procs)
		fputs("# procedures (printed reversed)\n", stdout);
	for (proc = a.procs; proc; proc = proc->next) {
		printf("proc %s(", proc->tok.u.name);
		if (proc->params) {
			for (par = proc->params; par->next; par = par->next)
				printf("%s, ", par->tok.u.name);
			fputs(par->tok.u.name, stdout);
		}
		fputs(") {", stdout);
		printf(" # %d:%d:%d\n",
		       proc->tok.line,
		       proc->tok.col,
		       proc->tok.col_end);
		print_stmt(proc->body, 1);
		fputs("}\n", stdout);
	}
	printf("==> END %s <==\n", script_path);
}

static struct var *
get_var(struct var *locs, const char *name)
{
	struct var *var;

	for (var = locs; var; var = var->next)
		if (!strcmp(name, var->tok->u.name))
			return var;
	for (var = globals; var; var = var->next)
		if (!strcmp(name, var->tok->u.name))
			return var;
	return NULL;
}

static int
push_word(struct token *t, int64_t x)
{
	int addr;

	if (words_top >= WORDS_SIZE)
		eval_die(t, "word stack overflow");
	addr = words_top++;
	assert(addr >= 0);
	assert(addr < WORDS_SIZE);
	words[addr] = x;
	return addr;
}

static int
push_arr(struct token *t, int n)
{
	int addr;

	if (n < 1)
		eval_die(t, "array size < 1 (%d)", n);
	if (words_top + n >= WORDS_SIZE)
		eval_die(t, "word stack overflow: array is too large (%d)", n);
	addr = words_top;
	assert(addr >= 0);
	assert(addr < WORDS_SIZE);
	words_top += n;
	return addr;
}

static uint64_t
power(uint64_t base, uint64_t exp)
{
	uint64_t res = 1;

	while (exp > 0) {
		if (exp & 1)
			res *= base;
		base *= base;
		exp >>= 1;
	}
	return res;
}

static struct var *eval_args(struct var *locs, struct stmt *params, struct expr *procexpr, struct expr *args);
static struct val eval_expr(struct var *locs, struct expr *e, int constex);
static struct val eval_stmt(struct var *locs, struct stmt *stmt);

static struct val
builtin_str_to_int(struct var *locs, struct expr *procexpr, struct expr *args)
{
	struct val v = {0};
	struct var *var = NULL;
	struct stmt *par;
	int i, c;
	int64_t addr;
	char buf[TOKEN_BUF_SIZE];

	par = alloc(&eval_alloc, sizeof(struct stmt));
	par->tag = STMT_DECL;
	par->tok.tag = TOK_NAME;
	par->tok.u.name = "_bi_digits";
	var = eval_args(locs, par, procexpr, args);
	/* function */
	addr = words[var->addr];
	if (addr <= 0 || addr + LENGTH(buf) > WORDS_SIZE)
		eval_die(&procexpr->tok, "%s: expected a \"string\" argument, like argv[i]",
			 procexpr->tok.u.name);
	c = (unsigned char)words[addr];
	for (i = 0; c && i < LENGTH(buf) - 1;) {
		buf[i++] = c;
		c = (unsigned char)words[addr + i];
	}
	buf[i] = 0;
	if (i == LENGTH(buf) - 1)
		eval_die(&procexpr->tok, "%s: \"string\" argument is too large: `%s...`",
			 procexpr->tok.u.name, buf);
	tok = procexpr->tok;
	v.i = str_to_int(buf);
	return v;
}

static struct val
builtin_put_int(struct var *locs, struct expr *procexpr, struct expr *args)
{
	struct val v = {0};
	struct var *var = NULL;
	struct stmt *par;
	int ret;
	int64_t no_nl;
	union typepunning {
		uint64_t u;
		int64_t i;
	} tp;

	par = alloc(&eval_alloc, sizeof(struct stmt)); /* no newline? */
	par->tag = STMT_DECL;
	par->tok.tag = TOK_NAME;
	par->tok.u.name = "_bi_no_newline";
	par->next = alloc(&eval_alloc, sizeof(struct stmt)); /* int */
	par->next->tag = STMT_DECL;
	par->next->tok.tag = TOK_NAME;
	par->next->tok.u.name = "_bi_int";
	var = eval_args(locs, par, procexpr, args);
	assert(var->next->addr >= 1);
	assert(var->next->addr < WORDS_SIZE);
	assert(var->addr >= 1);
	assert(var->addr < WORDS_SIZE);
	/* function */
	tp.i = words[var->next->addr];
	no_nl = words[var->addr];
	if (procexpr->tok.u.name[3] == 'H') { /* PutHex() */
		if (tp.u <= 0xff)
			ret = printf("%02"PRIx64"%s", tp.u, no_nl ? "" : "\n");
		else if (tp.u <= 0xffff)
			ret = printf("%04"PRIx64"%s", tp.u, no_nl ? "" : "\n");
		else if (tp.u <= 0xffffff)
			ret = printf("%06"PRIx64"%s", tp.u, no_nl ? "" : "\n");
		else if (tp.u <= 0xffffffff)
			ret = printf("%08"PRIx64"%s", tp.u, no_nl ? "" : "\n");
		else if (tp.u <= 0xffffffffff)
			ret = printf("%010"PRIx64"%s", tp.u, no_nl ? "" : "\n");
		else if (tp.u <= 0xffffffffffff)
			ret = printf("%012"PRIx64"%s", tp.u, no_nl ? "" : "\n");
		else if (tp.u <= 0xffffffffffffff)
			ret = printf("%014"PRIx64"%s", tp.u, no_nl ? "" : "\n");
		else if (tp.u <= 0xffffffffffffffff)
			ret = printf("%016"PRIx64"%s", tp.u, no_nl ? "" : "\n");
		else
			assert(!"unexpected state");
	} else {
		ret = printf("%"PRId64"%s", tp.i, no_nl ? "" : "\n");
	}
	v.i = ret >= 1 ? ret : -1;
	if (fflush(stdout) == EOF)
		v.i = -1;
	return v;
}

static struct val
builtin_put_char(struct var *locs, struct expr *procexpr, struct expr *args)
{
	struct val v = {0};
	struct var *var = NULL;
	struct stmt *par;
	int c;

	par = alloc(&eval_alloc, sizeof(struct stmt));
	par->tag = STMT_DECL;
	par->tok.tag = TOK_NAME;
	par->tok.u.name = "_bi_char";
	var = eval_args(locs, par, procexpr, args);
	/* function */
	c = fputc((unsigned char)words[var->addr], stdout);
	v.i = c == EOF ? -1 : c;
	if (c == '\n' && fflush(stdout) == EOF)
		v.i = -1;
	return v;
}

static struct val
builtin_get_char(struct var *locs, struct expr *procexpr, struct expr *args)
{
	struct val v = {0};
	int c;

	eval_args(locs, NULL, procexpr, args);
	/* function */
	c = fgetc(stdin);
	v.i = c == EOF ? -1 : c;
	return v;
}

static struct val
builtin_random(struct var *locs, struct expr *procexpr, struct expr *args)
{
	static int64_t defstate = 0;

	struct val v = {0};
	struct var *var = NULL;
	struct stmt *par;
	int64_t seed;

	par = alloc(&eval_alloc, sizeof(struct stmt));
	par->tag = STMT_DECL;
	par->tok.tag = TOK_NAME;
	par->tok.u.name = "_bi_seed";
	var = eval_args(locs, par, procexpr, args);
	/* function */
	seed = words[var->addr];
	if (seed <= 0 || seed >= 2147483647) {
		if (!defstate) {
			defstate = time(NULL) % 2147483647;
			if (!defstate)
				defstate = 1;
		}
		seed = defstate;
	}
	v.i = seed * 48271 % 2147483647;
	defstate = v.i * 48271 % 2147483647;
	return v;
}

static struct val
builtin_exit(struct var *locs, struct expr *procexpr, struct expr *args)
{
	struct val v = {0};
	struct var *var = NULL;
	struct stmt *par;

	par = alloc(&eval_alloc, sizeof(struct stmt));
	par->tag = STMT_DECL;
	par->tok.tag = TOK_NAME;
	par->tok.u.name = "_bi_code";
	var = eval_args(locs, par, procexpr, args);
	/* function */
	exit((unsigned char)(words[var->addr] & 255));
	return v;
}

static struct val
builtin_assert(struct var *locs, struct expr *procexpr, struct expr *args)
{
	struct val v = {0};
	struct var *var = NULL;
	struct stmt *par;

	par = alloc(&eval_alloc, sizeof(struct stmt));
	par->tag = STMT_DECL;
	par->tok.tag = TOK_NAME;
	par->tok.u.name = "_bi_expression";
	var = eval_args(locs, par, procexpr, args);
	/* function */
	if (words[var->addr])
		return v;
	eval_die(&procexpr->tok, "assertion failed");
	return v;
}

static const struct {
	const char *name;
	struct val (*proc)(struct var *, struct expr *, struct expr *);
} builtins[] = {
	{"StrToInt", builtin_str_to_int},
	{"PutInt", builtin_put_int},
	{"PutHex", builtin_put_int},
	{"PutChar", builtin_put_char},
	{"GetChar", builtin_get_char},
	{"Rand", builtin_random},
	{"Exit", builtin_exit},
	{"Assert", builtin_assert},
};

static struct var *
eval_args(struct var *locs, struct stmt *params, struct expr *procexpr, struct expr *args)
{
	struct val v = {0};
	struct var *calleelocs = NULL, *var;
	struct stmt *par;
	struct expr *arg;
	int paramcount = 0;

	for (par = params, arg = args; par; par = par->next) {
		var = alloc(&eval_alloc, sizeof(struct var));
		var->tok = &par->tok;
		if (arg) {
			v = eval_expr(locs, arg->left, 0); DEREF(v);
			var->addr = push_word(var->tok, v.i);
			arg = arg->right;
		} else {
			v.i = 0;
			var->addr = push_word(var->tok, v.i);
		}
		if (calleelocs)
			var->next = calleelocs;
		calleelocs = var;
		++paramcount;
	}
	if (arg)
		eval_die(&procexpr->tok, "cannot pass more than %d argument(s) to `%s`",
			 paramcount, procexpr->tok.u.name);
	return calleelocs;
}

static struct val
eval_call(struct var *locs, struct expr *e)
{
	struct val v = {0};
	struct proc *proc;
	struct expr *procexpr;
	const char *procname;
	int i;
	int prev_words_top;
	char *prev_eval_top;

	assert(e->tag == EXPR_CALL);
	assert(e->left->tag == EXPR_NAME);
	assert(!e->right || e->right->tag == EXPR_ARGS);
	procexpr = e->left;
	procname = procexpr->tok.u.name;
	proc = get_proc(ast.procs, procname);
	prev_words_top = words_top;
	prev_eval_top = eval_alloc.top;
	if (!proc) {
		/* if procedure wasn't found, check builtins (they start with a upper) */
		if (IS_UPPER(procname[0])) for (i = 0; i < LENGTH(builtins); ++i) {
			if (!strcmp(procname, builtins[i].name)) {
				/* call builtin */
				v = builtins[i].proc(locs, procexpr, e->right);
				goto cleanup;
			}
		}
		eval_die(&procexpr->tok, "procedure undefined: `%s`", procname);
		assert(!"unexpected state");
	}
	/* call */
	v = eval_stmt(eval_args(locs, proc->params, procexpr, e->right), proc->body); DEREF(v);
cleanup:
	memset(prev_eval_top, 0, (size_t)(eval_alloc.top - prev_eval_top));
	memset(words, 0, words_top - prev_words_top);
	eval_alloc.top = prev_eval_top;
	words_top = prev_words_top;
	return v;
}

static struct val
eval_expr(struct var *locs, struct expr *e, int constex)
{
	struct val v = {0}, w = {0};
	struct var *var;
	union typepunning {
		uint64_t u;
		int64_t i;
	} tp;

	switch (e->tag) {
	case EXPR_INT:
		v.i = e->tok.u.i;
		break;
	case EXPR_NAME:
		var = get_var(locs, e->tok.u.name);
		if (!var)
			eval_die(&e->tok, "variable undefined: `%s`", e->tok.u.name);
		v.i = var->addr;
		v.deref = 1;
		break;
	case EXPR_ARRIDX:
		v = eval_expr(locs, e->left, constex);
		w = eval_expr(locs, e->right, constex); DEREF(w);
		if (!v.deref)
			eval_die(&e->tok, "cannot subscript non-variable");
		assert(v.i >= 0 && v.i < WORDS_SIZE);
		tp.u = (uint64_t)words[v.i] + (uint64_t)w.i;
		if (tp.i < 0 || tp.i >= WORDS_SIZE)
			eval_die(&e->tok, "out of bounds word access (%"PRId64")", tp.i);
		v.i = tp.i;
		break;
	case EXPR_CALL:
		if (constex)
			eval_die(&e->tok, "cannot call outside procedure");
		v = eval_call(locs, e); DEREF(v);
		break;
	case EXPR_ADDROF:
		v = eval_expr(locs, e->left, constex);
		if (!v.deref)
			eval_die(&e->tok, "cannot get address of non-variable");
		v.deref = 0;
		break;
	case EXPR_POS:
		v = eval_expr(locs, e->left, constex); DEREF(v);
		break;
	case EXPR_NEG:
		v = eval_expr(locs, e->left, constex); DEREF(v);
		v.i = -v.i;
		break;
	case EXPR_NOT:
		v = eval_expr(locs, e->left, constex); DEREF(v);
		v.i = !v.i;
		break;
	case EXPR_BIT_NOT:
		v = eval_expr(locs, e->left, constex); DEREF(v);
		v.i = ~v.i;
		break;
	case EXPR_POW:
		v = eval_expr(locs, e->left, constex); DEREF(v);
		w = eval_expr(locs, e->right, constex); DEREF(w);
		if (v.i < 0 || w.i < 0)
			eval_die(&e->tok, "cannot use `%s` with negative operand",
				 get_token_str(e->tok));
		tp.u = power(v.i, w.i);
		v.i = tp.i;
		break;
	case EXPR_REM:
		v = eval_expr(locs, e->left, constex); DEREF(v);
		w = eval_expr(locs, e->right, constex); DEREF(w);
		if (w.i == 0)
			goto div_by_zero;
		else if (w.i == -1)
			v.i = 0;
		else
			v.i = v.i % w.i;
		break;
	case EXPR_DIV:
		v = eval_expr(locs, e->left, constex); DEREF(v);
		w = eval_expr(locs, e->right, constex); DEREF(w);
		if (w.i == 0)
			goto div_by_zero;
		else if (v.i == INT64_MIN && w.i == -1)
			v.i = INT64_MIN;
		else
			v.i = v.i / w.i;
		break;
	case EXPR_MUL:
		v = eval_expr(locs, e->left, constex); DEREF(v);
		w = eval_expr(locs, e->right, constex); DEREF(w);
		if (v.i == INT64_MIN && w.i == -1) {
			v.i = INT64_MIN;
		} else {
			tp.u = (uint64_t)v.i * (uint64_t)w.i;
			v.i = tp.i;
		}
		break;
	case EXPR_SUB:
		v = eval_expr(locs, e->left, constex); DEREF(v);
		w = eval_expr(locs, e->right, constex); DEREF(w);
		tp.u = (uint64_t)v.i - (uint64_t)w.i;
		v.i = tp.i;
		break;
	case EXPR_ADD:
		v = eval_expr(locs, e->left, constex); DEREF(v);
		w = eval_expr(locs, e->right, constex); DEREF(w);
		tp.u = (uint64_t)v.i + (uint64_t)w.i;
		v.i = tp.i;
		break;
	case EXPR_BIT_SH_R:
		v = eval_expr(locs, e->left, constex); DEREF(v);
		w = eval_expr(locs, e->right, constex); DEREF(w);
		if (w.i >= 0 && w.i <= 63) {
			tp.u = (uint64_t)v.i >> (uint64_t)w.i;
			v.i = tp.i;
		} else {
			v.i = 0;
		}
		break;
	case EXPR_BIT_SH_L:
		v = eval_expr(locs, e->left, constex); DEREF(v);
		w = eval_expr(locs, e->right, constex); DEREF(w);
		if (w.i >= 0 && w.i <= 63) {
			tp.u = (uint64_t)v.i << (uint64_t)w.i;
			v.i = tp.i;
		} else {
			v.i = 0;
		}
		break;
	case EXPR_GE:
		v = eval_expr(locs, e->left, constex); DEREF(v);
		w = eval_expr(locs, e->right, constex); DEREF(w);
		v.i = v.i >= w.i;
		break;
	case EXPR_GT:
		v = eval_expr(locs, e->left, constex); DEREF(v);
		w = eval_expr(locs, e->right, constex); DEREF(w);
		v.i = v.i > w.i;
		break;
	case EXPR_LE:
		v = eval_expr(locs, e->left, constex); DEREF(v);
		w = eval_expr(locs, e->right, constex); DEREF(w);
		v.i = v.i <= w.i;
		break;
	case EXPR_LT:
		v = eval_expr(locs, e->left, constex); DEREF(v);
		w = eval_expr(locs, e->right, constex); DEREF(w);
		v.i = v.i < w.i;
		break;
	case EXPR_NE:
		v = eval_expr(locs, e->left, constex); DEREF(v);
		w = eval_expr(locs, e->right, constex); DEREF(w);
		v.i = v.i != w.i;
		break;
	case EXPR_EQ:
		v = eval_expr(locs, e->left, constex); DEREF(v);
		w = eval_expr(locs, e->right, constex); DEREF(w);
		v.i = v.i == w.i;
		break;
	case EXPR_BIT_AND:
		v = eval_expr(locs, e->left, constex); DEREF(v);
		w = eval_expr(locs, e->right, constex); DEREF(w);
		v.i &= w.i;
		break;
	case EXPR_BIT_XOR:
		v = eval_expr(locs, e->left, constex); DEREF(v);
		w = eval_expr(locs, e->right, constex); DEREF(w);
		v.i ^= w.i;
		break;
	case EXPR_BIT_OR:
		v = eval_expr(locs, e->left, constex); DEREF(v);
		w = eval_expr(locs, e->right, constex); DEREF(w);
		v.i |= w.i;
		break;
	case EXPR_AND:
		v = eval_expr(locs, e->left, constex); DEREF(v);
		if (v.i) { /* short-circuit */
			w = eval_expr(locs, e->right, constex); DEREF(w);
			v.i = v.i && w.i;
		}
		break;
	case EXPR_OR:
		v = eval_expr(locs, e->left, constex); DEREF(v);
		if (!v.i) { /* short-circuit */
			w = eval_expr(locs, e->right, constex); DEREF(w);
			v.i = v.i || w.i;
		} else {
			v.i = 1;
		}
		break;
	default:
		assert(!"unexpected state");
	}
	return v;
div_by_zero:
	eval_die(&e->tok, "cannot divide by zero");
	return v;
}

static struct val
eval_stmt(struct var *locs, struct stmt *stmt)
{
	struct val v = {0}, w = {0};
	struct var *var;

	while (stmt) {
		switch (stmt->tag) {
		case STMT_RETURN:
			if (stmt->u.expr) {
				v = eval_expr(locs, stmt->u.expr, 0); DEREF(v);
			} else {
				memset(&v, 0, sizeof(v));
			}
			v.ret = 1; /* flag to return early */
			return v;
		case STMT_DECL:
			var = alloc(&eval_alloc, sizeof(struct var));
			var->tok = &stmt->tok;
			if (stmt->u.decl.elems) {
				v = eval_expr(locs, stmt->u.decl.elems, 0); DEREF(v);
				var->addr = push_word(var->tok, push_arr(var->tok, v.i));
				var->elems = v.i;
			} else if (stmt->u.decl.expr) {
				v = eval_expr(locs, stmt->u.decl.expr, 0); DEREF(v);
				var->addr = push_word(var->tok, v.i);
			} else {
				var->addr = push_word(var->tok, 0);
			}
			if (locs)
				var->next = locs;
			locs = var;
			break;
		case STMT_ASSIGN:
			v = eval_expr(locs, stmt->u.ass.lhs, 0);
			w = eval_expr(locs, stmt->u.ass.rhs, 0); DEREF(w);
			if (!v.deref)
				eval_die(&stmt->tok, "cannot assign to non-variable");
			if (!v.i)
				eval_die(&stmt->tok, "cannot assign to null");
			assert(v.i >= 1);
			assert(v.i < WORDS_SIZE);
			words[v.i] = w.i;
			break;
		case STMT_CALL:
			v = eval_expr(locs, stmt->u.expr, 0); DEREF(v);
			break;
		case STMT_SELECT:
			v = eval_expr(locs, stmt->u.sel.cond, 0); DEREF(v);
			if (v.i) {
				v = eval_stmt(locs, stmt->u.sel.ifbody); DEREF(v);
			} else {
				v = eval_stmt(locs, stmt->u.sel.elsebody); DEREF(v);
			}
			if (v.ret)
				return v;
			break;
		case STMT_LOOP:
			while (1) {
				v = eval_expr(locs, stmt->u.loop.cond, 0); DEREF(v);
				if (!v.i)
					break;
				v = eval_stmt(locs, stmt->u.loop.body); DEREF(v);
				if (v.ret)
					return v;
			}
			break;
		default:
			assert(!"unexpected state");
		}
		stmt = stmt->next;
	}
	memset(&v, 0, sizeof(v));
	return v;
}

static int
eval(int argc, char **argv)
{
	struct val v = {0};
	struct var *locs = NULL, *gvar;
	struct proc *mainproc;
	struct stmt *gdecl;
	int i, j, off = 0;
	int argc_off;
	int prev_words_top;
	char *prev_eval_top;
	clock_t clkbeg, clkend;

	debuglog(__LINE__, "evaluating...");
	mainproc = get_proc(ast.procs, "main");
	if (!mainproc)
		eval_die(NULL, "missing procedure `main`");
	prev_eval_top = eval_alloc.top;
	prev_words_top = push_word(NULL, 0);
	/* initialize globals */
	for (gdecl = ast.globals; gdecl; gdecl = gdecl->next) {
		gvar = alloc(&eval_alloc, sizeof(struct var));
		gvar->tok = &gdecl->tok;
		if (gdecl->u.decl.elems) {
			v = eval_expr(NULL, gdecl->u.decl.elems, 1); DEREF(v);
			gvar->addr = push_word(gvar->tok, push_arr(gvar->tok, v.i));
			gvar->elems = v.i;
		} else if (gdecl->u.decl.expr) {
			v = eval_expr(NULL, gdecl->u.decl.expr, 1); DEREF(v);
			gvar->addr = push_word(gvar->tok, v.i);
		} else {
			gvar->addr = push_word(gvar->tok, 0);
		}
		if (globals)
			gvar->next = globals;
		globals = gvar;
	}
	for (i = 0; i < 15; ++i)
		push_word(NULL, 0xcccccccccccccccc);
	/* handle main(argc, argv) */
	if (mainproc->params) {
		if (!mainproc->params->next || mainproc->params->next->next)
			eval_die(&mainproc->tok, "`main` only accepts 0 or 2 parameters (argc, argv)");
		argc_off = push_word(NULL, argc);
		push_word(NULL, 0); /*argv*/
		off = argc_off + 1/*argc*/ + 1/*argv*/ + argc + 1/*argv[argc]*/;
		for (i = 0; i < argc; ++i) {
			push_word(NULL, off);
			off += strlen(argv[i]) + 1;
		}
		push_word(NULL, 0); /* argv[argc] == NULL */
		for (i = 0; i < argc; ++i) {
			for (j = 0; argv[i][j]; ++j)
				push_word(NULL, argv[i][j]);
			push_word(NULL, 0);
		}
		locs = alloc(&eval_alloc, sizeof(struct var)); /* argc */
		locs->tok = &mainproc->params->tok;
		locs->addr = argc_off;
		locs->next = alloc(&eval_alloc, sizeof(struct var)); /* argv */
		locs->next->tok = &mainproc->params->next->tok;
		locs->next->addr = argc_off + 1;
		words[locs->next->addr] = argc_off + 2;
	}
	if (debug && mainproc->params) {
		debuglog(__LINE__, "words[%d..]: (each word truncated by 1 byte)", prev_words_top);
		for (i = prev_words_top; words[i] || words[i + 1] || i < off; ++i) {
			printf("%02x ", (unsigned int)(words[i] & 255));
			if ((i + 1) % 16 == 0)
				putchar('\n');
			else if ((i + 1) % 8 == 0)
				putchar(' ');
		}
		printf("%02x\n", (unsigned int)(words[i] & 255));
	}
	if (debug) {
		debuglog(__LINE__, "calling main()");
		clkbeg = clock();
	}
	/* call main */
	v = eval_stmt(locs, mainproc->body); DEREF(v);
	if (debug) {
		clkend = clock();
		debuglog(__LINE__, "main() returned %"PRId64" (0x%"PRIx64"); cputime = %"PRIu64" ms",
			 v.i, (uint64_t)v.i, (uint64_t)(clkend - clkbeg) * 1000 / CLOCKS_PER_SEC);
	}
	/* final cleanup */
	memset(prev_eval_top, 0, (size_t)(eval_alloc.top - prev_eval_top));
	memset(words, 0, words_top - prev_words_top);
	eval_alloc.top = prev_eval_top;
	words_top = prev_words_top;
	assert(prev_eval_top == eval_alloc.beg);
	assert(prev_words_top == 0);
	return v.i & 255;
}

static void
usage(void)
{
	fprintf(stdout,
		"usage: %s SCRIPT [ARGUMENTS...]\n"
		"       %s -h           # show this\n"
		"       %s -v           # show version\n"
		"       %s -c COMMAND   # evaluate COMMAND\n"
		"\n"
		"error format:\n"
		"<script>:<line>:<column>: ERROR: <message>\n"
		"\n"
		"expected entry point:\n"
		"proc main() {}           # no command-line arguments\n"
		"proc main(argc, argv) {} # argument count and vector\n",
		progname, progname, progname, progname);
}

int
main(int argc, char **argv)
{
	progname = argv[0];
	--argc; ++argv;
	while (argc >= 1 && argv[0][0] == '-') {
		if (!strcmp(argv[0], "-h") || !strcmp(argv[0], "--help")) {
			usage();
			return 0;
		} else if (!strcmp(argv[0], "-v")) {
			puts(VERSION);
			return 0;
		} else if (!strcmp(argv[0], "-c")) {
			evalcmd = 1;
			--argc; ++argv;
			break;
		} else if (!strcmp(argv[0], "-D")) {
			debug = 1;
			--argc; ++argv;
		} else {
			usage();
			return 2;
		}
	}
	if (argc < 1) {
		usage();
		return 2;
	}
	assert(argv[0]);
	parse_alloc = bumpalloc_new(parse_data, sizeof(parse_data));
	eval_alloc = bumpalloc_new(eval_data, sizeof(eval_data));
	debuglog(__LINE__, "version: %s", VERSION);
	if (evalcmd) {
#ifdef __linux__
		FILE *fmemopen(void *buf, size_t size, const char *mode);
		script_path = "<cmd>";
		script_file = fmemopen(argv[0], strlen(argv[0]), "r");
		if (!script_file)
			parse_die("cannot read command");
		argv[0] = progname;
#else
		script_path = progname;
		parse_die("not implemented");
#endif
	} else {
		script_path = argv[0];
		script_file = fopen(script_path, "r");
		if (!script_file)
			parse_die("cannot open script to read");
	}
	parse();
	fclose(script_file);
	script_file = NULL;
	if (debug) {
		debuglog(__LINE__, "print AST:");
		print_program(ast);
	}
	return eval(argc, argv);
}
