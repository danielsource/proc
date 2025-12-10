/* proc.c -- proc language tree-walk interpreter */

#include <stddef.h>
#include <limits.h>
#include <assert.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define LENGTH(x) ((int)(sizeof(x) / sizeof((x)[0])))
#define IS_SPACE(c) (((c) >= 0 && (c) <= 32) || (c) == 127)
#define IS_DIGIT(c) ((c) >= 48 && (c) <= 57)
#define IS_ALPHA(c) (((c) >= 65 && (c) <= 90) || ((c) >= 97 && (c) <= 122))
#define IS_IDENT1(c) (IS_ALPHA(c) || (c) == 95)                /* A-Za-z_ */
#define IS_IDENT2(c) (IS_ALPHA(c) || (c) == 95 || IS_DIGIT(c)) /* A-Za-z_0-9 */
#define ALIGN sizeof(void *)
#define TOKEN_BUF_SIZE (64)
#define PARSE_DATA_SIZE (50l*1000*1000)
#define EVAL_DATA_SIZE (50l*1000*1000)
#define WORDS_SIZE (500l*1000)

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
	TOK_PERCENT,
	TOK_SLASH,
	TOK_ASTERISK,
	TOK_GREATER,
	TOK_GREATER_EQUALS,
	TOK_LESS,
	TOK_LESS_EQUALS,
	TOK_NOT_EQUALS,
	TOK_EQUALS,
	TOK_ASSIGN,
	TOK_ASSIGN_ADD,
	TOK_ASSIGN_SUB,
	TOK_ASSIGN_MUL,
	TOK_ASSIGN_DIV,
	TOK_ASSIGN_REM,
	TOK_AND,
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
		long i;
		char *name;
	} u;
};

struct ast {
	struct stmt *globals;
	struct proc *procs;
};

struct proc {
	char *name;
	struct token tok;
	struct stmt *params;
	struct stmt *body;
	struct proc *next;
};

enum expr_tag {
	EXPR_INVALID = 0,
	EXPR_INT,    /* "unary" left == right == NULL, int  -> tok.u.i */
	EXPR_NAME,   /* "unary" left == right == NULL, name -> tok.u.name */
	EXPR_ARRIDX, /* "binary" left in {name, arridx}, right == expr */
	EXPR_CALL,   /* "binary" left == name, right in {args, NULL} */
	EXPR_ARGS,   /* "binary" left == expr, right in {args, NULL} */
	EXPR_ADDROF, /* "unary" left == expr, right == NULL */
	EXPR_POS,    /* "unary" left == expr, right == NULL */
	EXPR_NEG,    /* "unary" left == expr, right == NULL */
	EXPR_NOT,    /* "unary" left == expr, right == NULL */
	EXPR_REM,    /* "binary" left == expr, right == expr */
	EXPR_DIV,    /* "binary" left == expr, right == expr */
	EXPR_MUL,    /* "binary" left == expr, right == expr */
	EXPR_SUB,    /* "binary" left == expr, right == expr */
	EXPR_ADD,    /* "binary" left == expr, right == expr */
	EXPR_GE,     /* "binary" left == expr, right == expr */
	EXPR_GT,     /* "binary" left == expr, right == expr */
	EXPR_LE,     /* "binary" left == expr, right == expr */
	EXPR_LT,     /* "binary" left == expr, right == expr */
	EXPR_NEQ,    /* "binary" left == expr, right == expr */
	EXPR_EQ,     /* "binary" left == expr, right == expr */
	EXPR_AND,    /* "binary" left == expr, right == expr */
	EXPR_OR      /* "binary" left == expr, right == expr */
};

struct expr {
	enum expr_tag tag;
	struct token tok;
	struct expr *left, *right;
};

struct stmt_decl {
	char *name;
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
	STMT_EXPR,
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

char *argv0, *script_path;
FILE *script_file;

char parse_data[PARSE_DATA_SIZE];
struct bumpalloc parse_alloc;

char eval_data[EVAL_DATA_SIZE];
struct bumpalloc eval_alloc;

long words[WORDS_SIZE];
struct bumpalloc word_alloc;

struct token tok = {TOK_START};
struct token tok_undo;

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

	new = a->top + ((n + ALIGN - 1) & ~(ALIGN - 1));
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

	new = a->top - ((n + ALIGN - 1) & ~(ALIGN - 1));
	assert(new >= a->beg);
	assert(new <= a->top);
	assert(new <= a->end);
	memset(new, 0, a->top - new);
}

