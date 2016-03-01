#include <assert.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <stdarg.h>

// ..., FMEMOPEN(3)
#include <stdio.h>

// OPEN(2), LSEEK(2), WAIT(2)
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>

// READ(2), LSEEK(2), FORK(2), EXECVE(2)
#include <unistd.h>

// BCMP(3)
#include <strings.h>

#include <errno.h>

#include "ngs.h"
#include "vm.h"
#include "ast.h"
#include "compile.h"
#include "decompile.h"
#include "syntax.h"
#include "syntax.auto.h"

extern char **environ;

char BYTECODE_SIGNATURE[] = "NGS BYTECODE";

char *opcodes_names[] = {
	/*  0 */ "HALT",
	/*  1 */ "PUSH_NULL",
	/*  2 */ "PUSH_FALSE",
	/*  3 */ "PUSH_TRUE",
	/*  4 */ "PUSH_INT",
	/*  5 */ "PUSH_L_STR",
	/*  6 */ "DUP",
	/*  7 */ "POP",
	/*  8 */ "XCHG",
	/*  9 */ "RESOLVE_GLOBAL",
	/* 10 */ "PATCH",
	/* 11 */ "FETCH_GLOBAL",
	/* 12 */ "STORE_GLOBAL",
	/* 13 */ "FETCH_LOCAL",
	/* 14 */ "STORE_LOCAL",
	/* 15 */ "CALL",
	/* 16 */ "CALL_EXC",
	/* 17 */ "CALL_ARR",
	/* 18 */ "RET",
	/* 19 */ "JMP",
	/* 20 */ "JMP_TRUE",
	/* 21 */ "JMP_FALSE",
	/* 22 */ "MAKE_ARR",
	/* 23 */ "MAKE_CLOSURE",
	/* 24 */ "TO_STR",
	/* 25 */ "MAKE_STR",
	/* 26 */ "PUSH_EMPTY_STR",
	/* 27 */ "GLOBAL_DEF_P",
	/* 28 */ "LOCAL_DEF_P",
	/* 29 */ "DEF_GLOBAL_FUNC",
	/* 30 */ "DEF_LOCAL_FUNC",
	/* 31 */ "FETCH_UPVAR",
	/* 32 */ "STORE_UPVAR",
	/* 33 */ "UPVAR_DEF_P",
	/* 34 */ "DEF_UPVAR_FUNC",
	/* 35 */ "MAKE_HASH",
	/* 36 */ "TO_BOOL",
	/* 37 */ "TO_ARR",
	/* 38 */ "ARR_APPEND",
	/* 39 */ "ARR_CONCAT",
	/* 40 */ "GUARD",
	/* 41 */ "TRY_START",
	/* 42 */ "TRY_END",
	/* 43 */ "ARR_REVERSE",
	/* 44 */ "THROW",
	/* 45 */ "MAKE_CMD",
};


#define EXPECT_STACK_DEPTH(n) assert(ctx->stack_ptr > (n));
#define PUSH(v) assert(ctx->stack_ptr<MAX_STACK); ctx->stack[ctx->stack_ptr++] = v
#define POP(dst) assert(ctx->stack_ptr); ctx->stack_ptr--; dst = ctx->stack[ctx->stack_ptr]
#define TOP (ctx->stack[ctx->stack_ptr-1])
#define SECOND (ctx->stack[ctx->stack_ptr-2])
#define DUP assert(ctx->stack_ptr<MAX_STACK); ctx->stack[ctx->stack_ptr] = ctx->stack[ctx->stack_ptr-1]; ctx->stack_ptr++;
#define REMOVE_TOP assert(ctx->stack_ptr); ctx->stack_ptr--;
#define REMOVE_N(n) DEBUG_VM_RUN("Popping %d argument(s) after call from stack\n", (int)n); assert(ctx->stack_ptr >= (unsigned int)n); ctx->stack_ptr-=n;
#define PUSH_NULL PUSH((VALUE){.num=V_NULL})
#define GLOBALS (vm->globals)
#define THIS_FRAME (ctx->frames[ctx->frame_ptr-1])
#define THIS_FRAME_CLOSURE (THIS_FRAME.closure)
#define LOCALS (THIS_FRAME.locals)
#define UPLEVELS CLOSURE_OBJ_UPLEVELS(THIS_FRAME_CLOSURE)
#define ARG(name, type) name = *(type *) &vm->bytecode[ip]; ip += sizeof(type);
#define ARG_LVI ARG(lvi, LOCAL_VAR_INDEX);
#define ARG_GVI ARG(gvi, GLOBAL_VAR_INDEX);
#define ARG_UVI ARG(uvi, UPVAR_INDEX);
#define PUSH_FUNC(dst, fn) if(IS_NGS_TYPE(dst)) { array_push(NGS_TYPE_CONSTRUCTORS(dst), (fn)); } else { array_push(dst, fn); };
#define CONVERTING_OP(test, type) \
	assert(ctx->stack_ptr); \
	v = TOP; \
	if(test(v)) { \
		goto main_loop; \
	} \
	PUSH(v); \
	mr = vm_call(vm, ctx, &ctx->stack[ctx->stack_ptr-2], (VALUE){.ptr = vm->type}, 1, &ctx->stack[ctx->stack_ptr-1]); \
	REMOVE_N(1); \
	assert(mr == METHOD_OK); \
	goto main_loop;

#define METHOD_PARAMS (VALUE *argv, VALUE *result)
#define EXT_METHOD_PARAMS (VM *vm, CTX *ctx, VALUE *argv, VALUE *result)
#define METHOD_RETURN(v) { *result = (v); return METHOD_OK; }
#define THROW_EXCEPTION(t) { *result = make_string(t); return METHOD_EXCEPTION; }
#define THROW_EXCEPTION_INSTANCE(e) { *result = e; return METHOD_EXCEPTION; }

#define INT_METHOD(name, op) \
METHOD_RESULT native_ ## name ## _int_int METHOD_PARAMS { \
	SET_INT(*result, GET_INT(argv[0]) op GET_INT(argv[1])); \
	return METHOD_OK; \
}

#define INT_CMP_METHOD(name, op) \
METHOD_RESULT native_ ## name ## _int_int METHOD_PARAMS { \
	SET_BOOL(*result, GET_INT(argv[0]) op GET_INT(argv[1])); \
	return METHOD_OK; \
}

#define ARG_LEN(n) OBJ_LEN(argv[n])
#define ARG_DATA_PTR(n) OBJ_DATA_PTR(argv[n])

#define BYTECODE_ADD(ptr, type, val) \
	*(type *) ptr = val; \
	ptr += sizeof(type);

#define BYTECODE_GET(dst, ptr, type) \
	dst = *(type *) ptr; \
	ptr += sizeof(type);

INT_METHOD(plus, +);
INT_METHOD(minus, -);
INT_METHOD(mul, *);
INT_METHOD(div, /);
INT_METHOD(mod, %);
INT_CMP_METHOD(less, <);
INT_CMP_METHOD(less_eq, <=);
INT_CMP_METHOD(greater, >);
INT_CMP_METHOD(greater_eq, >=);
INT_CMP_METHOD(eq, ==);

METHOD_RESULT native_dump_any METHOD_PARAMS {
	dump(argv[0]);
	SET_NULL(*result);
	return METHOD_OK;
}

METHOD_RESULT native_echo_str METHOD_PARAMS {
	SET_INT(*result, printf("%s\n", obj_to_cstring(argv[0])));
	return METHOD_OK;
}

METHOD_RESULT native_plus_arr_arr METHOD_PARAMS {
	*result = make_array(ARG_LEN(0) + ARG_LEN(1));
	memcpy(ARRAY_ITEMS(*result)+0, ARG_DATA_PTR(0), sizeof(VALUE)*ARG_LEN(0));
	memcpy(ARRAY_ITEMS(*result)+ARG_LEN(0), OBJ_DATA_PTR(argv[1]), sizeof(VALUE)*ARG_LEN(1));
	return METHOD_OK;
}

METHOD_RESULT native_push_arr_any METHOD_PARAMS { array_push(argv[0], argv[1]); METHOD_RETURN(argv[0]); }

METHOD_RESULT native_shift_arr METHOD_PARAMS { METHOD_RETURN(array_shift(argv[0])); }

METHOD_RESULT native_shift_arr_any METHOD_PARAMS {
	if(!OBJ_LEN(argv[0])) {
		METHOD_RETURN(argv[1]);
	}
	METHOD_RETURN(array_shift(argv[0]));
}

METHOD_RESULT native_index_get_arr_int_any METHOD_PARAMS {
	int idx, len;
	idx = GET_INT(argv[1]);
	if(idx < 0) {
		METHOD_RETURN(argv[2]);
	}
	len = OBJ_LEN(argv[0]);
	if(idx<len) {
		METHOD_RETURN(ARRAY_ITEMS(argv[0])[idx]);
	}
	METHOD_RETURN(argv[2]);
}

