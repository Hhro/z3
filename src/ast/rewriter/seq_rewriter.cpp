/*++
Copyright (c) 2015 Microsoft Corporation

Module Name:

    seq_rewriter.cpp

Abstract:

    Basic rewriting rules for sequences constraints.

Author:

    Nikolaj Bjorner (nbjorner) 2015-12-5
    Murphy Berzish 2017-02-21

Notes:

--*/

#include "util/uint_set.h"
#include "ast/rewriter/seq_rewriter.h"
#include "ast/arith_decl_plugin.h"
#include "ast/array_decl_plugin.h"
#include "ast/ast_pp.h"
#include "ast/ast_ll_pp.h"
#include "ast/ast_util.h"
#include "ast/well_sorted.h"
#include "ast/rewriter/var_subst.h"
#include "ast/rewriter/bool_rewriter.h"
#include "ast/rewriter/seq_rewriter_params.hpp"
#include "math/automata/automaton.h"
#include "math/automata/symbolic_automata_def.h"


expr_ref sym_expr::accept(expr* e) {
    ast_manager& m = m_t.get_manager();
    expr_ref result(m);
    var_subst subst(m);
    seq_util u(m);
    unsigned r1, r2, r3;
    switch (m_ty) {
    case t_pred:         
        result = subst(m_t, 1, &e);
        break;    
    case t_not:
        result = m_expr->accept(e);
        result = m.mk_not(result);
        break;
    case t_char:
        SASSERT(m.get_sort(e) == m.get_sort(m_t));
        SASSERT(m.get_sort(e) == m_sort);
        result = m.mk_eq(e, m_t);
        break;
    case t_range: 
        if (u.is_const_char(m_t, r1) && u.is_const_char(e, r2) && u.is_const_char(m_s, r3)) {
            result = m.mk_bool_val((r1 <= r2) && (r2 <= r3));            
        }
        else {
            result = m.mk_and(u.mk_le(m_t, e), u.mk_le(e, m_s));
        }
        break;
    }
    
    return result;
}

std::ostream& sym_expr::display(std::ostream& out) const {
    switch (m_ty) {
    case t_char: return out << m_t;
    case t_range: return out << m_t << ":" << m_s;
    case t_pred: return out << m_t;
    case t_not: return m_expr->display(out << "not ");
    }
    return out << "expression type not recognized";
}

struct display_expr1 {
    ast_manager& m;
    display_expr1(ast_manager& m): m(m) {}
    std::ostream& display(std::ostream& out, sym_expr* e) const {
        return e->display(out);
    }
};

class sym_expr_boolean_algebra : public boolean_algebra<sym_expr*> {
    ast_manager& m;
    expr_solver& m_solver;
    expr_ref     m_var;
    typedef sym_expr* T;
public:
    sym_expr_boolean_algebra(ast_manager& m, expr_solver& s): 
        m(m), m_solver(s), m_var(m) {}

    T mk_false() override {
        expr_ref fml(m.mk_false(), m);
        return sym_expr::mk_pred(fml, m.mk_bool_sort()); // use of Bool sort for bound variable is arbitrary
    }
    T mk_true() override {
        expr_ref fml(m.mk_true(), m);
        return sym_expr::mk_pred(fml, m.mk_bool_sort());
    }
    T mk_and(T x, T y) override {
        seq_util u(m);
        if (x->is_char() && y->is_char()) {
            if (x->get_char() == y->get_char()) {
                return x;
            }
            if (m.are_distinct(x->get_char(), y->get_char())) {
                expr_ref fml(m.mk_false(), m);
                return sym_expr::mk_pred(fml, x->get_sort());
            }
        }
        unsigned lo1, hi1, lo2, hi2;
        if (x->is_range() && y->is_range() &&
            u.is_const_char(x->get_lo(), lo1) && u.is_const_char(x->get_hi(), hi1) &&
            u.is_const_char(y->get_lo(), lo2) && u.is_const_char(y->get_hi(), hi2)) {
            lo1 = std::max(lo1, lo2);
            hi1 = std::min(hi1, hi2);
            if (lo1 > hi1) {
                expr_ref fml(m.mk_false(), m);
                return sym_expr::mk_pred(fml, x->get_sort());
            }
            expr_ref _start(u.mk_char(lo1), m);
            expr_ref _stop(u.mk_char(hi1), m);
            return sym_expr::mk_range(_start, _stop);
        }

        sort* s = x->get_sort();
        if (m.is_bool(s)) s = y->get_sort();
        var_ref v(m.mk_var(0, s), m);
        expr_ref fml1 = x->accept(v);
        expr_ref fml2 = y->accept(v);
        if (m.is_true(fml1)) {
            return y;
        }
        if (m.is_true(fml2)) {
            return x;
        }
        if (fml1 == fml2) {
            return x;   
        }
        if (is_complement(fml1, fml2)) {
            expr_ref ff(m.mk_false(), m);
            return sym_expr::mk_pred(ff, x->get_sort());
        }
        bool_rewriter br(m);
        expr_ref fml(m);
        br.mk_and(fml1, fml2, fml);
        return sym_expr::mk_pred(fml, x->get_sort());
    }

    bool is_complement(expr* f1, expr* f2) {
        expr* f = nullptr;
        return 
            (m.is_not(f1, f) && f == f2) ||
            (m.is_not(f2, f) && f == f1);
    }

    T mk_or(T x, T y) override {
        if (x->is_char() && y->is_char() &&
            x->get_char() == y->get_char()) {
            return x;
        }
        if (x == y) return x;
        var_ref v(m.mk_var(0, x->get_sort()), m);
        expr_ref fml1 = x->accept(v);
        expr_ref fml2 = y->accept(v);        
        if (m.is_false(fml1)) return y;
        if (m.is_false(fml2)) return x;
        bool_rewriter br(m);
        expr_ref fml(m);
        br.mk_or(fml1, fml2, fml);
        return sym_expr::mk_pred(fml, x->get_sort());
    }

    T mk_and(unsigned sz, T const* ts) override {
        switch (sz) {
        case 0: return mk_true();
        case 1: return ts[0];
        default: {
            T t = ts[0];
            for (unsigned i = 1; i < sz; ++i) {
                t = mk_and(t, ts[i]);
            }
            return t;
        }
        }
    }

    T mk_or(unsigned sz, T const* ts) override {
        switch (sz) {
        case 0: return mk_false();
        case 1: return ts[0];
        default: {
            T t = ts[0];
            for (unsigned i = 1; i < sz; ++i) {
                t = mk_or(t, ts[i]);
            }
            return t;
        }
        }
    }

    lbool is_sat(T x) override {
        unsigned lo, hi;
        seq_util u(m);

        if (x->is_char()) {
            return l_true;
        }
        if (x->is_range() && u.is_const_char(x->get_lo(), lo) && u.is_const_char(x->get_hi(), hi)) {
            return (lo <= hi) ? l_true : l_false; 
        }
        if (x->is_not() && x->get_arg()->is_range() && u.is_const_char(x->get_arg()->get_lo(), lo) && 0 < lo) {
            return l_true;
        }            
        if (!m_var || m.get_sort(m_var) != x->get_sort()) {
            m_var = m.mk_fresh_const("x", x->get_sort()); 
        }
        expr_ref fml = x->accept(m_var);
        if (m.is_true(fml)) {
            return l_true;
        }
        if (m.is_false(fml)) {
            return l_false;
        }
        return m_solver.check_sat(fml);
    }

    T mk_not(T x) override {
        return sym_expr::mk_not(m, x);    
    }

};

re2automaton::re2automaton(ast_manager& m): m(m), u(m), m_ba(nullptr), m_sa(nullptr) {}

re2automaton::~re2automaton() {}

void re2automaton::set_solver(expr_solver* solver) {
    m_solver = solver;
    m_ba = alloc(sym_expr_boolean_algebra, m, *solver);
    m_sa = alloc(symbolic_automata_t, sm, *m_ba.get());
}

eautomaton* re2automaton::mk_product(eautomaton* a1, eautomaton* a2) {
    return m_sa->mk_product(*a1, *a2);
}

eautomaton* re2automaton::operator()(expr* e) { 
    eautomaton* r = re2aut(e); 
    if (r) {        
        r->compress(); 
        bool_rewriter br(m);
        TRACE("seq", display_expr1 disp(m); r->display(tout << mk_pp(e, m) << " -->\n", disp););
    }
    return r;
} 

bool re2automaton::is_unit_char(expr* e, expr_ref& ch) {
    zstring s;
    expr* c = nullptr;
    if (u.str.is_string(e, s) && s.length() == 1) {
        ch = u.mk_char(s[0]);
        return true;
    }
    if (u.str.is_unit(e, c)) {
        ch = c;
        return true;
    }
    return false;
}

eautomaton* re2automaton::re2aut(expr* e) {
    SASSERT(u.is_re(e));
    expr *e0, *e1, *e2;
    scoped_ptr<eautomaton> a, b;
    unsigned lo, hi;
    zstring s1, s2;
    if (u.re.is_to_re(e, e1)) {
        return seq2aut(e1);
    }
    else if (u.re.is_concat(e, e1, e2) && (a = re2aut(e1)) && (b = re2aut(e2))) {
        return eautomaton::mk_concat(*a, *b);
    }
    else if (u.re.is_union(e, e1, e2) && (a = re2aut(e1)) && (b = re2aut(e2))) {
        return eautomaton::mk_union(*a, *b);
    }
    else if (u.re.is_star(e, e1) && (a = re2aut(e1))) {
        a->add_final_to_init_moves();
        a->add_init_to_final_states();        
        return a.detach();            
    }
    else if (u.re.is_plus(e, e1) && (a = re2aut(e1))) {
        a->add_final_to_init_moves();
        return a.detach();            
    }
    else if (u.re.is_opt(e, e1) && (a = re2aut(e1))) {
        a = eautomaton::mk_opt(*a);
        return a.detach();                    
    }
    else if (u.re.is_range(e, e1, e2)) {
        expr_ref _start(m), _stop(m);
        if (is_unit_char(e1, _start) &&
            is_unit_char(e2, _stop)) {
            TRACE("seq", tout << "Range: " << _start << " " << _stop << "\n";);
            a = alloc(eautomaton, sm, sym_expr::mk_range(_start, _stop));
            return a.detach();            
        }
        else {
            // if e1/e2 are not unit, (re.range e1 e2) is defined to be the empty language
            return alloc(eautomaton, sm);
        }
    }
    else if (u.re.is_complement(e, e0) && (a = re2aut(e0)) && m_sa) {
        return m_sa->mk_complement(*a);
    }
    else if (u.re.is_loop(e, e1, lo, hi) && (a = re2aut(e1))) {
        scoped_ptr<eautomaton> eps = eautomaton::mk_epsilon(sm);
        b = eautomaton::mk_epsilon(sm);
        while (hi > lo) {
            scoped_ptr<eautomaton> c = eautomaton::mk_concat(*a, *b);
            b = eautomaton::mk_union(*eps, *c);
            --hi;
        }
        while (lo > 0) {
            b = eautomaton::mk_concat(*a, *b);
            --lo;
        }
        return b.detach();        
    }
    else if (u.re.is_loop(e, e1, lo) && (a = re2aut(e1))) {
        b = eautomaton::clone(*a);
        b->add_final_to_init_moves();
        b->add_init_to_final_states();        
        while (lo > 0) {
            b = eautomaton::mk_concat(*a, *b);
            --lo;
        }
        return b.detach();        
    }
    else if (u.re.is_empty(e)) {
        return alloc(eautomaton, sm);
    }
    else if (u.re.is_full_seq(e)) {
        expr_ref tt(m.mk_true(), m);
        sort *seq_s = nullptr, *char_s = nullptr;
        VERIFY (u.is_re(m.get_sort(e), seq_s));
        VERIFY (u.is_seq(seq_s, char_s));
        sym_expr* _true = sym_expr::mk_pred(tt, char_s);
        return eautomaton::mk_loop(sm, _true);
    }
    else if (u.re.is_full_char(e)) {
        expr_ref tt(m.mk_true(), m);
        sort *seq_s = nullptr, *char_s = nullptr;
        VERIFY (u.is_re(m.get_sort(e), seq_s));
        VERIFY (u.is_seq(seq_s, char_s));
        sym_expr* _true = sym_expr::mk_pred(tt, char_s);
        a = alloc(eautomaton, sm, _true);
        return a.detach();
    }
    else if (u.re.is_intersection(e, e1, e2) && m_sa && (a = re2aut(e1)) && (b = re2aut(e2))) {
        eautomaton* r = m_sa->mk_product(*a, *b);
        TRACE("seq", display_expr1 disp(m); a->display(tout << "a:", disp); b->display(tout << "b:", disp); r->display(tout << "intersection:", disp););
        return r;
    }
    else {        
        TRACE("seq", tout << "not handled " << mk_pp(e, m) << "\n";);
    }
    
    return nullptr;
}

eautomaton* re2automaton::seq2aut(expr* e) {
    SASSERT(u.is_seq(e));
    zstring s;
    expr* e1, *e2;
    scoped_ptr<eautomaton> a, b;
    if (u.str.is_concat(e, e1, e2) && (a = seq2aut(e1)) && (b = seq2aut(e2))) {
        return eautomaton::mk_concat(*a, *b);
    }
    else if (u.str.is_unit(e, e1)) {
        return alloc(eautomaton, sm, sym_expr::mk_char(m, e1));
    }
    else if (u.str.is_empty(e)) {
        return eautomaton::mk_epsilon(sm);
    }
    else if (u.str.is_string(e, s)) {        
        unsigned init = 0;
        eautomaton::moves mvs;        
        unsigned_vector final;
        final.push_back(s.length());
        for (unsigned k = 0; k < s.length(); ++k) {
            // reference count?
            mvs.push_back(eautomaton::move(sm, k, k+1, sym_expr::mk_char(m, u.str.mk_char(s, k))));
        }
        return alloc(eautomaton, sm, init, final, mvs);
    }
    return nullptr;
}

void seq_rewriter::updt_params(params_ref const & p) {
    seq_rewriter_params sp(p);
    m_coalesce_chars = sp.coalesce_chars();
}

