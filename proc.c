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

#define VERSION "WIP -- https://github.com/danielsource/proc -- public domain"
#define TOKEN_BUF_SIZE (64)
#define PARSE_DATA_SIZE (50*1000*1000)
#define EVAL_DATA_SIZE (50*1000*1000)
#define WORDS_SIZE (500*1000)
#define UNARY_PREC 10

#define ROUND_UP(n, to) (((n) + ((to) - 1)) & ~((to) - 1))
#define LENGTH(x) ((int)(sizeof(x) / sizeof((x)[0])))
#define DEREF(v) (v.deref = v.deref ? (v.i = words[v.i], 0) : 0)

/* assumes ASCII */
#define IS_SPACE(c) (((c) >= 0 && (c) <= 32) || (c) == 127)
#define IS_DIGIT(c) ((c) >= '0' && (c) <= '9')
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
		const char *name;
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

int debug = 0;

const char *argv0, *script_path;
FILE *script_file;

char parse_data[PARSE_DATA_SIZE];
struct bumpalloc parse_alloc;

char eval_data[EVAL_DATA_SIZE];
struct bumpalloc eval_alloc;

int64_t words[WORDS_SIZE];
int words_top;

struct token tok = {TOK_START};
struct token tok_undo[2];
int tok_undo_top = 0;

struct ast ast;

struct var *globals;

int compiletime_assert1(int [sizeof(int) >= 4   ?1:-1]);
int compiletime_assert2(int [(int64_t)-1 == ~0  ?1:-1]);

/* implementation */

void
dlog(int ln, const char *msg, ...)
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

struct bumpalloc
bumpalloc_new(void *p, int capacity)
{
	struct bumpalloc a;

	assert(p);
	assert(capacity >= 1);
	a.beg = a.top = p;
	a.end = (char *)p + capacity;
	return a;
}

void *
alloc(struct bumpalloc *a, int n)
{
	char *new, *last;

	assert(n >= 1);
	new = a->top + ROUND_UP(n, sizeof(void *));
	assert(new >= a->beg);
	assert(new >= a->top);
	assert(new <= a->end);
	last = a->top;
	a->top = new;
	return last;
}

void
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
	exit(2);
}

void
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
	exit(3);
}

int64_t
str_to_int(const char *digits)
{
	int64_t n = 0, sign = 1, base = 10, digitval;
	const char *s;

	s = digits;
	if (!*s) {
		goto invalid_int_lit;
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
			goto invalid_int_lit;
		}
		s += 2;
		if (!*s)
			goto invalid_int_lit;
	}
	while (*s) {
		if (IS_DIGIT(*s))
			digitval = *s - '0';
		else if (base == 16 && *s >= 'A' && *s <= 'F') /* A-F */
			digitval = *s - 'A' + 10;
		else if (base == 16 && *s >= 'a' && *s <= 'f') /* a-f */
			digitval = *s - 'a' + 10;
		else
			goto invalid_int_lit;
		if (digitval >= base)
			goto invalid_int_lit;
		if (n > INT64_MAX / base)
			parse_die("integer literal is too large: `%s` [A]", digits);
		n *= base;
		if (n > INT64_MAX - digitval)
			parse_die("integer literal is too large: `%s` [B]", digits);
		n += digitval;
		++s;
	}
	return n * sign;
invalid_int_lit:
	parse_die("invalid integer literal: `%s`", digits);
	return 0;
}

const char *
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
void
next_token(void)
{
	int c, i, line, col;
	char buf[TOKEN_BUF_SIZE], *name;

	if (tok.tag == TOK_NULL)
		return;
	else if (tok_undo_top && tok_undo[tok_undo_top - 1].tag != TOK_NULL) {
		tok = tok_undo[tok_undo_top - 1];
		memset(&tok_undo[tok_undo_top - 1], 0, sizeof(*tok_undo));
		--tok_undo_top;
		return;
	}
	if (feof(script_file)) {
		tok.tag = TOK_NULL;
		return;
	} else if (ferror(script_file)) {
		parse_die("reading script failed");
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
			}
			goto end_consuming;
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
			}
			goto end_consuming;
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
		if (c == EOF)
			parse_die("unexpected end-of-file");
		else if (c > 127)
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

void
undo_token(void)
{
	assert(tok_undo_top < LENGTH(tok_undo));
	assert(tok_undo[tok_undo_top].tag == TOK_NULL);
	++tok_undo_top;
	tok_undo[tok_undo_top - 1] = tok;
}