METHOD_RESULT native_index_get_arr_int EXT_METHOD_PARAMS {
	int idx, len;
	(void) ctx;
	len = OBJ_LEN(argv[0]);
	idx = GET_INT(argv[1]);
	if((idx < 0) || (idx >= len)) {
		VALUE e;
		e = make_normal_type_instance(vm->IndexNotFound);
		set_normal_type_instance_attribute(e, make_string("container"), argv[0]);
		set_normal_type_instance_attribute(e, make_string("key"), argv[1]);
		THROW_EXCEPTION_INSTANCE(e);
	}
	*result = ARRAY_ITEMS(argv[0])[idx];
	return METHOD_OK;
}


METHOD_RESULT native_Str_int METHOD_PARAMS {
	char s[MAX_INT_TO_STR_LEN];
	size_t len;
	len = snprintf(s, sizeof(s), "%" PRIiPTR, GET_INT(argv[0]));
	if(len >= sizeof(s)) {
		THROW_EXCEPTION("ResultTooLarge");
	}
	*result = make_string(s);
	return METHOD_OK;
}

METHOD_RESULT native_is_any_type METHOD_PARAMS {
	SET_BOOL(*result, obj_is_of_type(argv[0], argv[1]));
	return METHOD_OK;
}

METHOD_RESULT native_Bool_any METHOD_PARAMS {
	// printf("Bool()\n");
	// dump(argv[0]);
	if(IS_BOOL(argv[0])) METHOD_RETURN(argv[0])
	if(IS_INT(argv[0])) METHOD_RETURN(MAKE_BOOL(GET_INT(argv[0])))
	if(IS_STRING(argv[0]) || IS_ARRAY(argv[0]) || IS_HASH(argv[0])) METHOD_RETURN(MAKE_BOOL(OBJ_LEN(argv[0])))
	return METHOD_ARGS_MISMATCH;
}

METHOD_RESULT native_in_any_hash METHOD_PARAMS {
	SET_BOOL(*result, get_hash_key(argv[1], argv[0]));
	return METHOD_OK;
}

METHOD_RESULT native_hash_any METHOD_PARAMS {
	// METHOD_RETURN(MAKE_INT(hash(argv[0])));
	SET_INT(*result, hash(argv[0]));
	return METHOD_OK;
}

METHOD_RESULT native_keys_hash METHOD_PARAMS {
	size_t i;
	HASH_OBJECT_ENTRY *e;
	*result = make_array(OBJ_LEN(argv[0]));
	for(e=HASH_HEAD(argv[0]), i=0; e; e=e->insertion_order_next, i++) {
		*(VALUE *)(ARRAY_ITEMS(*result)+i) = e->key;
	}
	return METHOD_OK;
}

METHOD_RESULT native_values_hash METHOD_PARAMS {
	size_t i;
	HASH_OBJECT_ENTRY *e;
	*result = make_array(OBJ_LEN(argv[0]));
	for(e=HASH_HEAD(argv[0]), i=0; e; e=e->insertion_order_next, i++) {
		*(VALUE *)(ARRAY_ITEMS(*result)+i) = e->val;
	}
	return METHOD_OK;
}

METHOD_RESULT native_len METHOD_PARAMS {
	*result = MAKE_INT(OBJ_LEN(argv[0]));
	return METHOD_OK;
}

METHOD_RESULT native_index_get_hash_any_any METHOD_PARAMS {
	HASH_OBJECT_ENTRY *e;
	e = get_hash_key(argv[0], argv[1]);
	if(!e) {
		METHOD_RETURN(argv[2]);
	}
	*result = e->val;
	return METHOD_OK;
}

METHOD_RESULT native_index_get_hash_any EXT_METHOD_PARAMS {
	HASH_OBJECT_ENTRY *e;
	(void) ctx;
	e = get_hash_key(argv[0], argv[1]);
	if(!e) {
		VALUE exc;
		exc = make_normal_type_instance(vm->KeyNotFound);
		set_normal_type_instance_attribute(exc, make_string("container"), argv[0]);
		set_normal_type_instance_attribute(exc, make_string("key"), argv[1]);
		THROW_EXCEPTION_INSTANCE(exc);
	}
	*result = e->val;
	return METHOD_OK;
}

METHOD_RESULT native_index_set_hash_any_any METHOD_PARAMS {
	set_hash_key(argv[0], argv[1], argv[2]);
	*result = argv[2];
	return METHOD_OK;
}

METHOD_RESULT native_index_del_hash_any METHOD_PARAMS {
	del_hash_key(argv[0], argv[1]);
	*result = argv[0];
	return METHOD_OK;
}

// TODO: locking for dlerror?
// TODO: Support other dlopen() flags?
METHOD_RESULT native_CLib_str METHOD_PARAMS {
	VALUE v;
	CLIB_OBJECT *o;
	void *out;
	out = dlopen(obj_to_cstring(argv[0]), RTLD_NOW);
	if(!out) {
		fprintf(stderr, "dlopen() failed: %s\n", dlerror());
		assert(0=="Fail to dlopen()");
	}
	o = NGS_MALLOC(sizeof(*o));
	assert(o);
	o->base.type.num = T_CLIB;
	o->base.val.ptr = out;
	o->name = argv[0];
	SET_OBJ(v, o);
	METHOD_RETURN(v);
}

METHOD_RESULT native_in_str_clib METHOD_PARAMS {
	METHOD_RETURN(MAKE_BOOL(dlsym(OBJ_DATA_PTR(argv[1]), obj_to_cstring(argv[0]))));
}

METHOD_RESULT native_index_get_clib_str METHOD_PARAMS {
	VALUE v;
	CSYM_OBJECT *o;
	o = NGS_MALLOC(sizeof(*o));
	assert(o);
	o->base.type.num = T_CSYM;
	o->base.val.ptr = dlsym(OBJ_DATA_PTR(argv[0]), obj_to_cstring(argv[1]));
	assert(o->base.val.ptr);
	o->lib = argv[0];
	o->name = argv[1];
	SET_OBJ(v, o);
	METHOD_RETURN(v);
}

// OPEN(2)
// TODO: support more flags
// TODO: a  way to return errno. Exception maybe?
METHOD_RESULT native_c_open_str_str METHOD_PARAMS {
	const char *pathname = obj_to_cstring(argv[0]);
	const char *flags_str = obj_to_cstring(argv[1]);
	int flags = 0;
	if(!flags && !strcmp(flags_str, "r"))  { flags = O_RDONLY; }
	if(!flags && !strcmp(flags_str, "w"))  { flags = O_WRONLY; }
	if(!flags && !strcmp(flags_str, "rw")) { flags = O_RDWR; }
	SET_INT(*result, open(pathname, flags));
	return METHOD_OK;
}

// READ(2)
// TODO: error handling support
METHOD_RESULT native_c_read_int_int METHOD_PARAMS {
	// Params: fd, count
	char *buf;
	size_t count = GET_INT(argv[1]);
	ssize_t ret;
	assert(count <= SSIZE_MAX);
	buf = NGS_MALLOC_ATOMIC(count);
	assert(buf);
	ret = read(GET_INT(argv[0]), buf, count);

	if(ret < 0) {
		SET_INT(*result, ret);
		return METHOD_OK;
	}

	*result = make_string_of_len(buf, ret);
	return METHOD_OK;
}

METHOD_RESULT native_c_lseek_int_int_str EXT_METHOD_PARAMS {
	off_t offset;
	const char *whence_str = obj_to_cstring(argv[2]);
	int whence = 0;
	(void) ctx;
	if(!strcmp(whence_str, "set")) {
		whence = SEEK_SET;
	} else {
		if(!strcmp(whence_str, "cur")) {
			whence = SEEK_CUR;
		} else {
			if(!strcmp(whence_str, "end")) {
				whence = SEEK_END;
			} else {
				VALUE exc;
				exc = make_normal_type_instance(vm->InvalidParameter);
				set_normal_type_instance_attribute(exc, make_string("which"), make_string("Third parameter to c_lseek(), 'whence'"));
				set_normal_type_instance_attribute(exc, make_string("given"), argv[2]);
				// TODO: Array of expected values maybe?
				set_normal_type_instance_attribute(exc, make_string("expected"), make_string("One of: 'set', 'cur', 'end'"));
				THROW_EXCEPTION_INSTANCE(exc);
			}
		}
	}
	offset = lseek(GET_INT(argv[0]), GET_INT(argv[1]), whence);
	METHOD_RETURN(MAKE_INT(offset));
}

METHOD_RESULT native_c_close_int METHOD_PARAMS { METHOD_RETURN(MAKE_INT(close(GET_INT(argv[0])))); }

METHOD_RESULT native_c_exit_int METHOD_PARAMS { (void)result; exit(GET_INT(argv[0])); }

METHOD_RESULT native_eq_str_str METHOD_PARAMS {
	size_t len;
	if(OBJ_LEN(argv[0]) != OBJ_LEN(argv[1])) { METHOD_RETURN(MAKE_BOOL(0)); }
	if(OBJ_DATA_PTR(argv[0]) == OBJ_DATA_PTR(argv[1])) { METHOD_RETURN(MAKE_BOOL(1)); }
	len = OBJ_LEN(argv[0]);
	METHOD_RETURN(MAKE_BOOL(!bcmp(OBJ_DATA_PTR(argv[0]), OBJ_DATA_PTR(argv[1]), len)));
}