void seq_rewriter::get_param_descrs(param_descrs & r) {
    seq_rewriter_params::collect_param_descrs(r);
}

br_status seq_rewriter::mk_bool_app(func_decl* f, unsigned n, expr* const* args, expr_ref& result) {
    switch (f->get_decl_kind()) {
    case OP_AND:
        return mk_bool_app_helper(true, n, args, result);
    case OP_OR:
        return mk_bool_app_helper(false, n, args, result);
    default:
        return BR_FAILED;
    }
}

br_status seq_rewriter::mk_bool_app_helper(bool is_and, unsigned n, expr* const* args, expr_ref& result) {
    bool found = false;
    expr* arg = nullptr;
    
    for (unsigned i = 0; i < n && !found; ++i) {        
        found = m_util.str.is_in_re(args[i]) || (m().is_not(args[i], arg) && m_util.str.is_in_re(arg));
    }
    if (!found) return BR_FAILED;
    
    obj_map<expr, expr*> in_re, not_in_re;
    bool found_pair = false;
    
    for (unsigned i = 0; i < n; ++i) {
        expr* args_i = args[i];
        expr* x = nullptr;
        expr* y = nullptr;
        expr* z = nullptr;
        if (m_util.str.is_in_re(args_i, x, y)) {
            if (in_re.find(x, z)) {				
                in_re[x] = is_and ? re().mk_inter(z, y) : re().mk_union(z, y);
                found_pair = true;
            }
            else {
                in_re.insert(x, y);
            }
            found_pair |= not_in_re.contains(x);
        }
        else if (m().is_not(args_i, arg) && m_util.str.is_in_re(arg, x, y)) {
            if (not_in_re.find(x, z)) {
                not_in_re[x] = is_and ? re().mk_union(z, y) : re().mk_inter(z, y);
                found_pair = true;
            }
            else {
                not_in_re.insert(x, y);
            }
            found_pair |= in_re.contains(x);
        }
    }
    
    if (!found_pair) {
        return BR_FAILED;
    }
    
    ptr_buffer<expr> new_args;
    for (auto const & kv : in_re) {
        expr* x = kv.m_key;
        expr* y = kv.m_value;
        expr* z = nullptr;
        if (not_in_re.find(x, z)) {
            expr* z_c = re().mk_complement(z);
            expr* w = is_and ? re().mk_inter(y, z_c) : re().mk_union(y, z_c);
            new_args.push_back(re().mk_in_re(x, w));
        }
        else {
            new_args.push_back(re().mk_in_re(x, y));
        }
    }
    for (auto const& kv : not_in_re) {
        expr* x = kv.m_key;
        expr* y = kv.m_value;
        if (!in_re.contains(x)) {
            new_args.push_back(re().mk_in_re(x, re().mk_complement(y)));
        }
    }
    for (unsigned i = 0; i < n; ++i) {
        expr* arg = args[i], * x;
        if (!m_util.str.is_in_re(arg) && !(m().is_not(arg, x) && m_util.str.is_in_re(x))) {
            new_args.push_back(arg);
        }
    }
    
    result = is_and ? m().mk_and(new_args.size(), new_args.c_ptr()) : m().mk_or(new_args.size(), new_args.c_ptr());
    return BR_REWRITE_FULL;
}

br_status seq_rewriter::mk_app_core(func_decl * f, unsigned num_args, expr * const * args, expr_ref & result) {
    SASSERT(f->get_family_id() == get_fid());
    br_status st = BR_FAILED;
    switch(f->get_decl_kind()) {
        
    case OP_SEQ_UNIT:
        SASSERT(num_args == 1);
        st = mk_seq_unit(args[0], result);
        break;
    case OP_SEQ_EMPTY:
        return BR_FAILED;
    case OP_RE_PLUS:
        SASSERT(num_args == 1);
        st = mk_re_plus(args[0], result);
        break;
    case OP_RE_STAR:
        SASSERT(num_args == 1);
        st = mk_re_star(args[0], result);
        break;
    case OP_RE_OPTION:
        SASSERT(num_args == 1);
        st = mk_re_opt(args[0], result);
        break;
    case OP_RE_CONCAT:
        if (num_args == 1) {
            result = args[0]; 
            st = BR_DONE;
        }
        else {
            SASSERT(num_args == 2);
            st = mk_re_concat(args[0], args[1], result); 
        }
        break;
    case OP_RE_UNION:
        if (num_args == 1) {
            result = args[0]; 
            st = BR_DONE;
        }
        else {
            SASSERT(num_args == 2);
            st = mk_re_union(args[0], args[1], result);
        }
        break;
    case OP_RE_RANGE:
        SASSERT(num_args == 2);
        st = mk_re_range(args[0], args[1], result);
        break;
    case OP_RE_INTERSECT:
        if (num_args == 1) {
            result = args[0];
            st = BR_DONE;
        }
        else {
            SASSERT(num_args == 2);
            st = mk_re_inter(args[0], args[1], result);
        }
        break;
    case OP_RE_COMPLEMENT:
        SASSERT(num_args == 1);
        st = mk_re_complement(args[0], result);
        break;
    case OP_RE_LOOP:
        st = mk_re_loop(f, num_args, args, result);
        break;
    case OP_RE_POWER:
        st = mk_re_power(f, args[0], result);
        break;
    case OP_RE_EMPTY_SET:
        return BR_FAILED;    
    case OP_RE_FULL_SEQ_SET:
        return BR_FAILED;    
    case OP_RE_FULL_CHAR_SET:
        return BR_FAILED;    
    case OP_RE_OF_PRED:
        return BR_FAILED;    
    case _OP_SEQ_SKOLEM:
        return BR_FAILED;    
    case OP_SEQ_CONCAT: 
        if (num_args == 1) {
            result = args[0];
            st = BR_DONE;
        }
        else {
            SASSERT(num_args == 2);
            st = mk_seq_concat(args[0], args[1], result);
        }
        break;
    case OP_SEQ_LENGTH:
        SASSERT(num_args == 1);
        st = mk_seq_length(args[0], result);
        break;
    case OP_SEQ_EXTRACT:
        SASSERT(num_args == 3);
        st = mk_seq_extract(args[0], args[1], args[2], result);
        break;
    case OP_SEQ_CONTAINS: 
        SASSERT(num_args == 2);
        st = mk_seq_contains(args[0], args[1], result);
        break;
    case OP_SEQ_AT:
        SASSERT(num_args == 2);
        st = mk_seq_at(args[0], args[1], result); 
        break;
    case OP_SEQ_NTH:
        SASSERT(num_args == 2);
        return mk_seq_nth(args[0], args[1], result); 
    case OP_SEQ_NTH_I:
        SASSERT(num_args == 2);
        return mk_seq_nth_i(args[0], args[1], result); 
    case OP_SEQ_PREFIX: 
        SASSERT(num_args == 2);
        st = mk_seq_prefix(args[0], args[1], result);
        break;
    case OP_SEQ_SUFFIX: 
        SASSERT(num_args == 2);
        st = mk_seq_suffix(args[0], args[1], result);
        break;
    case OP_SEQ_INDEX:
        if (num_args == 2) {
            expr_ref arg3(zero(), m());
            result = m_util.str.mk_index(args[0], args[1], arg3);
            st = BR_REWRITE1;
        }
        else {
            SASSERT(num_args == 3);
            st = mk_seq_index(args[0], args[1], args[2], result);
        }
        break;
    case OP_SEQ_LAST_INDEX:
        SASSERT(num_args == 2);
        st = mk_seq_last_index(args[0], args[1], result);
        break;
    case OP_SEQ_REPLACE:
        SASSERT(num_args == 3);
        st = mk_seq_replace(args[0], args[1], args[2], result);
        break;
    case OP_SEQ_TO_RE:
        SASSERT(num_args == 1);
        st = mk_str_to_regexp(args[0], result);
        break;
    case OP_SEQ_IN_RE:
        SASSERT(num_args == 2);
        st = mk_str_in_regexp(args[0], args[1], result);
        break;
    case OP_STRING_LE:
        SASSERT(num_args == 2);
        st = mk_str_le(args[0], args[1], result);
        break;
    case OP_STRING_LT:
        SASSERT(num_args == 2);
        st = mk_str_lt(args[0], args[1], result);
        break;
    case OP_STRING_FROM_CODE:
        SASSERT(num_args == 1);
        st = mk_str_from_code(args[0], result);
        break;
    case OP_STRING_TO_CODE:
        SASSERT(num_args == 1);
        st = mk_str_to_code(args[0], result);
        break;
    case OP_STRING_IS_DIGIT:
        SASSERT(num_args == 1);
        st = mk_str_is_digit(args[0], result);
        break;
    case OP_STRING_CONST:
        st = BR_FAILED;
        if (!m_coalesce_chars) {
            st = mk_str_units(f, result);
        }
        break;
    case OP_STRING_ITOS: 
        SASSERT(num_args == 1);
        st = mk_str_itos(args[0], result);
        break;
    case OP_STRING_STOI: 
        SASSERT(num_args == 1);
        st = mk_str_stoi(args[0], result);
        break;
    case _OP_STRING_CONCAT:
    case _OP_STRING_PREFIX:
    case _OP_STRING_SUFFIX:
    case _OP_STRING_STRCTN:
    case _OP_STRING_LENGTH:
    case _OP_STRING_CHARAT:
    case _OP_STRING_IN_REGEXP: 
    case _OP_STRING_TO_REGEXP: 
    case _OP_STRING_SUBSTR: 
    case _OP_STRING_STRREPL:
    case _OP_STRING_STRIDOF: 
        UNREACHABLE();
    }
    if (st == BR_FAILED) {
        st = lift_ite(f, num_args, args, result);
    }
    if (st != BR_FAILED && m().get_sort(result) != f->get_range()) {
        std::cout << expr_ref(m().mk_app(f, num_args, args), m()) << " -> " << result << "\n";
    }
    CTRACE("seq_verbose", st != BR_FAILED, tout << expr_ref(m().mk_app(f, num_args, args), m()) << " -> " << result << "\n";);
    SASSERT(st == BR_FAILED || m().get_sort(result) == f->get_range());
    return st;
}

/*
 * (seq.unit (_ BitVector 8)) ==> String constant
 */
br_status seq_rewriter::mk_seq_unit(expr* e, expr_ref& result) {
    unsigned ch;
    // specifically we want (_ BitVector 8)
    if (m_util.is_const_char(e, ch) && m_coalesce_chars) {
        // convert to string constant
        zstring str(ch);
        TRACE("seq_verbose", tout << "rewrite seq.unit of 8-bit value " << ch << " to string constant \"" << str<< "\"" << std::endl;);
        result = m_util.str.mk_string(str);
        return BR_DONE;
    }

    return BR_FAILED;
}

/*
   string + string = string
   (a + b) + c = a + (b + c)
   a + "" = a
   "" + a = a
   string + (string + a) = string + a
*/
br_status seq_rewriter::mk_seq_concat(expr* a, expr* b, expr_ref& result) {
    zstring s1, s2;
    expr* c, *d;
    bool isc1 = m_util.str.is_string(a, s1) && m_coalesce_chars;
    bool isc2 = m_util.str.is_string(b, s2) && m_coalesce_chars;
    if (isc1 && isc2) {
        result = m_util.str.mk_string(s1 + s2);
        return BR_DONE;
    }
    if (m_util.str.is_concat(a, c, d)) {
        result = m_util.str.mk_concat(c, m_util.str.mk_concat(d, b));
        return BR_REWRITE2;
    }
    if (m_util.str.is_empty(a)) {
        result = b;
        return BR_DONE;
    }
    if (m_util.str.is_empty(b)) {
        result = a;
        return BR_DONE;
    }
    if (isc1 && m_util.str.is_concat(b, c, d) && m_util.str.is_string(c, s2)) {
        result = m_util.str.mk_concat(m_util.str.mk_string(s1 + s2), d);
        return BR_DONE;
    }
    return BR_FAILED;
}

br_status seq_rewriter::mk_seq_length(expr* a, expr_ref& result) {
    zstring b;
    m_es.reset();
    m_util.str.get_concat(a, m_es);
    unsigned len = 0;
    unsigned j = 0;
    for (expr* e : m_es) {
        if (m_util.str.is_string(e, b)) {
            len += b.length();
        }
        else if (m_util.str.is_unit(e)) {
            len += 1;
        }
        else if (m_util.str.is_empty(e)) {
            // skip
        }
        else {
            m_es[j++] = e;
        }
    }
    if (j == 0) {
        result = m_autil.mk_int(len);
        return BR_DONE;
    }
    if (j != m_es.size() || j != 1) {
        expr_ref_vector es(m());        
        for (unsigned i = 0; i < j; ++i) {
            es.push_back(m_util.str.mk_length(m_es.get(i)));
        }
        if (len != 0) {
            es.push_back(m_autil.mk_int(len));
        }
        result = m_autil.mk_add(es.size(), es.c_ptr());
        return BR_REWRITE2;
    }
    return BR_FAILED;
}

br_status seq_rewriter::lift_ite(func_decl* f, unsigned n, expr* const* args, expr_ref& result) {
    expr* c = nullptr, *t = nullptr, *e = nullptr;
    for (unsigned i = 0; i < n; ++i) {        
        if (m().is_ite(args[i], c, t, e) && 
            (get_depth(t) <= 2 || t->get_ref_count() == 1 ||
             get_depth(e) <= 2 || e->get_ref_count() == 1)) {
            ptr_buffer<expr> new_args;
            for (unsigned j = 0; j < n; ++j) new_args.push_back(args[j]);
            new_args[i] = t;
            expr_ref arg1(m().mk_app(f, new_args), m());
            new_args[i] = e;
            expr_ref arg2(m().mk_app(f, new_args), m());
            result = m().mk_ite(c, arg1, arg2);
            return BR_REWRITE2;
        }
    }
    return BR_FAILED;
}


bool seq_rewriter::is_suffix(expr* s, expr* offset, expr* len) {
    expr_ref_vector lens(m());
    rational a, b;
    return 
        get_lengths(len, lens, a) && 
        (a.neg(), m_autil.is_numeral(offset, b) && 
         b.is_pos() && 
         a == b && 
         lens.contains(s));
}

