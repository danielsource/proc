/* proc.c -- proc language tree-walk interpreter */

#include <stddef.h>
#include <limits.h>
#include <inttypes.h>
#include <assert.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define VERSION "WIP -- https://github.com/danielsource/proc -- public domain"
#define TOKEN_BUF_SIZE (64)
#define PARSE_DATA_SIZE (50l*1000*1000)
#define EVAL_DATA_SIZE (50l*1000*1000)
#define WORDS_SIZE (500l*1000)
#define UNARY_PREC 10

#define ROUND_UP(n, to) (((n) + ((to) - 1)) & ~((to) - 1))
#define LENGTH(x) ((int)(sizeof(x) / sizeof((x)[0])))

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
	EXPR_NEQ,
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
	STMT_ASSIGN_ADD,
	STMT_ASSIGN_SUB,
	STMT_ASSIGN_MUL,
	STMT_ASSIGN_DIV,
	STMT_ASSIGN_REM,
	STMT_ASSIGN_BIT_SH_L,
	STMT_ASSIGN_BIT_SH_R,
	STMT_ASSIGN_BIT_AND,
	STMT_ASSIGN_BIT_XOR,
	STMT_ASSIGN_BIT_OR,
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

/* globals */

const char *argv0, *script_path;
FILE *script_file;

char parse_data[PARSE_DATA_SIZE];
struct bumpalloc parse_alloc;

char eval_data[EVAL_DATA_SIZE];
struct bumpalloc eval_alloc;

int64_t words[WORDS_SIZE];
struct bumpalloc word_alloc;

struct token tok = {TOK_START};
struct token tok_undo[2];
int tok_undo_top = 0;

struct ast ast;

/* implementation */

struct bumpalloc
bumpalloc_new(void *p, size_t capacity)
{
	struct bumpalloc a;

	assert(p);
	a.beg = a.top = p;
	a.end = (char *)p + capacity;
	return a;
}

void *
alloc(struct bumpalloc *a, size_t n)
{
	char *new, *last;

	new = a->top + ROUND_UP(n, sizeof(void *));
	assert(new >= a->beg);
	assert(new >= a->top);
	assert(new <= a->end);
	last = a->top;
	a->top = new;
	return last;
}

void
dealloc(struct bumpalloc *a, size_t n)
{
	char *new;

	new = a->top - ROUND_UP(n, sizeof(void *));
	assert(new >= a->beg);
	assert(new <= a->top);
	assert(new <= a->end);
	memset(new, 0, a->top - new);
}