METHOD_RESULT native_eq_bool_bool METHOD_PARAMS { METHOD_RETURN(MAKE_BOOL(argv[0].num == argv[1].num)); }
METHOD_RESULT native_not_bool METHOD_PARAMS { METHOD_RETURN(MAKE_BOOL(argv[0].num == V_FALSE)); }

// XXX: glibc specific fmemopen()
METHOD_RESULT native_compile_str_str METHOD_PARAMS {
	ast_node *tree = NULL;
	char *bytecode;
	size_t len;
	yycontext yyctx;
	int parse_ok;
	memset(&yyctx, 0, sizeof(yycontext));
	yyctx.fail_pos = -1;
	yyctx.fail_rule = "(unknown)";
	yyctx.lines = 0;
	yyctx.lines_postions[0] = 0;
	// printf("PT 1 %p\n", OBJ_DATA_PTR(argv[0]));
	yyctx.input_file = fmemopen(OBJ_DATA_PTR(argv[0]), OBJ_LEN(argv[0]), "r");
	parse_ok = yyparse(&yyctx);
	if(!parse_ok) {
		// TODO: error message and/or exception
		assert(0 == "compile() failed");
	}
	tree = yyctx.__;
	IF_DEBUG(COMPILER, print_ast(tree, 0);)
	yyrelease(&yyctx);
	bytecode = compile(tree, &len);
	// BROKEN SINCE BYTECODE FORMAT CHANGE // IF_DEBUG(COMPILER, decompile(bytecode, 0, len);)
	METHOD_RETURN(make_string_of_len(bytecode, len));
}

METHOD_RESULT native_load_str_str EXT_METHOD_PARAMS {
	size_t ip;
	(void) ctx;
	ip = vm_load_bytecode(vm, OBJ_DATA_PTR(argv[0]));
	METHOD_RETURN(make_closure_obj(ip, 0, 0, 0, 0, 0, NULL));
}

METHOD_RESULT native_parse_json_str EXT_METHOD_PARAMS {
	METHOD_RESULT mr;
	(void) ctx;
	mr = parse_json(argv[0], result);
	if(mr == METHOD_EXCEPTION) {
		VALUE exc;
		// TODO: more specific error
		exc = make_normal_type_instance(vm->Error);
		set_normal_type_instance_attribute(exc, make_string("message"), *result);
		*result = exc;
	}
	return mr;
}

METHOD_RESULT native_type_str METHOD_PARAMS { METHOD_RETURN(make_normal_type(argv[0])); }
METHOD_RESULT native_get_attr_nti_str EXT_METHOD_PARAMS {
	// WARNING: for now get_normal_type_instace_attribute can only throw AttrNotFound
	//          if it changes in future the calling convention below should be changed
	//          The reason for such calling convention is not to pass the VM to
	//          get_normal_type_instace_attribute() just so it will have access to the
	//          exceptions.
	METHOD_RESULT mr;
	(void) ctx;
	mr = get_normal_type_instace_attribute(argv[0], argv[1], result);
	if(mr == METHOD_EXCEPTION) {
		VALUE exc;
		exc = make_normal_type_instance(vm->AttrNotFound);
		set_normal_type_instance_attribute(exc, make_string("container"), argv[0]);
		set_normal_type_instance_attribute(exc, make_string("key"), argv[1]);
		*result = exc;
	}
	return mr;
}
METHOD_RESULT native_set_attr_nti_str_any METHOD_PARAMS { set_normal_type_instance_attribute(argv[0], argv[1], argv[2]); METHOD_RETURN(argv[2]); }
METHOD_RESULT native_inherit_nt_nt METHOD_PARAMS { add_normal_type_inheritance(argv[0], argv[1]); METHOD_RETURN(argv[0]); }

// Consider moving to obj.c
METHOD_RESULT native_join_arr_str METHOD_PARAMS {
	size_t i, len=OBJ_LEN(argv[0]), dst_len, l, sep_l;
	char *p, *sep_p;
	if(!len) {
		METHOD_RETURN(make_string(""));
	}
	for(i=0, dst_len=0; i<len; i++) {
		dst_len += OBJ_LEN(ARRAY_ITEMS(argv[0])[i]);
	}
	dst_len += (OBJ_LEN(argv[1]) * (len-1));

	*result = make_string_of_len(NULL, dst_len);
	OBJ_LEN(*result) = dst_len;
	p = OBJ_DATA_PTR(*result);

	l = OBJ_LEN(ARRAY_ITEMS(argv[0])[0]);
	memcpy(p, OBJ_DATA_PTR(ARRAY_ITEMS(argv[0])[0]), l);
	p += l;

	sep_p = OBJ_DATA_PTR(argv[1]);
	sep_l = OBJ_LEN(argv[1]);

	for(i=1; i<len; i++) {
		memcpy(p, sep_p, sep_l);
		p += sep_l;

		l = OBJ_LEN(ARRAY_ITEMS(argv[0])[i]);
		memcpy(p, OBJ_DATA_PTR(ARRAY_ITEMS(argv[0])[i]), l);
		p += l;
	}
	return METHOD_OK;
}

METHOD_RESULT native_c_fork METHOD_PARAMS {
	(void) argv;
	pid_t pid;
	pid = fork();
	METHOD_RETURN(MAKE_INT(pid));
}

METHOD_RESULT native_c_waitpid METHOD_PARAMS {
	VALUE ret;
	pid_t pid;
	int status;
	pid = waitpid(GET_INT(argv[0]), &status, 0);
	ret = make_array(2);
	ARRAY_ITEMS(ret)[0] = MAKE_INT(pid);
	ARRAY_ITEMS(ret)[1] = MAKE_INT(status);
	METHOD_RETURN(ret);
}

METHOD_RESULT native_get_attr_nt_str EXT_METHOD_PARAMS {
	VALUE exc;
	char *attr = obj_to_cstring(argv[1]);
	(void) ctx;
	if(!strcmp(attr, "constructors")) {
		// dump_titled("constructors", NGS_TYPE_CONSTRUCTORS(argv[0]));
		METHOD_RETURN(NGS_TYPE_CONSTRUCTORS(argv[0]));
	}

	exc = make_normal_type_instance(vm->AttrNotFound);
	set_normal_type_instance_attribute(exc, make_string("container"), argv[0]);
	set_normal_type_instance_attribute(exc, make_string("key"), argv[1]);
	*result = exc;
	return METHOD_EXCEPTION;
}

METHOD_RESULT native_c_pipe METHOD_PARAMS {
	VALUE ret;
	int status;
	int pipefd[2];
	(void) argv;
	status = pipe(pipefd);
	ret = make_array(3);
	ARRAY_ITEMS(ret)[0] = MAKE_INT(status);
	ARRAY_ITEMS(ret)[1] = MAKE_INT(pipefd[0]);
	ARRAY_ITEMS(ret)[2] = MAKE_INT(pipefd[1]);
	METHOD_RETURN(ret);
}

METHOD_RESULT native_c_dup2_int_int METHOD_PARAMS {
	METHOD_RETURN(MAKE_INT(dup2(GET_INT(argv[0]), GET_INT(argv[1]))));
}

METHOD_RESULT native_get_c_errno METHOD_PARAMS {
	(void) argv;
	METHOD_RETURN(MAKE_INT(errno));
}

METHOD_RESULT native_c_execve METHOD_PARAMS {
	char *exec_filename;
	char **exec_argv, **exec_envp;
	exec_filename = obj_to_cstring(argv[0]);
	exec_argv = obj_to_cstring_array(argv[1]);
	exec_envp = obj_to_cstring_array(argv[2]);
	METHOD_RETURN(MAKE_INT(execve(exec_filename, exec_argv, exec_envp)));
}

GLOBAL_VAR_INDEX check_global_index(VM *vm, const char *name, size_t name_len, int *found) {
	VAR_INDEX *var;
	HASH_FIND(hh, vm->globals_indexes, name, name_len, var);
	if(var) {
		*found = 1;
		return var->index;
	}
	*found = 0;
	return 0;
}

GLOBAL_VAR_INDEX get_global_index(VM *vm, const char *name, size_t name_len) {
	VAR_INDEX *var;
	GLOBAL_VAR_INDEX index;
	int found;
	DEBUG_VM_RUN("entering get_global_index() vm=%p name=%.*s\n", vm, (int)name_len, name);
	index = check_global_index(vm, name, name_len, &found);
	if(found) {
		DEBUG_VM_RUN("leaving get_global_index() status=found vm=%p name=%.*s -> index=" GLOBAL_VAR_INDEX_FMT "\n", vm, (int)name_len, name, index);
		return index;
	}
	assert(vm->globals_len < (MAX_GLOBALS-1));
	var = NGS_MALLOC(sizeof(*var));
	var->name = NGS_MALLOC(name_len);
	memcpy(var->name, name, name_len);
	var->index = vm->globals_len++;
	HASH_ADD_KEYPTR(hh, vm->globals_indexes, var->name, name_len, var);
	GLOBALS[var->index].num = V_UNDEF;
	vm->globals_names[var->index] = var->name;
	DEBUG_VM_RUN("leaving get_global_index() status=new vm=%p name=%.*s -> index=" GLOBAL_VAR_INDEX_FMT "\n", vm, (int)name_len, name, var->index);
	return var->index;
}