bool seq_rewriter::sign_is_determined(expr* e, sign& s) {
    s = sign_zero;
    if (m_autil.is_add(e)) {
        for (expr* arg : *to_app(e)) {
            sign s1;
            if (!sign_is_determined(arg, s1))
                return false;
            if (s == sign_zero) 
                s = s1;
            else if (s1 == sign_zero)
                continue;
            else if (s1 != s)
                return false;
        }
        return true;
    }
    if (m_autil.is_mul(e)) {
        for (expr* arg : *to_app(e)) {
            sign s1;
            if (!sign_is_determined(arg, s1))
                return false;
            if (s1 == sign_zero) {
                s = sign_zero;
                return true;
            }
            if (s == sign_zero)
                s = s1;
            else if (s != s1)
                s = sign_neg;
            else 
                s = sign_pos;
        }
        return true;
    }
    if (m_util.str.is_length(e)) {
        s = sign_pos;
        return true;
    }
    rational r;
    if (m_autil.is_numeral(e, r)) {
        if (r.is_pos())
            s = sign_pos;
        else if (r.is_neg())
            s = sign_neg;
        return true;
    }
    return false;
}

br_status seq_rewriter::mk_seq_extract(expr* a, expr* b, expr* c, expr_ref& result) {
    zstring s;
    rational pos, len;

    TRACE("seq_verbose", tout << mk_pp(a, m()) << " " << mk_pp(b, m()) << " " << mk_pp(c, m()) << "\n";);
    bool constantBase = m_util.str.is_string(a, s);
    bool constantPos = m_autil.is_numeral(b, pos);
    bool constantLen = m_autil.is_numeral(c, len);
    bool lengthPos   = m_util.str.is_length(b) || m_autil.is_add(b);
    sort* a_sort = m().get_sort(a);

    sign sg;
    if (sign_is_determined(c, sg) && sg == sign_neg) {
        result = m_util.str.mk_empty(a_sort);
        return BR_DONE;
    }
    
    // case 1: pos<0 or len<=0
    // rewrite to ""
    if ( (constantPos && pos.is_neg()) || (constantLen && !len.is_pos()) ) {
        result = m_util.str.mk_empty(a_sort);
        return BR_DONE;
    }
    // case 1.1: pos >= length(base)
    // rewrite to ""
    if (constantPos && constantBase && pos >= rational(s.length())) {
        result = m_util.str.mk_empty(a_sort);
        return BR_DONE;
    }

    constantPos &= pos.is_unsigned();
    constantLen &= len.is_unsigned();

    if (constantPos && constantLen && constantBase) {
        unsigned _pos = pos.get_unsigned();
        unsigned _len = len.get_unsigned();
        if (_pos + _len >= s.length()) {
            // case 2: pos+len goes past the end of the string
            unsigned _len = s.length() - _pos + 1;
            result = m_util.str.mk_string(s.extract(_pos, _len));
        } else {
            // case 3: pos+len still within string
            result = m_util.str.mk_string(s.extract(_pos, _len));
        }
        return BR_DONE;
    }


    expr_ref_vector as(m()), bs(m());
    m_util.str.get_concat_units(a, as);
    if (as.empty()) {
        result = m_util.str.mk_empty(m().get_sort(a));
        return BR_DONE;
    }

    // extract(a + b + c, len(a + b), s) -> extract(c, 0, s)
    // extract(a + b + c, len(a) + len(b), s) -> extract(c, 0, s)
    if (lengthPos) {
        
        m_lhs.reset();
        expr_ref_vector lens(m());
        m_util.str.get_concat(a, m_lhs);
        TRACE("seq", tout << m_lhs << " " << pos << " " << lens << "\n";);
        if (!get_lengths(b, lens, pos) || pos.is_neg()) {
            return BR_FAILED;
        }
        unsigned i = 0;
        for (; i < m_lhs.size(); ++i) {
            expr* lhs = m_lhs.get(i);
            if (lens.contains(lhs)) {
                lens.erase(lhs);
            }
            else if (m_util.str.is_unit(lhs) && pos.is_pos()) {
                pos -= rational(1);
            }
            else {
                break;
            }
        }
        if (i == 0) return BR_FAILED;
        expr_ref t1(m()), t2(m());
        t1 = m_util.str.mk_concat(m_lhs.size() - i, m_lhs.c_ptr() + i, m().get_sort(a));        
        t2 = m_autil.mk_int(pos);
        for (expr* rhs : lens) {
            t2 = m_autil.mk_add(t2, m_util.str.mk_length(rhs));
        }
        result = m_util.str.mk_substr(t1, t2, c);
        TRACE("seq", tout << result << "\n";);
        return BR_REWRITE2;
    }

    if (!constantPos) {
        return BR_FAILED;
    }
    unsigned _pos = pos.get_unsigned();

    // (extract s 0 (len s)) = s 
    expr* a2 = nullptr;
    if (_pos == 0 && m_util.str.is_length(c, a2)) {
        m_lhs.reset();
        m_util.str.get_concat(a, m_lhs);
        if (!m_lhs.empty() && m_lhs.get(0) == a2) {
            result = a2;
            return BR_DONE;
        }
    }

    expr* a1 = nullptr, *b1 = nullptr, *c1 = nullptr;
    if (m_util.str.is_extract(a, a1, b1, c1) && 
        is_suffix(a1, b1, c1) && is_suffix(a, b, c)) {
        result = m_util.str.mk_substr(a1, m_autil.mk_add(b1, b), m_autil.mk_sub(c1, b));
        return BR_REWRITE3;
    }

    unsigned offset = 0;
    for (; offset < as.size() && m_util.str.is_unit(as.get(offset)) && offset < _pos; ++offset) {};
    if (offset == 0 && _pos > 0) {
        return BR_FAILED;
    }
    std::function<bool(expr*)> is_unit = [&](expr *e) { return m_util.str.is_unit(e); };

    if (_pos == 0 && as.forall(is_unit)) {
        result = m_util.str.mk_empty(m().get_sort(a));
        for (unsigned i = 1; i <= as.size(); ++i) {
            result = m().mk_ite(m_autil.mk_ge(c, m_autil.mk_int(i)), 
                                m_util.str.mk_concat(i, as.c_ptr(), m().get_sort(a)), 
                                result);
        }
        return BR_REWRITE_FULL;
    }
    if (_pos == 0 && !constantLen) {
        return BR_FAILED;
    }
    // (extract (++ (unit x) (unit y)) 3 c) = empty
    if (offset == as.size()) {
        result = m_util.str.mk_empty(m().get_sort(a));
        return BR_DONE;
    }
    SASSERT(offset != 0 || _pos == 0);
    
    if (constantLen && _pos == offset) {
        unsigned _len = len.get_unsigned();
        // (extract (++ (unit a) (unit b) (unit c) x) 1 2) = (++ (unit b) (unit c))
        unsigned i = offset;
        for (; i < as.size() && m_util.str.is_unit(as.get(i)) && i - offset < _len; ++i);
        if (i - offset == _len) {
            result = m_util.str.mk_concat(_len, as.c_ptr() + offset, m().get_sort(a));
            return BR_DONE;
        }
        if (i == as.size()) {
            result = m_util.str.mk_concat(as.size() - offset, as.c_ptr() + offset, m().get_sort(as.get(0)));
            return BR_DONE;
        }
    }
    if (offset == 0) {
        return BR_FAILED;
    }
    expr_ref pos1(m());
    pos1 = m_autil.mk_sub(b, m_autil.mk_int(offset));
    result = m_util.str.mk_concat(as.size() - offset, as.c_ptr() + offset, m().get_sort(as.get(0)));
    result = m_util.str.mk_substr(result, pos1, c);
    return BR_REWRITE3;
}

bool seq_rewriter::get_lengths(expr* e, expr_ref_vector& lens, rational& pos) {
    expr* arg = nullptr;
    rational pos1;
    if (m_autil.is_add(e)) {
        for (expr* arg1 : *to_app(e)) {
            if (!get_lengths(arg1, lens, pos)) return false;
        }
    }
    else if (m_util.str.is_length(e, arg)) {
        lens.push_back(arg);
    }
    else if (m_autil.is_numeral(e, pos1)) {
        pos += pos1;
    }
    else {
        return false;
    }
    return true;
}

bool seq_rewriter::cannot_contain_suffix(expr* a, expr* b) {
    
    if (m_util.str.is_unit(a) && m_util.str.is_unit(b) && m().are_distinct(a, b)) {
        return true;
    }
    zstring A, B;
    if (m_util.str.is_string(a, A) && m_util.str.is_string(b, B)) {
        // some prefix of a is a suffix of b
        bool found = false;
        for (unsigned i = 1; !found && i <= A.length(); ++i) {
            found = A.extract(0, i).suffixof(B);
        }
        return !found;
    }

    return false;
}


bool seq_rewriter::cannot_contain_prefix(expr* a, expr* b) {
    
    if (m_util.str.is_unit(a) && m_util.str.is_unit(b) && m().are_distinct(a, b)) {
        return true;
    }
    zstring A, B;
    if (m_util.str.is_string(a, A) && m_util.str.is_string(b, B)) {
        // some suffix of a is a prefix of b
        bool found = false;
        for (unsigned i = 0; !found && i < A.length(); ++i) {
            found = A.extract(i, A.length()-i).suffixof(B);
        }
        return !found;
    }

    return false;
}



br_status seq_rewriter::mk_seq_contains(expr* a, expr* b, expr_ref& result) {
    zstring c, d;
    if (m_util.str.is_string(a, c) && m_util.str.is_string(b, d)) {
        result = m().mk_bool_val(c.contains(d));
        return BR_DONE;
    }
    expr* x = nullptr, *y, *z;
    if (m_util.str.is_extract(b, x, y, z) && x == a) {
        result = m().mk_true();
        return BR_DONE;
    }

    // check if subsequence of a is in b.
    expr_ref_vector as(m()), bs(m());
    m_util.str.get_concat_units(a, as);
    m_util.str.get_concat_units(b, bs);
    
    TRACE("seq", tout << mk_pp(a, m()) << " contains " << mk_pp(b, m()) << "\n";);
   
    if (bs.empty()) {
        result = m().mk_true();
        return BR_DONE;
    }

    if (as.empty()) {
        result = m_util.str.mk_is_empty(b);
        return BR_REWRITE2;
    }

    for (unsigned i = 0; bs.size() + i <= as.size(); ++i) {
        unsigned j = 0;
        for (; j < bs.size() && as.get(j+i) == bs.get(j); ++j) {};
        if (j == bs.size()) {
            result = m().mk_true();
            return BR_DONE;
        }
    }
    std::function<bool(expr*)> is_value = [&](expr* e) { return m().is_value(e); };
    if (bs.forall(is_value) && as.forall(is_value)) {
        result = m().mk_false();
        return BR_DONE;
    }

    unsigned lenA = 0, lenB = 0;
    bool lA = min_length(as, lenA);
    if (lA) {
        min_length(bs, lenB);
        if (lenB > lenA) {
            result = m().mk_false();
            return BR_DONE;
        }
    }

    unsigned offs = 0;
    unsigned sz = as.size();
    expr* b0 = bs.get(0);
    expr* bL = bs.get(bs.size()-1);
    for (; offs < as.size() && cannot_contain_prefix(as[offs].get(), b0); ++offs) {}
    for (; sz > offs && cannot_contain_suffix(as.get(sz-1), bL); --sz) {}
    if (offs == sz) {
        result = m_util.str.mk_is_empty(b);
        return BR_REWRITE2;
    }
    if (offs > 0 || sz < as.size()) {
        SASSERT(sz > offs);
        result = m_util.str.mk_contains(m_util.str.mk_concat(sz-offs, as.c_ptr()+offs, m().get_sort(a)), b);
        return BR_REWRITE2;
    }    

    std::function<bool(expr*)> is_unit = [&](expr *e) { return m_util.str.is_unit(e); };

    if (bs.forall(is_unit) && as.forall(is_unit)) {
        expr_ref_vector ors(m());
        for (unsigned i = 0; i + bs.size() <= as.size(); ++i) {
            expr_ref_vector ands(m());
            for (unsigned j = 0; j < bs.size(); ++j) {
                ands.push_back(m().mk_eq(as.get(i + j), bs.get(j)));
            }
            ors.push_back(::mk_and(ands));
        }
        result = ::mk_or(ors);
        return BR_REWRITE_FULL;
    }

    if (bs.size() == 1 && bs.forall(is_unit) && as.size() > 1) {
        expr_ref_vector ors(m());        
        for (expr* ai : as) {
            ors.push_back(m_util.str.mk_contains(ai, bs.get(0)));
        }
        result = ::mk_or(ors);
        return BR_REWRITE_FULL;
    }


    return BR_FAILED;
}

/*
 * (str.at s i), constants s/i, i < 0 or i >= |s| ==> (str.at s i) = ""
 */
br_status seq_rewriter::mk_seq_at(expr* a, expr* b, expr_ref& result) {
    zstring c;
    rational r;
    expr_ref_vector lens(m());
    sort* sort_a = m().get_sort(a);
    if (!get_lengths(b, lens, r)) {
        return BR_FAILED;
    }
    if (lens.empty() && r.is_neg()) {
        result = m_util.str.mk_empty(sort_a);
        return BR_DONE;
    } 

    expr* a2 = nullptr, *i2 = nullptr;
    if (lens.empty() && m_util.str.is_at(a, a2, i2)) {
        if (r.is_pos()) {
            result = m_util.str.mk_empty(sort_a);
        }
        else {
            result = a;
        }
        return BR_DONE;            
    }

    m_lhs.reset();
    m_util.str.get_concat_units(a, m_lhs);

    if (m_lhs.empty()) {
        result = m_util.str.mk_empty(m().get_sort(a));
        return BR_DONE;        
    }
    
    unsigned i = 0;
    for (; i < m_lhs.size(); ++i) {
        expr* lhs = m_lhs.get(i);
        if (lens.contains(lhs) && !r.is_neg()) {
            lens.erase(lhs);
        }
        else if (m_util.str.is_unit(lhs) && r.is_zero() && lens.empty()) {
            result = lhs;
            return BR_REWRITE1;
        }
        else if (m_util.str.is_unit(lhs) && r.is_pos()) {
            r -= rational(1);
        }
        else {
            break;
        }
    }
    if (i == 0) {
        return BR_FAILED;
    }
    if (m_lhs.size() == i) {
        result = m_util.str.mk_empty(sort_a);
        return BR_DONE;
    }
    expr_ref pos(m_autil.mk_int(r), m());
    for (expr* rhs : lens) {
        pos = m_autil.mk_add(pos, m_util.str.mk_length(rhs));
    }
    result = m_util.str.mk_concat(m_lhs.size() - i , m_lhs.c_ptr() + i, sort_a);
    result = m_util.str.mk_at(result, pos);
    return BR_REWRITE2;   
}