struct expr *parse_expr(int minprec);

struct expr *
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
	e = alloc(&parse_alloc, sizeof(*e));
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

enum expr_tag
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

struct expr *
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
			parse_die("expected a procedure name, but got `%s` expression", get_token_str(left->tok));
		arg = &right;
		next_token();
		while (tok.tag != TOK_PAREN_R) {
			undo_token();
			*arg = alloc(&parse_alloc, sizeof(*e));
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
	e = alloc(&parse_alloc, sizeof(*e));
	e->tag = tag;
	e->tok = op;
	e->left = left;
	e->right = right;
	return e;
}

/* https://www.youtube.com/watch?v=fIPO4G42wYE */
struct expr *
parse_expr(int minprec)
{
	struct expr *e, *left;

	left = parse_unary_expr();
	while (1) {
		e = parse_binary_expr(left, minprec);
		if (e == left)
			break;
		left = e;
	}
	return e;
}

struct stmt *
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

void
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

struct stmt *parse_stmt(int depth);

struct stmt *
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

struct stmt *
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

struct stmt *
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
					parse_die("expected procedure call statement",
						  get_token_str((*stmt)->u.expr->tok));
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

struct stmt *
get_decl(struct stmt *decls, const char *name)
{
	struct stmt *decl;

	for (decl = decls; decl; decl = decl->next)
		if (!strcmp(decl->tok.u.name, name))
			return decl;
	return NULL;
}

struct proc *
get_proc(struct proc *procs, const char *name)
{
	struct proc *proc;

	for (proc = procs; proc; proc = proc->next)
		if (!strcmp(proc->tok.u.name, name))
			return proc;
	return NULL;
}