void register_global_func(VM *vm, int pass_extra_params, char *name, void *func_ptr, LOCAL_VAR_INDEX argc, ...) {
	size_t index;
	int i;
	va_list varargs;
	NATIVE_METHOD_OBJECT *o;
	VALUE *argv = NULL;
	o = NGS_MALLOC(sizeof(*o));
	o->base.type.num = T_NATIVE_METHOD;
	o->base.val.ptr = func_ptr;
	o->params.n_params_required = argc;
	o->params.n_params_optional = 0; /* currently none of builtins uses optional parameters */
	o->pass_extra_params = pass_extra_params;
	if(argc) {
		argv = NGS_MALLOC(argc * sizeof(VALUE) * 2);
		assert(argv);
		va_start(varargs, argc);
		for(i=0; i<argc; i++) {
			// name:
			argv[i*2+0] = make_string(va_arg(varargs, char *));
			// type:
			argv[i*2+1] = (VALUE){.ptr = va_arg(varargs, NGS_TYPE *)};
		}
		va_end(varargs);
	}
	o->params.params = argv;
	index = get_global_index(vm, name, strlen(name));
	if(IS_ARRAY(GLOBALS[index])) {
		array_push(GLOBALS[index], MAKE_OBJ(o));
		return;
	}
	if(IS_NGS_TYPE(GLOBALS[index])) {
		array_push(NGS_TYPE_CONSTRUCTORS(GLOBALS[index]), MAKE_OBJ(o));
		return;
	}
	if(IS_UNDEF(GLOBALS[index])) {
		GLOBALS[index] = make_array_with_values(1, &MAKE_OBJ(o));
		return;
	}
	assert(0 == "register_global_func fail");
}

void set_global(VM *vm, const char *name, VALUE v) {
	size_t index;
	index = get_global_index(vm, name, strlen(name));
	GLOBALS[index] = v;
}

NGS_TYPE *register_builtin_type(VM *vm, const char *name, IMMEDIATE_TYPE native_type_id) {
	size_t index;
	VALUE t;
	t = make_normal_type(make_string(name));
	// Fixes for built-ins - start
	NGS_TYPE_ID(t) = native_type_id;
	OBJ_LEN(NGS_TYPE_CONSTRUCTORS(t)) = 0;
	// Fixes for built-ins - end
	index = get_global_index(vm, name, strlen(name));
	assert(IS_UNDEF(GLOBALS[index]));
	GLOBALS[index] = t;
	return t.ptr;
}

void vm_init(VM *vm, int argc, char **argv) {
	char **env, *equal_sign;
	VALUE env_hash, k, v;
	VALUE argv_array;
	VALUE exception_type, error_type, lookup_fail_type, key_not_found_type, index_not_found_type, attr_not_found_type, invalid_param_type;
	VALUE command_type;
	int i;
	vm->bytecode = NULL;
	vm->bytecode_len = 0;
	vm->globals_indexes = NULL; // UT_hash_table
	vm->globals_len = 0;
	vm->globals = NGS_MALLOC(sizeof(*(vm->globals)) * MAX_GLOBALS);
	vm->globals_names = NGS_MALLOC(sizeof(char *) * MAX_GLOBALS);
	// Keep global functions registration in order.
	// This way the compiler can use globals_indexes as the beginning of
	// it's symbol table for globals.
	vm->Null = register_builtin_type(vm, "Null", T_NULL);
	vm->Bool = register_builtin_type(vm, "Bool", T_BOOL);
	vm->Int  = register_builtin_type(vm, "Int",  T_INT);
	vm->Str  = register_builtin_type(vm, "Str",  T_STR);
	vm->Arr  = register_builtin_type(vm, "Arr",  T_ARR);
	vm->Fun  = register_builtin_type(vm, "Fun",  T_FUN);
	vm->Any  = register_builtin_type(vm, "Any",  T_ANY);
		vm->BasicTypeInstance  = register_builtin_type(vm, "BasicTypeInstance",  T_BASICTI);
		vm->NormalTypeInstance = register_builtin_type(vm, "NormalTypeInstance", T_NORMTI);
	vm->Seq  = register_builtin_type(vm, "Seq",  T_SEQ);
	vm->Type = register_builtin_type(vm, "Type", T_TYPE);
		vm->BasicType  = register_builtin_type(vm, "BasicType",  T_BASICT);
		vm->NormalType = register_builtin_type(vm, "NormalType", T_NORMT);
	vm->Hash = register_builtin_type(vm, "Hash", T_HASH);
	vm->CLib = register_builtin_type(vm, "CLib", T_CLIB);
	vm->CSym = register_builtin_type(vm, "CSym", T_CSYM);

	// CLib and c calls
	register_global_func(vm, 0, "CLib",     &native_CLib_str,          1, "name",   vm->Str);
	register_global_func(vm, 0, "in",       &native_in_str_clib,       2, "symbol", vm->Str, "lib", vm->CLib);
	register_global_func(vm, 0, "[]",       &native_index_get_clib_str,2, "lib",    vm->CLib,"symbol", vm->Str);

	// NormalType
	register_global_func(vm, 1, ".",        &native_get_attr_nt_str,       2, "obj", vm->NormalType,         "attr", vm->Str);
	register_global_func(vm, 1, ".",        &native_get_attr_nti_str,      2, "obj", vm->NormalTypeInstance, "attr", vm->Str);
	register_global_func(vm, 0, ".=",       &native_set_attr_nti_str_any,  3, "obj", vm->NormalTypeInstance, "attr", vm->Str, "v", vm->Any);
	register_global_func(vm, 0, "inherit",  &native_inherit_nt_nt,         2, "t",   vm->NormalType,         "parent", vm->NormalType);

	// Type
	register_global_func(vm, 0, "Type",     &native_type_str          ,1, "name",   vm->Str);

	// low level file operations
	register_global_func(vm, 0, "c_dup2",   &native_c_dup2_int_int,    2, "oldfd",    vm->Int, "newfd", vm->Int);
	register_global_func(vm, 0, "c_open",   &native_c_open_str_str,    2, "pathname", vm->Str, "flags", vm->Str);
	register_global_func(vm, 0, "c_close",  &native_c_close_int,       1, "fd",       vm->Int);
	register_global_func(vm, 0, "c_read",   &native_c_read_int_int,    2, "fd",       vm->Int, "count", vm->Int);
	register_global_func(vm, 1, "c_lseek",  &native_c_lseek_int_int_str,3,"fd",       vm->Int, "offset", vm->Int, "whence", vm->Str);

	// low level misc
	register_global_func(vm, 0, "c_exit",   &native_c_exit_int,        1, "status",   vm->Int);
	register_global_func(vm, 0, "c_fork",   &native_c_fork,            0);
	register_global_func(vm, 0, "c_pipe",   &native_c_pipe,            0);
	register_global_func(vm, 0, "c_waitpid",&native_c_waitpid,         1, "pid",      vm->Int);
	register_global_func(vm, 0, "c_execve", &native_c_execve,          3, "filename", vm->Str, "argv", vm->Arr, "envp", vm->Arr);

	register_global_func(vm, 0, "get_c_errno", &native_get_c_errno,    0);

	// boolean
	register_global_func(vm, 0, "==",       &native_eq_bool_bool,      2, "a",   vm->Bool, "b", vm->Bool);
	register_global_func(vm, 0, "not",      &native_not_bool,          1, "x",   vm->Bool);

	// array
	register_global_func(vm, 0, "+",        &native_plus_arr_arr,      2, "a",   vm->Arr, "b", vm->Arr);
	register_global_func(vm, 0, "push",     &native_push_arr_any,      2, "arr", vm->Arr, "v", vm->Any);
	register_global_func(vm, 0, "shift",    &native_shift_arr,         1, "arr", vm->Arr);
	register_global_func(vm, 0, "shift",    &native_shift_arr_any,     2, "arr", vm->Arr, "dflt", vm->Any);
	register_global_func(vm, 0, "len",      &native_len,               1, "arr", vm->Arr);
	register_global_func(vm, 0, "get",      &native_index_get_arr_int_any, 3, "arr", vm->Arr, "idx", vm->Int, "dflt", vm->Any);
	register_global_func(vm, 1, "[]",       &native_index_get_arr_int, 2, "arr", vm->Arr, "idx", vm->Int);
	register_global_func(vm, 0, "join",     &native_join_arr_str,      2, "arr", vm->Arr, "s", vm->Str);

	// string
	// TODO: other string comparison operators
	register_global_func(vm, 0, "len",      &native_len,               1, "s",   vm->Str);
	register_global_func(vm, 0, "==",       &native_eq_str_str,        2, "a",   vm->Str, "b", vm->Str);

	// int
	register_global_func(vm, 0, "+",        &native_plus_int_int,      2, "a",   vm->Int, "b", vm->Int);
	register_global_func(vm, 0, "*",        &native_mul_int_int,       2, "a",   vm->Int, "b", vm->Int);
	register_global_func(vm, 0, "/",        &native_div_int_int,       2, "a",   vm->Int, "b", vm->Int);
	register_global_func(vm, 0, "%",        &native_mod_int_int,       2, "a",   vm->Int, "b", vm->Int);
	register_global_func(vm, 0, "-",        &native_minus_int_int,     2, "a",   vm->Int, "b", vm->Int);
	register_global_func(vm, 0, "<",        &native_less_int_int,      2, "a",   vm->Int, "b", vm->Int);
	register_global_func(vm, 0, "<=",       &native_less_eq_int_int,   2, "a",   vm->Int, "b", vm->Int);
	register_global_func(vm, 0, ">",        &native_greater_int_int,   2, "a",   vm->Int, "b", vm->Int);
	register_global_func(vm, 0, ">=",       &native_greater_eq_int_int,2, "a",   vm->Int, "b", vm->Int);
	register_global_func(vm, 0, "==",       &native_eq_int_int,        2, "a",   vm->Int, "b", vm->Int);

	// misc
	register_global_func(vm, 0, "dump",     &native_dump_any,          1, "obj", vm->Any);
	register_global_func(vm, 0, "echo",     &native_echo_str,          1, "s",   vm->Str);
	register_global_func(vm, 0, "Bool",     &native_Bool_any,          1, "x",   vm->Any);
	register_global_func(vm, 0, "Str",      &native_Str_int,           1, "n",   vm->Int);
	register_global_func(vm, 0, "is",       &native_is_any_type,       2, "obj", vm->Any, "t", vm->Type);
	register_global_func(vm, 0, "compile",  &native_compile_str_str,   2, "code",vm->Str, "fname", vm->Str);
	register_global_func(vm, 1, "load",     &native_load_str_str,      2, "bytecode", vm->Str, "func_name", vm->Str);
	register_global_func(vm, 1, "parse_json",&native_parse_json_str,   1, "s", vm->Str);

	// hash
	register_global_func(vm, 0, "in",       &native_in_any_hash,       2, "x",   vm->Any, "h", vm->Hash);
	register_global_func(vm, 0, "hash",     &native_hash_any,          1, "x",   vm->Any);
	register_global_func(vm, 0, "keys",     &native_keys_hash,         1, "h",   vm->Hash);
	register_global_func(vm, 0, "values",   &native_values_hash,       1, "h",   vm->Hash);
	register_global_func(vm, 0, "len",      &native_len,               1, "h",   vm->Hash);
	register_global_func(vm, 0, "get",      &native_index_get_hash_any_any,    3, "h",   vm->Hash,"k", vm->Any, "dflt", vm->Any);
	register_global_func(vm, 1, "[]",       &native_index_get_hash_any,        2, "h",   vm->Hash,"k", vm->Any);
	register_global_func(vm, 0, "[]=",      &native_index_set_hash_any_any,    3, "h",   vm->Hash,"k", vm->Any, "v", vm->Any);
	register_global_func(vm, 0, "del",      &native_index_del_hash_any,        2, "h",   vm->Hash,"k", vm->Any);

	// http://stackoverflow.com/questions/3473692/list-environment-variables-with-c-in-unix
	env_hash = make_hash(32);
	for (env = environ; *env; ++env) {
		equal_sign = strchr(*env, '=');
		if(equal_sign) {
			// should be there but ...
			k = make_string_of_len(*env, equal_sign-*env);
			v = make_string(equal_sign+1);
			set_hash_key(env_hash, k, v);
		}
	}
	set_global(vm, "ENV", env_hash);

	argv_array = make_array(argc);
	for(i=0; i<argc; i++) {
		v = make_string(argv[i]);
		ARRAY_ITEMS(argv_array)[i] = v;
	}
	set_global(vm, "ARGV", argv_array);

	// TODO: Some good solution for many defines
	set_global(vm, "C_EINTR", MAKE_INT(EINTR));

#define MKTYPE(var_name, name) \
	var_name = make_normal_type(make_string(name)); \
	set_global(vm, name, var_name);

	MKTYPE(exception_type, "Exception");

		MKTYPE(error_type, "Error");

			MKTYPE(lookup_fail_type, "LookupFail");

				MKTYPE(key_not_found_type, "KeyNotFound");
				add_normal_type_inheritance(key_not_found_type, lookup_fail_type);
				vm->KeyNotFound = key_not_found_type;

				MKTYPE(index_not_found_type, "IndexNotFound");
				add_normal_type_inheritance(index_not_found_type, lookup_fail_type);
				vm->IndexNotFound = index_not_found_type;

				MKTYPE(attr_not_found_type, "AttrNotFound");
				add_normal_type_inheritance(attr_not_found_type, lookup_fail_type);
				vm->AttrNotFound = attr_not_found_type;

			add_normal_type_inheritance(lookup_fail_type, error_type);
			vm->LookupFail = lookup_fail_type;

			MKTYPE(invalid_param_type, "InvalidParameter");
			add_normal_type_inheritance(invalid_param_type, error_type);
			vm->InvalidParameter = invalid_param_type;

		add_normal_type_inheritance(error_type, exception_type);
		vm->Error = error_type;

	vm->Exception = exception_type;

	MKTYPE(command_type, "Command");
	vm->Command = command_type;

#undef MKTYPE

}

