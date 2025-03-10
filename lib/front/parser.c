// TODO list
//   - make sure all extensions are errors on pedantic mode
//
//   - enable some sort of error recovery when we encounter bad syntax within a
//   statement, basically just find the semicolon and head out :p
//
//   - ugly ass code
//
//   - make the expect(...) code be a little smarter, if it knows that it's expecting
//   a token for a specific operation... tell the user
#include "parser.h"
#include <targets/targets.h>
#include <timer.h>
#undef VOID // winnt.h loves including garbage

#define OUT_OF_ORDER_CRAP 1

// how big are the phase2 parse tasks
#define PARSE_MUNCH_SIZE (32768)

typedef struct {
    Atom key;
    Cuik_Type* value;
} TagEntry;

typedef struct {
    Atom key;
    Symbol value;
} SymbolEntry;

typedef struct {
    Atom key;
    Stmt* value;
} LabelEntry;

typedef struct {
    enum {
        PENDING_ALIGNAS
    } mode;
    size_t expr_pos;
    int* dst;
} PendingExpr;

// starting point is used to disable the function literals
// from reading from their parent function
thread_local static int local_symbol_start = 0;
thread_local static int local_symbol_count = 0;
thread_local static Symbol* local_symbols;

thread_local static int local_tag_count = 0;
thread_local static TagEntry* local_tags;

// Global symbol stuff
thread_local static PendingExpr* pending_exprs;  // stb_ds array
thread_local static TagEntry* global_tags;       // stb_ds hash map
thread_local static SymbolEntry* global_symbols; // stb_ds hash map
thread_local static LabelEntry* labels;          // stb_ds hash map

thread_local static Stmt* current_switch_or_case;
thread_local static Stmt* current_breakable;
thread_local static Stmt* current_continuable;

// we build a chain in that lets us know what symbols are used by a function
thread_local static Expr* symbol_chain_start;
thread_local static Expr* symbol_chain_current;

// we allocate nodes from here but once the threaded parsing stuff is complete we'll stitch this
// to the original AST arena so that it may be freed later
thread_local static Arena local_ast_arena;
thread_local static bool out_of_order_mode;

static void expect(TokenStream* restrict s, char ch);
static void expect_closing_paren(TranslationUnit* tu, TokenStream* restrict s, SourceLocIndex opening);
static void expect_with_reason(TokenStream* restrict s, char ch, const char* reason);
static Symbol* find_local_symbol(TokenStream* restrict s);

static Stmt* parse_stmt(TranslationUnit* tu, TokenStream* restrict s);
static Stmt* parse_stmt_or_expr(TranslationUnit* tu, TokenStream* restrict s);
static Stmt* parse_compound_stmt(TranslationUnit* tu, TokenStream* restrict s);
static void parse_decl_or_expr(TranslationUnit* tu, TokenStream* restrict s, size_t* body_count);

static bool skip_over_declspec(TokenStream* restrict s);
static bool try_parse_declspec(TranslationUnit* tu, TokenStream* restrict s, Attribs* attr);
static Cuik_Type* parse_declspec(TranslationUnit* tu, TokenStream* restrict s, Attribs* attr);

static Decl parse_declarator(TranslationUnit* tu, TokenStream* restrict s, Cuik_Type* type, bool is_abstract, bool disabled_paren);
static Cuik_Type* parse_typename(TranslationUnit* tu, TokenStream* restrict s);

// It's like parse_expr but it doesn't do anything with comma operators to avoid
// parsing issues.
static intmax_t parse_const_expr(TranslationUnit* tu, TokenStream* restrict s);
static Expr* parse_initializer(TranslationUnit* tu, TokenStream* restrict s, Cuik_Type* type);
static Expr* parse_function_literal(TranslationUnit* tu, TokenStream* restrict s, Cuik_Type* type);
static void parse_function_definition(TranslationUnit* tu, TokenStream* restrict s, Stmt* n);
static Cuik_Type* parse_type_suffix(TranslationUnit* tu, TokenStream* restrict s, Cuik_Type* type, Atom name);

static bool is_typename(TokenStream* restrict s);

static _Noreturn void generic_error(TokenStream* restrict s, const char* msg);

// Usage:
//  LOCAL_SCOPE {
//      /* do parse work */
//  }
#define LOCAL_SCOPE                                                         \
for (int saved = local_symbol_count, saved2 = local_tag_count, _i_ = 0; \
    _i_ == 0; _i_ += 1, local_symbol_count = saved, local_tag_count = saved2)

static int align_up(int a, int b) {
    if (b == 0) return 0;

    return a + (b - (a % b)) % b;
}

// allocated size is sizeof(struct StmtHeader) + extra_size
static Stmt* make_stmt(TranslationUnit* tu, TokenStream* restrict s, StmtOp op, size_t extra_size) {
    Stmt* stmt = arena_alloc(&local_ast_arena, sizeof(Stmt), _Alignof(max_align_t));

    memset(stmt, 0, offsetof(Stmt, backing) + sizeof(((Stmt*)0)->backing));
    stmt->op = op;
    stmt->loc = tokens_get_location_index(s);
    return stmt;
}

static Expr* make_expr(TranslationUnit* tu) {
    return ARENA_ALLOC(&local_ast_arena, Expr);
}

static Symbol* find_global_symbol(const char* name) {
    ptrdiff_t temp;
    ptrdiff_t search = shgeti_ts(global_symbols, name, temp);

    return (search >= 0) ? &global_symbols[search].value : NULL;
}

static Cuik_Type* find_tag(const char* name) {
    // try locals
    size_t i = local_tag_count;
    while (i--) {
        if (strcmp((const char*)local_tags[i].key, name) == 0) {
            return local_tags[i].value;
        }
    }

    // try globals
    ptrdiff_t temp;
    ptrdiff_t search = shgeti_ts(global_tags, name, temp);
    return (search >= 0) ? global_tags[search].value : NULL;
}

// ( SOMETHING )
// ( SOMETHING ,
static size_t skip_expression_in_parens(TokenStream* restrict s, TknType* out_terminator) {
    size_t saved = s->current;

    // by default we expect to exit with closing parens
    *out_terminator = ')';

    int depth = 1;
    while (depth) {
        Token* t = tokens_get(s);

        if (t->type == '\0') {
            *out_terminator = '\0';
            break;
        } else if (t->type == '(') {
            depth++;
        } else if (t->type == ')') {
            depth--;
        } else if (t->type == ',' && depth == 1) {
            *out_terminator = ',';
            depth--;
        }

        tokens_next(s);
    }

    return saved;
}

// SOMETHING ,
// SOMETHING }
//
// this code sucks btw, idk why i just think it's lowkey ugly
static size_t skip_expression_in_enum(TokenStream* restrict s, TknType* out_terminator) {
    size_t saved = s->current;

    // our basic bitch expectations
    *out_terminator = ',';

    int depth = 1;
    while (depth) {
        Token* t = tokens_get(s);

        if (t->type == '\0') {
            *out_terminator = '\0';
            break;
        } else if (t->type == '{') {
            depth++;
        } else if (t->type == '}' && depth == 1) {
            *out_terminator = '}';
            break;
        } else if (t->type == ',' && depth == 1) {
            break;
        }

        tokens_next(s);
    }

    return saved;
}

#include "expr_parser.h"
#include "decl_parser.h"

typedef struct {
    // shared state, every run of phase2_parse_task will decrement this by one
    atomic_size_t* tasks_remaining;
    size_t start, end;

    TranslationUnit* tu;
    TagEntry* global_tags;       // stb_ds hash map
    SymbolEntry* global_symbols; // stb_ds hash map

    const TokenStream* base_token_stream;
} ParserTaskInfo;