br_status seq_rewriter::mk_seq_nth(expr* a, expr* b, expr_ref& result) {

    rational pos1, pos2;
    expr* s = nullptr, *p = nullptr, *len = nullptr;
    if (m_util.str.is_unit(a, s) && m_autil.is_numeral(b, pos1) && pos1.is_zero()) {
        result = s;
        return BR_DONE;
    }
    if (m_util.str.is_extract(a, s, p, len) && m_autil.is_numeral(p, pos1)) {
        expr_ref_vector lens(m());
        rational pos2;
        if (get_lengths(len, lens, pos2) && (pos1 == -pos2) && (lens.size() == 1) && (lens.get(0) == s)) {
            expr_ref idx(m_autil.mk_int(pos1), m());
            idx = m_autil.mk_add(b, idx);
            expr* es[2] = { s, idx };
            result = m().mk_app(m_util.get_family_id(), OP_SEQ_NTH, 2, es);
            return BR_REWRITE_FULL;
        }
    }

    expr* es[2] = { a, b};
    expr* la = m_util.str.mk_length(a);
    result = m().mk_ite(m().mk_and(m_autil.mk_ge(b, zero()), m().mk_not(m_autil.mk_le(la, b))), 
                        m().mk_app(m_util.get_family_id(), OP_SEQ_NTH_I, 2, es), 
                        m().mk_app(m_util.get_family_id(), OP_SEQ_NTH_U, 2, es));
    return BR_REWRITE_FULL;
}


br_status seq_rewriter::mk_seq_nth_i(expr* a, expr* b, expr_ref& result) {
    zstring c;
    rational r;
    if (!m_autil.is_numeral(b, r) || !r.is_unsigned()) {
        return BR_FAILED;
    }
    unsigned len = r.get_unsigned();

    expr_ref_vector as(m());
    m_util.str.get_concat_units(a, as);

    for (unsigned i = 0; i < as.size(); ++i) {
        expr* a = as.get(i), *u = nullptr;
        if (m_util.str.is_unit(a, u)) {
            if (len == i) {
                result = u;
                return BR_DONE;
            }            
        }
        else {
            return BR_FAILED;
        }
    }
    return BR_FAILED;
}

br_status seq_rewriter::mk_seq_last_index(expr* a, expr* b, expr_ref& result) {
    zstring s1, s2;
    bool isc1 = m_util.str.is_string(a, s1);
    bool isc2 = m_util.str.is_string(b, s2);
    if (isc1 && isc2) {
        int idx = s1.last_indexof(s2);
        result = m_autil.mk_numeral(rational(idx), true);
        return BR_DONE;
    }
    return BR_FAILED;
}

/**

  Index of first occurrence of second string in first one starting at 
  the position specified by the third argument.
  (str.indexof s t i), with 0 <= i <= |s| is the position of the first
  occurrence of t in s at or after position i, if any. 
  Otherwise, it is -1. Note that the result is i whenever i is within
  the range [0, |s|] and t is empty.
  (str.indexof String String Int Int)

   indexof(s, b, c) -> -1 if c < 0
   indexof(a, "", 0) -> if a = "" then 0 else -1
   indexof("", b, r) -> if b = "" and r = 0 then 0 else -1
   indexof(unit(x)+a, b, r+1) -> indexof(a, b, r) 
   indexof(unit(x)+a, unit(y)+b, 0) -> indexof(a,unit(y)+b, 0) if x != y
*/
br_status seq_rewriter::mk_seq_index(expr* a, expr* b, expr* c, expr_ref& result) {
    zstring s1, s2;
    rational r;
    bool isc1 = m_util.str.is_string(a, s1);
    bool isc2 = m_util.str.is_string(b, s2);
    sort* sort_a = m().get_sort(a);

    if (isc1 && isc2 && m_autil.is_numeral(c, r) && r.is_unsigned()) {
        int idx = s1.indexofu(s2, r.get_unsigned());
        result = m_autil.mk_numeral(rational(idx), true);
        return BR_DONE;
    }
    if (m_autil.is_numeral(c, r) && r.is_neg()) {
        result = m_autil.mk_numeral(rational(-1), true);
        return BR_DONE;
    }
    
    if (m_util.str.is_empty(b) && m_autil.is_numeral(c, r) && r.is_zero()) {
        result = c;
        return BR_DONE;
    }

    
    if (m_util.str.is_empty(a)) {
        expr* emp = m_util.str.mk_is_empty(b);
        result = m().mk_ite(m().mk_and(m().mk_eq(c, zero()), emp), zero(), minus_one());
        return BR_REWRITE2;
    }

    if (a == b) {
        if (m_autil.is_numeral(c, r)) {
            result = r.is_zero() ? zero() : minus_one();            
            return BR_DONE;
        }
        else {
            result = m().mk_ite(m().mk_eq(zero(), c), zero(), minus_one());
            return BR_REWRITE2;
        }
    }

    expr_ref_vector as(m()), bs(m());
    m_util.str.get_concat_units(a, as);
    unsigned i = 0;
    if (m_autil.is_numeral(c, r)) {
        i = 0;
        while (r.is_pos() && i < as.size() && m_util.str.is_unit(as.get(i))) {
            r -= rational(1);
            ++i;
        }
        if (i > 0) {
            expr_ref a1(m());
            a1 = m_util.str.mk_concat(as.size() - i, as.c_ptr() + i, sort_a);
            result = m_util.str.mk_index(a1, b, m_autil.mk_int(r));
            result = m().mk_ite(m_autil.mk_ge(result, zero()), m_autil.mk_add(m_autil.mk_int(i), result), minus_one());
            return BR_REWRITE_FULL;
        }
    }
    bool is_zero = m_autil.is_numeral(c, r) && r.is_zero();
    m_util.str.get_concat_units(b, bs);
    i = 0;
    while (is_zero && i < as.size() && 
           0 < bs.size() && 
           m_util.str.is_unit(as.get(i)) && 
           m_util.str.is_unit(bs.get(0)) &&
           m().are_distinct(as.get(i), bs.get(0))) {
        ++i;
    }
    if (i > 0) {
        result = m_util.str.mk_index(
            m_util.str.mk_concat(as.size() - i, as.c_ptr() + i, sort_a), b, c);
        result = m().mk_ite(m_autil.mk_ge(result, zero()), m_autil.mk_add(m_autil.mk_int(i), result), minus_one());
        return BR_REWRITE_FULL;
    }

    switch (compare_lengths(as, bs)) {
    case shorter_c:
        if (is_zero) {
            result = minus_one();
            return BR_DONE;
        }
        break;
    case same_length_c:
        result = m().mk_ite(m_autil.mk_le(c, minus_one()), minus_one(), 
                            m().mk_ite(m().mk_eq(c, zero()), 
                                       m().mk_ite(m().mk_eq(a, b), zero(), minus_one()),
                                       minus_one()));
        return BR_REWRITE_FULL;
    default:
        break;
    }
    if (is_zero && !as.empty() && m_util.str.is_unit(as.get(0))) {
        expr_ref a1(m_util.str.mk_concat(as.size() - 1, as.c_ptr() + 1, m().get_sort(as.get(0))), m());
        expr_ref b1(m_util.str.mk_index(a1, b, c), m());
        result = m().mk_ite(m_util.str.mk_prefix(b, a), zero(), 
                            m().mk_ite(m_autil.mk_ge(b1, zero()), m_autil.mk_add(one(), b1), minus_one()));
        return BR_REWRITE3;
    }
    // Enhancement: walk segments of a, determine which segments must overlap, must not overlap, may overlap.
    return BR_FAILED;
}

seq_rewriter::length_comparison seq_rewriter::compare_lengths(unsigned sza, expr* const* as, unsigned szb, expr* const* bs) {
    unsigned units_a = 0, units_b = 0, k = 0;
    obj_map<expr, unsigned> mults;
    bool b_has_foreign = false;
    for (unsigned i = 0; i < sza; ++i) {
        if (m_util.str.is_unit(as[i]))
            units_a++;
        else 
            mults.insert_if_not_there(as[i], 0)++;
    }
    for (unsigned i = 0; i < szb; ++i) {
        if (m_util.str.is_unit(bs[i]))
            units_b++;
        else if (mults.find(bs[i], k)) {
            --k;
            if (k == 0) {
                mults.erase(bs[i]);
            }
            else {
                mults.insert(bs[i], k);
            }
        }
        else {
            b_has_foreign = true;
        }
    }
    if (units_a > units_b && !b_has_foreign) {
        return longer_c;
    }
    if (units_a == units_b && !b_has_foreign && mults.empty()) {
        return same_length_c;
    }
    if (units_b > units_a && mults.empty()) {
        return shorter_c;
    }
    return unknown_c;
}


//  (str.replace s t t') is the string obtained by replacing the first occurrence 
//  of t in s, if any, by t'. Note that if t is empty, the result is to prepend
//  t' to s; also, if t does not occur in s then the result is s.

br_status seq_rewriter::mk_seq_replace(expr* a, expr* b, expr* c, expr_ref& result) {
    zstring s1, s2, s3;
    sort* sort_a = m().get_sort(a);
    if (m_util.str.is_string(a, s1) && m_util.str.is_string(b, s2) && 
        m_util.str.is_string(c, s3)) {
        result = m_util.str.mk_string(s1.replace(s2, s3));
        return BR_DONE;
    }
    if (b == c) {
        result = a;
        return BR_DONE;
    }
    if (a == b) {
        result = c;
        return BR_DONE;
    }
    if (m_util.str.is_empty(b)) {
        result = m_util.str.mk_concat(c, a);
        return BR_REWRITE1;
    }

    m_lhs.reset();
    m_util.str.get_concat(a, m_lhs);

    // a = "", |b| > 0 -> replace("",b,c) = ""
    if (m_lhs.empty()) {
        unsigned len = 0;
        m_util.str.get_concat(b, m_lhs);
        min_length(m_lhs, len);
        if (len > 0) {
            result = a;
            return BR_DONE;
        }
        return BR_FAILED;
    }

    // a := b + rest
    if (m_lhs.get(0) == b) {
        m_lhs[0] = c;
        result = m_util.str.mk_concat(m_lhs.size(), m_lhs.c_ptr(), sort_a);
        return BR_REWRITE1;
    }

    // a : a' + rest string, b is string, c is string, a' contains b
    if (m_util.str.is_string(b, s2) && m_util.str.is_string(c, s3) && 
        m_util.str.is_string(m_lhs.get(0), s1) && s1.contains(s2) ) {
        m_lhs[0] = m_util.str.mk_string(s1.replace(s2, s3));
        result = m_util.str.mk_concat(m_lhs.size(), m_lhs.c_ptr(), sort_a);
        return BR_REWRITE1;
    }
    m_lhs.reset();
    m_rhs.reset();
    m_util.str.get_concat_units(a, m_lhs);
    m_util.str.get_concat_units(b, m_rhs);
    if (m_rhs.empty()) {
        result = m_util.str.mk_concat(c, a);
        return BR_REWRITE1;
    }

    // is b a prefix of m_lhs at position i?
    auto compare_at_i = [&](unsigned i) {
        for (unsigned j = 0; j < m_rhs.size() && i + j < m_lhs.size(); ++j) {
            expr* b0 = m_rhs.get(j);
            expr* a0 = m_lhs.get(i + j);
            if (m().are_equal(a0, b0))
                continue;
            if (!m_util.str.is_unit(b0) || !m_util.str.is_unit(a0)) 
                return l_undef;
            if (m().are_distinct(a0, b0)) 
                return l_false;
            return l_undef;
        }
        return l_true;
    };

    unsigned i = 0;
    for (; i < m_lhs.size(); ++i) {
        lbool cmp = compare_at_i(i);
        if (cmp == l_false && m_util.str.is_unit(m_lhs.get(i)))
            continue;
        if (cmp == l_true && m_lhs.size() < i + m_rhs.size()) {
            expr_ref a1(m_util.str.mk_concat(i, m_lhs.c_ptr(), sort_a), m());
            expr_ref a2(m_util.str.mk_concat(m_lhs.size()-i, m_lhs.c_ptr()+i, sort_a), m());
            result = m().mk_ite(m().mk_eq(a2, b), m_util.str.mk_concat(a1, c), a);
            return BR_REWRITE_FULL;            
        }
        if (cmp == l_true) {
            expr_ref_vector es(m());
            es.append(i, m_lhs.c_ptr());
            es.push_back(c);
            es.append(m_lhs.size()-i-m_rhs.size(), m_lhs.c_ptr()+i+m_rhs.size());
            result = m_util.str.mk_concat(es, sort_a);
            return BR_REWRITE_FULL;        
        }
        break;
    }

    if (i > 0) {
        expr_ref a1(m_util.str.mk_concat(i, m_lhs.c_ptr(), sort_a), m());
        expr_ref a2(m_util.str.mk_concat(m_lhs.size()-i, m_lhs.c_ptr()+i, sort_a), m());
        result = m_util.str.mk_concat(a1, m_util.str.mk_replace(a2, b, c));
        return BR_REWRITE_FULL;        
    }

    return BR_FAILED;
}

br_status seq_rewriter::mk_seq_replace_all(expr* a, expr* b, expr* c, expr_ref& result) {
    return BR_FAILED;
}

br_status seq_rewriter::mk_seq_replace_re_all(expr* a, expr* b, expr* c, expr_ref& result) {
    return BR_FAILED;
}

br_status seq_rewriter::mk_seq_replace_re(expr* a, expr* b, expr* c, expr_ref& result) {
    return BR_FAILED;
}