void ctx_init(CTX *ctx) {
	ctx->stack_ptr = 0;
	ctx->frame_ptr = 0;
	// XXX: correct sizeof?
	memset(ctx->stack, 0, sizeof(ctx->stack));
	memset(ctx->frames, 0, sizeof(ctx->frames));
}

size_t vm_load_bytecode(VM *vm, char *bc) {

	// For BYTECODE_SECTION_TYPE_CODE
	BYTECODE_HANDLE *bytecode;
	BYTECODE_SECTION_TYPE type;
	BYTECODE_SECTION_LEN len;
	BYTECODE_SECTIONS_COUNT i;
	char *data;
	char *p;

	// For BYTECODE_SECTION_TYPE_GLOBALS
	BYTECODE_GLOBALS_COUNT g, g_max;
	BYTECODE_GLOBALS_LOC_COUNT l, l_max;
	BYTECODE_GLOBALS_OFFSET o;
	GLOBAL_VAR_INDEX gvi;
	unsigned char global_name_len;
	char global_name[257];

	size_t ip = 0;
	DEBUG_VM_API("vm_load_bytecode() VM=%p bytecode=%p\n", vm, bc);

	bytecode = ngs_start_unserializing_bytecode(bc);


	for(i=0; i<bytecode->sections_count; i++) {
		ngs_fetch_bytecode_section(bytecode, &type, &len, &data);
		switch(type) {
			case BYTECODE_SECTION_TYPE_CODE:
				assert(data[len-1] == OP_RET);
				if(vm->bytecode) {
					vm->bytecode = NGS_REALLOC(vm->bytecode, vm->bytecode_len + len);
				} else {
					vm->bytecode = NGS_MALLOC(len);
				}
				assert(vm->bytecode);
				memcpy(vm->bytecode + vm->bytecode_len, data, len);
				ip = vm->bytecode_len;
				vm->bytecode_len += len;
				break;
			case BYTECODE_SECTION_TYPE_GLOBALS:
				p = data;
				BYTECODE_GET(g_max, p, BYTECODE_GLOBALS_COUNT);
				for(g=0; g<g_max; g++) {
					BYTECODE_GET(global_name_len, p, unsigned char); // XXX - check what happens with len 128 and more (unsigned char = char)
					assert(global_name_len);
					memcpy(global_name, p, global_name_len);
					global_name[global_name_len] = 0;
					p += global_name_len;
					BYTECODE_GET(l_max, p, BYTECODE_GLOBALS_LOC_COUNT);
					for(l=0; l<l_max; l++) {
						BYTECODE_GET(o, p, BYTECODE_GLOBALS_OFFSET);
						gvi = get_global_index(vm, global_name, global_name_len);
						DEBUG_VM_API("vm_load_bytecode() processing global patching num=%i name=%s offset=%i resolved_index=%i\n", g, global_name, o, gvi);
						*(GLOBAL_VAR_INDEX *)(&vm->bytecode[ip + o]) = gvi;
					}
				}
				break;
			default:
				// Types 0-255 are a must. Types above that are optional.
				if(type < 0x100) {
					assert(0 == "vm_load_bytecode(): Unknwon section type");
				}
		}
	}
	return ip;
}

