/*
 * final.c - Implementation of final code generation.
 *
 * Copyright (C) 2014 Andrew Schwartzmeyer
 *
 * This file released under the AGPLv3 license.
 */

#include <stdio.h>
#include <string.h>

#include "intermediate.h"
#include "type.h"
#include "args.h"

#include "list.h"
#include "hasht.h"

#define p(...) fprintf(stream, __VA_ARGS__)

extern struct list *yyscopes;

static void map_instruction(FILE *stream, struct list_node *iter);
static char *map_op(enum opcode code);
static char *map_print(enum opcode code);
static char *map_region(enum region r);
static void map_address(FILE *stream, struct address a);
static void print_prototypes(FILE *stream, struct hasht *table, char *class);

void final_code(FILE *stream, struct list *code)
{
	struct hasht *global = list_index(yyscopes, 1)->data;

	/* print typedefs */
	p("typedef char *class;\n");
	for (size_t i = 0; i < global->size; ++i) {
		struct hasht_node *slot = global->table[i];
		if (slot && !hasht_node_deleted(slot)) {
			struct typeinfo *value = slot->value;
			char *key = slot->key;
			if (value->base == CLASS_T)
				p("typedef char *%s;\n", key);
		}
	}

	print_prototypes(stream, global, NULL);

	/* print class prototypes */
	for (size_t i = 0; i < global->size; ++i) {
		struct hasht_node *slot = global->table[i];
		if (slot && !hasht_node_deleted(slot)) {
			struct typeinfo *value = slot->value;
			char *key = slot->key;
			if (value->base == CLASS_T) {
				if (value->class.public)
					print_prototypes(stream, value->class.public, key);
				if (value->class.private)
					print_prototypes(stream, value->class.private, key);
			}
		}
	}

	p("void _initialize_constants()\n{\n");
	p("\t/* initializing constant region */\n");
	struct hasht *constant = list_front(yyscopes);
	for (size_t i = 0; i < constant->size; ++i) {
		struct hasht_node *slot = constant->table[i];
		if (slot && !hasht_node_deleted(slot)) {
			struct typeinfo *v = slot->value;
			if (v->base == FLOAT_T) {
				p("\t");
				map_address(stream, v->place);
				p(" = %s;\n", v->token->text);
			} else if (v->base == CHAR_T && v->pointer) {
				p("\tmemcpy(constant + %d, %s, %zu);\n",
				  v->place.offset, v->token->text, v->token->ssize);
			}
		}
	}
	p("}\n\n");

	/* generate C instructions for list of TAC ops */
	struct list_node *iter = list_head(code);
	while (!list_end(iter)) {
		map_instruction(stream, iter);
		iter = iter->next;
	}
}

static void print_t(FILE *stream, struct address a)
{
	fprintf(stream, "%s%s", print_basetype(a.type),
	        a.type->pointer ? " *" : " ");
}