br_status seq_rewriter::mk_seq_prefix(expr* a, expr* b, expr_ref& result) {
    TRACE("seq", tout << mk_pp(a, m()) << " " << mk_pp(b, m()) << "\n";);
    zstring s1, s2;
    bool isc1 = m_util.str.is_string(a, s1);
    bool isc2 = m_util.str.is_string(b, s2);
    sort* sort_a = m().get_sort(a);
    if (isc1 && isc2) {
        result = m().mk_bool_val(s1.prefixof(s2));
        TRACE("seq", tout << result << "\n";);
        return BR_DONE;
    }
    if (m_util.str.is_empty(a)) {
        result = m().mk_true();
        return BR_DONE;
    }
    expr* a1 = m_util.str.get_leftmost_concat(a);
    expr* b1 = m_util.str.get_leftmost_concat(b);
    isc1 = m_util.str.is_string(a1, s1);
    isc2 = m_util.str.is_string(b1, s2);
    expr_ref_vector as(m()), bs(m());

    if (a1 != b1 && isc1 && isc2) {
        if (s1.length() <= s2.length()) {
            if (s1.prefixof(s2)) {
                if (a == a1) {
                    result = m().mk_true();
                    TRACE("seq", tout << s1 << " " << s2 << " " << result << "\n";);
                    return BR_DONE;
                }               
                m_util.str.get_concat(a, as);
                m_util.str.get_concat(b, bs);
                SASSERT(as.size() > 1);
                s2 = s2.extract(s1.length(), s2.length()-s1.length());
                bs[0] = m_util.str.mk_string(s2);
                result = m_util.str.mk_prefix(m_util.str.mk_concat(as.size()-1, as.c_ptr()+1, sort_a),
                                              m_util.str.mk_concat(bs.size(), bs.c_ptr(), sort_a));
                TRACE("seq", tout << s1 << " " << s2 << " " << result << "\n";);
                return BR_REWRITE_FULL;
            }
            else {
                result = m().mk_false();
                TRACE("seq", tout << s1 << " " << s2 << " " << result << "\n";);
                return BR_DONE;
            }
        }
        else {
            if (s2.prefixof(s1)) {
                if (b == b1) {
                    result = m().mk_false();
                    TRACE("seq", tout << s1 << " " << s2 << " " << result << "\n";);
                    return BR_DONE;
                }
                m_util.str.get_concat(a, as);
                m_util.str.get_concat(b, bs);
                SASSERT(bs.size() > 1);
                s1 = s1.extract(s2.length(), s1.length() - s2.length());
                as[0] = m_util.str.mk_string(s1);
                result = m_util.str.mk_prefix(m_util.str.mk_concat(as.size(), as.c_ptr(), sort_a),
                                              m_util.str.mk_concat(bs.size()-1, bs.c_ptr()+1, sort_a));
                TRACE("seq", tout << s1 << " " << s2 << " " << result << "\n";);
                return BR_REWRITE_FULL;                
            }
            else {
                result = m().mk_false();
                TRACE("seq", tout << s1 << " " << s2 << " " << result << "\n";);
                return BR_DONE;
            }
        }        
    }
    m_util.str.get_concat_units(a, as);
    m_util.str.get_concat_units(b, bs);
    unsigned i = 0;
    expr_ref_vector eqs(m());
    for (; i < as.size() && i < bs.size(); ++i) {
        expr* ai = as.get(i), *bi = bs.get(i);
        if (m().are_equal(ai, bi)) {
            continue;
        }
        if (m().are_distinct(ai, bi)) {
            result = m().mk_false();
            return BR_DONE;
        }
        if (m_util.str.is_unit(ai) && m_util.str.is_unit(bi)) {
            eqs.push_back(m().mk_eq(ai, bi));
            continue;
        }
        break;
    }
    if (i == as.size()) {
        result = mk_and(eqs);
        TRACE("seq", tout << result << "\n";);
        return BR_REWRITE3;
    }
    SASSERT(i < as.size());
    if (i == bs.size()) {
        for (unsigned j = i; j < as.size(); ++j) {
            eqs.push_back(m_util.str.mk_is_empty(as.get(j)));
        }
        result = mk_and(eqs);
        TRACE("seq", tout << result << "\n";);
        return BR_REWRITE3;
    }
    if (i > 0) {
        SASSERT(i < as.size() && i < bs.size());
        a = m_util.str.mk_concat(as.size() - i, as.c_ptr() + i, sort_a);
        b = m_util.str.mk_concat(bs.size() - i, bs.c_ptr() + i, sort_a); 
        eqs.push_back(m_util.str.mk_prefix(a, b));
        result = mk_and(eqs);
        TRACE("seq", tout << result << "\n";);
        return BR_REWRITE3;
    }

    return BR_FAILED;    
}

br_status seq_rewriter::mk_seq_suffix(expr* a, expr* b, expr_ref& result) {
    if (a == b) {
        result = m().mk_true();
        return BR_DONE;
    }
    zstring s1, s2;
    sort* sort_a = m().get_sort(a);
    if (m_util.str.is_empty(a)) {
        result = m().mk_true();
        return BR_DONE;
    }
    if (m_util.str.is_empty(b)) {
        result = m_util.str.mk_is_empty(a);
        return BR_REWRITE3;
    }
    
    expr_ref_vector as(m()), bs(m()), eqs(m());
    m_util.str.get_concat_units(a, as);
    m_util.str.get_concat_units(b, bs);
    unsigned i = 1, sza = as.size(), szb = bs.size();
    for (; i <= sza && i <= szb; ++i) {
        expr* ai = as.get(sza-i), *bi = bs.get(szb-i);
        if (m().are_equal(ai, bi)) {
            continue;
        }
        if (m().are_distinct(ai, bi)) {
            result = m().mk_false();
            return BR_DONE;
        }
        if (m_util.str.is_unit(ai) && m_util.str.is_unit(bi)) {
            eqs.push_back(m().mk_eq(ai, bi));
            continue;
        }
        break;
    }
    if (i > sza) {
        result = mk_and(eqs);
        TRACE("seq", tout << result << "\n";);
        return BR_REWRITE3;
    }
    if (i > szb) {
        for (unsigned j = i; j <= sza; ++j) {
            expr* aj = as.get(sza-j);
            eqs.push_back(m_util.str.mk_is_empty(aj));
        }
        result = mk_and(eqs);
        TRACE("seq", tout << result << "\n";);
        return BR_REWRITE3;
    }

    if (i > 1) {
        SASSERT(i <= sza && i <= szb);
        a = m_util.str.mk_concat(sza - i + 1, as.c_ptr(), sort_a);
        b = m_util.str.mk_concat(szb - i + 1, bs.c_ptr(), sort_a);
        eqs.push_back(m_util.str.mk_suffix(a, b));
        result = mk_and(eqs);
        TRACE("seq", tout << result << "\n";);
        return BR_REWRITE3;
    }

    return BR_FAILED;
}

br_status seq_rewriter::mk_str_units(func_decl* f, expr_ref& result) {
    zstring s;
    VERIFY(m_util.str.is_string(f, s));
    expr_ref_vector es(m());
    unsigned sz = s.length();
    for (unsigned j = 0; j < sz; ++j) {
        es.push_back(m_util.str.mk_unit(m_util.str.mk_char(s, j)));
    }        
    result = m_util.str.mk_concat(es, f->get_range());    
    return BR_DONE;
}

br_status seq_rewriter::mk_str_le(expr* a, expr* b, expr_ref& result) {
    result = m().mk_not(m_util.str.mk_lex_lt(b, a));
    return BR_REWRITE2;
}

br_status seq_rewriter::mk_str_lt(expr* a, expr* b, expr_ref& result) {
    zstring as, bs;
    if (m_util.str.is_string(a, as) && m_util.str.is_string(b, bs)) {
        unsigned sz = std::min(as.length(), bs.length());
        for (unsigned i = 0; i < sz; ++i) {
            if (as[i] < bs[i]) {
                result = m().mk_true();
                return BR_DONE;
            }
            if (as[i] > bs[i]) {
                result = m().mk_false();
                return BR_DONE;
            }
        }
        result = m().mk_bool_val(as.length() < bs.length());
        return BR_DONE;
    }
    return BR_FAILED;
}

br_status seq_rewriter::mk_str_from_code(expr* a, expr_ref& result) {
    rational r;
    if (m_autil.is_numeral(a, r)) {
        if (r.is_neg() || r > zstring::max_char()) {
            result = m_util.str.mk_string(symbol(""));
        }
        else {
            unsigned num = r.get_unsigned();
            zstring s(1, &num);
            result = m_util.str.mk_string(s);
        }
        return BR_DONE;
    }
    return BR_FAILED;
}

br_status seq_rewriter::mk_str_to_code(expr* a, expr_ref& result) {
    zstring str;
    if (m_util.str.is_string(a, str)) {
        if (str.length() == 1) 
            result = m_autil.mk_int(str[0]);
        else
            result = m_autil.mk_int(-1);
        return BR_DONE;
    }    
    return BR_FAILED;
}

br_status seq_rewriter::mk_str_is_digit(expr* a, expr_ref& result) {
    zstring str;
    if (m_util.str.is_string(a, str)) {
        if (str.length() == 1 && '0' <= str[0] && str[0] <= '9')
            result = m().mk_true();
        else
            result = m().mk_false();
        return BR_DONE;
    }
    if (m_util.str.is_empty(a)) {
        result = m().mk_false();
        return BR_DONE;
    }
    // when a has length > 1 -> false
    // when a is a unit character -> evaluate
   
    return BR_FAILED;
}


br_status seq_rewriter::mk_str_itos(expr* a, expr_ref& result) {
    rational r;
    if (m_autil.is_numeral(a, r)) {
        if (r.is_int() && !r.is_neg()) {
            result = m_util.str.mk_string(symbol(r.to_string().c_str()));
        }
        else {
            result = m_util.str.mk_string(symbol(""));
        }
        return BR_DONE;
    }
    return BR_FAILED;
}

/**
   \brief rewrite str.to.int according to the rules:
   - if the expression is a string which is a non-empty 
     sequence of digits 0-9 extract the corresponding numeral.
   - if the expression is a string that contains any other character 
     or is empty, produce -1
   - if the expression is int.to.str(x) produce
      ite(x >= 0, x, -1)
     
*/
br_status seq_rewriter::mk_str_stoi(expr* a, expr_ref& result) {
    zstring str;
    if (m_util.str.is_string(a, str)) {
        std::string s = str.encode();
        if (s.length() == 0) {
            result = minus_one();
            return BR_DONE;
        } 
        for (unsigned i = 0; i < s.length(); ++i) {
            if (!('0' <= s[i] && s[i] <= '9')) {
                result = minus_one();
                return BR_DONE;
            }
        }
        rational r(s.c_str());
        result = m_autil.mk_numeral(r, true);
        return BR_DONE;
    }
    expr* b;
    if (m_util.str.is_itos(a, b)) {
        result = m().mk_ite(m_autil.mk_ge(b, zero()), b, minus_one());
        return BR_DONE;
    }
    
    expr* c = nullptr, *t = nullptr, *e = nullptr;
    if (m().is_ite(a, c, t, e)) {
        result = m().mk_ite(c, m_util.str.mk_stoi(t), m_util.str.mk_stoi(e));
        return BR_REWRITE_FULL;
    }

    expr* u = nullptr;
    unsigned ch = 0;
    if (m_util.str.is_unit(a, u) && m_util.is_const_char(u, ch)) {
        if ('0' <= ch && ch <= '9') {
            result = m_autil.mk_int(ch - '0');
        }
        else {
            result = m_autil.mk_int(-1);
        }
        return BR_DONE;
    }        

    expr_ref_vector as(m());
    m_util.str.get_concat_units(a, as);
    if (as.empty()) {
        result = m_autil.mk_int(-1);
        return BR_DONE;
    }
    if (m_util.str.is_unit(as.back())) {
        // if head = "" then tail else
        // if tail < 0 then tail else 
        // if stoi(head) >= 0 and then stoi(head)*10+tail else -1
        expr_ref tail(m_util.str.mk_stoi(as.back()), m());
        expr_ref head(m_util.str.mk_concat(as.size() - 1, as.c_ptr(), m().get_sort(a)), m());
        expr_ref stoi_head(m_util.str.mk_stoi(head), m());
        result = m().mk_ite(m_autil.mk_ge(stoi_head, m_autil.mk_int(0)), 
                            m_autil.mk_add(m_autil.mk_mul(m_autil.mk_int(10), stoi_head), tail),
                            m_autil.mk_int(-1));
        
        result = m().mk_ite(m_autil.mk_ge(tail, m_autil.mk_int(0)), 
                            result,
                            tail);
        result = m().mk_ite(m_util.str.mk_is_empty(head), 
                            tail,
                            result);
        return BR_REWRITE_FULL;
    }

    return BR_FAILED;
}

void seq_rewriter::add_next(u_map<expr*>& next, expr_ref_vector& trail, unsigned idx, expr* cond) {
    expr* acc;
    if (!m().is_true(cond) && next.find(idx, acc)) {              
        expr* args[2] = { cond, acc };
        cond = mk_or(m(), 2, args);
    }
    trail.push_back(cond);
    next.insert(idx, cond);   

}

bool seq_rewriter::is_sequence(eautomaton& aut, expr_ref_vector& seq) {
    seq.reset();
    unsigned state = aut.init();
    uint_set visited;
    eautomaton::moves mvs;
    unsigned_vector states;
    aut.get_epsilon_closure(state, states);
    bool has_final = false;
    for (unsigned i = 0; !has_final && i < states.size(); ++i) {
        has_final = aut.is_final_state(states[i]);
    }
    aut.get_moves_from(state, mvs, true);       
    while (!has_final) {
        if (mvs.size() != 1) {
            return false;
        }
        if (visited.contains(state)) {
            return false;
        }
        if (aut.is_final_state(mvs[0].src())) {
            return false;
        }
        visited.insert(state);
        sym_expr* t = mvs[0].t();
        if (!t || !t->is_char()) {
            return false;
        }
        seq.push_back(m_util.str.mk_unit(t->get_char()));
        state = mvs[0].dst();
        mvs.reset();
        aut.get_moves_from(state, mvs, true);
        states.reset();
        has_final = false;
        aut.get_epsilon_closure(state, states);
        for (unsigned i = 0; !has_final && i < states.size(); ++i) {
            has_final = aut.is_final_state(states[i]);
        }
    }
    return mvs.empty();
}