METHOD_RESULT vm_call(VM *vm, CTX *ctx, VALUE *result, VALUE callable, LOCAL_VAR_INDEX argc, const VALUE *argv) {
	VALUE *local_var_ptr;
	LOCAL_VAR_INDEX lvi;
	int i;
	METHOD_RESULT mr;
	VALUE *callable_items;
	int args_to_use;

	if(IS_ARRAY(callable)) {
		for(i=OBJ_LEN(callable)-1, callable_items = OBJ_DATA_PTR(callable); i>=0; i--) {
			mr = vm_call(vm, ctx, result, callable_items[i], argc, argv);
			if((mr == METHOD_OK) || (mr == METHOD_EXCEPTION)) {
				return mr;
			}
			// Don't know how to handle other conditions yet
			assert(mr == METHOD_ARGS_MISMATCH);
		}
		return METHOD_IMPL_MISSING;
	}

	if(IS_NATIVE_METHOD(callable)) {
		// None of native method uses optional parameters for now
		if(NATIVE_METHOD_OBJ_N_OPT_PAR(callable)) {
			assert(0=="Optional parameters are not implemented yet");
		}
		// dump_titled("Native callable", callable);
		if(argc != NATIVE_METHOD_OBJ_N_REQ_PAR(callable)) {
			return METHOD_ARGS_MISMATCH;
		}
		// printf("PT 0\n");
		for(lvi=0; lvi<NATIVE_METHOD_OBJ_N_REQ_PAR(callable); lvi++) {
			// TODO: make sure second argument is type durng closure creation
			// dump_titled("ARGV[lvi]", argv[lvi]);
			if(!obj_is_of_type(argv[lvi], NATIVE_METHOD_OBJ_PARAMS(callable)[lvi*2+1])) {
				return METHOD_ARGS_MISMATCH;
			}
		}
		// printf("PT 2\n");
		if(NATIVE_METHOD_EXTRA_PARAMS(callable)) {
			mr = ((VM_EXT_FUNC)OBJ_DATA_PTR(callable))(vm, ctx, argv, result);
		} else {
			mr = ((VM_FUNC)OBJ_DATA_PTR(callable))(argv, result);
		}
		return mr;
	}

	if(IS_CLOSURE(callable)) {
		// Check parameters type matching
		if(CLOSURE_OBJ_N_OPT_PAR(callable)) {
			assert(0=="Optional parameters are not implemented yet");
		}
		if(argc < CLOSURE_OBJ_N_REQ_PAR(callable)) {
			return METHOD_ARGS_MISMATCH;
		}
		args_to_use = CLOSURE_OBJ_N_REQ_PAR(callable);
		if(!(CLOSURE_OBJ_PARAMS_FLAGS(callable) & PARAMS_FLAG_ARR_SPLAT)) {
			if(argc > args_to_use) {
				return METHOD_ARGS_MISMATCH;
			}
		}
		for(lvi=0; lvi<CLOSURE_OBJ_N_REQ_PAR(callable); lvi++) {
			// TODO: make sure second argument is a type (when creating a closure)
			if(!obj_is_of_type(argv[lvi], CLOSURE_OBJ_PARAMS(callable)[lvi*2+1])) {
				return METHOD_ARGS_MISMATCH;
			}
		}

		// Setup call frame
		assert(ctx->frame_ptr < MAX_FRAMES);
		lvi = CLOSURE_OBJ_N_LOCALS(callable);
		// printf("N LOCALS %d\n", lvi);
		if(lvi) {
			ctx->frames[ctx->frame_ptr].locals = NGS_MALLOC(lvi * sizeof(VALUE));
			assert(ctx->frames[ctx->frame_ptr].locals);
			assert(argc >= args_to_use);
			memcpy(ctx->frames[ctx->frame_ptr].locals, argv, sizeof(VALUE) * args_to_use);
			if(CLOSURE_OBJ_PARAMS_FLAGS(callable) & PARAMS_FLAG_ARR_SPLAT) {
				ctx->frames[ctx->frame_ptr].locals[args_to_use] = make_array_with_values(argc - args_to_use, &argv[args_to_use]);
				args_to_use++; // from this point on: as opposed to number of all locals, used in calculation of how much locals to SET_UNDEF()
			}
			lvi -= args_to_use;
			// printf("LVI %d\n", lvi);
			for(local_var_ptr=&ctx->frames[ctx->frame_ptr].locals[args_to_use];lvi;lvi--,local_var_ptr++) {
				SET_UNDEF(*local_var_ptr);
			}
		} else {
			ctx->frames[ctx->frame_ptr].locals = NULL;
		}
		ctx->frames[ctx->frame_ptr].closure = callable;
		ctx->frames[ctx->frame_ptr].try_info_ptr = 0;
		// printf("INCREASING FRAME PTR\n");
		ctx->frame_ptr++;
		mr = vm_run(vm, ctx, CLOSURE_OBJ_IP(callable), result);
		ctx->frame_ptr--;
		return mr;
	}

	if(IS_NGS_TYPE(callable)) {
		return vm_call(vm, ctx, result, NGS_TYPE_CONSTRUCTORS(callable), argc, argv);
	}

	if(IS_NORMAL_TYPE_CONSTRUCTOR(callable)) {
		*result = make_normal_type_instance(UT_CONSTRUCTOR_UT(callable));
		return METHOD_OK;
	}

	// TODO: allow handling of calling of undefined methods by the NGS language
	dump_titled("vm_call(): Don't know how to call", callable);
	abort();
	// TODO: return the exception
	return METHOD_EXCEPTION;

}