void
parse_die(const char *msg, ...)
{
	va_list ap;
	char *s;

	s = alloc(&parse_alloc, TOKEN_BUF_SIZE * 4);
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

int64_t
str_to_int(const char *digits)
{
	int64_t n = 0, sign = 1, base = 10, digitval;
	const char *s;

	s = digits;
	if (*s == '+') {
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
		if (n > LONG_MAX / base)
			parse_die("integer literal too large `%s` [A]", digits);
		n *= base;
		if (n > LONG_MAX - digitval)
			parse_die("integer literal too large `%s` [B]", digits);
		n += digitval;
		++s;
	}
	return n * sign;
invalid_int_lit:
	parse_die("invalid integer literal `%s`", digits);
	assert(!"unexpected state");
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
			sprintf(s, "%"PRIi64, t.u.i);
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
			parse_die("invalid integer literal `%s...`", buf);
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
			parse_die("invalid name `%s...`", buf);
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
			case '\'': i = '\''; break; /* \\ */
			case '\\': i = '\\'; break; /* \\ */
			case 'a': i = '\a'; break; /* \a */
			case 'b': i = '\b'; break; /* \b */
			case 't': i = '\t'; break; /* \t */
			case 'n': i = '\n'; break; /* \n */
			case 'r': i = '\r'; break; /* \r */
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
			parse_die("script contains non-ASCII character %d;\n"
				  "... this can happen when copying text from websites, PDFs, etc.",
				  c);
		else
			parse_die("unexpected character %c%c%c (%d)",
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
	case TOK_BRACKET_L:      *prec = 11; return EXPR_ARRIDX;
	case TOK_PAREN_L:        *prec = 11; return EXPR_CALL;
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
	case TOK_NOT_EQUALS:     *prec = 6; return EXPR_NEQ;
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
		right = parse_expr(prec);
		next_token();
		if (tok.tag != TOK_BRACKET_R)
			parse_die("expected `]`, but got `%s`", get_token_str(tok));
	} else if (tag == EXPR_CALL) {
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
parse_decl(struct stmt **decl)
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
	next_token();
	switch (tok.tag) {
	case TOK_ASSIGN:
	case TOK_ASSIGN_ADD:
	case TOK_ASSIGN_SUB:
	case TOK_ASSIGN_MUL:
	case TOK_ASSIGN_DIV:
	case TOK_ASSIGN_REM:
	case TOK_ASSIGN_BIT_SH_L:
	case TOK_ASSIGN_BIT_SH_R:
	case TOK_ASSIGN_BIT_AND:
	case TOK_ASSIGN_BIT_XOR:
	case TOK_ASSIGN_BIT_OR:
		(*stmt)->tag = tok.tag - TOK_ASSIGN + STMT_ASSIGN;
		break;
	default:
		parse_die("expected an assignment, but got `%s`", get_token_str(tok));
	}
	(*stmt)->u.ass.rhs = parse_expr(0);
	next_token();
	if (tok.tag != TOK_SEMICOLON)
		parse_die("expected `;`, but got `%s`", get_token_str(tok));
}

struct stmt *parse_stmt(void);

struct stmt *
parse_if(void)
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
		(*sel)->u.sel.ifbody = parse_stmt();
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
			(*sel)->u.sel.elsebody = parse_stmt();
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
parse_while(void)
{
	struct stmt *root = NULL;

	root = alloc(&parse_alloc, sizeof(struct stmt));
	root->tag = STMT_LOOP;
	root->tok = tok;
	root->u.loop.cond = parse_expr(0);
	next_token();
	if (tok.tag != TOK_BRACE_L)
		parse_die("expected `{`, but got `%s`", get_token_str(tok));
	root->u.loop.body = parse_stmt();
	next_token();
	if (tok.tag != TOK_BRACE_R)
		parse_die("expected `}`, but got `%s`", get_token_str(tok));
	return root;
}

struct stmt *
parse_stmt(void)
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
			stmt = &parse_decl(stmt)->next;
			break;
		case TOK_IF:
			*stmt = parse_if();
			stmt = &(*stmt)->next;
			break;
		case TOK_WHILE:
			*stmt = parse_while();
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
	struct stmt *param;

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
		param = alloc(&parse_alloc, sizeof(*param));
		param->tag = STMT_DECL;
		param->tok = tok;
		if (get_decl(proc->params, tok.u.name))
			parse_die("procedure `%s` contains duplicated parameter `%s`",
				 proc->tok.u.name, tok);
		if (proc->params)
			param->next = proc->params;
		proc->params = param;
		next_token();
		if (tok.tag == TOK_COMMA)
			next_token();
		else if (tok.tag != TOK_PAREN_R)
			parse_die("expected `)` or `,`, but got `%s`", get_token_str(tok));
	}
	next_token();
	if (tok.tag != TOK_BRACE_L)
		parse_die("expected `{`, but got `%s`", get_token_str(tok));
	proc->body = parse_stmt();
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
	while (!eof) {
		next_token();
		switch (tok.tag) {
		case TOK_INT_KW:
			decl = &parse_decl(decl)->next;
			break;
		case TOK_PROC:
			proc = parse_proc();
			if (get_proc(ast.procs, proc->tok.u.name))
				parse_die("procedure `%s` redefinition",
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
	fclose(script_file);
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
		print_expr(e->right, depth);
		fputs("]", stdout);
		if (depth)
			fputs(")", stdout);
		break;
	case EXPR_CALL:
		fputs(get_token_str(e->left->tok), stdout);
		fputs("(", stdout);
		if (e->right) {
			for (arg = e->right; arg->right; arg = arg->right) {
				print_expr(arg->left, depth);
				fputs(", ", stdout);
			}
			print_expr(arg->left, depth);
		}
		fputs(")", stdout);
		break;
	case EXPR_ADDROF:
	case EXPR_POS:
	case EXPR_NEG:
	case EXPR_NOT:
	case EXPR_BIT_NOT:
		fputs(get_token_str(e->tok), stdout);
		fputs("(", stdout);
		print_expr(e->left, depth);
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
	case EXPR_NEQ:
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
print_indent(int indent) {
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
	case STMT_ASSIGN_ADD:
	case STMT_ASSIGN_SUB:
	case STMT_ASSIGN_MUL:
	case STMT_ASSIGN_DIV:
	case STMT_ASSIGN_REM:
	case STMT_ASSIGN_BIT_SH_L:
	case STMT_ASSIGN_BIT_SH_R:
	case STMT_ASSIGN_BIT_AND:
	case STMT_ASSIGN_BIT_XOR:
	case STMT_ASSIGN_BIT_OR:
		print_expr(stmt->u.ass.lhs, 0);
		printf(" %.3s ", &" = += -= *= /= %= <<=>>=&= ^= |= "[(stmt->tag - STMT_ASSIGN) * 3]);
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
		fputs("if ...", stdout);
		printf("\t# %d:%d:%d\n",
		       stmt->tok.line,
		       stmt->tok.col,
		       stmt->tok.col_end);
		print_stmt(stmt->next, indent);
		return;
	case STMT_LOOP:
		fputs("while ...", stdout);
		printf("\t# %d:%d:%d\n",
		       stmt->tok.line,
		       stmt->tok.col,
		       stmt->tok.col_end);
		print_stmt(stmt->next, indent);
		return;
	default:
		assert(!"unexpected state");
	}
}

/* XXX */
void
print_program(struct ast a)
{
	struct stmt *globdecl, *par;
	struct proc *proc;

	if (a.globals)
		fputs("# globals\n", stdout);
	for (globdecl = a.globals; globdecl; globdecl = globdecl->next) {
		if (globdecl->u.decl.elems) {
			printf("int %s[", globdecl->tok.u.name);
			print_expr(globdecl->u.decl.elems, 0);
			fputs("];", stdout);
		} else if (globdecl->u.decl.expr) {
			printf("int %s = ", globdecl->tok.u.name);
			print_expr(globdecl->u.decl.expr, 0);
			fputs(";", stdout);
		} else {
			printf("int %s;", globdecl->tok.u.name);
		}
		printf(" # %d:%d:%d\n",
		       globdecl->tok.line,
		       globdecl->tok.col,
		       globdecl->tok.col_end);
	}
	if (a.globals)
		fputs("\n", stdout);
	if (a.procs)
		fputs("# procedures (procedures/parameters are reversed)\n", stdout);
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
}

/* XXX */
int
eval(int argc, const char **argv)
{
	assert((int64_t)-1 == ~0);
	return 0;
}

void
usage() {
	fprintf(stderr,
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
	if (argc < 2) {
		usage();
		return 2;
	} else if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
		usage();
		return 0;
	} else if (!strcmp(argv[1], "-v")) {
		puts(VERSION);
		return 0;
	}
	script_path = argv[1];
	parse_alloc = bumpalloc_new(parse_data, sizeof(parse_data));
	eval_alloc = bumpalloc_new(eval_data, sizeof(eval_data));
	word_alloc = bumpalloc_new(words, sizeof(words));
	parse();
	print_program(ast);
	return eval(--argc, ++argv);
}