bool seq_rewriter::is_sequence(expr* e, expr_ref_vector& seq) {
    seq.reset();
    zstring s;
    ptr_vector<expr> todo;
    expr *e1, *e2;
    todo.push_back(e);
    while (!todo.empty()) {
        e = todo.back();
        todo.pop_back();
        if (m_util.str.is_string(e, s)) {
            for (unsigned i = 0; i < s.length(); ++i) {
                seq.push_back(m_util.str.mk_char(s, i));
            }
        }
        else if (m_util.str.is_empty(e)) {
            continue;
        }
        else if (m_util.str.is_unit(e, e1)) {
            seq.push_back(e1);
        }
        else if (m_util.str.is_concat(e, e1, e2)) {
            todo.push_back(e2);
            todo.push_back(e1);
        }
        else {
            return false;
        }
    }
    return true;
}

bool seq_rewriter::get_head_tail(expr* s, expr_ref& head, expr_ref& tail) {
    expr* h = nullptr, *t = nullptr;
    zstring s1;
    if (m_util.str.is_unit(s, h)) {
        head = h;
        tail = m_util.str.mk_empty(m().get_sort(s));
        return true;
    }
    if (m_util.str.is_string(s, s1) && s1.length() > 0) {
        head = m_util.mk_char(s1[0]);
        tail = m_util.str.mk_string(s1.extract(1, s1.length()));
        return true;    
    }
    if (m_util.str.is_concat(s, h, t) && get_head_tail(h, head, tail)) {
        tail = m_util.str.mk_concat(tail, t);
        return true;
    }
    return false;
}

expr_ref seq_rewriter::kleene_and(expr* cond, expr* r) {
    if (m().is_true(cond))
        return expr_ref(r, m());    
    expr* re_empty = re().mk_empty(m().get_sort(r));
    if (m().is_false(cond))
        return expr_ref(re_empty, m());
    return expr_ref(m().mk_ite(cond, r, re_empty), m());
}

expr_ref seq_rewriter::kleene_predicate(expr* cond, sort* seq_sort) {
    expr_ref re_with_empty(re().mk_to_re(m_util.str.mk_empty(seq_sort)), m());
    return kleene_and(cond, re_with_empty);
}

expr_ref seq_rewriter::is_nullable(expr* r) {
    SASSERT(m_util.is_re(r));
    expr* r1 = nullptr, *r2 = nullptr;
    unsigned lo = 0, hi = 0;
    expr_ref result(m());
    if (re().is_concat(r, r1, r2) ||
        re().is_intersection(r, r1, r2)) {
        result = mk_and(m(), is_nullable(r1), is_nullable(r2));
    }
    else if (re().is_union(r, r1, r2)) {
        result = mk_or(m(), is_nullable(r1), is_nullable(r2));
    }
    else if (re().is_diff(r, r1, r2)) {
        result = mk_not(m(), is_nullable(r2));
        result = mk_and(m(), is_nullable(r1), result);
    }
    else if (re().is_star(r) || 
        re().is_opt(r) ||
        re().is_full_seq(r) ||
        (re().is_loop(r, r1, lo) && lo == 0) || 
        (re().is_loop(r, r1, lo, hi) && lo == 0)) {
        result = m().mk_true();
    }
    else if (re().is_full_char(r) ||
        re().is_empty(r) ||
        re().is_of_pred(r) ||
        re().is_range(r)) {
        result = m().mk_false();
    }
    else if (re().is_plus(r, r1) ||
        (re().is_loop(r, r1, lo) && lo > 0) ||
        (re().is_loop(r, r1, lo, hi) && lo > 0)) {
        result = is_nullable(r1);
    }
    else if (re().is_complement(r, r1)) {
        result = mk_not(m(), is_nullable(r1));
    }
    else if (re().is_to_re(r, r1)) {
        sort* seq_sort = nullptr;
        VERIFY(m_util.is_re(r, seq_sort));
        expr* emptystr = m_util.str.mk_empty(seq_sort);
        result = m().mk_eq(emptystr, r1);
    }
    else {
        sort* seq_sort = nullptr;
        VERIFY(m_util.is_re(r, seq_sort));
        result = re().mk_in_re(m_util.str.mk_empty(seq_sort), r);
    }
    return result;
}

/*
    Symbolic derivative
    Evaluates recursively.
    Returns null expression `expr_ref(m())` on failure.
*/
expr_ref seq_rewriter::derivative(expr* elem, expr* r) {
    sort* seq_sort = nullptr, *elem_sort = nullptr;
    VERIFY(m_util.is_re(r, seq_sort));
    VERIFY(m_util.is_seq(seq_sort, elem_sort));
    SASSERT(elem_sort == m().get_sort(elem));
    expr* r1 = nullptr, * r2 = nullptr, *p = nullptr;
    expr_ref dr1(m()), dr2(m()), result(m());
    unsigned lo = 0, hi = 0;
    if (re().is_concat(r, r1, r2)) {
        expr_ref is_n = is_nullable(r1);
        dr1 = derivative(elem, r1);
        if (!dr1) {
            result = dr1; // failed
        }
        else if (m().is_false(is_n)) {
            result = re().mk_concat(dr1, r2);
        }
        else {
            dr2 = derivative(elem, r2);
            if (!dr2) {
                result = dr2; // failed
            }
            else if (m().is_true(is_n)) {
                result = re().mk_union(
                    re().mk_concat(dr1, r2),
                    dr2
                );
            }
            else {
                result = re().mk_union(
                    re().mk_concat(dr1, r2),
                    kleene_and(is_n, dr2)
                );
            }
        }
    }
    else if (re().is_star(r, r1)) {
        result = derivative(elem, r1);
        if (result) {
            result = re().mk_concat(result, r);
        }
    }
    else if (re().is_plus(r, r1)) {
        result = re().mk_star(r1);
        result = derivative(elem, result);
    }
    else if (re().is_union(r, r1, r2)) {
        dr1 = derivative(elem, r1);
        dr2 = derivative(elem, r2);
        if (dr1 && dr2) {
            result = re().mk_union(dr1, dr2);
        }
    }
    else if (re().is_intersection(r, r1, r2)) {
        dr1 = derivative(elem, r1);
        dr2 = derivative(elem, r2);
        if (dr1 && dr2) {
            result = re().mk_inter(dr1, dr2);
        }
    }
    else if (re().is_opt(r, r1)) {
        result = derivative(elem, r1);
    }
    else if (re().is_complement(r, r1)) {
        result = derivative(elem, r1);
        if (result) {
            result = re().mk_complement(result);
        }
    }
    else if (re().is_loop(r, r1, lo)) {
        result = derivative(elem, r1);
        if (result) {
            if (lo > 0) {
                lo--;
            }
            result = re().mk_concat(
                result,
                re().mk_loop(r1, lo)
            );
        }
    }
    else if (re().is_loop(r, r1, lo, hi)) {
        if (hi == 0) {
            result = re().mk_empty(m().get_sort(r));
        }
        else {
            result = derivative(elem, r1);
            if (result) {
                hi--;
                if (lo > 0) {
                    lo--;
                }
                result = re().mk_concat(
                    result,
                    re().mk_loop(r1, lo, hi)
                );
            }
        }
    }
    else if (re().is_full_seq(r) ||
             re().is_empty(r)) {
        result = r;
    }
    else if (re().is_to_re(r, r1)) {
        // r1 is a string here (not a regexp)
        expr_ref hd(m());
        expr_ref tl(m());
        if (get_head_tail(r1, hd, tl)) {
            // head must be equal; if so, derivative is tail
            result = kleene_and(
                m().mk_eq(elem, hd),
                re().mk_to_re(tl)
            );
        }
        else if (m_util.str.is_empty(r1)) {
            result = re().mk_empty(m().get_sort(r));
        }
    }
    else if (re().is_range(r, r1, r2)) {
        // r1, r2 are sequences.
        zstring s1, s2;
        if (m_util.str.is_string(r1, s1) && m_util.str.is_string(r2, s2)) {
            if (s1.length() == 1 && s2.length() == 1) {
                r1 = m_util.mk_char(s1[0]);
                r2 = m_util.mk_char(s2[0]);
                result = m().mk_and(m_util.mk_le(r1, elem), m_util.mk_le(elem, r2));
                result = kleene_predicate(result, seq_sort);
            }
            else {
                result = re().mk_empty(m().get_sort(r));
            }
        }
    }
    else if (re().is_full_char(r)) {
        result = re().mk_to_re(m_util.str.mk_empty(seq_sort));
    }
    else if (re().is_of_pred(r, p)) {
        array_util array(m());
        expr* args[2] = { p, elem };
        result = array.mk_select(2, args);
        result = kleene_predicate(result, seq_sort);
    }
    return result;
}

br_status seq_rewriter::mk_str_in_regexp(expr* a, expr* b, expr_ref& result) {

    if (re().is_empty(b)) {
        result = m().mk_false();
        return BR_DONE;
    }
    if (re().is_full_seq(b)) {
        result = m().mk_true();
        return BR_DONE;
    }
    expr* b1 = nullptr;
    if (re().is_to_re(b, b1)) {
        result = m().mk_eq(a, b1);
        return BR_REWRITE1;
    }
    if (m_util.str.is_empty(a)) {
        result = is_nullable(b);
        if (m_util.str.is_in_re(result))
            return BR_DONE;
        else
            return BR_REWRITE_FULL;
    }

    expr_ref hd(m()), tl(m());
    if (get_head_tail(a, hd, tl)) {
        expr_ref db = derivative(hd, b); // null if failed
        if (db) {
            result = re().mk_in_re(tl, db);
            return BR_REWRITE_FULL;
        }
    }

    return BR_FAILED; 
}

br_status seq_rewriter::mk_str_to_regexp(expr* a, expr_ref& result) {
    return BR_FAILED;
}

br_status seq_rewriter::mk_re_concat(expr* a, expr* b, expr_ref& result) {
    if (re().is_full_seq(a) && re().is_full_seq(b)) {
        result = a;
        return BR_DONE;
    }
    if (re().is_empty(a)) {
        result = a;
        return BR_DONE;
    }
    if (re().is_empty(b)) {
        result = b;
        return BR_DONE;
    }
    if (is_epsilon(a)) {
        result = b;
        return BR_DONE;
    }
    if (is_epsilon(b)) {
        result = a;
        return BR_DONE;
    }
    expr* a1 = nullptr, *b1 = nullptr;
    if (re().is_to_re(a, a1) && re().is_to_re(b, b1)) {
        result = re().mk_to_re(m_util.str.mk_concat(a1, b1));
        return BR_REWRITE2;
    }
    if (re().is_star(a, a1) && re().is_star(b, b1) && a1 == b1) {
        result = a;
        return BR_DONE;
    }
    if (re().is_star(a, a1) && a1 == b) {
        result = re().mk_concat(b, a);
        return BR_DONE;
    }
    unsigned lo1, hi1, lo2, hi2;

    if (re().is_loop(a, a1, lo1, hi1) && lo1 <= hi1 && re().is_loop(b, b1, lo2, hi2) && lo2 <= hi2 && a1 == b1) {
        result = re().mk_loop(a1, lo1 + lo2, hi1 + hi2);
        return BR_DONE;
    }
    if (re().is_loop(a, a1, lo1) && re().is_loop(b, b1, lo2) && a1 == b1) {
        result = re().mk_loop(a1, lo1 + lo2);
        return BR_DONE;
    }
    for (unsigned i = 0; i < 2; ++i) {
        // (loop a lo1) + (loop a lo2 hi2) = (loop a lo1 + lo2) 
        if (re().is_loop(a, a1, lo1) && re().is_loop(b, b1, lo2, hi2) && lo2 <= hi2 && a1 == b1) {
            result = re().mk_loop(a1, lo1 + lo2);
            return BR_DONE;
        }
        // (loop a lo1 hi1) + a* = (loop a lo1)
        if (re().is_loop(a, a1, lo1, hi1) && re().is_star(b, b1) && a1 == b1) {
            result = re().mk_loop(a1, lo1);
            return BR_DONE;
        }
        // (loop a lo1) + a* = (loop a lo1)
        if (re().is_loop(a, a1, lo1) && re().is_star(b, b1) && a1 == b1) {
            result = a;
            return BR_DONE;
        }
        // (loop a lo1 hi1) + a = (loop a lo1+1 hi1+1)
        if (re().is_loop(a, a1, lo1, hi1) && lo1 <= hi1 && a1 == b) {
            result = re().mk_loop(a1, lo1+1, hi1+1);
            return BR_DONE;
        }
        std::swap(a, b);
    }
    return BR_FAILED;
}
/*
  (a + a) = a
  (a + eps) = a
  (eps + a) = a
*/
br_status seq_rewriter::mk_re_union(expr* a, expr* b, expr_ref& result) {
    if (a == b) {
        result = a;
        return BR_DONE;
    }
    if (re().is_empty(a)) {
        result = b;
        return BR_DONE;
    }
    if (re().is_empty(b)) {
        result = a;
        return BR_DONE;
    }
    if (re().is_full_seq(a)) {
        result = a;
        return BR_DONE;
    }
    if (re().is_full_seq(b)) {
        result = b;
        return BR_DONE;
    }
    if (re().is_star(a) && is_epsilon(b)) {
        result = a;
        return BR_DONE;
    }
    if (re().is_star(b) && is_epsilon(a)) {
        result = b;
        return BR_DONE;
    }
    return BR_FAILED;
}

br_status seq_rewriter::mk_re_complement(expr* a, expr_ref& result) {
    expr* e1, *e2;
    if (re().is_intersection(a, e1, e2)) {
        result = re().mk_union(re().mk_complement(e1), re().mk_complement(e2));
        return BR_REWRITE2;
    }
    if (re().is_union(a, e1, e2)) {
        result = re().mk_inter(re().mk_complement(e1), re().mk_complement(e2));
        return BR_REWRITE2;
    }
    if (re().is_empty(a)) {
        result = re().mk_full_seq(m().get_sort(a));
        return BR_DONE;
    }
    if (re().is_full_seq(a)) {
        result = re().mk_empty(m().get_sort(a));
        return BR_DONE;
    }
    return BR_FAILED;
}