// we have a bunch of thread locals and for the sake of it, we wanna reset em before
// a brand new parser on this thread
static void reset_global_parser_state() {
    labels = NULL;
    global_symbols = NULL;
    global_tags = NULL;
    pending_exprs = NULL;
    local_symbol_start = local_symbol_count = 0;
    current_switch_or_case = current_breakable = current_continuable = NULL;
    symbol_chain_start = symbol_chain_current = NULL;

    // allocate sum shit
    local_symbols = realloc(local_symbols, sizeof(Symbol) * MAX_LOCAL_SYMBOLS);
    local_tags = realloc(local_tags, sizeof(TagEntry) * MAX_LOCAL_TAGS);
}

static void parse_global_symbols(TranslationUnit* tu, size_t start, size_t end, TokenStream tokens) {
    CUIK_TIMED_BLOCK("phase 3: %zu-%zu", start, end) {
        out_of_order_mode = false;

        for (size_t i = start; i < end; i++) {
            Symbol* sym = &global_symbols[i].value;

            // don't worry about normal globals, those have been taken care of...
            if (sym->current != 0 && (sym->storage_class == STORAGE_STATIC_FUNC || sym->storage_class == STORAGE_FUNC)) {
                // Spin up a mini parser here
                tokens.current = sym->current;

                // intitialize use list
                symbol_chain_start = symbol_chain_current = NULL;

                // Some sanity checks in case a local symbol is leaked funny.
                assert(local_symbol_start == 0 && local_symbol_count == 0);
                parse_function_definition(tu, &tokens, sym->stmt);
                local_symbol_start = local_symbol_count = 0;

                // finalize use list
                sym->stmt->decl.first_symbol = symbol_chain_start;
            }
        }
    }
}

static void phase3_parse_task(void* arg) {
    ParserTaskInfo task = *((ParserTaskInfo*)arg);
    reset_global_parser_state();

    // intitialize any thread local state that might not be set on this thread
    global_tags = task.global_tags;
    global_symbols = task.global_symbols;

    tls_init();
    atoms_init();

    parse_global_symbols(task.tu, task.start, task.end, *task.base_token_stream);
    *task.tasks_remaining -= 1;

    // move local AST arena to TU's AST arena
    {
        mtx_lock(&task.tu->arena_mutex);

        arena_trim(&local_ast_arena);
        arena_append(&task.tu->ast_arena, &local_ast_arena);
        local_ast_arena = (Arena){0};

        mtx_unlock(&task.tu->arena_mutex);
    }
}

// 0 no cycles
// 1 cycles
// 2 cycles and we gave an error msg
static int type_cycles_dfs(TranslationUnit* restrict tu, Cuik_Type* type, uint8_t* visited, uint8_t* finished) {
    // non-record types are always finished :P
    if (type->kind != KIND_STRUCT && type->kind != KIND_UNION) {
        return 0;
    }

    // if (finished[o]) return false
    int o = type->ordinal;
    if (finished[o / 8] & (1u << (o % 8))) {
        return 0;
    }

    // if (visited[o]) return true
    if (visited[o / 8] & (1u << (o % 8))) {
        return 1;
    }

    // visited[o] = true
    visited[o / 8] |= (1u << (o % 8));

    // for each m in members
    //   if (dfs(m)) return true
    for (size_t i = 0; i < type->record.kid_count; i++) {
        int c = type_cycles_dfs(tu, type->record.kids[i].type, visited, finished);
        if (c) {
            // we already gave an error message, don't be redundant
            if (c != 2) {
                const char* name = type->record.name ? (const char*)type->record.name : "<unnamed>";

                char tmp[256];
                sprintf_s(tmp, sizeof(tmp), "type %s has cycles", name);

                report_two_spots(REPORT_ERROR, tu->errors,
                    &tu->tokens, type->loc, type->record.kids[i].loc,
                    tmp, NULL, NULL, "on");
            }

            return 2;
        }
    }

    // finished[o] = true
    finished[o / 8] |= (1u << (o % 8));
    return 0;
}

static void type_resolve_pending_align(TranslationUnit* restrict tu, Cuik_Type* type) {
    size_t pending_count = arrlen(pending_exprs);
    for (size_t i = 0; i < pending_count; i++) {
        if (pending_exprs[i].dst == &type->align) {
            assert(pending_exprs[i].mode == PENDING_ALIGNAS);

            TokenStream mini_lex = tu->tokens;
            mini_lex.current = pending_exprs[i].expr_pos;

            SourceLocIndex loc = tokens_get_location_index(&mini_lex);

            int align = 0;
            if (is_typename(&mini_lex)) {
                Cuik_Type* new_align = parse_typename(tu, &mini_lex);
                if (new_align == NULL || new_align->align) {
                    REPORT(ERROR, loc, "_Alignas cannot operate with incomplete");
                } else {
                    align = new_align->align;
                }
            } else {
                intmax_t new_align = parse_const_expr(tu, &mini_lex);
                if (new_align == 0) {
                    REPORT(ERROR, loc, "_Alignas cannot be applied with 0 alignment", new_align);
                } else if (new_align >= INT16_MAX) {
                    REPORT(ERROR, loc, "_Alignas(%zu) exceeds max alignment of %zu", new_align, INT16_MAX);
                } else {
                    align = new_align;
                }
            }

            assert(align != 0);
            type->align = align;
            return;
        }
    }

    abort();
}

void type_layout(TranslationUnit* restrict tu, Cuik_Type* type) {
    if (type->size != 0) return;
    if (type->is_inprogress) {
        REPORT(ERROR, type->loc, "Type has a circular dependency");
        abort();
    }

    type->is_inprogress = true;

    if (type->kind == KIND_ARRAY) {
        if (type->array_count_lexer_pos) {
            // run mini parser for array count
            TokenStream mini_lex = tu->tokens;
            mini_lex.current = type->array_count_lexer_pos;
            type->array_count = parse_const_expr(tu, &mini_lex);
            expect(&mini_lex, ']');
        }

        // layout crap
        if (type->array_count != 0) {
            if (type->array_of->size == 0) {
                type_layout(tu, type->array_of);
            }
            assert(type->array_of->size > 0);
        }

        uint64_t result = type->array_of->size * type->array_count;

        // size checks
        if (result >= INT32_MAX) {
            REPORT(ERROR, type->loc, "cannot declare an array that exceeds 0x7FFFFFFE bytes (got 0x%zX or %zi)", result, result);
            abort();
        }

        type->size = result;
        type->align = type->array_of->align;
    } else if (type->kind == KIND_ENUM) {
        int cursor = 0;

        for (int i = 0; i < type->enumerator.count; i++) {
            // if the value is undecided, best time to figure it out is now
            if (type->enumerator.entries[i].lexer_pos != 0) {
                // Spin up a mini expression parser here
                TokenStream mini_lex = tu->tokens;
                mini_lex.current = type->enumerator.entries[i].lexer_pos;

                cursor = parse_const_expr(tu, &mini_lex);
            }

            type->enumerator.entries[i].value = cursor;
            cursor += 1;
        }

        type->size = 4;
        type->align = 4;
        type->is_incomplete = false;
    } else if (type->kind == KIND_STRUCT || type->kind == KIND_UNION) {
        bool is_union = (type->kind == KIND_UNION);

        size_t member_count = type->record.kid_count;
        Member* members = type->record.kids;

        // for unions this just represents the max size
        int offset = 0;
        int last_member_size = 0;
        int current_bit_offset = 0;
        // struct/union are aligned to the biggest member alignment
        int align = 0;

        for (size_t i = 0; i < member_count; i++) {
            Member* member = &members[i];

            if (member->type->kind == KIND_FUNC) {
                REPORT(ERROR, type->loc, "Cannot put function types into a struct, try a function pointer");
            } else {
                type_layout(tu, member->type);
            }

            int member_align = member->type->align;
            int member_size = member->type->size;
            if (!is_union) {
                int new_offset = align_up(offset, member_align);

                // If we realign, reset the bit offset
                if (offset != new_offset) {
                    current_bit_offset = last_member_size = 0;
                }
                offset = new_offset;
            }

            member->offset = is_union ? 0 : offset;
            member->align = member_align;

            // bitfields
            if (member->is_bitfield) {
                int bit_width = member->bit_width;
                int bits_in_region = member->type->kind == KIND_BOOL ? 1 : (member_size * 8);
                if (bit_width > bits_in_region) {
                    REPORT(ERROR, type->loc, "Bitfield cannot fit in this type.");
                }

                if (current_bit_offset + bit_width > bits_in_region) {
                    current_bit_offset = 0;

                    offset = align_up(offset + member_size, member_align);
                    member->bit_offset = offset;
                }

                current_bit_offset += bit_width;
            } else {
                if (is_union) {
                    if (member_size > offset) offset = member_size;
                } else {
                    offset += member_size;
                }
            }

            // the total alignment of a struct/union is based on the biggest member
            last_member_size = member_size;
            if (member_align > align) align = member_align;
        }

        offset = align_up(offset, align);
        type->align = align;
        type->size = offset;
        type->is_incomplete = false;
    }

    type->is_inprogress = false;
}

