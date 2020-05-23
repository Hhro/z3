/*++
Copyright (c) 2011 Microsoft Corporation

Module Name:

    seq_regex.h

Abstract:

    Solver for regexes 

Author:

    Nikolaj Bjorner (nbjorner) 2020-5-22

--*/
#pragma once

#include "util/scoped_vector.h"
#include "ast/seq_decl_plugin.h"
#include "ast/rewriter/seq_rewriter.h"
#include "smt/smt_context.h"
#include "smt/seq_skolem.h"

namespace smt {

    class theory_seq;

    class seq_regex {
        struct s_in_re {
            literal m_lit;
            expr*   m_s;
            expr*   m_re;
            bool    m_active;
        s_in_re(literal l, expr* s, expr* r):
            m_lit(l), m_s(s), m_re(r), m_active(true) {}
        };

        theory_seq&      th;
        context&         ctx;
        ast_manager&     m;
        vector<s_in_re> m_s_in_re;
        scoped_vector<literal> m_to_propagate;

        seq_util& u();
        class seq_util::re& re();
        class seq_util::str& str();
        seq_rewriter& seq_rw();
        seq_skolem& sk();

        void rewrite(expr_ref& e);

        bool propagate(literal lit);

        bool coallesce_in_re(literal lit);

        bool block_unfolding(literal lit, expr* s);

        expr_ref mk_first(expr* r);

        expr_ref unroll_non_empty(expr* r, expr_mark& seen, unsigned depth);

    public:

        seq_regex(theory_seq& th);

        bool propagate();

        void propagate_in_re(literal lit);

        void propagate_is_empty(literal lit);
        
        void propagate_is_nonempty(expr* r);
    };

};

