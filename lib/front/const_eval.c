#include "parser.h"

#define unsigned_const(x) (ConstValue) { false, .unsigned_value = (x) }
#define signed_const(x) (ConstValue) { true, .signed_value = (x) }

// Const eval probably needs some rework...
Cuik_Type* sema_expr(TranslationUnit* tu, Expr* e);
Cuik_Type* sema_guess_type(TranslationUnit* tu, Stmt* restrict s);
Member* sema_traverse_members(TranslationUnit* tu, Cuik_Type* record_type, Atom name, uint32_t* out_offset);

// I've forsaken god by doing this, im sorry... it's ugly because it has to be i swear...
typedef struct {
    Cuik_Type* type;
    uint32_t offset;
} WalkMemberReturn;

// just returns the byte offset it traveled in these DOTs and ARROWs.
// the type we pass in is that of whatever's at the end of the DOT and ARROW chain
static WalkMemberReturn walk_member_accesses(TranslationUnit* tu, const Expr* e, Cuik_Type* type) {
    const Expr* base_expr = e->dot_arrow.base;

    WalkMemberReturn base = {0};
    if (base_expr->op != EXPR_ARROW && base_expr->op != EXPR_DOT) {
        // use that base type we've been patiently keeping around
        base = (WalkMemberReturn){type, 0};
    } else {
        base = walk_member_accesses(tu, e->dot_arrow.base, type);
    }

    uint32_t relative = 0;
    Member* member = sema_traverse_members(tu, base.type, e->dot_arrow.name, &relative);
    if (!member) abort();
    if (member->is_bitfield) abort();

    return (WalkMemberReturn){member->type, base.offset + relative};
}

bool const_eval_try_offsetof_hack(TranslationUnit* tu, const Expr* e, uint64_t* out) {
    // hacky but handles things like:
    //   &(((T*)0)->apple)
    //   sizeof(((T*)0).apple)
    if (e->op == EXPR_DOT || e->op == EXPR_ARROW) {
        Expr* base_e = e->dot_arrow.base;

        while (base_e->op == EXPR_ARROW ||
               base_e->op == EXPR_DOT) {
            // traverse any dot/arrow chains
            base_e = base_e->dot_arrow.base;
        }

        uint32_t offset = 0;
        Cuik_Type* record_type = NULL;
        Expr* arrow_base = base_e;
        if (arrow_base->op == EXPR_CAST) {
            record_type = arrow_base->cast.type;

            // dereference
            if (record_type->kind == KIND_PTR) {
                record_type = record_type->ptr_to;
            } else {
                abort();
            }

            if (record_type->kind != KIND_STRUCT && record_type->kind != KIND_UNION) {
                return false;
            }

            ConstValue pointer_value = const_eval(tu, arrow_base->cast.src);
            assert(pointer_value.unsigned_value < UINT32_MAX);
            offset += pointer_value.unsigned_value;
        } else {
            return false;
        }

        offset += walk_member_accesses(tu, e, record_type).offset;
        *out = offset;
        return true;
    }

    return false;
}

static ConstValue const_eval_bin_op(ExprOp op, ConstValue a, ConstValue b) {
    bool is_signed = a.is_signed | b.is_signed;

    switch (op) {
        case EXPR_PLUS:
        return (ConstValue){is_signed, .unsigned_value = a.unsigned_value + b.unsigned_value};
        case EXPR_MINUS:
        return (ConstValue){is_signed, .unsigned_value = a.unsigned_value - b.unsigned_value};
        case EXPR_TIMES:
        return (ConstValue){is_signed, .unsigned_value = a.unsigned_value * b.unsigned_value};
        case EXPR_SLASH:
        return (ConstValue){is_signed, .unsigned_value = a.unsigned_value / b.unsigned_value};
        case EXPR_AND:
        return (ConstValue){is_signed, .unsigned_value = a.unsigned_value & b.unsigned_value};
        case EXPR_OR:
        return (ConstValue){is_signed, .unsigned_value = a.unsigned_value | b.unsigned_value};

        case EXPR_SHL:
        return (ConstValue){is_signed, .unsigned_value = a.unsigned_value << b.unsigned_value};
        case EXPR_SHR:
        return (ConstValue){is_signed, .unsigned_value = a.unsigned_value >> b.unsigned_value};

        case EXPR_CMPEQ:
        return unsigned_const(a.unsigned_value == b.unsigned_value);
        case EXPR_CMPNE:
        return unsigned_const(a.unsigned_value != b.unsigned_value);

        case EXPR_CMPGE:
        if (is_signed)
            return unsigned_const(a.signed_value >= b.signed_value);
        else
            return unsigned_const(a.unsigned_value >= b.unsigned_value);

        case EXPR_CMPLE:
        if (is_signed)
            return unsigned_const(a.signed_value <= b.signed_value);
        else
            return unsigned_const(a.unsigned_value <= b.unsigned_value);

        case EXPR_CMPGT:
        if (is_signed)
            return unsigned_const(a.signed_value > b.signed_value);
        else
            return unsigned_const(a.unsigned_value > b.unsigned_value);

        case EXPR_CMPLT:
        if (is_signed)
            return unsigned_const(a.signed_value < b.signed_value);
        else
            return unsigned_const(a.unsigned_value < b.unsigned_value);

        default:
        __builtin_unreachable();
    }
}