CUIK_API TranslationUnit* cuik_parse_translation_unit(const Cuik_TranslationUnitDesc* restrict desc) {
    assert(desc->tokens != NULL);
    assert(desc->errors != NULL);
    memset(desc->errors, 0, sizeof(*desc->errors));

    // hacky but i don't wanna wrap it in a CUIK_TIMED_BLOCK
    uint64_t timer_start = cuik_time_in_nanos();

    TranslationUnit* tu = calloc(1, sizeof(TranslationUnit));
    tu->filepath = desc->tokens->filepath;
    tu->ir_mod = desc->ir_module;
    tu->is_windows_long = desc->target->sys == TB_SYSTEM_WINDOWS;
    tu->target = *desc->target;
    tu->tokens = *desc->tokens;
    tu->errors = desc->errors;

    tls_init();
    atoms_init();
    mtx_init(&tu->arena_mutex, mtx_plain);

    reset_global_parser_state();

    ////////////////////////////////
    // Parse translation unit
    ////////////////////////////////
    out_of_order_mode = true;
    DynArray(int) static_assertions = dyn_array_create(int);
    TokenStream* restrict s = desc->tokens;

    // Phase 1: resolve all top level statements
    CUIK_TIMED_BLOCK("phase 1") {
        while (tokens_get(s)->type) {
            while (tokens_get(s)->type == ';') tokens_next(s);

            // TODO(NeGate): Correctly parse pragmas instead of ignoring them.
            if (tokens_get(s)->type == TOKEN_KW_Pragma) {
                tokens_next(s);
                expect(s, '(');

                if (tokens_get(s)->type != TOKEN_STRING_DOUBLE_QUOTE) {
                    generic_error(s, "pragma declaration expects string literal");
                }
                tokens_next(s);

                expect(s, ')');
            } else if (tokens_get(s)->type == TOKEN_KW_Static_assert) {
                tokens_next(s);
                expect(s, '(');

                TknType terminator;
                size_t current = skip_expression_in_parens(s, &terminator);
                dyn_array_put(static_assertions, current);

                tokens_prev(s);
                if (tokens_get(s)->type == ',') {
                    tokens_next(s);

                    Token* t = tokens_get(s);
                    if (t->type != TOKEN_STRING_DOUBLE_QUOTE) {
                        generic_error(s, "static assertion expects string literal");
                    }
                    tokens_next(s);
                }

                expect(s, ')');
            } else {
                SourceLocIndex loc = tokens_get_location_index(s);

                // must be a declaration since it's a top level statement
                Attribs attr = {0};
                Cuik_Type* type = parse_declspec(tu, s, &attr);

                if (attr.is_typedef) {
                    // declarator (',' declarator)+ ';'
                    while (true) {
                        Decl decl = parse_declarator(tu, s, type, false, false);

                        if (decl.name != NULL) {
                            // make typedef
                            Stmt* n = make_stmt(tu, s, STMT_DECL, sizeof(struct StmtDecl));
                            n->loc = decl.loc;
                            n->decl = (struct StmtDecl){
                                .name = decl.name,
                                .type = decl.type,
                                .attrs = attr,
                            };

                            // typedefs can't be roots ngl
                            n->decl.attrs.is_root = false;
                            arrput(tu->top_level_stmts, n);

                            // check for collision
                            Symbol* search = find_global_symbol((const char*)decl.name);
                            if (search != NULL) {
                                if (search->storage_class != STORAGE_TYPEDEF) {
                                    report_two_spots(REPORT_ERROR, tu->errors, s, decl.loc, search->loc,
                                        "typedef overrides previous declaration.",
                                        "old", "new", NULL);
                                    abort();
                                }

                                Cuik_Type* placeholder_space = search->type;
                                if (placeholder_space->kind != KIND_PLACEHOLDER && !type_equal(tu, decl.type, search->type)) {
                                    report_two_spots(REPORT_ERROR, tu->errors, s, decl.loc, search->loc,
                                        "typedef overrides previous declaration.",
                                        "old", "new", NULL);
                                    abort();
                                }

                                // replace placeholder with actual entry
                                Atom old_name = decl.name;

                                memcpy(placeholder_space, decl.type, sizeof(Cuik_Type));
                                placeholder_space->also_known_as = old_name;
                                placeholder_space->loc = decl.loc;
                            } else {
                                // add new entry
                                Symbol sym = {
                                    .name = decl.name,
                                    .type = decl.type,
                                    .loc = decl.loc,
                                    .storage_class = STORAGE_TYPEDEF,
                                };
                                shput(global_symbols, decl.name, sym);
                            }
                        }

                        if (tokens_get(s)->type == 0) {
                            REPORT(ERROR, loc, "declaration list ended with EOF instead of semicolon.");
                            abort();
                        } else if (tokens_get(s)->type == '=') {
                            REPORT(ERROR, loc, "why did you just try that goofy shit wit me. You cannot assign a typedef.");

                            // error recovery
                        } else if (tokens_get(s)->type == ';') {
                            tokens_next(s);
                            break;
                        } else if (tokens_get(s)->type == ',') {
                            tokens_next(s);
                            continue;
                        }
                    }
                } else {
                    if (tokens_get(s)->type == ';') {
                        Stmt* n = make_stmt(tu, s, STMT_GLOBAL_DECL, sizeof(struct StmtDecl));
                        n->loc = loc;
                        n->decl = (struct StmtDecl){
                            .name = NULL,
                            .type = type,
                            .attrs = attr,
                        };
                        n->decl.attrs.is_root = true;
                        arrput(tu->top_level_stmts, n);

                        tokens_next(s);
                        continue;
                    }

                    // normal variable lists
                    // declarator (',' declarator )+ ';'
                    while (true) {
                        SourceLocIndex decl_loc = tokens_get_location_index(s);
                        Decl decl = parse_declarator(tu, s, type, false, false);
                        if (decl.name == NULL) {
                            REPORT(ERROR, decl_loc, "Declaration has no name");
                            abort();
                        }

                        Stmt* n = make_stmt(tu, s, STMT_GLOBAL_DECL, sizeof(struct StmtDecl));
                        n->loc = decl.loc;
                        n->decl = (struct StmtDecl){
                            .name = decl.name,
                            .type = decl.type,
                            .attrs = attr,
                        };
                        arrput(tu->top_level_stmts, n);

                        Symbol sym = {
                            .name = decl.name,
                            .type = decl.type,
                            .loc = decl.loc,
                            .stmt = n,
                        };

                        Symbol* old_definition = find_global_symbol((const char*)decl.name);
                        if (decl.type->kind == KIND_FUNC) {
                            sym.storage_class = (attr.is_static ? STORAGE_STATIC_FUNC : STORAGE_FUNC);
                        } else {
                            sym.storage_class = (attr.is_static ? STORAGE_STATIC_VAR : STORAGE_GLOBAL);
                        }

                        if (sym.name[0] == 'm' && strcmp((char*) sym.name, "main") == 0) {
                            tu->entrypoint_status = CUIK_ENTRYPOINT_MAIN;
                        } else if (sym.name[0] == 'W' && strcmp((char*) sym.name, "WinMain") == 0) {
                            tu->entrypoint_status = CUIK_ENTRYPOINT_WINMAIN;
                        }

                        // parse attributes... currently it doesn't but one day...
                        while (parse_attributes(tu, s, n)) {}

                        bool requires_terminator = true;
                        if (tokens_get(s)->type == '=') {
                            tokens_next(s);

                            // variables with definitions can be roots
                            n->decl.attrs.is_root = !(attr.is_static || attr.is_inline);

                            if (tokens_get(s)->type == '{') {
                                sym.current = s->current;
                                sym.terminator = '}';

                                tokens_next(s);

                                int depth = 1;
                                while (depth) {
                                    Token* t = tokens_get(s);

                                    if (t->type == '\0') {
                                        REPORT(ERROR, decl.loc, "Declaration ended in EOF");
                                        abort();
                                    } else if (t->type == '{') {
                                        depth++;
                                    } else if (t->type == '}') {
                                        if (depth == 0) {
                                            REPORT(ERROR, decl.loc, "Unbalanced brackets");
                                            abort();
                                        }

                                        depth--;
                                    }

                                    tokens_next(s);
                                }
                            } else {
                                // '=' EXPRESSION ','
                                // '=' EXPRESSION ';'
                                sym.current = s->current;
                                sym.terminator = ';';

                                int depth = 1;
                                while (depth) {
                                    Token* t = tokens_get(s);

                                    if (t->type == '\0') {
                                        REPORT(ERROR, decl.loc, "Declaration ended in EOF");
                                        abort();
                                    } else if (t->type == '(') {
                                        depth++;
                                    } else if (t->type == ')') {
                                        depth--;

                                        if (depth == 0) {
                                            REPORT(ERROR, decl.loc, "Unbalanced parenthesis");
                                            abort();
                                        }
                                    } else if (t->type == ';' || t->type == ',') {
                                        if (depth > 1 && t->type == ';') {
                                            REPORT(ERROR, decl.loc, "Declaration's expression has a weird semicolon");
                                            abort();
                                        } else if (depth == 1) {
                                            sym.terminator = t->type;
                                            depth--;
                                        }
                                    }

                                    tokens_next(s);
                                }

                                // we ate the terminator but the code right below it
                                // does need to know what it is...
                                tokens_prev(s);
                            }
                        } else if (tokens_get(s)->type == '{') {
                            // function bodies dont end in semicolon or comma, it just terminates
                            // the declaration list
                            requires_terminator = false;

                            if (decl.type->kind != KIND_FUNC) {
                                REPORT(ERROR, decl.loc, "Somehow parsing a function body... on a non-function type?");
                                abort();
                            }

                            if (old_definition && old_definition->current != 0) {
                                report_two_spots(REPORT_ERROR, tu->errors, s, decl.loc, old_definition->stmt->loc,
                                    "Cannot redefine function declaration",
                                    NULL, NULL, "previous definition was:");
                                abort();
                            }

                            //n->decl.type = decl.type;
                            //n->decl.attrs = attr;
                            n->decl.attrs.is_root = !(attr.is_static || attr.is_inline);

                            sym.terminator = '}';
                            sym.current = s->current;
                            tokens_next(s);

                            // we postpone parsing the function bodies
                            // balance some brackets: '{' SOMETHING '}'
                            int depth = 1;
                            while (depth) {
                                Token* t = tokens_get(s);

                                if (t->type == '\0') {
                                    SourceLocIndex l = tokens_get_last_location_index(s);
                                    report_fix(REPORT_ERROR, tu->errors, s, l, "}", "Function body ended in EOF");
                                    abort();
                                } else if (t->type == '{') {
                                    depth++;
                                } else if (t->type == '}') {
                                    depth--;
                                }

                                tokens_next(s);
                            }
                        }

                        if (decl.name != NULL) {
                            // slap that bad boy into the symbol table
                            shput(global_symbols, decl.name, sym);
                        }

                        if (!requires_terminator) {
                            // function bodies just end the declaration list
                            break;
                        }

                        if (tokens_get(s)->type == 0) {
                            REPORT(ERROR, loc, "declaration list ended with EOF instead of semicolon.");
                            abort();
                        } else if (tokens_get(s)->type == ';') {
                            tokens_next(s);
                            break;
                        } else if (tokens_get(s)->type == ',') {
                            tokens_next(s);
                            continue;
                        }
                    }
                }
            }
        }
    }
    out_of_order_mode = false;

    // Phase 2: resolve top level types, layout records and anything else so that
    // we have a complete global symbol table
    CUIK_TIMED_BLOCK("phase 2") {
        ////////////////////////////////
        // first we wanna check for cycles
        ////////////////////////////////
        size_t type_count = 0;
        for (ArenaSegment* a = tu->type_arena.base; a != NULL; a = a->next) {
            for (size_t used = 0; used < a->used; used += sizeof(Cuik_Type)) {
                Cuik_Type* type = (Cuik_Type*)&a->data[used];
                if (type->kind == KIND_STRUCT || type->kind == KIND_UNION) {
                    type->ordinal = type_count++;
                } else if (type->kind == KIND_PLACEHOLDER) {
                    REPORT(ERROR, type->loc, "could not find type '%s'!", type->placeholder.name);
                }
            }
        }

        if (has_reports(REPORT_ERROR, tu->errors)) goto parse_error;

        // bitvectors amirite
        size_t bitvec_bytes = (type_count + 7) / 8;
        uint8_t* visited = tls_push(bitvec_bytes);
        uint8_t* finished = tls_push(bitvec_bytes);

        memset(visited, 0, bitvec_bytes);
        memset(finished, 0, bitvec_bytes);

        // for each type, check for cycles
        for (ArenaSegment* a = tu->type_arena.base; a != NULL; a = a->next) {
            for (size_t used = 0; used < a->used; used += sizeof(Cuik_Type)) {
                Cuik_Type* type = (Cuik_Type*)&a->data[used];

                if (type->kind == KIND_STRUCT || type->kind == KIND_UNION) {
                    // if cycles... quit lmao
                    if (type_cycles_dfs(tu, type, visited, finished)) goto fuck_outta_there;
                }
            }
        }

        fuck_outta_there:
        if (has_reports(REPORT_ERROR, tu->errors)) goto parse_error;

        // parse all global declarations
        for (size_t i = 0, count = shlen(global_symbols); i < count; i++) {
            Symbol* sym = &global_symbols[i].value;

            if (sym->current != 0 &&
                (sym->storage_class == STORAGE_STATIC_VAR || sym->storage_class == STORAGE_GLOBAL)) {
                // Spin up a mini parser here
                TokenStream mini_lex = *s;
                mini_lex.current = sym->current;

                // intitialize use list
                symbol_chain_start = symbol_chain_current = NULL;

                Expr* e;
                if (tokens_get(&mini_lex)->type == '@') {
                    // function literals are a Cuik extension
                    // TODO(NeGate): error messages
                    tokens_next(&mini_lex);

                    e = parse_function_literal(tu, &mini_lex, sym->type);
                } else if (tokens_get(&mini_lex)->type == '{') {
                    tokens_next(&mini_lex);

                    e = parse_initializer(tu, &mini_lex, NULL);
                } else {
                    e = parse_expr_l14(tu, &mini_lex);
                    expect(&mini_lex, sym->terminator);
                }

                sym->stmt->decl.initial = e;

                // finalize use list
                sym->stmt->decl.first_symbol = symbol_chain_start;
            }
        }

        if (has_reports(REPORT_ERROR, tu->errors)) goto parse_error;

        // do record layouts and shi
        for (ArenaSegment* a = tu->type_arena.base; a != NULL; a = a->next) {
            for (size_t used = 0; used < a->used; used += sizeof(Cuik_Type)) {
                Cuik_Type* type = (Cuik_Type*)&a->data[used];

                if (type->align == -1) {
                    // this means it's got a pending expression for an alignment
                    type_resolve_pending_align(tu, type);
                }

                if (type->size == 0) type_layout(tu, type);
            }
        }

        if (has_reports(REPORT_ERROR, tu->errors)) goto parse_error;
        arrfree(pending_exprs);

        ////////////////////////////////
        // Resolve any static assertions
        ////////////////////////////////
        for (size_t i = 0, count = dyn_array_length(static_assertions); i < count; i++) {
            // Spin up a mini expression parser here
            size_t current_lex_pos = static_assertions[i];

            TokenStream mini_lex = *s;
            mini_lex.current = current_lex_pos;

            intmax_t condition = parse_const_expr(tu, &mini_lex);
            if (tokens_get(&mini_lex)->type == ',') {
                tokens_next(&mini_lex);

                Token* t = tokens_get(&mini_lex);
                if (t->type != TOKEN_STRING_DOUBLE_QUOTE) {
                    generic_error(&mini_lex, "static assertion expects string literal");
                }
                tokens_next(&mini_lex);

                if (condition == 0) {
                    REPORT(ERROR, tokens_get_location_index(&mini_lex), "Static assertion failed: %.*s", (int)(t->end - t->start), t->start);
                }
            } else {
                if (condition == 0) {
                    REPORT(ERROR, tokens_get_location_index(&mini_lex), "Static assertion failed");
                }
            }
        }

        // we don't need to keep it afterwards
        tls_restore(visited);
    }
    dyn_array_destroy(static_assertions);

    // Phase 3: resolve all expressions or function bodies
    // This part is parallel because im the fucking GOAT
    CUIK_TIMED_BLOCK("phase 3") {
        // append any AST nodes we might've created in this thread
        arena_trim(&local_ast_arena);
        arena_append(&tu->ast_arena, &local_ast_arena);
        local_ast_arena = (Arena){0};

        if (desc->thread_pool != NULL) {
            // disabled until we change the tables to arenas
            size_t count = shlen(global_symbols);
            size_t padded = (count + (PARSE_MUNCH_SIZE - 1)) & ~(PARSE_MUNCH_SIZE - 1);

            // passed to the threads to identify when things are done
            atomic_size_t tasks_remaining = (count + (PARSE_MUNCH_SIZE - 1)) / PARSE_MUNCH_SIZE;
            ParserTaskInfo* tasks = malloc(sizeof(ParserTaskInfo) * tasks_remaining);

            size_t j = 0;
            for (size_t i = 0; i < padded; i += PARSE_MUNCH_SIZE) {
                size_t limit = i + PARSE_MUNCH_SIZE;
                if (limit > count) limit = count;

                ParserTaskInfo* task = &tasks[j++];
                *task = (ParserTaskInfo){
                    .tasks_remaining = &tasks_remaining,
                    .start = i,
                    .end = limit
                };

                // we transfer a bunch of our thread local state to the task
                // and they'll use that to continue and build up parse on other
                // threads.
                task->tu = tu;
                task->global_tags = global_tags;
                task->global_symbols = global_symbols;
                task->base_token_stream = s;

                CUIK_CALL(desc->thread_pool, submit, phase3_parse_task, task);
            }

            while (tasks_remaining != 0) {
                CUIK_CALL(desc->thread_pool, work_one_job);
                thrd_yield();
            }

            free(tasks);
        } else {
            // single threaded mode
            parse_global_symbols(tu, 0, shlen(global_symbols), *s);
        }

        if (has_reports(REPORT_ERROR, tu->errors)) goto parse_error;

        // check for any qualified types and resolve them correctly
        for (ArenaSegment* a = tu->type_arena.base; a != NULL; a = a->next) {
            for (size_t used = 0; used < a->used; used += sizeof(Cuik_Type)) {
                Cuik_Type* type = (Cuik_Type*)&a->data[used];

                if (type->kind == KIND_QUALIFIED_TYPE) {
                    bool is_atomic = type->is_atomic;
                    bool is_const = type->is_const;
                    int align = type->align;

                    // copy and replace the qualifier slots
                    memcpy(type, type->qualified_ty, sizeof(Cuik_Type));
                    type->align = align;
                    type->is_const = is_const;
                    type->is_atomic = is_atomic;
                }
            }
        }
    }

    //printf("AST arena: %zu MB\n", arena_get_memory_usage(&tu->ast_arena) / (1024*1024));
    //printf("Type arena: %zu MB\n", arena_get_memory_usage(&tu->type_arena) / (1024*1024));
    //printf("Thread arena: %zu MB\n", arena_get_memory_usage(&thread_arena) / (1024*1024));

    CUIK_TIMED_BLOCK("freeing parser internals") {
        current_switch_or_case = current_breakable = current_continuable = 0;
        local_symbol_start = local_symbol_count = 0;

        // free parser crap
        free(local_tags);
        free(local_symbols);
        shfree(global_tags);
        shfree(global_symbols);

        // free tokens
        arrfree(tu->tokens.tokens);
    }

    // run type checker
    CUIK_TIMED_BLOCK("phase 4") {
        cuik__sema_pass(tu, desc->thread_pool);
        if (has_reports(REPORT_ERROR, tu->errors)) goto parse_error;
    }

    {
        char temp[256];
        snprintf(temp, sizeof(temp), "parse: %s", tu->filepath);
        temp[sizeof(temp) - 1] = 0;

        char* p = temp;
        for (; *p; p++) {
            if (*p == '\\') *p = '/';
        }

        cuik_profile_region(timer_start, temp);
    }

    return tu;

    parse_error:
    // TODO(NeGate): free all translation unit resources because we failed :(
    return NULL;
}