/**
   (emp n r) = emp
   (r n emp) = emp
   (all n r) = r
   (r n all) = r
   (r n r) = r
 */
br_status seq_rewriter::mk_re_inter(expr* a, expr* b, expr_ref& result) {
    if (a == b) {
        result = a;
        return BR_DONE;
    }
    if (re().is_empty(a)) {
        result = a;
        return BR_DONE;
    }
    if (re().is_empty(b)) {
        result = b;
        return BR_DONE;
    }
    if (re().is_full_seq(a)) {
        result = b;
        return BR_DONE;
    }
    if (re().is_full_seq(b)) {
        result = a;
        return BR_DONE;
    }
    expr* ac = nullptr, *bc = nullptr;
    if ((re().is_complement(a, ac) && ac == b) ||
        (re().is_complement(b, bc) && bc == a)) {
        result = re().mk_empty(m().get_sort(a));
        return BR_DONE;
    }
    if (re().is_to_re(b)) 
        std::swap(a, b);
    expr* s = nullptr;
    if (re().is_to_re(a, s)) {
        result = m().mk_ite(re().mk_in_re(s, b), a, re().mk_empty(m().get_sort(a)));
        return BR_REWRITE2;
    }
    return BR_FAILED;
}

br_status seq_rewriter::mk_re_diff(expr* a, expr* b, expr_ref& result) {
    result = re().mk_inter(a, re().mk_complement(b));
    return BR_REWRITE2;
}


br_status seq_rewriter::mk_re_loop(func_decl* f, unsigned num_args, expr* const* args, expr_ref& result) {
    rational n1, n2;
    unsigned lo, hi, lo2, hi2, np;
    expr* a = nullptr;
    switch (num_args) {
    case 1: 
        np = f->get_num_parameters();
        lo2 = np > 0 ? f->get_parameter(0).get_int() : 0;
        hi2 = np > 1 ? f->get_parameter(1).get_int() : lo2;
        // (loop a 0 0) = ""
        if  (np == 2 && lo2 > hi2) {
            result = re().mk_empty(m().get_sort(args[0]));
            return BR_DONE;
        }
        if (np == 2 && hi2 == 0) {
            result = re().mk_to_re(m_util.str.mk_empty(re().to_seq(m().get_sort(args[0]))));
            return BR_DONE;
        }
        // (loop (loop a lo) lo2) = (loop lo*lo2)
        if (re().is_loop(args[0], a, lo) && np == 1) {
            result = re().mk_loop(a, lo2 * lo);
            return BR_REWRITE1;
        }
        // (loop (loop a l l) h h) = (loop a l*h l*h)
        if (re().is_loop(args[0], a, lo, hi) && np == 2 && lo == hi && lo2 == hi2) {
            result = re().mk_loop(a, lo2 * lo, hi2 * hi);
            return BR_REWRITE1;
        }
        // (loop a 1 1) = a
        if (np == 2 && lo2 == 1 && hi2 == 1) {
            result = args[0];
            return BR_DONE;
        }
        // (loop a 0) = a*
        if (np == 1 && lo2 == 0) {
            result = re().mk_star(args[0]);
            return BR_DONE;
        }
        break;
    case 2:
        if (m_autil.is_numeral(args[1], n1) && n1.is_unsigned()) {
            result = re().mk_loop(args[0], n1.get_unsigned());
            return BR_REWRITE1;
        }
        break;
    case 3:
        if (m_autil.is_numeral(args[1], n1) && n1.is_unsigned() &&
            m_autil.is_numeral(args[2], n2) && n2.is_unsigned()) {
            result = re().mk_loop(args[0], n1.get_unsigned(), n2.get_unsigned());
            return BR_REWRITE1;
        }
        break;
    default:
        break;
    }
    return BR_FAILED;
}

br_status seq_rewriter::mk_re_power(func_decl* f, expr* a, expr_ref& result) {
    unsigned p = f->get_parameter(0).get_int();
    result = re().mk_loop(a, p, p);
    return BR_REWRITE1;
}


/*
  a** = a*
  (a* + b)* = (a + b)*
  (a + b*)* = (a + b)*
  (a*b*)*   = (a + b)*
   a+* = a*
   emp* = ""
   all* = all   
*/
br_status seq_rewriter::mk_re_star(expr* a, expr_ref& result) {
    expr* b, *c, *b1, *c1;
    if (re().is_star(a) || re().is_full_seq(a)) {
        result = a;
        return BR_DONE;
    }
    if (re().is_full_char(a)) {
        result = re().mk_full_seq(m().get_sort(a));
        return BR_DONE;
    }
    if (re().is_empty(a)) {
        sort* seq_sort = nullptr;
        VERIFY(m_util.is_re(a, seq_sort));
        result = re().mk_to_re(m_util.str.mk_empty(seq_sort));
        return BR_DONE;
    }
    if (re().is_plus(a, b)) {
        result = re().mk_star(b);
        return BR_DONE;
    }
    if (re().is_union(a, b, c)) {
        if (re().is_star(b, b1)) {
            result = re().mk_star(re().mk_union(b1, c));
            return BR_REWRITE2;
        }
        if (re().is_star(c, c1)) {
            result = re().mk_star(re().mk_union(b, c1));
            return BR_REWRITE2;
        }
        if (is_epsilon(b)) {
            result = re().mk_star(c);
            return BR_REWRITE2;
        }
        if (is_epsilon(c)) {
            result = re().mk_star(b);
            return BR_REWRITE2;
        }
    }
    if (re().is_concat(a, b, c) &&
        re().is_star(b, b1) && re().is_star(c, c1)) {
        result = re().mk_star(re().mk_union(b1, c1));
        return BR_REWRITE2;
    }

    return BR_FAILED;
}

/*
 * (re.range c_1 c_n) 
 */
br_status seq_rewriter::mk_re_range(expr* lo, expr* hi, expr_ref& result) {
    return BR_FAILED;
}

/*
   emp+ = emp
   all+ = all
   a*+ = a*
   a++ = a+
   a+ = aa*
*/
br_status seq_rewriter::mk_re_plus(expr* a, expr_ref& result) {
    if (re().is_empty(a)) {
        result = a;
        return BR_DONE;
    }
    if (re().is_full_seq(a)) {
        result = a;
        return BR_DONE;
    }
    if (is_epsilon(a)) {
        result = a;
        return BR_DONE;
    }
    if (re().is_plus(a)) {
        result = a;
        return BR_DONE;
    }
    if (re().is_star(a)) {
        result = a;
        return BR_DONE;
    }

    result = re().mk_concat(a, re().mk_star(a));
    return BR_REWRITE2;
}

br_status seq_rewriter::mk_re_opt(expr* a, expr_ref& result) {
    sort* s = nullptr;
    VERIFY(m_util.is_re(a, s));
    result = re().mk_union(re().mk_to_re(m_util.str.mk_empty(s)), a);
    return BR_REWRITE1;
}

bool seq_rewriter::has_cofactor(expr* r, expr_ref& cond, expr_ref& th, expr_ref& el) {
    expr_ref_vector trail(m()), args_th(m()), args_el(m());
    expr* c = nullptr, *tt = nullptr, *ee = nullptr;
    obj_map<expr,expr*> cache_th, cache_el;
    expr_mark no_cofactor, visited;
    ptr_vector<expr> todo;
    todo.push_back(r);
    while (!todo.empty()) {
        expr* e = todo.back();
        if (visited.is_marked(e) || !is_app(e)) {
            todo.pop_back();
            continue;
        }        
        app* a = to_app(e);
        if (m().is_ite(e, c, tt, ee)) {
            if (!cond) {
                cond = c;
                cache_th.insert(a, tt);
                cache_el.insert(a, ee);
            }
            else if (cond == c) {
                cache_th.insert(a, tt);
                cache_el.insert(a, ee);
            }
            else {
                no_cofactor.mark(a);
            }
            visited.mark(e, true);
            todo.pop_back();
            continue;
        }

        if (a->get_family_id() != u().get_family_id()) {
            visited.mark(e, true);
            no_cofactor.mark(e, true);
            todo.pop_back();
            continue;
        }
        switch (a->get_decl_kind()) {
        case OP_RE_CONCAT:
        case OP_RE_UNION:
        case OP_RE_INTERSECT:
        case OP_RE_COMPLEMENT:
            break;
        case OP_RE_STAR:
        case OP_RE_LOOP:
        default:
            visited.mark(e, true);
            no_cofactor.mark(e, true);
            continue;
        }
        args_th.reset(); 
        args_el.reset();
        bool has_cof = false;
        for (expr* arg : *a) {
            if (no_cofactor.is_marked(arg)) {
                args_th.push_back(arg);
                args_el.push_back(arg);
            }
            else if (cache_th.contains(arg)) {
                args_th.push_back(cache_th[arg]);
                args_el.push_back(cache_el[arg]);
                has_cof = true;
            }
            else {
                todo.push_back(arg);
            }
        }
        if (args_th.size() == a->get_num_args()) {
            if (has_cof) {
                th = m().mk_app(a->get_decl(), args_th);
                el = m().mk_app(a->get_decl(), args_el);
                trail.push_back(th);
                trail.push_back(el);
                cache_th.insert(a, th);
                cache_el.insert(a, el);
            }
            else {
                no_cofactor.mark(a, true);
            }
            visited.mark(e, true);
            todo.pop_back();
        }
    }
    SASSERT(cond == !no_cofactor.is_marked(r));
    if (cond) {
        th = cache_th[r];
        el = cache_el[r];
        return true;
    }
    else {
        return false;
    }
}

    
br_status seq_rewriter::reduce_re_is_empty(expr* r, expr_ref& result) {
    expr* r1, *r2, *r3, *r4;
    zstring s1, s2;
    unsigned lo, hi;
    auto eq_empty = [&](expr* r) { return m().mk_eq(r, re().mk_empty(m().get_sort(r))); };
    if (re().is_union(r, r1, r2)) {
        result = m().mk_and(eq_empty(r1), eq_empty(r2));
        return BR_REWRITE2;
    }
    if (re().is_star(r) ||
        re().is_to_re(r) ||
        re().is_full_char(r) ||
        re().is_full_seq(r)) {
        result = m().mk_false();
        return BR_DONE;
    }
    if (re().is_concat(r, r1, r2)) {
        result = m().mk_or(eq_empty(r1), eq_empty(r2));
        return BR_REWRITE2;
    }
    else if (re().is_range(r, r1, r2) && 
             m_util.str.is_string(r1, s1) && m_util.str.is_string(r2, s2) && 
             s1.length() == 1 && s2.length() == 1) {
        result = m().mk_bool_val(s1[0] <= s2[0]);
        return BR_DONE;
    }
    else if ((re().is_loop(r, r1, lo) ||
              re().is_loop(r, r1, lo, hi)) && lo == 0) {
        result = m().mk_false();
        return BR_DONE;
    }
    else if (re().is_loop(r, r1, lo) ||
             (re().is_loop(r, r1, lo, hi) && lo <= hi)) {
        result = eq_empty(r1);
        return BR_REWRITE1;
    }
    // Partial DNF expansion:
    else if (re().is_intersection(r, r1, r2) && re().is_union(r1, r3, r4)) {
        result = eq_empty(re().mk_union(re().mk_inter(r3, r2), re().mk_inter(r4, r2)));
        return BR_REWRITE3;
    }
    else if (re().is_intersection(r, r1, r2) && re().is_union(r2, r3, r4)) {
        result = eq_empty(re().mk_union(re().mk_inter(r3, r1), re().mk_inter(r4, r1)));
        return BR_REWRITE3;
    }
    return BR_FAILED;
}

br_status seq_rewriter::reduce_re_eq(expr* l, expr* r, expr_ref& result) {
    if (re().is_empty(l)) {
        std::swap(l, r);
    }
    if (re().is_empty(r)) {
        return reduce_re_is_empty(l, result);
    }
    return BR_FAILED;
}

br_status seq_rewriter::mk_eq_core(expr * l, expr * r, expr_ref & result) {
    expr_ref_vector res(m());
    expr_ref_pair_vector new_eqs(m());
    if (m_util.is_re(l)) {
        return reduce_re_eq(l, r, result);
    }
    bool changed = false;
    if (!reduce_eq(l, r, new_eqs, changed)) {
        result = m().mk_false();
        TRACE("seq_verbose", tout << result << "\n";);
        return BR_DONE;
    }
    if (!changed) {
        return BR_FAILED;
    }
    for (auto const& p : new_eqs) {
        res.push_back(m().mk_eq(p.first, p.second));
    }
    result = mk_and(res);
    TRACE("seq_verbose", tout << result << "\n";);
    return BR_REWRITE3;
}

void seq_rewriter::remove_empty_and_concats(expr_ref_vector& es) {
    unsigned j = 0;
    bool has_concat = false;
    for (expr* e : es) {
        has_concat |= m_util.str.is_concat(e);
        if (!m_util.str.is_empty(e))
            es[j++] = e;
    }
    es.shrink(j);
    if (has_concat) {
        expr_ref_vector fs(m());
        for (expr* e : es) 
            m_util.str.get_concat(e, fs);
        es.swap(fs);
    }
}

void seq_rewriter::remove_leading(unsigned n, expr_ref_vector& es) {
    SASSERT(n <= es.size());
    if (n == 0)
        return;
    for (unsigned i = n; i < es.size(); ++i) {
        es[i-n] = es.get(i);
    }
    es.shrink(es.size() - n);
}