// TODO(NeGate): Cuik_Type check this better
ConstValue const_eval(TranslationUnit* tu, const Expr* e) {
    switch (e->op) {
        case EXPR_INT: {
            switch (e->int_num.suffix) {
                case INT_SUFFIX_NONE:
                case INT_SUFFIX_L:
                case INT_SUFFIX_LL:
                return signed_const(e->int_num.num);

                case INT_SUFFIX_U:
                case INT_SUFFIX_UL:
                case INT_SUFFIX_ULL:
                return unsigned_const(e->int_num.num);

                default:
                __builtin_unreachable();
            }
        }

        case EXPR_ENUM: {
            /*if (e->type->is_incomplete) {
                    type_layout(tu, e->type);
                }*/

            return signed_const(*e->enum_val.num);
        }

        case EXPR_CHAR:
        case EXPR_WCHAR: {
            return signed_const(e->char_lit);
        }
        case EXPR_TERNARY: {
            ConstValue cond = const_eval(tu, e->ternary_op.left);
            if (cond.unsigned_value != 0) {
                return const_eval(tu, e->ternary_op.middle);
            } else {
                return const_eval(tu, e->ternary_op.right);
            }
        }

        case EXPR_PLUS:
        case EXPR_MINUS:
        case EXPR_TIMES:
        case EXPR_SLASH:
        case EXPR_AND:
        case EXPR_OR:
        case EXPR_SHL:
        case EXPR_SHR:
        case EXPR_CMPEQ:
        case EXPR_CMPNE:
        case EXPR_CMPGE:
        case EXPR_CMPLE:
        case EXPR_CMPGT:
        case EXPR_CMPLT: {
            ConstValue a = const_eval(tu, e->bin_op.right);

            ExprOp op = e->op;
            Expr* current = e->bin_op.left;
            if (current->op != op) {
                ConstValue b = const_eval(tu, current);
                return const_eval_bin_op(op, b, a);
            } else {
                // try tail calling
                do {
                    ConstValue b = const_eval(tu, current->bin_op.right);
                    a = const_eval_bin_op(op, b, a);

                    current = current->bin_op.left;
                } while (current->op == op);

                ConstValue b = const_eval(tu, current);
                return const_eval_bin_op(op, b, a);
            }
        }
        case EXPR_SIZEOF: {
            Cuik_Type* src = sema_expr(tu, e->x_of_expr.expr);
            if (src->size == 0) {
                type_layout(tu, src);
            }

            assert(src->size && "Something went wrong...");
            return unsigned_const(src->size);
        }
        case EXPR_SIZEOF_T: {
            if (e->x_of_type.type->size == 0) {
                type_layout(tu, e->x_of_type.type);
            }

            return unsigned_const(e->x_of_type.type->size);
        }
        case EXPR_ALIGNOF_T: {
            if (e->x_of_type.type->size == 0) {
                type_layout(tu, e->x_of_type.type);
            }

            return unsigned_const(e->x_of_type.type->align);
        }
        case EXPR_NEGATE: {
            ConstValue src = const_eval(tu, e->unary_op.src);
            return signed_const(-src.signed_value);
        }

        case EXPR_ADDR: {
            uint64_t dst;
            if (const_eval_try_offsetof_hack(tu, e->unary_op.src, &dst)) {
                return unsigned_const(dst);
            }
            break;
        }

        case EXPR_CAST: {
            return const_eval(tu, e->cast.src);
        }
        default:
        break;
    }

    REPORT(ERROR, e->start_loc, "Could not resolve as constant expression");
    abort();
}