CUIK_API void cuik_destroy_translation_unit(TranslationUnit* restrict tu) {
    arrfree(tu->top_level_stmts);

    arena_free(&tu->ast_arena);
    arena_free(&tu->type_arena);
    mtx_destroy(&tu->arena_mutex);
    free(tu);
}

CUIK_API TranslationUnit* cuik_next_translation_unit(TranslationUnit* restrict tu) {
    return tu->next;
}

CUIK_API bool cuik_is_in_main_file(TranslationUnit* restrict tu, SourceLocIndex loc) {
    if (SOURCE_LOC_GET_TYPE(loc) == SOURCE_LOC_UNKNOWN) {
        return false;
    }

    SourceLoc* l = &tu->tokens.locations[SOURCE_LOC_GET_DATA(loc)];
    return l->line->filepath == tu->filepath;
}

Stmt* resolve_unknown_symbol(TranslationUnit* tu, Expr* e) {
    Symbol* sym = find_global_symbol((char*)e->unknown_sym);
    if (sym != NULL) return 0;

    // Parameters are local and a special case how tf
    assert(sym->storage_class != STORAGE_PARAM);

    e->op = EXPR_SYMBOL;
    e->symbol = sym->stmt;
    return sym->stmt;
}

static Symbol* find_local_symbol(TokenStream* restrict s) {
    Token* t = tokens_get(s);
    const unsigned char* name = t->start;
    size_t length = t->end - t->start;

    // Try local variables
    size_t i = local_symbol_count;
    size_t start = local_symbol_start;
    while (i-- > start) {
        const unsigned char* sym = local_symbols[i].name;
        size_t sym_length = strlen((const char*)sym);

        if (sym_length == length && memcmp(name, sym, length) == 0) {
            return &local_symbols[i];
        }
    }

    return NULL;
}