METHOD_RESULT vm_run(VM *vm, CTX *ctx, IP ip, VALUE *result) {
	VALUE v, callable, command;
	VAR_LEN_OBJECT *vlo;
	int i;
	unsigned char opcode;
	GLOBAL_VAR_INDEX gvi;
	PATCH_OFFSET po;
	JUMP_OFFSET jo;
	LOCAL_VAR_INDEX lvi;
	size_t vlo_len, j;
	METHOD_RESULT mr;
	size_t saved_stack_ptr = ctx->stack_ptr;
	size_t string_components_count;

	// for OP_MAKE_CLOSURE
	LOCAL_VAR_INDEX n_locals, n_params_required, n_params_optional;
	UPVAR_INDEX n_uplevels, uvi;
	int params_flags;

main_loop:
	opcode = vm->bytecode[ip++];
#ifdef DO_NGS_DEBUG
	if(opcode <= sizeof(opcodes_names) / sizeof(char *)) {
		DEBUG_VM_RUN("main_loop FRAME_PTR=%zu IP=%zu OP=%s STACK_LEN=%zu TRY_LEN=%i\n", ctx->frame_ptr, ip-1, opcodes_names[opcode], ctx->stack_ptr, THIS_FRAME.try_info_ptr);
		decompile(vm->bytecode, ip-1, ip);
		for(j=ctx->stack_ptr; j>0; j--) {
			printf("Stack @ %zu\n", j-1);
			dump(ctx->stack[j-1]);
		}
	}
#endif

	// Guidelines
	// * increase ip as soon as arguments extraction is finished
	switch(opcode) {
		case OP_HALT:
							goto end_main_loop;
		case OP_PUSH_NULL:
							PUSH_NULL;
							goto main_loop;
		case OP_PUSH_FALSE:
							SET_FALSE(v);
							PUSH(v);
							goto main_loop;
		case OP_PUSH_TRUE:
							SET_TRUE(v);
							PUSH(v);
							goto main_loop;
		case OP_PUSH_INT:
							// Arg: n
							// In ...
							// Out: ... n
							// TODO: make it push_intSIZE maybe?
							i = *(int *) &vm->bytecode[ip];
							ip += sizeof(i);
							SET_INT(v, i);
							PUSH(v);
							goto main_loop;
		case OP_PUSH_L_STR:
							// Arg: LEN + string
							// In: ...
							// Out: ... string
							// printf("LSTR @ %p\n", &vm->bytecode[ip]);
							vlo = NGS_MALLOC(sizeof(*vlo));
							vlo->len = (size_t) vm->bytecode[ip];
							vlo->base.type.num = T_STR;
							vlo->base.val.ptr = NGS_MALLOC_ATOMIC(vlo->len);
							memcpy(vlo->base.val.ptr, &(vm->bytecode[ip+1]), vlo->len);
							ip += 1 + vm->bytecode[ip];
							SET_OBJ(v, vlo);
							PUSH(v);
							goto main_loop;
		case OP_DUP:
							DUP;
							goto main_loop;
		case OP_POP:
							REMOVE_TOP;
							goto main_loop;
		case OP_XCHG:
							EXPECT_STACK_DEPTH(2);
							v = TOP;
							TOP = SECOND;
							SECOND = v;
							goto main_loop;
		case OP_RESOLVE_GLOBAL:
							// Probably not worh optimizing
							POP(v);
							assert(IS_STRING(v));
							SET_INT(v, get_global_index(vm, OBJ_DATA_PTR(v), OBJ_LEN(v)));
							PUSH(v);
							goto main_loop;
		case OP_PATCH:
							// Arg: offset
							// In ... n
							// Out: ...
							// Effect: bytecode[offset] <- n
							POP(v);
							ARG(po, PATCH_OFFSET);
#ifdef DO_NGS_DEBUG
							DEBUG_VM_RUN("OP_PATCH dst_idx=%zu v=%d\n", ip+po, *(GLOBAL_VAR_INDEX *)&vm->bytecode[ip+po]);
							assert(*(GLOBAL_VAR_INDEX *)&vm->bytecode[ip+po] == 0); // try to catch patching at invalid offset
#endif
							*(GLOBAL_VAR_INDEX *)&vm->bytecode[ip+po] = GET_INT(v);
							goto main_loop;
		case OP_FETCH_GLOBAL:
							ARG_GVI;
#ifdef DO_NGS_DEBUG
							// DEBUG_VM_RUN("OP_FETCH_GLOBAL gvi=%d len=%d\n", gvi, vm->globals_len);
							assert(gvi < vm->globals_len);
#endif
							// TODO: report error here instead of crashing
							if(IS_UNDEF(GLOBALS[gvi])) {
								fprintf(stderr, "Global '%s' (index %d) not found\n", vm->globals_names[gvi], gvi);
								assert(0=="Global not found");
							}
							// dump_titled("FETCH_GLOBAL", GLOBALS[gvi]);
							PUSH(GLOBALS[gvi]);
							goto main_loop;
		case OP_STORE_GLOBAL:
							ARG_GVI;
							POP(v);
#ifdef DO_NGS_DEBUG
							// DEBUG_VM_RUN("OP_STORE_GLOBAL gvi=%d len=%zu\n", gvi, vm->globals_len);
							assert(gvi < vm->globals_len);
							// TODO: report error here instead of crashing
#endif
							GLOBALS[gvi] = v;
							goto main_loop;
		case OP_FETCH_LOCAL:
							ARG_LVI;
							assert(IS_NOT_UNDEF(LOCALS[lvi]));
							PUSH(LOCALS[lvi]);
							// printf("LVI %d FRAME_PTR %d\n", lvi, ctx->frame_ptr-1);
							// dump_titled("OP_FETCH_LOCAL", LOCALS[lvi]);
							goto main_loop;
		case OP_STORE_LOCAL:
							ARG_LVI;
							POP(v);
							LOCALS[lvi] = v;
							goto main_loop;
		case OP_CALL:
							// TODO: print arguments of failed call, not just the callable
							// In (current): ... result_placeholder (null), arg1, ..., argN, argc, callable
							// In (WIP): ...
							//     result_placeholder (null),
							//     pos_arg1, ..., pos_argN,
							//     opt_arg1, ..., opt_argN,
							//     n_params_required,
							//     n_params_optional,
							//     callable
							// Out: ... result
							POP(callable);
							// dump_titled("CALLABLE", callable);
							POP(v); // number of arguments
							// POP(n_params_required); // number of arguments
							// POP(n_params_optional);
							mr = vm_call(vm, ctx, &ctx->stack[ctx->stack_ptr-GET_INT(v)-1], callable, GET_INT(v), &ctx->stack[ctx->stack_ptr-GET_INT(v)]);
							// assert(ctx->stack[ctx->stack_ptr-GET_INT(v)-1].num);
							if(mr == METHOD_EXCEPTION) {
								*result = ctx->stack[ctx->stack_ptr-GET_INT(v)-1];
								// dump_titled("E1", *result);
								goto exception;
							}
							if(mr != METHOD_OK) {
								dump_titled("Failed callable", callable);
								assert(0=="Handling failed method calls is not implemented yet");
							}
							REMOVE_N(GET_INT(v));
							goto main_loop;
		case OP_CALL_EXC:
							// Calls exception handler, METHOD_IMPL_MISSING means we should re-throw the exception
							POP(callable);
							POP(v); // number of arguments
							mr = vm_call(vm, ctx, &ctx->stack[ctx->stack_ptr-GET_INT(v)-1], callable, GET_INT(v), &ctx->stack[ctx->stack_ptr-GET_INT(v)]);
							if(mr == METHOD_EXCEPTION) {
								// TODO: special handling? Exception during exception handling.
								*result = ctx->stack[ctx->stack_ptr-GET_INT(v)-1];
								goto exception;
							}
							if(mr == METHOD_IMPL_MISSING) {
								goto exception_return;
							}
							if(mr != METHOD_OK) {
								dump_titled("Failed callable", callable);
								assert(0=="Handling failed method calls is not implemented yet");
							}
							REMOVE_N(GET_INT(v));
							goto main_loop;
		case OP_CALL_ARR:
							POP(callable);
							mr = vm_call(vm, ctx, &ctx->stack[ctx->stack_ptr-2], callable, OBJ_LEN(ctx->stack[ctx->stack_ptr-1]), ARRAY_ITEMS(ctx->stack[ctx->stack_ptr-1]));
							// assert(ctx->stack[ctx->stack_ptr-2].num);
							if(mr == METHOD_EXCEPTION) {
								// printf("E2\n");
								*result = ctx->stack[ctx->stack_ptr-2];
								goto exception;
							}
							if(mr != METHOD_OK) {
								dump_titled("Failed callable", callable);
								assert(0=="Handling failed method calls is not implemented yet");
							}
							REMOVE_N(1);
							goto main_loop;
		case OP_RET:
							if(saved_stack_ptr < ctx->stack_ptr) {
								POP(*result);
								// dump_titled("RESULT", *result);
							} else {
								assert(0=="Function does not have result value");
							}
							assert(saved_stack_ptr == ctx->stack_ptr);
							return METHOD_OK;
		case OP_JMP:
do_jump:
							ARG(jo, JUMP_OFFSET);
							ip += jo;
							goto main_loop;
		case OP_JMP_TRUE:
							POP(v);
#ifdef DO_NGS_DEBUG
							assert(IS_BOOL(v));
#endif
							if(IS_TRUE(v)) goto do_jump;
							ip += sizeof(jo);
							goto main_loop;
		case OP_JMP_FALSE:
							POP(v);
#ifdef DO_NGS_DEBUG
							assert(IS_BOOL(v));
#endif
							if(IS_FALSE(v)) goto do_jump;
							ip += sizeof(jo);
							goto main_loop;
		case OP_MAKE_ARR:
							POP(v);
							vlo_len = GET_INT(v);
							v = make_array_with_values(vlo_len, &(ctx->stack[ctx->stack_ptr-vlo_len]));
							ctx->stack_ptr -= vlo_len;
							PUSH(v);
							goto main_loop;
		case OP_MAKE_CLOSURE:
							// Arg: code_jump_offset, number_of_locals
							// In: ...,
							//   arg1_name, arg1_type, ... argN_name, argN_type,
							//   argN+1_name, argN+1_type, argN+1_default_value, ... argM_name, argM_type, argM_default_value,
							//   argc_of_required_args, argc_of_optional_args
							// Out: ..., CLOSURE_OBJECT
							ARG(jo, JUMP_OFFSET);
							ARG(n_params_required, LOCAL_VAR_INDEX);
							ARG(n_params_optional, LOCAL_VAR_INDEX);
							ARG(n_locals, LOCAL_VAR_INDEX);
							ARG(n_uplevels, UPVAR_INDEX);
							ARG(params_flags, int);
							v = make_closure_obj(
									ip+jo,
									n_locals, n_params_required, n_params_optional, n_uplevels, params_flags,
									&ctx->stack[ctx->stack_ptr - (n_params_required + ADDITIONAL_PARAMS_COUNT)*2 - n_params_optional*3]
							);
							ctx->stack_ptr -= (n_params_required + ADDITIONAL_PARAMS_COUNT)*2 - n_params_optional*3;
							if(n_uplevels) {
								assert(CLOSURE_OBJ_N_UPLEVELS(THIS_FRAME_CLOSURE) >= n_uplevels-1);
								CLOSURE_OBJ_UPLEVELS(v) = NGS_MALLOC(sizeof(CLOSURE_OBJ_UPLEVELS(v)[0]) * n_uplevels);
								// First level of upvars are the local variables of current frame
								CLOSURE_OBJ_UPLEVELS(v)[0] = LOCALS;
								// Other levels come from current closure upvars
								memcpy(&(CLOSURE_OBJ_UPLEVELS(v)[1]), CLOSURE_OBJ_UPLEVELS(THIS_FRAME_CLOSURE), sizeof(CLOSURE_OBJ_UPLEVELS(v)[0]) * (n_uplevels - 1));
							}
							PUSH(v);
							goto main_loop;
		case OP_TO_STR:
							CONVERTING_OP(IS_STRING, Str);
		case OP_MAKE_STR:
							// TODO: (optimization) update top of the stack instead of POP and PUSH
							POP(v);
							string_components_count = GET_INT(v);
							v = join_strings(string_components_count, &(ctx->stack[ctx->stack_ptr-string_components_count]));
							assert(ctx->stack_ptr >= string_components_count);
							ctx->stack_ptr -= string_components_count;
							PUSH(v);
							goto main_loop;
		case OP_PUSH_EMPTY_STR:
							v = make_var_len_obj(T_STR, 1, 0);
							PUSH(v);
							goto main_loop;
		case OP_GLOBAL_DEF_P:
							ARG_GVI;
							PUSH(MAKE_BOOL(IS_NOT_UNDEF(GLOBALS[gvi])));
							goto main_loop;
		case OP_LOCAL_DEF_P:
							ARG_LVI;
							PUSH(MAKE_BOOL(IS_NOT_UNDEF(LOCALS[lvi])));
							goto main_loop;
		case OP_DEF_GLOBAL_FUNC:
							// Arg: gvi
							// In: ..., closure
							// Out: ..., closure
							assert(ctx->stack_ptr);
							ARG_GVI;
#ifdef DO_NGS_DEBUG
							// DEBUG_VM_RUN("OP_STORE_GLOBAL gvi=%d len=%zu\n", gvi, vm->globals_len);
							assert(gvi < vm->globals_len);
							// TODO: report error here instead of crashing
#endif
							if(IS_UNDEF(GLOBALS[gvi])) {
								GLOBALS[gvi] = make_array_with_values(1, &TOP);
							} else {
								PUSH_FUNC(GLOBALS[gvi], TOP);
							}
							goto main_loop;
		case OP_DEF_LOCAL_FUNC:
							// Arg: lvi
							// In: ..., closure
							// Out: ..., closure
							assert(ctx->stack_ptr);
							ARG_LVI;
							if(IS_UNDEF(LOCALS[lvi])) {
								LOCALS[lvi] = make_array_with_values(1, &TOP);
							} else {
								PUSH_FUNC(LOCALS[lvi], TOP);
							}
							goto main_loop;
		case OP_FETCH_UPVAR:
#ifdef DO_NGS_DEBUG
							assert(ctx->frame_ptr);
#endif
							ARG_UVI;
							ARG_LVI;
							// printf("uvi=%d lvi=%d\n", uvi, lvi);
							PUSH(UPLEVELS[uvi][lvi]);
							goto main_loop;
		case OP_STORE_UPVAR:
#ifdef DO_NGS_DEBUG
							assert(ctx->frame_ptr);
#endif
							ARG_UVI;
							ARG_LVI;
							POP(v);
							UPLEVELS[uvi][lvi] = v;
							goto main_loop;
		case OP_UPVAR_DEF_P:
#ifdef DO_NGS_DEBUG
							assert(ctx->frame_ptr);
#endif
							ARG_UVI;
							ARG_LVI;
							PUSH(MAKE_BOOL(IS_NOT_UNDEF(UPLEVELS[uvi][lvi])));
							goto main_loop;
		case OP_DEF_UPVAR_FUNC:
							// XXX: untested and not covered by tests yet
#ifdef DO_NGS_DEBUG
							assert(ctx->frame_ptr);
#endif
							// Arg: uvi, lvi
							// In: ..., closure
							// Out: ..., closure
							assert(ctx->stack_ptr);
							ARG_UVI;
							ARG_LVI;
							if(IS_UNDEF(UPLEVELS[uvi][lvi])) {
								UPLEVELS[uvi][lvi] = make_array_with_values(1, &TOP);
							} else {
								PUSH_FUNC(UPLEVELS[uvi][lvi], TOP);
							}
							goto main_loop;
		case OP_MAKE_HASH:
							POP(v);
							vlo_len = GET_INT(v);
							v = make_hash(vlo_len);
							ctx->stack_ptr -= vlo_len * 2;
							for(j=0; j<vlo_len;j++) {
								set_hash_key(v, ctx->stack[ctx->stack_ptr+j*2], ctx->stack[ctx->stack_ptr+j*2+1]);
							}
							PUSH(v);
							goto main_loop;
		case OP_TO_BOOL:
							CONVERTING_OP(IS_BOOL, Bool);
		case OP_TO_ARR:
							CONVERTING_OP(IS_ARRAY, Arr);
		case OP_ARR_APPEND:
							EXPECT_STACK_DEPTH(2);
							array_push(ctx->stack[ctx->stack_ptr-2], ctx->stack[ctx->stack_ptr-1]);
							REMOVE_N(1);
							goto main_loop;
		case OP_ARR_CONCAT:
							EXPECT_STACK_DEPTH(2);
							native_plus_arr_arr(&ctx->stack[ctx->stack_ptr-2], &v);
							REMOVE_N(2);
							PUSH(v);
							goto main_loop;
		case OP_GUARD:
							EXPECT_STACK_DEPTH(1);
							POP(v);
							assert(IS_BOOL(v));
							if(IS_TRUE(v)) {
								goto main_loop;
							}
							assert(saved_stack_ptr == ctx->stack_ptr);
							return METHOD_ARGS_MISMATCH;
		case OP_TRY_START:
							// printf("TRY START %i\n", THIS_FRAME.try_info_ptr);
							assert(THIS_FRAME.try_info_ptr < MAX_TRIES_PER_FRAME);
							ARG(jo, JUMP_OFFSET);
							THIS_FRAME.try_info[THIS_FRAME.try_info_ptr].catch_ip = ip + jo;
							THIS_FRAME.try_info[THIS_FRAME.try_info_ptr].saved_stack_ptr = ctx->stack_ptr;
							THIS_FRAME.try_info_ptr++;
							// printf("TRY START %i\n", THIS_FRAME.try_info_ptr);
							goto main_loop;
		case OP_TRY_END:
							assert(THIS_FRAME.try_info_ptr);
							THIS_FRAME.try_info_ptr--;
							// printf("TRY END %i\n", THIS_FRAME.try_info_ptr);
							goto do_jump;
		case OP_ARR_REVERSE:
							EXPECT_STACK_DEPTH(1);
							array_reverse(TOP);
							goto main_loop;
		case OP_THROW:
							POP(*result);
							goto exception;
		case OP_MAKE_CMD:
							EXPECT_STACK_DEPTH(1);
							command = make_normal_type_instance(vm->Command);
							POP(v);
							set_normal_type_instance_attribute(command, make_string("argv"), v);
							PUSH(command);
							goto main_loop;
		default:
							// TODO: exception
							printf("ERROR: Unknown opcode %d\n", opcode);
	}

end_main_loop:
	return METHOD_OK;

exception:
	// TOP is the excepion value
	if (THIS_FRAME.try_info_ptr) {
		// We have local exception handler
		THIS_FRAME.try_info_ptr--;
		ip = THIS_FRAME.try_info[THIS_FRAME.try_info_ptr].catch_ip;
		ctx->stack_ptr = THIS_FRAME.try_info[THIS_FRAME.try_info_ptr].saved_stack_ptr;
		PUSH(*result);
		goto main_loop;
	}

exception_return:
	// We don't handle the exception
	ctx->stack_ptr = saved_stack_ptr;
	return METHOD_EXCEPTION;
}
#undef GLOBALS
#undef LOCALS