struct proc *
parse_proc(void)
{
	struct proc *proc;
	struct stmt *par;

	proc = alloc(&parse_alloc, sizeof(*proc));
	next_token();
	if (tok.tag != TOK_NAME)
		parse_die("expected a name, but got `%s`", get_token_str(tok));
	proc->tok = tok;
	next_token();
	if (tok.tag != TOK_PAREN_L)
		parse_die("expected `(`, but got `%s`", get_token_str(tok));
	next_token();
	while (tok.tag != TOK_PAREN_R) {
		if (tok.tag != TOK_NAME)
			parse_die("expected `)` or a name, but got `%s`", get_token_str(tok));
		par = alloc(&parse_alloc, sizeof(*par));
		par->tag = STMT_DECL;
		par->tok = tok;
		if (get_decl(proc->params, tok.u.name))
			parse_die("parameter redeclaration: `%s` in procedure `%s`",
				  proc->tok.u.name, tok.u.name);
		if (proc->params)
			par->next = proc->params;
		proc->params = par;
		next_token();
		if (tok.tag == TOK_COMMA)
			next_token();
		else if (tok.tag != TOK_PAREN_R)
			parse_die("expected `)` or `,`, but got `%s`", get_token_str(tok));
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

void
parse(void)
{
	struct stmt **decl;
	struct proc *proc;
	int eof = 0;

	script_file = fopen(script_path, "r");
	if (!script_file)
		parse_die("cannot open script to read");
	memset(&ast, 0, sizeof(ast));
	decl = &ast.globals;
	dlog(__LINE__, "parsing...");
	while (!eof) {
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
			eof = 1;
			break;
		default:
			parse_die("expected `int` or `proc`, but got `%s`", get_token_str(tok));
		}
	}
	dlog(__LINE__, "parsing...done");
	fclose(script_file);
	script_file = NULL;
}

void
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

void
print_indent(int indent)
{
	while (indent--)
		fputs("  ", stdout);
}

void
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

void
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
		fputs("# procedures (procedures/parameters are printed reversed)\n", stdout);
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

struct var *
get_var(struct var *locs, const char *name)
{
	struct var *var;

	for (var = locs; var; var = var->next)
		if (!strcmp(var->tok->u.name, name))
			return var;
	for (var = globals; var; var = var->next)
		if (!strcmp(var->tok->u.name, name))
			return var;
	return NULL;
}

int
push_word(struct token *t, int64_t x)
{
	int addr;

	if (words_top + 1 > WORDS_SIZE)
		eval_die(t, "word stack overflow");
	addr = words_top++;
	words[addr] = x;
	return addr;
}

int
push_arr(struct token *t, int n)
{
	int addr;

	if (n < 1)
		eval_die(t, "array size < 1 (%d)", n);
	if (words_top + n > WORDS_SIZE)
		eval_die(t, "word stack overflow: array is too large (%d)", n);
	addr = words_top;
	words_top += n;
	return addr;
}

uint64_t
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

struct var *eval_args(struct var *locs, struct stmt *params, struct expr *procexpr, struct expr *args);
struct val eval_expr(struct var *locs, struct expr *e, int constexpr);
struct val eval_stmt(struct var *locs, struct stmt *stmt);

struct val
builtin_str_to_int(struct var *locs, struct expr *procexpr, struct expr *args) /* arity: 1 */
{
	struct val v = {0};
	struct var *var = NULL;
	struct stmt *par;
	int i, c;
	int64_t addr;
	char buf[TOKEN_BUF_SIZE];

	par = alloc(&eval_alloc, sizeof(*par));
	par->tag = STMT_DECL;
	par->tok.tag = TOK_NAME;
	par->tok.u.name = "_bi_digits";
	var = eval_args(locs, par, procexpr, args);
	addr = words[var->addr];
	if (addr <= 0 || addr + TOKEN_BUF_SIZE > WORDS_SIZE)
		eval_die(&procexpr->tok, "%s: expected a \"string\" argument, like argv[i]",
			 procexpr->tok.u.name);
	c = (unsigned char)words[addr];
	for (i = 0; c && i < LENGTH(buf) - 1;) {
		buf[i++] = c;
		c = (unsigned char)words[addr + i];
	}
	buf[i] = 0;
	if (i == LENGTH(buf) - 1)
		eval_die(&procexpr->tok, "%s: \"string\" argument is too large",
			 procexpr->tok.u.name);
	tok = procexpr->tok;
	v.i = str_to_int(buf);
	return v;
}

struct val
builtin_put_int(struct var *locs, struct expr *procexpr, struct expr *args) /* arity: 1 */
{
	struct val v = {0};
	struct var *var = NULL;
	struct stmt *par;
	int ret;

	par = alloc(&eval_alloc, sizeof(*par));
	par->tag = STMT_DECL;
	par->tok.tag = TOK_NAME;
	par->tok.u.name = "_bi_int";
	var = eval_args(locs, par, procexpr, args);
	ret = printf("%"PRId64"\n", words[var->addr]);
	v.i = ret >= 1 ? ret : -1;
	if (fflush(stdout) == EOF)
		v.i = -1;
	return v;
}

struct val
builtin_put_char(struct var *locs, struct expr *procexpr, struct expr *args) /* arity: 1 */
{
	struct val v = {0};
	struct var *var = NULL;
	struct stmt *par;
	int c;

	par = alloc(&eval_alloc, sizeof(*par));
	par->tag = STMT_DECL;
	par->tok.tag = TOK_NAME;
	par->tok.u.name = "_bi_char";
	var = eval_args(locs, par, procexpr, args);
	c = fputc(words[var->addr], stdout);
	v.i = c != EOF ? c : -1;
	if (c == '\n' && fflush(stdout) == EOF)
		v.i = -1;
	return v;
}

struct val
builtin_get_char(struct var *locs, struct expr *procexpr, struct expr *args) /* arity: 0 */
{
	struct val v = {0};
	int c;

	eval_args(locs, NULL, procexpr, args);
	c = fgetc(stdin);
	v.i = c != EOF ? c : -1;
	return v;
}

struct val
builtin_random(struct var *locs, struct expr *procexpr, struct expr *args) /* arity: 1 */
{
	static int defstate = 0;

	struct val v = {0};
	struct var *var = NULL;
	struct stmt *par;
	int64_t seed;

	par = alloc(&eval_alloc, sizeof(*par));
	par->tag = STMT_DECL;
	par->tok.tag = TOK_NAME;
	par->tok.u.name = "_bi_seed";
	var = eval_args(locs, par, procexpr, args);
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
	defstate = v.i;
	return v;
}

struct val
builtin_exit(struct var *locs, struct expr *procexpr, struct expr *args) /* arity: 1 */
{
	struct val v = {0};
	struct var *var = NULL;
	struct stmt *par;

	par = alloc(&eval_alloc, sizeof(*par));
	par->tag = STMT_DECL;
	par->tok.tag = TOK_NAME;
	par->tok.u.name = "_bi_code";
	var = eval_args(locs, par, procexpr, args);
	exit(words[var->addr]);
	return v;
}

struct val
builtin_assert(struct var *locs, struct expr *procexpr, struct expr *args) /* arity: 1 */
{
	struct val v = {0};
	struct var *var = NULL;
	struct stmt *par;

	par = alloc(&eval_alloc, sizeof(*par));
	par->tag = STMT_DECL;
	par->tok.tag = TOK_NAME;
	par->tok.u.name = "_bi_code";
	var = eval_args(locs, par, procexpr, args);
	if (words[var->addr])
		return v;
	eval_die(&procexpr->tok, "assertion failed");
	return v;
}

const struct {
	const char *name;
	struct val (*proc)(struct var *, struct expr *, struct expr *);
} builtins[] = {
	{"StrToInt", builtin_str_to_int},
	{"PutInt", builtin_put_int},
	{"PutChar", builtin_put_char},
	{"GetChar", builtin_get_char},
	{"Rand", builtin_random},
	{"Exit", builtin_exit},
	{"Assert", builtin_assert},
};

struct var *
eval_args(struct var *locs, struct stmt *params, struct expr *procexpr, struct expr *args)
{
	struct val v = {0};
	struct var *calleelocs = NULL, *var;
	struct stmt *par;
	struct expr *arg;
	int paramcount = 0;

	for (par = params, arg = args; par; par = par->next) {
		var = alloc(&eval_alloc, sizeof(*var));
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
		eval_die(&procexpr->tok, "cannot pass more than `%d` arguments to `%s`",
			 paramcount, procexpr->tok.u.name);
	return calleelocs;
}

struct val
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
	assert(e->right->tag == EXPR_ARGS);
	procexpr = e->left;
	procname = procexpr->tok.u.name;
	proc = get_proc(ast.procs, procname);
	prev_words_top = words_top;
	prev_eval_top = eval_alloc.top;
	if (!proc) {
		for (i = 0; i < LENGTH(builtins); ++i) {
			if (!strcmp(procname, builtins[i].name)) {
				/* call builtin */
				v = builtins[i].proc(locs, procexpr, e->right);
				goto cleanup;
			}
		}
		eval_die(&procexpr->tok, "procedure undefined: `%s`", procname);
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

struct val
eval_expr(struct var *locs, struct expr *e, int constexpr)
{
	struct val v = {0}, w = {0};
	struct var *var;
	union typepunning {
		int64_t u;
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
		v = eval_expr(locs, e->left, constexpr);
		w = eval_expr(locs, e->right, constexpr); DEREF(w);
		if (!v.deref)
			eval_die(&e->tok, "cannot subscript non-variable");
		if (v.i < 0 || v.i >= WORDS_SIZE)
			eval_die(&e->tok, "out of bounds word access (%"PRId64") [A]", v.i);
		tp.u = (uint64_t)words[v.i] + (uint64_t)w.i;
		if (tp.i < 0 || tp.i >= WORDS_SIZE)
			eval_die(&e->tok, "out of bounds word access (%"PRId64") [B]", tp.i);
		v.i = tp.i;
		break;
	case EXPR_CALL:
		if (constexpr)
			eval_die(&e->tok, "expression is not constant");
		v = eval_call(locs, e); DEREF(v);
		break;
	case EXPR_ADDROF:
		v = eval_expr(locs, e->left, constexpr);
		if (!v.deref)
			eval_die(&e->tok, "cannot get address of non-variable");
		v.deref = 0;
		break;
	case EXPR_POS:
		v = eval_expr(locs, e->left, constexpr); DEREF(v);
		break;
	case EXPR_NEG:
		v = eval_expr(locs, e->left, constexpr); DEREF(v);
		v.i = -v.i;
		break;
	case EXPR_NOT:
		v = eval_expr(locs, e->left, constexpr); DEREF(v);
		v.i = !v.i;
		break;
	case EXPR_BIT_NOT:
		v = eval_expr(locs, e->left, constexpr); DEREF(v);
		v.i = ~v.i;
		break;
	case EXPR_POW:
		v = eval_expr(locs, e->left, constexpr); DEREF(v);
		w = eval_expr(locs, e->right, constexpr); DEREF(w);
		if (v.i < 0 || w.i < 0)
			eval_die(&e->tok, "cannot use `%s` with negative operand",
				 get_token_str(e->tok));
		tp.u = power(v.i, w.i);
		v.i = tp.i;
		break;
	case EXPR_REM:
		v = eval_expr(locs, e->left, constexpr); DEREF(v);
		w = eval_expr(locs, e->right, constexpr); DEREF(w);
		if (w.i == 0)
			goto div_by_zero;
		tp.u = (uint64_t)v.i % (uint64_t)w.i;
		v.i = tp.i;
		break;
	case EXPR_DIV:
		v = eval_expr(locs, e->left, constexpr); DEREF(v);
		w = eval_expr(locs, e->right, constexpr); DEREF(w);
		if (w.i == 0)
			goto div_by_zero;
		tp.u = (uint64_t)v.i / (uint64_t)w.i;
		v.i = tp.i;
		break;
	case EXPR_MUL:
		v = eval_expr(locs, e->left, constexpr); DEREF(v);
		w = eval_expr(locs, e->right, constexpr); DEREF(w);
		tp.u = (uint64_t)v.i * (uint64_t)w.i;
		v.i = tp.i;
		break;
	case EXPR_SUB:
		v = eval_expr(locs, e->left, constexpr); DEREF(v);
		w = eval_expr(locs, e->right, constexpr); DEREF(w);
		tp.u = (uint64_t)v.i - (uint64_t)w.i;
		v.i = tp.i;
		break;
	case EXPR_ADD:
		v = eval_expr(locs, e->left, constexpr); DEREF(v);
		w = eval_expr(locs, e->right, constexpr); DEREF(w);
		tp.u = (uint64_t)v.i + (uint64_t)w.i;
		v.i = tp.i;
		break;
	case EXPR_BIT_SH_R:
		v = eval_expr(locs, e->left, constexpr); DEREF(v);
		w = eval_expr(locs, e->right, constexpr); DEREF(w);
		if (w.i >= 0 && w.i <= 63) {
			tp.u = (uint64_t)v.i >> (uint64_t)w.i;
			v.i = tp.i;
		} else {
			v.i = 0;
		}
		break;
	case EXPR_BIT_SH_L:
		v = eval_expr(locs, e->left, constexpr); DEREF(v);
		w = eval_expr(locs, e->right, constexpr); DEREF(w);
		if (w.i >= 0 && w.i <= 63) {
			tp.u = (uint64_t)v.i << (uint64_t)w.i;
			v.i = tp.i;
		} else {
			v.i = 0;
		}
		break;
	case EXPR_GE:
		v = eval_expr(locs, e->left, constexpr); DEREF(v);
		w = eval_expr(locs, e->right, constexpr); DEREF(w);
		v.i = v.i >= w.i;
		break;
	case EXPR_GT:
		v = eval_expr(locs, e->left, constexpr); DEREF(v);
		w = eval_expr(locs, e->right, constexpr); DEREF(w);
		v.i = v.i > w.i;
		break;
	case EXPR_LE:
		v = eval_expr(locs, e->left, constexpr); DEREF(v);
		w = eval_expr(locs, e->right, constexpr); DEREF(w);
		v.i = v.i <= w.i;
		break;
	case EXPR_LT:
		v = eval_expr(locs, e->left, constexpr); DEREF(v);
		w = eval_expr(locs, e->right, constexpr); DEREF(w);
		v.i = v.i < w.i;
		break;
	case EXPR_NE:
		v = eval_expr(locs, e->left, constexpr); DEREF(v);
		w = eval_expr(locs, e->right, constexpr); DEREF(w);
		v.i = v.i != w.i;
		break;
	case EXPR_EQ:
		v = eval_expr(locs, e->left, constexpr); DEREF(v);
		w = eval_expr(locs, e->right, constexpr); DEREF(w);
		v.i = v.i == w.i;
		break;
	case EXPR_BIT_AND:
		v = eval_expr(locs, e->left, constexpr); DEREF(v);
		w = eval_expr(locs, e->right, constexpr); DEREF(w);
		v.i &= w.i;
		break;
	case EXPR_BIT_XOR:
		v = eval_expr(locs, e->left, constexpr); DEREF(v);
		w = eval_expr(locs, e->right, constexpr); DEREF(w);
		v.i ^= w.i;
		break;
	case EXPR_BIT_OR:
		v = eval_expr(locs, e->left, constexpr); DEREF(v);
		w = eval_expr(locs, e->right, constexpr); DEREF(w);
		v.i |= w.i;
		break;
	case EXPR_AND:
		v = eval_expr(locs, e->left, constexpr); DEREF(v);
		if (v.i) { /* short-circuit */
			w = eval_expr(locs, e->right, constexpr); DEREF(w);
			v.i = v.i && w.i;
		}
		break;
	case EXPR_OR:
		v = eval_expr(locs, e->left, constexpr); DEREF(v);
		if (!v.i) { /* short-circuit */
			w = eval_expr(locs, e->right, constexpr); DEREF(w);
			v.i = v.i && w.i;
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

struct val
eval_stmt(struct var *locs, struct stmt *stmt)
{
	struct val v = {0};

	while (stmt) {
		switch (stmt->tag) {
		case STMT_RETURN:
			if (stmt->u.expr) {
				v = eval_expr(locs, stmt->u.expr, 0); DEREF(v);
			}
			v.ret = 1; /* flag to return early */
			return v;
		case STMT_DECL:
			assert(!"not implemented"); /* XXX */
			break;
		case STMT_ASSIGN:
			assert(!"not implemented"); /* XXX */
			break;
		case STMT_CALL:
			assert(!"not implemented"); /* XXX */
			break;
		case STMT_SELECT:
			assert(!"not implemented"); /* XXX */
			break;
		case STMT_LOOP:
			assert(!"not implemented"); /* XXX */
			break;
		default:
			assert(!"unexpected state");
		}
	}
	return v;
}

int
eval(int argc, const char **argv)
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

	dlog(__LINE__, "evaluating...");
	mainproc = get_proc(ast.procs, "main");
	if (!mainproc)
		eval_die(NULL, "missing procedure `main`");
	prev_eval_top = eval_alloc.top;
	prev_words_top = push_word(NULL, 0);
	/* initialize globals */
	for (gdecl = ast.globals; gdecl; gdecl = gdecl->next) {
		gvar = alloc(&eval_alloc, sizeof(*gvar));
		gvar->tok = &gdecl->tok;
		if (gdecl->u.decl.elems) {
			v = eval_expr(NULL, gdecl->u.decl.elems, 1); DEREF(v);
			gvar->addr = push_arr(gvar->tok, v.i);
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
		locs = alloc(&eval_alloc, sizeof(*locs)); /* argv */
		locs->tok = &mainproc->params->tok;
		locs->addr = argc_off + 1;
		words[locs->addr] = argc_off + 2;
		locs->next = alloc(&eval_alloc, sizeof(*locs)); /* argc */
		locs->next->tok = &mainproc->params->next->tok;
		locs->next->addr = argc_off;
	}
	if (debug && mainproc->params) {
		dlog(__LINE__, "words[%d..]: (each word truncated by 1 byte)", prev_words_top);
		for (i = prev_words_top; words[i] || words[i + 1] || i < off; ++i) {
			printf("%02x ", (unsigned int)(words[i] & 255));
			if ((i + 1) % 16 == 0)
				putchar('\n');
			else if ((i + 1) % 8 == 0)
				putchar(' ');
		}
		printf("%02x\n", (unsigned int)(words[i] & 255));
		dlog(__LINE__, "calling main()");
		clkbeg = clock();
	}
	/* call main */
	v = eval_stmt(locs, mainproc->body); DEREF(v);
	if (debug) {
		clkend = clock();
		dlog(__LINE__, "main() returned %"PRId64" (0x%"PRIx64"); cputime = %"PRIu64" ms",
		     v.i, (uint64_t)v.i, (uint64_t)(clkend - clkbeg) * 1000 / CLOCKS_PER_SEC);
	}
	/* final cleanup */
	memset(prev_eval_top, 0, (size_t)(eval_alloc.top - prev_eval_top));
	memset(words, 0, words_top - prev_words_top);
	eval_alloc.top = prev_eval_top;
	words_top = prev_words_top;
	return v.i & 255;
}

void
usage(void)
{
	fprintf(stdout,
		"usage: %s SCRIPT [ARGUMENTS...]\n"
		"       %s -h                        # show this\n"
		"       %s -v                        # show version\n"
		"\n"
		"error format:\n"
		"<script>:<line>:<column>: ERROR: <message>\n",
		argv0, argv0, argv0);
}

int
main(int argc, const char **argv)
{
	argv0 = argv[0];
	if (argc >= 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
		usage();
		return 0;
	} else if (argc >= 2 && !strcmp(argv[1], "-v")) {
		puts(VERSION);
		return 0;
	} else if (argc >= 2 && !strcmp(argv[1], "-D")) {
		debug = 1;
		dlog(__LINE__, "version: %s", VERSION);
		--argc; ++argv;
	}
	if (argc < 2) {
		usage();
		return 2;
	}
	script_path = argv[1];
	parse_alloc = bumpalloc_new(parse_data, sizeof(parse_data));
	eval_alloc = bumpalloc_new(eval_data, sizeof(eval_data));
	parse();
	if (debug) {
		dlog(__LINE__, "printing AST:");
		print_program(ast);
	}
	return eval(--argc, ++argv);
}