////////////////////////////////
// STATEMENTS
////////////////////////////////
static void parse_function_definition(TranslationUnit* tu, TokenStream* restrict s, Stmt* n) {
    Cuik_Type* type = n->decl.type;

    Param* param_list = type->func.param_list;
    size_t param_count = type->func.param_count;

    assert(local_symbol_start == local_symbol_count);
    if (param_count >= INT16_MAX) {
        REPORT(ERROR, n->loc, "Function parameter count cannot exceed %d (got %d)", param_count, MAX_LOCAL_SYMBOLS);
        abort();
    }

    for (size_t i = 0; i < param_count; i++) {
        Param* p = &param_list[i];

        if (p->name) {
            local_symbols[local_symbol_count++] = (Symbol){
                .name = p->name,
                .type = p->type,
                .storage_class = STORAGE_PARAM,
                .param_num = i};
        }
    }

    // skip {
    tokens_next(s);

    Stmt* body = parse_compound_stmt(tu, s);
    assert(body != NULL);

    n->op = STMT_FUNC_DECL;
    n->decl.initial_as_stmt = body;

    // hmm... how tf do labels operate in this case...
    // TODO(NeGate): redo the label look in a sec
    shfree(labels);
}

static Stmt* parse_compound_stmt(TranslationUnit* tu, TokenStream* restrict s) {
    Stmt* node = NULL;

    LOCAL_SCOPE {
        node = make_stmt(tu, s, STMT_COMPOUND, sizeof(struct StmtCompound));

        size_t kid_count = 0;
        Stmt** kids = tls_save();

        while (tokens_get(s)->type != '}') {
            if (tokens_get(s)->type == ';') {
                tokens_next(s);
            } else {
                Stmt* stmt = parse_stmt(tu, s);
                if (stmt) {
                    tls_push(sizeof(Stmt*));
                    kids[kid_count++] = stmt;
                } else {
                    // this will push the decl or expression if it catches one
                    parse_decl_or_expr(tu, s, &kid_count);
                }
            }
        }
        expect(s, '}');

        Stmt** permanent_storage = arena_alloc(&thread_arena, kid_count * sizeof(Stmt*), _Alignof(Stmt*));
        memcpy(permanent_storage, kids, kid_count * sizeof(Stmt*));

        node->compound = (struct StmtCompound){
            .kids = permanent_storage,
            .kids_count = kid_count,
        };

        tls_restore(kids);
    }

    return node;
}