bool seq_rewriter::reduce_back(expr_ref_vector& ls, expr_ref_vector& rs, expr_ref_pair_vector& new_eqs) {    
    expr* a, *b;    
    zstring s, s1, s2;
    while (true) {
        if (ls.empty() || rs.empty()) {
            break;
        }
        expr* l = ls.back();
        expr* r = rs.back();            
        if (m_util.str.is_unit(r) && m_util.str.is_string(l)) {
            std::swap(l, r);
            ls.swap(rs);
        }
        if (l == r) {
            ls.pop_back();
            rs.pop_back();
        }
        else if(m_util.str.is_unit(l, a) &&
                m_util.str.is_unit(r, b)) {
            if (m().are_distinct(a, b)) {
                return false;
            }
            new_eqs.push_back(a, b);
            ls.pop_back();
            rs.pop_back();
        }
        else if (m_util.str.is_unit(l, a) && m_util.str.is_string(r, s)) {
            SASSERT(s.length() > 0);
            
            app* ch = m_util.str.mk_char(s, s.length()-1);
            SASSERT(m().get_sort(ch) == m().get_sort(a));
            new_eqs.push_back(ch, a);
            ls.pop_back();
            if (s.length() == 1) {
                rs.pop_back();
            }
            else {
                expr_ref s2(m_util.str.mk_string(s.extract(0, s.length()-1)), m());
                rs[rs.size()-1] = s2;
            }
        }
        else if (m_util.str.is_string(l, s1) && m_util.str.is_string(r, s2)) {
            unsigned min_l = std::min(s1.length(), s2.length());
            for (unsigned i = 0; i < min_l; ++i) {
                if (s1[s1.length()-i-1] != s2[s2.length()-i-1]) {
                    return false;
                }
            }
            ls.pop_back();          
            rs.pop_back();
            if (min_l < s1.length()) {
                ls.push_back(m_util.str.mk_string(s1.extract(0, s1.length()-min_l)));
            }
            if (min_l < s2.length()) {
                rs.push_back(m_util.str.mk_string(s2.extract(0, s2.length()-min_l)));
            }        
        }
        else {
            break;
        }
    }
    return true;
}

bool seq_rewriter::reduce_front(expr_ref_vector& ls, expr_ref_vector& rs, expr_ref_pair_vector& new_eqs) {    
    expr* a, *b;    
    zstring s, s1, s2;
    unsigned head1 = 0, head2 = 0;
    while (true) {
        if (head1 == ls.size() || head2 == rs.size()) {
            break;
        }
        SASSERT(head1 < ls.size() && head2 < rs.size());

        expr* l = ls.get(head1);
        expr* r = rs.get(head2);
        if (m_util.str.is_unit(r) && m_util.str.is_string(l)) {
            std::swap(l, r);
            ls.swap(rs);
            std::swap(head1, head2);
        }
        if (l == r) {
            ++head1;
            ++head2;
        }
        else if(m_util.str.is_unit(l, a) &&
                m_util.str.is_unit(r, b)) {
            if (m().are_distinct(a, b)) {
                return false;
            }
            new_eqs.push_back(a, b);
            ++head1;
            ++head2;
        }
        else if (m_util.str.is_unit(l, a) && m_util.str.is_string(r, s)) {
            SASSERT(s.length() > 0);
            app* ch = m_util.str.mk_char(s, 0);
            SASSERT(m().get_sort(ch) == m().get_sort(a));
            new_eqs.push_back(ch, a);
            ++head1;
            if (s.length() == 1) {
                ++head2;
            }
            else {
                expr_ref s2(m_util.str.mk_string(s.extract(1, s.length()-1)), m());
                rs[head2] = s2;
            }            
        }
        else if (m_util.str.is_string(l, s1) &&
                 m_util.str.is_string(r, s2)) {
            TRACE("seq", tout << s1 << " - " << s2 << " " << s1.length() << " " << s2.length() << "\n";);
            unsigned min_l = std::min(s1.length(), s2.length());
            for (unsigned i = 0; i < min_l; ++i) {
                if (s1[i] != s2[i]) {
                    TRACE("seq", tout << "different at position " << i << " " << s1[i] << " " << s2[i] << "\n";);
                    return false;
                }
            }
            if (min_l == s1.length()) {
                ++head1;            
            }
            else {
                ls[head1] = m_util.str.mk_string(s1.extract(min_l, s1.length()-min_l));
            }
            if (min_l == s2.length()) {
                ++head2;            
            }
            else {
                rs[head2] = m_util.str.mk_string(s2.extract(min_l, s2.length()-min_l));
            }
        }
        else {
            break;
        }
    }
    remove_leading(head1, ls);
    remove_leading(head2, rs);
    return true;
}

/**
   \brief simplify equality ls = rs
   - New equalities are inserted into eqs.
   - Last remaining equalities that cannot be simplified further are kept in ls, rs
   - returns false if equality is unsatisfiable   
   - sets change to true if some simplification occurred
*/
bool seq_rewriter::reduce_eq(expr_ref_vector& ls, expr_ref_vector& rs, expr_ref_pair_vector& eqs, bool& change) {
    TRACE("seq_verbose", tout << ls << "\n"; tout << rs << "\n";);
    unsigned hash_l = ls.hash();
    unsigned hash_r = rs.hash();
    unsigned sz_eqs = eqs.size();
    remove_empty_and_concats(ls);
    remove_empty_and_concats(rs);
    return 
        reduce_back(ls, rs, eqs) && 
        reduce_front(ls, rs, eqs) &&
        reduce_itos(ls, rs, eqs) &&
        reduce_itos(rs, ls, eqs) &&
        reduce_by_length(ls, rs, eqs) &&
        reduce_subsequence(ls, rs, eqs) &&
        (change = (hash_l != ls.hash() || hash_r != rs.hash() || eqs.size() != sz_eqs), 
         true);
}

bool seq_rewriter::reduce_eq(expr* l, expr* r, expr_ref_pair_vector& new_eqs, bool& changed) {
    m_lhs.reset();
    m_rhs.reset();
    m_util.str.get_concat(l, m_lhs);
    m_util.str.get_concat(r, m_rhs);
    bool change = false;
    if (reduce_eq(m_lhs, m_rhs, new_eqs, change)) {
        if (!change) {
            new_eqs.push_back(l, r);
        }
        else {
            add_seqs(m_lhs, m_rhs, new_eqs);
        }
        changed |= change;
        return true;
    }
    else {
        TRACE("seq", tout << mk_bounded_pp(l, m()) << " != " << mk_bounded_pp(r, m()) << "\n";);
        return false;
    }
}

void seq_rewriter::add_seqs(expr_ref_vector const& ls, expr_ref_vector const& rs, expr_ref_pair_vector& eqs) {
    if (!ls.empty() || !rs.empty()) {
        sort * s = m().get_sort(ls.empty() ? rs[0] : ls[0]);
        eqs.push_back(m_util.str.mk_concat(ls, s), m_util.str.mk_concat(rs, s));
    }
}


bool seq_rewriter::reduce_contains(expr* a, expr* b, expr_ref_vector& disj) {
    m_lhs.reset();
    m_util.str.get_concat(a, m_lhs);
    TRACE("seq", tout << expr_ref(a, m()) << " " << expr_ref(b, m()) << "\n";);
    sort* sort_a = m().get_sort(a);
    zstring s;
    for (unsigned i = 0; i < m_lhs.size(); ++i) {
        expr* e = m_lhs.get(i);
        if (m_util.str.is_empty(e)) {
            continue;
        }

        if (m_util.str.is_string(e, s)) {
            unsigned sz = s.length();
            expr_ref_vector es(m());
            for (unsigned j = 0; j < sz; ++j) {
                es.push_back(m_util.str.mk_unit(m_util.str.mk_char(s, j)));
            }
            es.append(m_lhs.size() - i, m_lhs.c_ptr() + i);
            for (unsigned j = 0; j < sz; ++j) {
                disj.push_back(m_util.str.mk_prefix(b, m_util.str.mk_concat(es.size() - j, es.c_ptr() + j, sort_a)));
            }
            continue;
        }
        if (m_util.str.is_unit(e)) {
            disj.push_back(m_util.str.mk_prefix(b, m_util.str.mk_concat(m_lhs.size() - i, m_lhs.c_ptr() + i, sort_a)));
            continue;
        }

        if (m_util.str.is_string(b, s)) {
            expr* all = re().mk_full_seq(re().mk_re(m().get_sort(b)));
            disj.push_back(re().mk_in_re(m_util.str.mk_concat(m_lhs.size() - i, m_lhs.c_ptr() + i, sort_a),
                                              re().mk_concat(all, re().mk_concat(re().mk_to_re(b), all))));
            return true;
        }

        if (i == 0) {
            return false;
        }
        disj.push_back(m_util.str.mk_contains(m_util.str.mk_concat(m_lhs.size() - i, m_lhs.c_ptr() + i, sort_a), b));
        return true;
    }
    disj.push_back(m_util.str.mk_is_empty(b));
    return true;
}


expr* seq_rewriter::concat_non_empty(expr_ref_vector& es) {
    sort* s = m().get_sort(es.get(0));
    unsigned j = 0;
    for (expr* e : es) {
        if (m_util.str.is_unit(e) || m_util.str.is_string(e))
            es[j++] = e;
    }
    es.shrink(j);
    return m_util.str.mk_concat(es, s);
}

/**
  \brief assign the non-unit and non-string elements to the empty sequence.
  If all is true, then return false if there is a unit or non-empty substring.
*/

bool seq_rewriter::set_empty(unsigned sz, expr* const* es, bool all, expr_ref_pair_vector& eqs) {
    zstring s;
    expr* emp = nullptr;
    for (unsigned i = 0; i < sz; ++i) {
        if (m_util.str.is_unit(es[i])) {
            if (all) return false;
        }
        else if (m_util.str.is_empty(es[i])) {
            continue;
        }
        else if (m_util.str.is_string(es[i], s)) {
            if (s.length() == 0)
                continue;
            if (all) {
                return false;
            }
        }
        else {
            emp = emp?emp:m_util.str.mk_empty(m().get_sort(es[i]));
            eqs.push_back(emp, es[i]);
        }
    }
    return true;
}

/***
    \brief extract the minimal length of the sequence.
    Return true if the minimal length is equal to the 
    maximal length (the sequence is bounded).
*/

bool seq_rewriter::min_length(expr_ref_vector const& es, unsigned& len) {
    zstring s;
    bool bounded = true;
    len = 0;
    for (expr* e : es) {
        if (m_util.str.is_unit(e)) {
            ++len;
        }
        else if (m_util.str.is_empty(e)) {
            continue;
        }
        else if (m_util.str.is_string(e, s)) {
            len += s.length();
        }
        else {
            bounded = false;
        }
    }
    return bounded;
}

bool seq_rewriter::is_string(unsigned n, expr* const* es, zstring& s) const {
    zstring s1;
    expr* e;
    unsigned ch;
    for (unsigned i = 0; i < n; ++i) {
        if (m_util.str.is_string(es[i], s1)) {
            s = s + s1;
        }
        else if (m_util.str.is_unit(es[i], e) && m_util.is_const_char(e, ch)) {
            s = s + zstring(ch);
        }
        else {
            return false;
        }
    }
    return true;
}

/**
 * itos(n) = <numeric string> -> n = numeric
 */

bool seq_rewriter::reduce_itos(expr_ref_vector& ls, expr_ref_vector& rs,
                              expr_ref_pair_vector& eqs) {
    expr* n = nullptr;
    zstring s;
    if (ls.size() == 1 && 
        m_util.str.is_itos(ls.get(0), n) &&
        is_string(rs.size(), rs.c_ptr(), s)) {
        std::string s1 = s.encode();
        rational r(s1.c_str());
        if (s1 == r.to_string()) {
            eqs.push_back(n, m_autil.mk_numeral(r, true));
            ls.reset(); 
            rs.reset();
            return true;
        }
    }
    return true;
}

bool seq_rewriter::reduce_by_length(expr_ref_vector& ls, expr_ref_vector& rs,
                                    expr_ref_pair_vector& eqs) {

    if (ls.empty() && rs.empty())
        return true;

    unsigned len1 = 0, len2 = 0;
    bool bounded1 = min_length(ls, len1);
    bool bounded2 = min_length(rs, len2);
    if (bounded1 && len1 < len2) 
        return false;
    if (bounded2 && len2 < len1) 
        return false;
    if (bounded1 && len1 == len2 && len1 > 0) {
        if (!set_empty(rs.size(), rs.c_ptr(), false, eqs))
            return false;
        eqs.push_back(concat_non_empty(ls), concat_non_empty(rs));
        ls.reset(); 
        rs.reset();
    }
    else if (bounded2 && len1 == len2 && len1 > 0) {
        if (!set_empty(ls.size(), ls.c_ptr(), false, eqs))
            return false;
        eqs.push_back(concat_non_empty(ls), concat_non_empty(rs));
        ls.reset(); 
        rs.reset();
    }    
    return true;
}


bool seq_rewriter::is_epsilon(expr* e) const {
    expr* e1;
    return re().is_to_re(e, e1) && m_util.str.is_empty(e1);
}

bool seq_rewriter::reduce_subsequence(expr_ref_vector& ls, expr_ref_vector& rs, expr_ref_pair_vector& eqs) {

    if (ls.size() > rs.size()) 
        ls.swap(rs);

    if (ls.size() == rs.size())
        return true;

    if (ls.empty() && rs.size() == 1)
        return true;
    
    uint_set rpos;
    for (expr* x : ls) {
        unsigned j = 0;
        bool is_unit = m_util.str.is_unit(x);
        for (expr* y : rs) {
            if (!rpos.contains(j) && (x == y || (is_unit && m_util.str.is_unit(y)))) {
                rpos.insert(j);
                break;
            }
            ++j;
        }
        if (j == rs.size())
            return true;
    }
    // if we reach here, then every element of l is contained in r in some position.
    // or each non-unit in l is matched by a non-unit in r, and otherwise, the non-units match up.
    unsigned i = 0, j = 0;
    for (expr* y : rs) {
        if (rpos.contains(i)) {
            rs[j++] = y;
        }
        else if (!set_empty(1, &y, true, eqs)) {
            return false;
        }
        ++i;
    }
    if (j == rs.size()) {
        return true;
    }
    rs.shrink(j);
    SASSERT(ls.size() == rs.size());
    if (!ls.empty()) {
        sort* srt = m().get_sort(ls.get(0));
        eqs.push_back(m_util.str.mk_concat(ls, srt),
                      m_util.str.mk_concat(rs, srt));
        ls.reset();
        rs.reset();
        TRACE("seq", tout << "subsequence " << eqs << "\n";);
    }
    return true;
} 