static void map_instruction(FILE *stream, struct list_node *iter)
{
	struct op *op = iter->data;
	if (arguments.debug) {
		p("/*\n");
		print_op(stream, op);
		p("*/\n");
	}
	static int param_offset = 0;
	struct address a = op->address[0];
	struct address b = op->address[1];
	struct address c = op->address[2];
	switch (op->code) {
	case PROC_O:
		print_typeinfo(stream, "", typeinfo_return(c.type));
		p("%s()\n{\n", op->name);
		p("\tchar local[%d];\n", b.offset);
		if (strcmp(op->name, "main") == 0)
			p("\t_initialize_constants();\n");
		/* copy parameters from faux stack into front of local region */
		p("\tmemcpy(local, stack, %d); /* copy parameters */\n", a.offset);
		if (strstr(op->name, "__")) /* probably a class */
			p("\tclass *instance = (class *)local;\n");
		break;
	case END_O:
		p("}\n\n");
		break;
	case PARAM_O:
		/* save parameters into faux stack */
		p("\t(*(");
		print_t(stream, a);
		p("*)(stack + %d))", param_offset);
		p(" = ");
		map_address(stream, a);
		p(";\n");
		param_offset += typeinfo_size(a.type);
		break;
	case CALL_O:
		/* reset parameter offset and call function, saving return */
		param_offset = 0;
		p("\t");
		if (a.region != UNKNOWN_R) {
			map_address(stream, a);
			p(" = ");
		}
		p("%s();\n", op->name);
		break;
	case CALLC_O:
		param_offset = 0;
		p("\t");
		if (a.region != UNKNOWN_R) {
			map_address(stream, a);
			p(" = ");
		}
		p("%s(", op->name);
		/* add parameters */
		iter = iter->prev;
		for (int i = 0; i < b.offset; ++i) {
			struct op *param = iter->data;
			map_address(stream, param->address[0]);
			/* append separator */
			if (i != b.offset - 1)
				p(", ");
			iter = iter->prev;
		}
		p(");\n");
		break;
	case RET_O:
		p("\treturn");
		/* if an immediate, use it */
		if (a.region == UNKNOWN_R) {
		} else if (a.region == CONST_R && !a.type->pointer
		    && (a.type->base == INT_T
		        || a.type->base == CHAR_T
		        || a.type->base == BOOL_T)) {
			p(" %d", a.offset);
		} else if (!(a.type->base == VOID_T && !a.type->pointer)) {
			if (a.type->base == CHAR_T && a.type->pointer) {
				/* TODO: no idea why this is a special
				   case and other primitives are not */
				p(" ((");
				print_t(stream, a);
				p(")(%s + %d))", map_region(a.region), a.offset);
			}
			else {
				p(" (*(");
				print_t(stream, a);
				p("*)(%s + %d))", map_region(a.region), a.offset);
			}
		}
		p(";\n");
		break;
	case LABEL_O:
		p("L_%d:\n", a.offset);
		break;
	case GOTO_O:
		p("\tgoto L_%d;\n", a.offset);
		break;
	case NEW_O:
		p("\t");
		map_address(stream, a);
		p(" = calloc(%d, sizeof(char));\n", b.offset);
		break;
	case DEL_O:
		p("\tfree(");
		map_address(stream, a);
		p(");\n");
		break;
	case PINT_O:
	case PCHAR_O:
	case PBOOL_O:
	case PFLOAT_O:
	case PSTR_O:
		p("\tprintf(\"%s\", ", map_print(op->code));
		map_address(stream, a);
		p(");\n");
		break;
	case ADD_O:
	case FADD_O:
	case SUB_O:
	case FSUB_O:
	case MUL_O:
	case FMUL_O:
	case DIV_O:
	case FDIV_O:
	case MOD_O:
	case LT_O:
	case FLT_O:
	case LE_O:
	case FLE_O:
	case GT_O:
	case FGT_O:
	case GE_O:
	case FGE_O:
	case EQ_O:
	case FEQ_O:
	case NE_O:
	case FNE_O:
	case OR_O:
	case AND_O:
		p("\t");
		map_address(stream, a);
		p(" = ");
		map_address(stream, b);
		p(" %s ", map_op(op->code));
		map_address(stream, c);
		p(";\n");
		break;
	case NEG_O:
	case FNEG_O:
	case NOT_O:
	case ASN_O:
	case RSTAR_O:
		p("\t");
		map_address(stream, a);
		p(" = ");
		p("%s", map_op(op->code));
		map_address(stream, b);
		p(";\n");
		break;
	case LSTAR_O:
		p("\t");
		p("(**(");
		print_t(stream, a);
		p("*)(%s + %d))", map_region(a.region), a.offset);
		p(" = ");
		map_address(stream, b);
		p(";\n");
		break;
	case ADDR_O:
		p("\t");
		map_address(stream, a);
		p(" = ");
		p("(");
		print_t(stream, b);
		p("*)(%s + %d)", map_region(b.region), b.offset);
		p(";\n");
		break;
	case RARR_O:
		p("\t");
		map_address(stream, a);
		p(" = ");
		p("(*(");
		print_t(stream, b);
		p("*)(%s + %d + ", map_region(b.region), b.offset);
		map_address(stream, c);
		p("));\n");
		break;
	case LARR_O:
		p("\t");
		map_address(stream, a);
		p(" = ");
		p("(");
		print_t(stream, b);
		p("*)(%s + %d + ", map_region(b.region), b.offset);
		map_address(stream, c);
		p(");\n");
		break;
	case LFIELD_O:
		p("\t");
		map_address(stream, a);
		p(" = ");
		p("(");
		print_t(stream, a);
		p(")(*(");
		print_t(stream, b);
		p("**)(%s + %d) + ", map_region(b.region), b.offset);
		map_address(stream, c);
		p(");\n");
		break;
	case RFIELD_O:
		p("\t");
		map_address(stream, a);
		p(" = ");
		p("(");
		print_t(stream, a);
		p(")(*(*(");
		print_t(stream, b);
		p("**)(%s + %d) + ", map_region(b.region), b.offset);
		map_address(stream, c);
		p("));\n");
		break;
	case IF_O:
		p("\tif (");
		map_address(stream, a);
		p(")\n");
		p("\t\tgoto L_%d;\n", b.offset);
		break;
	case ERRC_O:
		p("\texit(-1); /* operation error */");
		break;
	}
}