// TODO(NeGate): Doesn't handle declarators or expression-statements
static Stmt* parse_stmt(TranslationUnit* tu, TokenStream* restrict s) {
    TknType peek = tokens_get(s)->type;

    if (peek == '{') {
        tokens_next(s);
        return parse_compound_stmt(tu, s);
    } else if (peek == TOKEN_KW_return) {
        tokens_next(s);

        Expr* e = 0;
        if (tokens_get(s)->type != ';') {
            e = parse_expr(tu, s);
        }

        Stmt* n = make_stmt(tu, s, STMT_RETURN, sizeof(struct StmtReturn));
        n->return_ = (struct StmtReturn){
            .expr = e};

        expect_with_reason(s, ';', "return");
        return n;
    } else if (peek == TOKEN_KW_if) {
        tokens_next(s);
        Stmt* n = make_stmt(tu, s, STMT_IF, sizeof(struct StmtIf));

        LOCAL_SCOPE {
            Expr* cond;
            {
                SourceLocIndex opening_loc = tokens_get_location_index(s);
                expect(s, '(');

                cond = parse_expr(tu, s);

                expect_closing_paren(tu, s, opening_loc);
            }

            Stmt* body;
            LOCAL_SCOPE {
                body = parse_stmt_or_expr(tu, s);
            }

            Stmt* next = 0;
            if (tokens_get(s)->type == TOKEN_KW_else) {
                tokens_next(s);

                LOCAL_SCOPE {
                    next = parse_stmt_or_expr(tu, s);
                }
            }

            n->if_ = (struct StmtIf){
                .cond = cond,
                .body = body,
                .next = next};
        }

        return n;
    } else if (peek == TOKEN_KW_switch) {
        tokens_next(s);
        Stmt* n = make_stmt(tu, s, STMT_SWITCH, sizeof(struct StmtSwitch));

        LOCAL_SCOPE {
            expect(s, '(');
            Expr* cond = parse_expr(tu, s);
            expect(s, ')');

            n->switch_ = (struct StmtSwitch){
                .condition = cond};

            // begin a new chain but keep the old one
            Stmt* old_switch = current_switch_or_case;
            current_switch_or_case = n;

            Stmt* old_breakable = current_breakable;
            current_breakable = n;
            LOCAL_SCOPE {
                Stmt* body = parse_stmt_or_expr(tu, s);
                n->switch_.body = body;
            }
            current_breakable = old_breakable;
            current_switch_or_case = old_switch;
        }
        return n;
    } else if (peek == TOKEN_KW_case) {
        // TODO(NeGate): error messages
        assert(current_switch_or_case);

        tokens_next(s);
        Stmt* n = make_stmt(tu, s, STMT_CASE, sizeof(struct StmtCase));
        Stmt* top = n;

        intmax_t key = parse_const_expr(tu, s);
        if (tokens_get(s)->type == TOKEN_TRIPLE_DOT) {
            // GNU extension, case ranges
            tokens_next(s);
            intmax_t key_max = parse_const_expr(tu, s);
            expect(s, ':');

            assert(key_max > key);
            n->case_.key = key;

            for (intmax_t i = key; i < key_max; i++) {
                Stmt* curr = make_stmt(tu, s, STMT_CASE, sizeof(struct StmtCase));
                curr->case_ = (struct StmtCase){.key = i + 1};

                // Append to list
                n->case_.next = n->case_.body = curr;
                n = curr;
            }
        } else {
            expect(s, ':');

            n->case_ = (struct StmtCase){
                .key = key, .body = 0, .next = 0};
        }

        switch (current_switch_or_case->op) {
            case STMT_CASE:
            current_switch_or_case->case_.next = top;
            break;
            case STMT_DEFAULT:
            current_switch_or_case->default_.next = top;
            break;
            case STMT_SWITCH:
            current_switch_or_case->switch_.next = top;
            break;
            default:
            abort();
        }
        current_switch_or_case = n;

        Stmt* body = parse_stmt_or_expr(tu, s);
        n->case_.body = body;
        return top;
    } else if (peek == TOKEN_KW_default) {
        // TODO(NeGate): error messages
        assert(current_switch_or_case);

        tokens_next(s);
        Stmt* n = make_stmt(tu, s, STMT_DEFAULT, sizeof(struct StmtDefault));

        switch (current_switch_or_case->op) {
            case STMT_CASE:
            current_switch_or_case->case_.next = n;
            break;
            case STMT_DEFAULT:
            current_switch_or_case->default_.next = n;
            break;
            case STMT_SWITCH:
            current_switch_or_case->switch_.next = n;
            break;
            default:
            abort();
        }
        current_switch_or_case = n;
        expect(s, ':');

        n->default_ = (struct StmtDefault){
            .body = 0, .next = 0,
        };

        Stmt* body = parse_stmt_or_expr(tu, s);
        n->default_.body = body;
        return n;
    } else if (peek == TOKEN_KW_break) {
        // TODO(NeGate): error messages
        assert(current_breakable);

        tokens_next(s);
        expect(s, ';');

        Stmt* n = make_stmt(tu, s, STMT_BREAK, sizeof(struct StmtBreak));
        n->break_ = (struct StmtBreak){
            .target = current_breakable,
        };
        return n;
    } else if (peek == TOKEN_KW_continue) {
        // TODO(NeGate): error messages
        assert(current_continuable);

        tokens_next(s);
        expect(s, ';');

        Stmt* n = make_stmt(tu, s, STMT_CONTINUE, sizeof(struct StmtContinue));
        n->continue_ = (struct StmtContinue){
            .target = current_continuable,
        };
        return n;
    } else if (peek == TOKEN_KW_while) {
        tokens_next(s);
        Stmt* n = make_stmt(tu, s, STMT_WHILE, sizeof(struct StmtWhile));

        LOCAL_SCOPE {
            expect(s, '(');
            Expr* cond = parse_expr(tu, s);
            expect(s, ')');

            // Push this as a breakable statement
            Stmt* body;
            LOCAL_SCOPE {
                Stmt* old_breakable = current_breakable;
                current_breakable = n;
                Stmt* old_continuable = current_continuable;
                current_continuable = n;

                body = parse_stmt_or_expr(tu, s);

                current_breakable = old_breakable;
                current_continuable = old_continuable;
            }

            n->while_ = (struct StmtWhile){
                .cond = cond,
                .body = body,
            };
        }

        return n;
    } else if (peek == TOKEN_KW_for) {
        tokens_next(s);
        Stmt* n = make_stmt(tu, s, STMT_FOR, sizeof(struct StmtFor));

        LOCAL_SCOPE {
            expect(s, '(');

            // it's either nothing, a declaration, or an expression
            Stmt* first = NULL;
            if (tokens_get(s)->type == ';') {
                /* nothing */
                tokens_next(s);
            } else {
                // NOTE(NeGate): This is just a decl list or a single expression.
                first = make_stmt(tu, s, STMT_COMPOUND, sizeof(struct StmtCompound));

                size_t kid_count = 0;
                Stmt** kids = tls_save();
                {
                    parse_decl_or_expr(tu, s, &kid_count);
                }
                Stmt** permanent_storage = arena_alloc(&thread_arena, kid_count * sizeof(Stmt*), _Alignof(Stmt*));
                memcpy(permanent_storage, kids, kid_count * sizeof(Stmt*));

                first->compound = (struct StmtCompound){
                    .kids = permanent_storage,
                    .kids_count = kid_count,
                };
                tls_restore(kids);
            }

            Expr* cond = NULL;
            if (tokens_get(s)->type == ';') {
                /* nothing */
                tokens_next(s);
            } else {
                cond = parse_expr(tu, s);
                expect(s, ';');
            }

            Expr* next = NULL;
            if (tokens_get(s)->type == ')') {
                /* nothing */
                tokens_next(s);
            } else {
                next = parse_expr(tu, s);
                expect(s, ')');
            }

            // Push this as a breakable statement
            Stmt* body;
            LOCAL_SCOPE {
                Stmt* old_breakable = current_breakable;
                current_breakable = n;
                Stmt* old_continuable = current_continuable;
                current_continuable = n;

                body = parse_stmt_or_expr(tu, s);

                current_breakable = old_breakable;
                current_continuable = old_continuable;
            }

            n->for_ = (struct StmtFor){
                .first = first,
                .cond = cond,
                .body = body,
                .next = next,
            };
        }

        return n;
    } else if (peek == TOKEN_KW_do) {
        tokens_next(s);
        Stmt* n = make_stmt(tu, s, STMT_DO_WHILE, sizeof(struct StmtDoWhile));

        // Push this as a breakable statement
        LOCAL_SCOPE {
            Stmt* body;
            LOCAL_SCOPE {
                Stmt* old_breakable = current_breakable;
                current_breakable = n;
                Stmt* old_continuable = current_continuable;
                current_continuable = n;

                body = parse_stmt_or_expr(tu, s);

                current_breakable = old_breakable;
                current_continuable = old_continuable;
            }

            if (tokens_get(s)->type != TOKEN_KW_while) {
                Token* t = tokens_get(s);

                REPORT(ERROR, t->location, "%s:%d: error: expected 'while' got '%.*s'", (int)(t->end - t->start), t->start);
                abort();
            }
            tokens_next(s);

            expect(s, '(');

            Expr* cond = parse_expr(tu, s);

            expect(s, ')');
            expect(s, ';');

            n->do_while = (struct StmtDoWhile){
                .cond = cond,
                .body = body,
            };
        }

        return n;
    } else if (peek == TOKEN_KW_goto) {
        tokens_next(s);
        Stmt* n = make_stmt(tu, s, STMT_GOTO, sizeof(struct StmtGoto));

        // read label name
        Token* t = tokens_get(s);
        SourceLocIndex loc = t->location;
        if (t->type != TOKEN_IDENTIFIER) {
            REPORT(ERROR, loc, "expected identifier for goto target name");
            return n;
        }

        Atom name = atoms_put(t->end - t->start, t->start);

        // skip to the semicolon
        tokens_next(s);

        Expr* target = make_expr(tu);
        ptrdiff_t search = shgeti(labels, name);
        if (search >= 0) {
            *target = (Expr){
                .op = EXPR_SYMBOL,
                .start_loc = loc,
                .end_loc = loc,
                .symbol = labels[search].value,
            };
        } else {
            // not defined yet, make a placeholder
            Stmt* label_decl = make_stmt(tu, s, STMT_LABEL, sizeof(struct StmtLabel));
            label_decl->label = (struct StmtLabel){
                .name = name};
            shput(labels, name, label_decl);

            *target = (Expr){
                .op = EXPR_SYMBOL,
                .start_loc = loc,
                .end_loc = loc,
                .symbol = label_decl,
            };
        }

        n->goto_ = (struct StmtGoto){
            .target = target,
        };

        expect(s, ';');
        return n;
    } else if (peek == TOKEN_IDENTIFIER && tokens_peek(s)->type == TOKEN_COLON) {
        // label amirite
        // IDENTIFIER COLON STMT
        Token* t = tokens_get(s);
        Atom name = atoms_put(t->end - t->start, t->start);

        Stmt* n = NULL;
        ptrdiff_t search = shgeti(labels, name);
        if (search >= 0) {
            n = labels[search].value;
        } else {
            n = make_stmt(tu, s, STMT_LABEL, sizeof(struct StmtLabel));
            n->label = (struct StmtLabel){
                .name = name,
            };
            shput(labels, name, n);
        }

        n->label.placed = true;

        tokens_next(s);
        tokens_next(s);
        return n;
    } else {
        return 0;
    }
}