// For composing bytecode
BYTECODE_HANDLE *ngs_create_bytecode() {
	BYTECODE_HANDLE *h;
	BYTECODE_SECTION_LEN len;
	char *p;
	h = NGS_MALLOC(sizeof(*h));
	assert(h);
	len = strlen(BYTECODE_SIGNATURE) + sizeof(BYTECODE_ORDER_CHECK) + sizeof(BYTECODE_SECTIONS_COUNT);
	h->data = NGS_MALLOC(len);
	h->len = len;
	p = h->data;

	memcpy(p, BYTECODE_SIGNATURE, strlen(BYTECODE_SIGNATURE));
	p += strlen(BYTECODE_SIGNATURE);

	BYTECODE_ADD(p, BYTECODE_ORDER_CHECK, BYTECODE_ORDER_CHECK_VAL);
	BYTECODE_ADD(p, BYTECODE_SECTIONS_COUNT, 0);

	return h;
}

// For composing bytecode
void ngs_add_bytecode_section(BYTECODE_HANDLE *h, BYTECODE_SECTION_TYPE type, BYTECODE_SECTION_LEN len, char *data) {
	char *p;
	size_t alloc_incr;
	alloc_incr = sizeof(BYTECODE_SECTION_TYPE) + sizeof(BYTECODE_SECTION_LEN) + len;
	h->data = NGS_REALLOC(h->data, h->len + alloc_incr);
	assert(h->data);
	p = &h->data[h->len];
	h->len += alloc_incr;

	BYTECODE_ADD(p, BYTECODE_SECTION_TYPE, type);
	BYTECODE_ADD(p, BYTECODE_SECTION_LEN, len);
	memcpy(p, data, len);

	p = &h->data[strlen(BYTECODE_SIGNATURE) + sizeof(BYTECODE_ORDER_CHECK)];
	(*(BYTECODE_SECTIONS_COUNT *) p)++;
}

BYTECODE_HANDLE *ngs_start_unserializing_bytecode(char *data) {
	BYTECODE_HANDLE *h;
	char *p;
	h = NGS_MALLOC(sizeof(*h));
	assert(h);
	p = data;
	h->data = data;
	h->next_section_num = 0;

	if(memcmp(p, BYTECODE_SIGNATURE, strlen(BYTECODE_SIGNATURE))) {
		assert(0 == "Bytecode has invalid signature");
	}
	p += strlen(BYTECODE_SIGNATURE);

	if(*(BYTECODE_ORDER_CHECK *)p != BYTECODE_ORDER_CHECK_VAL) {
		assert(0 == "Bytecode has invalid byte order");
	}
	p += sizeof(BYTECODE_ORDER_CHECK);

	h->sections_count = *(BYTECODE_SECTIONS_COUNT *)p;
	p += sizeof(BYTECODE_SECTIONS_COUNT);

	h->next_section_ptr = p;

	return h;
}

void ngs_fetch_bytecode_section(BYTECODE_HANDLE *h, BYTECODE_SECTION_TYPE *type, BYTECODE_SECTION_LEN *len, char **data) {
	char *p;
	if(h->next_section_num >= h->sections_count) {
		*type = 0;
		*len = 0;
		*data = NULL;
		return;
	}
	p = h->next_section_ptr;
	*type = *(BYTECODE_SECTION_TYPE *) p;
	p += sizeof(BYTECODE_SECTION_TYPE);
	*len = *(BYTECODE_SECTION_LEN *) p;
	p += sizeof(BYTECODE_SECTION_LEN);
	*data = p;
	p += *len;

	h->next_section_ptr = p;
	h->next_section_num++;
}

#undef BYTECODE_ADD
#undef BYTECODE_GET