/* returns code to get value at address */
static void map_address(FILE *stream, struct address a)
{
	if (a.region == UNKNOWN_R)
		return;
	if (a.region == CONST_R && !a.type->pointer
	    && (a.type->base == INT_T
	        || a.type->base == CHAR_T
	        || a.type->base == BOOL_T)) {
		/* use an immediate directly */
		p("%d", a.offset);
	} else if (a.region == CONST_R && a.type->base == CHAR_T && a.type->pointer) {
		/* use constant string directly */
		p("(char *)(%s + %d)", map_region(a.region), a.offset);
	} else {
		/* grab value from region */
		p("(*(");
		print_t(stream, a);
		p("*)(%s + %d))", map_region(a.region), a.offset);
	}
}

/* returns string representation of binary operator */
static char *map_op(enum opcode code)
{
	switch (code) {
	case ADD_O:
	case FADD_O:
		return "+";
	case SUB_O:
	case FSUB_O:
		return "-";
	case MUL_O:
	case FMUL_O:
		return "*";
	case DIV_O:
	case FDIV_O:
		return "/";
	case MOD_O:
		return "%";
	case LT_O:
	case FLT_O:
		return "<";
	case LE_O:
	case FLE_O:
		return "<=";
	case GT_O:
	case FGT_O:
		return ">";
	case GE_O:
	case FGE_O:
		return ">=";
	case EQ_O:
	case FEQ_O:
		return "==";
	case NE_O:
	case FNE_O:
		return "!=";
	case OR_O:
		return "||";
	case AND_O:
		return "&&";
	case NEG_O:
	case FNEG_O:
		return "-";
	case NOT_O:
		return "!";
	case ASN_O:
		return "";
	case ADDR_O:
		return "&";
	case RSTAR_O:
		return "*";
	default:
		return NULL;
	}
}

/* returns conversion specifier string */
static char *map_print(enum opcode code)
{
	switch (code) {
	case PINT_O:
	case PBOOL_O:
		return "%d";
	case PCHAR_O:
		return "%c";
	case PFLOAT_O:
		return "%f";
	case PSTR_O:
		return "%s";
	default:
		return NULL;
	}
}

/* returns name of region variable in generated code */
static char *map_region(enum region r)
{
	switch (r) {
	case GLOBE_R:
		return "global";
	case CONST_R:
		return "constant";
	case LOCAL_R:
	case PARAM_R:
		return "local";
	case CLASS_R:
		return "*instance";
	default:
		return "unimplemented";
	}
}

/* print prototypes of functions in symbol table */
static void print_prototypes(FILE *stream, struct hasht *table, char *class)
{
	for (size_t i = 0; i < table->size; ++i) {
		struct hasht_node *slot = table->table[i];
		if (slot && !hasht_node_deleted(slot)) {
			struct typeinfo *value = slot->value;
			char *key = slot->key;
			if (value->base == FUNCTION_T && value->function.symbols) {
				if (strcmp(slot->key, "main") == 0)
					continue;
				print_typeinfo(stream, "", typeinfo_return(value));
				if (class)
					p(" %s__%s();\n", class, key);
				else
					p(" %s();\n", key);
			} else if (value->base == FUNCTION_T && class) {
				print_typeinfo(stream, NULL, typeinfo_return(value));
				p(" %s__%s() { } /* noop function */\n", class, key);
			}
		}
	}
	p("\n");
}

#undef p