static void parse_decl_or_expr(TranslationUnit* tu, TokenStream* restrict s, size_t* body_count) {
    if (tokens_get(s)->type == TOKEN_KW_Pragma) {
        tokens_next(s);
        expect(s, '(');

        if (tokens_get(s)->type != TOKEN_STRING_DOUBLE_QUOTE) {
            generic_error(s, "pragma declaration expects string literal");
        }
        tokens_next(s);

        expect(s, ')');
    } else if (tokens_get(s)->type == ';') {
        tokens_next(s);
    } else if (is_typename(s)) {
        Attribs attr = {0};
        Cuik_Type* type = parse_declspec(tu, s, &attr);

        if (attr.is_typedef) {
            // don't expect one the first time
            bool expect_comma = false;
            while (tokens_get(s)->type != ';') {
                if (expect_comma) {
                    expect_with_reason(s, ',', "typedef");
                } else
                    expect_comma = true;

                Decl decl = parse_declarator(tu, s, type, false, false);
                assert(decl.name);

                // make typedef
                Stmt* n = make_stmt(tu, s, STMT_DECL, sizeof(struct StmtDecl));
                n->loc = decl.loc;
                n->decl = (struct StmtDecl){
                    .name = decl.name,
                    .type = decl.type,
                    .attrs = attr,
                };

                if (local_symbol_count >= MAX_LOCAL_SYMBOLS) {
                    REPORT(ERROR, decl.loc, "Local symbol count exceeds %d (got %d)", MAX_LOCAL_SYMBOLS, local_symbol_count);
                    abort();
                }

                local_symbols[local_symbol_count++] = (Symbol){
                    .name = decl.name,
                    .type = decl.type,
                    .storage_class = STORAGE_TYPEDEF,
                };
            }

            expect(s, ';');
        } else {
            // TODO(NeGate): Kinda ugly
            // don't expect one the first time
            bool expect_comma = false;
            while (tokens_get(s)->type != ';') {
                if (expect_comma) {
                    if (tokens_get(s)->type == '{') {
                        generic_error(s, "nested functions are not allowed... yet");
                    } else if (tokens_get(s)->type != ',') {
				        SourceLocIndex loc = tokens_get_last_location_index(s);

				        report_fix(REPORT_ERROR, tu->errors, s, loc, ";", "expected semicolon at the end of declaration");
                    }

                    tokens_next(s);
                } else {
                    expect_comma = true;
                }

                Decl decl = parse_declarator(tu, s, type, false, false);

                Stmt* n = make_stmt(tu, s, STMT_DECL, sizeof(struct StmtDecl));
                n->loc = decl.loc;
                n->decl = (struct StmtDecl){
                    .type = decl.type,
                    .name = decl.name,
                    .attrs = attr,
                    .initial = 0,
                };

                if (local_symbol_count >= MAX_LOCAL_SYMBOLS) {
                    REPORT(ERROR, decl.loc, "Local symbol count exceeds %d (got %d)", MAX_LOCAL_SYMBOLS, local_symbol_count);
                    abort();
                }

                local_symbols[local_symbol_count++] = (Symbol){
                    .name = decl.name,
                    .type = decl.type,
                    .storage_class = STORAGE_LOCAL,
                    .stmt = n,
                };

                Expr* initial = 0;
                if (tokens_get(s)->type == '=') {
                    tokens_next(s);

                    if (tokens_get(s)->type == '@') {
                        // function literals are a Cuik extension
                        // TODO(NeGate): error messages
                        tokens_next(s);
                        initial = parse_function_literal(tu, s, decl.type);
                    } else if (tokens_get(s)->type == '{') {
                        tokens_next(s);

                        initial = parse_initializer(tu, s, NULL);
                    } else {
                        initial = parse_expr_l14(tu, s);
                    }
                }

                n->decl.initial = initial;
                *((Stmt**)tls_push(sizeof(Stmt*))) = n;
                *body_count += 1;
            }

            expect(s, ';');
        }
    } else {
        Stmt* n = make_stmt(tu, s, STMT_EXPR, sizeof(struct StmtExpr));
        Expr* expr = parse_expr(tu, s);

        n->expr = (struct StmtExpr){ .expr = expr };

        *((Stmt**)tls_push(sizeof(Stmt*))) = n;
        *body_count += 1;
        expect(s, ';');
    }
}