void
parse_die(char *msg, ...)
{
	va_list ap;
	char *s;

	s = alloc(&parse_alloc, 256);
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

long
str_to_int(char *str)
{
	long n = 0, sign = 1, aux;
	char *s;

	s = str;
	if (s[0] == 43) { /* + */
		++s;
	} else if (s[0] == 45) { /* - */
		sign = -1;
		++s;
	}
	while (IS_DIGIT(s[0])) {
		if (n > LONG_MAX / 10)
			parse_die("decimal literal too large `%s` [A]", str);
		n *= 10;
		aux = s[0] - 48;
		if (n > LONG_MAX - aux)
			parse_die("decimal literal too large `%s` [B]", str);
		n += aux;
		++s;
	}
	return n * sign;
}

char *
get_token_str(struct token t)
{
	char *s;

	switch (t.tag) {
	case TOK_NULL: return "end-of-file";
	case TOK_INT:
		if (t.line > 0 && t.col > 0) {
			s = alloc(&parse_alloc, 32);
			sprintf(s, "%ld", t.u.i);
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
	case TOK_PERCENT: return "%";
	case TOK_SLASH: return "/";
	case TOK_ASTERISK: return "*";
	case TOK_GREATER: return ">";
	case TOK_GREATER_EQUALS: return ">=";
	case TOK_LESS: return "<";
	case TOK_LESS_EQUALS: return "<=";
	case TOK_NOT_EQUALS: return "!=";
	case TOK_EQUALS: return "==";
	case TOK_ASSIGN: return "=";
	case TOK_ASSIGN_ADD: return "+=";
	case TOK_ASSIGN_SUB: return "-=";
	case TOK_ASSIGN_MUL: return "*";
	case TOK_ASSIGN_DIV: return "/";
	case TOK_ASSIGN_REM: return "%";
	case TOK_AND: return "&&";
	case TOK_OR: return "||";
	case TOK_RETURN: return "return";
	case TOK_INT_KW: return "int";
	case TOK_IF: return "if";
	case TOK_ELSE: return "else";
	case TOK_WHILE: return "while";
	case TOK_PROC: return "proc";
	case TOK_PERIOD: return ".";
	case TOK_START:;
	default:
		assert(!"unexpected state");
	}
}

/* comment is # */
int
skip_comment(int *line, int *col, int c)
{
	if (c != 35)
		return c;
	while (c != 10) {
		if ((c = fgetc(script_file)) == EOF)
			return c;
		++*col;
	}
	++*line;
	*col = 0;
	return c;
}

int
skip_whitespace(int *line, int *col, int c)
{
	while (IS_SPACE(c)) {
		if (c == 10) {
			++*line;
			*col = 0;
		}
		if ((c = fgetc(script_file)) == EOF)
			return c;
		++*col;
	}
	return c;
}

/* tokenizer/lexer */
void
next_token(void)
{
	int c, i, line, col;
	char digits[TOKEN_BUF_SIZE];
	char name[TOKEN_BUF_SIZE];

	if (tok.tag == TOK_NULL)
		return;
	else if (tok_undo.tag != TOK_NULL) {
		tok = tok_undo;
		memset(&tok_undo, 0, sizeof(tok_undo));
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
tokenize:
	if ((c = fgetc(script_file)) == EOF)
		goto end;
	if (!line)
		line = 1;
	++col;
	if (c == 35) { /* comment before whitespace */
		if ((c = skip_comment(&line, &col, c)) == EOF)
			goto end;
		goto tokenize;
	}
	if ((c = skip_whitespace(&line, &col, c)) == EOF)
		goto end;
	if (c == 35) { /* comment after whitespace */
		if ((c = skip_comment(&line, &col, c)) == EOF)
			goto end;
		goto tokenize;
	}
	tok.line = line;
	tok.col = col;
	if (IS_DIGIT(c)) {
		for (i = 0; IS_DIGIT(c) && i < LENGTH(digits) - 1;) {
			digits[i++] = c;
			if ((c = fgetc(script_file)) == 95) { /* _ separator */
				if ((c = fgetc(script_file)) == 95 || !IS_DIGIT(c)) {
					digits[i] = 0;
					goto invalid_decimal;
				}
			}
		}
		digits[i] = 0;
		if (IS_IDENT1(c) || i == LENGTH(digits) - 1)
invalid_decimal:
			parse_die("invalid decimal literal `%s...`", digits);
		tok.tag = TOK_INT;
		tok.u.i = str_to_int(digits);
		col += i - 1;
		goto end;
	} else if (IS_IDENT1(c)) {
		for (i = 0; IS_IDENT2(c) && i < LENGTH(name) - 1; ++i) {
			name[i] = c;
			c = fgetc(script_file);
		}
		name[i] = 0;
		if (i == LENGTH(name) - 1)
			parse_die("invalid name `%s...`", name);
		if (!strcmp(name, "return")) {
			tok.tag = TOK_RETURN;
		} else if (!strcmp(name, "int")) {
			tok.tag = TOK_INT_KW;
		} else if (!strcmp(name, "if")) {
			tok.tag = TOK_IF;
		} else if (!strcmp(name, "else")) {
			tok.tag = TOK_ELSE;
		} else if (!strcmp(name, "while")) {
			tok.tag = TOK_WHILE;
		} else if (!strcmp(name, "proc")) {
			tok.tag = TOK_PROC;
		} else {
			tok.tag = TOK_NAME;
			tok.u.name = alloc(&parse_alloc, i + 1);
			memcpy(tok.u.name, name, i + 1);
		}
		col += i - 1;
		goto end;
	}
	switch (c) {
	case 40:
		tok.tag = TOK_PAREN_L;
		goto end_consuming;
	case 41:
		tok.tag = TOK_PAREN_R;
		goto end_consuming;
	case 91:
		tok.tag = TOK_BRACKET_L;
		goto end_consuming;
	case 93:
		tok.tag = TOK_BRACKET_R;
		goto end_consuming;
	case 123:
		tok.tag = TOK_BRACE_L;
		goto end_consuming;
	case 125:
		tok.tag = TOK_BRACE_R;
		goto end_consuming;
	case 59:
		tok.tag = TOK_SEMICOLON;
		goto end_consuming;
	case 44:
		tok.tag = TOK_COMMA;
		goto end_consuming;
	case 38:
		tok.tag = TOK_AMPERSAND;
		if ((c = fgetc(script_file)) == 38) {
			tok.tag = TOK_AND;
			++col;
			goto end_consuming;
		}
		goto end;
	case 43:
		tok.tag = TOK_PLUS;
		if ((c = fgetc(script_file)) == 61) {
			tok.tag = TOK_ASSIGN_ADD;
			++col;
			goto end_consuming;
		}
		goto end;
	case 45:
		tok.tag = TOK_MINUS;
		if ((c = fgetc(script_file)) == 61) {
			tok.tag = TOK_ASSIGN_SUB;
			++col;
			goto end_consuming;
		}
		goto end;
	case 33:
		tok.tag = TOK_EXCLAM;
		if ((c = fgetc(script_file)) == 61) {
			tok.tag = TOK_NOT_EQUALS;
			++col;
			goto end_consuming;
		}
		goto end;
	case 37:
		tok.tag = TOK_PERCENT;
		if ((c = fgetc(script_file)) == 61) {
			tok.tag = TOK_ASSIGN_REM;
			++col;
			goto end_consuming;
		}
		goto end;
	case 47:
		tok.tag = TOK_SLASH;
		if ((c = fgetc(script_file)) == 61) {
			tok.tag = TOK_ASSIGN_DIV;
			++col;
			goto end_consuming;
		}
		goto end;
	case 42:
		tok.tag = TOK_ASTERISK;
		if ((c = fgetc(script_file)) == 61) {
			tok.tag = TOK_ASSIGN_MUL;
			++col;
			goto end_consuming;
		}
		goto end;
	case 62:
		tok.tag = TOK_GREATER;
		if ((c = fgetc(script_file)) == 61) {
			tok.tag = TOK_GREATER_EQUALS;
			++col;
			goto end_consuming;
		}
		goto end;
	case 60:
		tok.tag = TOK_LESS;
		if ((c = fgetc(script_file)) == 61) {
			tok.tag = TOK_LESS_EQUALS;
			++col;
			goto end_consuming;
		}
		goto end;
	case 61:
		tok.tag = TOK_ASSIGN;
		if ((c = fgetc(script_file)) == 61) {
			tok.tag = TOK_EQUALS;
			++col;
			goto end_consuming;
		}
		goto end;
	case 124:
		tok.tag = TOK_OR;
		goto end_consuming;
	default:
		parse_die("unexpected character %c%c%c (%d)",
			  c==96?39:96, c, c==96?39:96, c);
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
	assert(tok_undo.tag == TOK_NULL);
	tok_undo = tok;
}

struct expr *
parse_expr(int lastprec)
{
	/* XXX */
	return NULL;
}

struct stmt *
parse_decl(struct stmt **last)
{
	struct stmt *root = NULL, **decl;

	decl = &root;
	while (1) {
		*decl = alloc(&parse_alloc, sizeof(struct stmt));
		(*decl)->tag = STMT_DECL;
		(*decl)->tok = tok;
		next_token();
		if (tok.tag != TOK_NAME)
			parse_die("expected a name, but got `%s`", get_token_str(tok));
		(*decl)->u.decl.name = tok.u.name;
		next_token();
		if (tok.tag == TOK_EQUALS) {
			(*decl)->u.decl.elems = parse_expr(0);
			next_token();
		} else if (tok.tag == TOK_BRACKET_L) {
			(*decl)->u.decl.expr = parse_expr(0);
			next_token();
			if (tok.tag != TOK_BRACKET_R)
				parse_die("expected `]`, but got `%s`", get_token_str(tok));
			next_token();
		}
		if (tok.tag == TOK_SEMICOLON) {
			*last = *decl;
			return root;
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
		(*stmt)->tag = STMT_ASSIGN;
		break;
	case TOK_ASSIGN_ADD:
		(*stmt)->tag = STMT_ASSIGN_ADD;
		break;
	case TOK_ASSIGN_SUB:
		(*stmt)->tag = STMT_ASSIGN_SUB;
		break;
	case TOK_ASSIGN_MUL:
		(*stmt)->tag = STMT_ASSIGN_MUL;
		break;
	case TOK_ASSIGN_DIV:
		(*stmt)->tag = STMT_ASSIGN_DIV;
		break;
	case TOK_ASSIGN_REM:
		(*stmt)->tag = STMT_ASSIGN_REM;
		break;
	default:
		parse_die("expected `=`, `+=`, `-=`, `*=`, `/=` or `%=`, but got `%s`", get_token_str(tok));
	}
	(*stmt)->u.ass.rhs = parse_expr(0);
	if (tok.tag != TOK_SEMICOLON)
		parse_die("expected `;`, but got `%s`", get_token_str(tok));
}

void
parse_call(struct stmt **stmt)
{
	(*stmt)->u.expr = parse_expr(0);
	if ((*stmt)->u.expr->tag != EXPR_CALL)
		parse_die("expected procedure call");
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
	struct stmt *root = NULL, **stmt, *lastdecl;
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
			if (tok.tag == TOK_SEMICOLON)
				return root;
			undo_token();
			(*stmt)->u.expr = parse_expr(0);
			next_token();
			if (tok.tag != TOK_SEMICOLON)
				parse_die("expected `;`, but got `%s`", get_token_str(tok));
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
					parse_die("unexpected text after `return` statement");
				default:
					;
				}
				next_token();
			}
			return root;
		case TOK_INT_KW:
			*stmt = parse_decl(&lastdecl);
			stmt = &lastdecl->next;
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
			if (tok.tag == TOK_PAREN_L) {
				parse_call(stmt);
			} else {
				undo_token();
				parse_assign(stmt);
			}
			stmt = &(*stmt)->next;
			break;
		default:
			parse_die("expected `}`, `return`, `int`, `if`, `while`, an assignment or a call, but got `%s`", get_token_str(tok));
			assert(!"unexpected state");
		}
		/* handle next statement */
	}
}

struct proc *
parse_proc(void)
{
	struct proc *proc;
	struct stmt *param;

	proc = alloc(&parse_alloc, sizeof(*proc));
	proc->tok = tok;
	next_token();
	if (tok.tag != TOK_NAME)
		parse_die("expected a name, but got `%s`", get_token_str(tok));
	proc->name = tok.u.name;
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
		param->u.decl.name = tok.u.name;
		if (proc->params)
			param->next = proc->params;
		proc->params = param;
		next_token();
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
	struct stmt *decl, *lastdecl;
	struct proc *proc;
	int eof = 0;

	script_file = fopen(script_path, "r");
	if (!script_file)
		parse_die("cannot open script to read");
	memset(&ast, 0, sizeof(ast));
	while (!eof) {
		next_token();
		switch (tok.tag) {
		case TOK_INT_KW:
			decl = parse_decl(&lastdecl);
			if (ast.globals)
				lastdecl->next = ast.globals;
			ast.globals = decl;
			break;
		case TOK_PROC:
			proc = parse_proc();
			if (ast.procs)
				proc->next = ast.procs;
			ast.procs = proc;
			break;
		default:
			parse_die("expected `int` or `proc`, but got `%s`", get_token_str(tok));
			assert(!"unexpected state");
		case TOK_NULL:
			eof = 1;
		}
	}
	fclose(script_file);
}

int
eval(int argc, char **argv)
{
	/* XXX */
	return argc & !argv & 255;
}

int
main(int argc, char **argv)
{
	argv0 = argv[0];
	if (argc < 2) {
		fprintf(stderr, "usage: %s SCRIPT [ARGUMENTS...]\n", argv0);
		return 2;
	}
	script_path = argv[1];
	parse_alloc = bumpalloc_new(parse_data, sizeof(parse_data));
	if (!(script_file = fopen(script_path, "r")))
		return 2;
	fputs("TESTING TOKENIZER:\n", stdout);
	fputs("start-of-file", stdout);
	while (tok.tag != TOK_NULL) {
		next_token();
		putchar(' ');
		fputs(get_token_str(tok), stdout);
	}
	putchar('\n');
	return 0;
#if 0
	eval_alloc = bumpalloc_new(eval_data, sizeof(eval_data));
	word_alloc = bumpalloc_new(words, sizeof(words));
	parse();
	return eval(--argc, ++argv);
#endif
}

/* 2025-12-10 -- public domain */