static Stmt* parse_stmt_or_expr(TranslationUnit* tu, TokenStream* restrict s) {
    if (tokens_get(s)->type == TOKEN_KW_Pragma) {
        tokens_next(s);
        expect(s, '(');

        if (tokens_get(s)->type != TOKEN_STRING_DOUBLE_QUOTE) {
            generic_error(s, "pragma declaration expects string literal");
        }
        tokens_next(s);

        expect(s, ')');
        return 0;
    } else if (tokens_get(s)->type == ';') {
        tokens_next(s);
        return 0;
    } else {
        Stmt* stmt = parse_stmt(tu, s);

        if (stmt) {
            return stmt;
        } else {
            Stmt* n = make_stmt(tu, s, STMT_EXPR, sizeof(struct StmtExpr));

            Expr* expr = parse_expr(tu, s);
            n->expr = (struct StmtExpr){ .expr = expr };

            expect(s, ';');
            return n;
        }
    }
}

static intmax_t parse_const_expr(TranslationUnit* tu, TokenStream* restrict s) {
    ConstValue v = const_eval(tu, parse_expr_l14(tu, s));
    intmax_t vi = v.signed_value;

    if (!v.is_signed && vi != v.unsigned_value) {
        generic_error(s, "Constant integer cannot be represented as signed integer.");
    }

    return vi;
}

////////////////////////////////
// ERRORS
////////////////////////////////
static _Noreturn void generic_error(TokenStream* restrict s, const char* msg) {
    SourceLocIndex loc = tokens_get_location_index(s);

    report(REPORT_ERROR, NULL, s, loc, msg);
    abort();
}

static void expect(TokenStream* restrict s, char ch) {
    if (tokens_get(s)->type != ch) {
        Token* t = tokens_get(s);
        SourceLocIndex loc = tokens_get_location_index(s);

        report(REPORT_ERROR, NULL, s, loc, "expected '%c' got '%.*s'", ch, (int)(t->end - t->start), t->start);
        abort();
    }

    tokens_next(s);
}

static void expect_closing_paren(TranslationUnit* tu, TokenStream* restrict s, SourceLocIndex opening) {
    if (tokens_get(s)->type != ')') {
        SourceLocIndex loc = tokens_get_location_index(s);

        report_two_spots(REPORT_ERROR, tu->errors, s, opening, loc,
            "expected closing parenthesis",
            "open", "close?", NULL
        );
        return;
    }

    tokens_next(s);
}

static void expect_with_reason(TokenStream* restrict s, char ch, const char* reason) {
    if (tokens_get(s)->type != ch) {
        SourceLocIndex loc = tokens_get_last_location_index(s);

        char fix[2] = { ch, '\0' };
        report_fix(REPORT_ERROR, NULL, s, loc, fix, "expected '%c' for %s", ch, reason);
        return;
    }

    tokens_next(s);
}
