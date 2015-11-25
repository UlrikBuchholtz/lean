/*
Copyright (c) 2015 Daniel Selsam. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Author: Daniel Selsam
*/
#include "kernel/instantiate.h"
#include "kernel/abstract.h"
#include "kernel/inductive/inductive.h"
#include "library/constants.h"
#include "library/util.h"
#include "library/blast/blast.h"
#include "library/blast/action_result.h"
#include "library/blast/unit/unit_action.h"
#include "library/blast/proof_expr.h"
#include "library/blast/choice_point.h"
#include "library/blast/hypothesis.h"
#include "library/blast/util.h"
#include "library/expr_lt.h"
#include "util/rb_multi_map.h"

namespace lean {
namespace blast {

static unsigned g_ext_id = 0;
struct unit_branch_extension : public branch_extension {
    rb_multi_map<expr, hypothesis_idx, expr_quick_cmp> m_lemma_map;
    rb_map<expr, hypothesis_idx, expr_quick_cmp> m_fact_map;

    unit_branch_extension() {}
    unit_branch_extension(unit_branch_extension const & b):
        m_lemma_map(b.m_lemma_map), m_fact_map(b.m_fact_map) {}
    virtual ~unit_branch_extension() {}
    virtual branch_extension * clone() override { return new unit_branch_extension(*this); }

    void insert_disjunction(expr const & e, hypothesis_idx hidx) {
        expr A, B;
        if (is_or(e, A, B)) {
            insert_disjunction(A, hidx);
            insert_disjunction(B, hidx);
        } else {
            m_lemma_map.insert(e, hidx);
        }
    }

    virtual void hypothesis_activated(hypothesis const & h, hypothesis_idx hidx) override {
        expr type = whnf(h.get_type());
        if (!is_pi(type)) {
            if (is_prop(type)) m_fact_map.insert(type, hidx);
            return;
        }
        bool has_antecedent = false;
        while (is_pi(type) && is_prop(binding_domain(type)) && closed(binding_body(type))) {
            has_antecedent = true;
            insert_disjunction(binding_domain(type), hidx);
            type = binding_body(type);
        }
        if (has_antecedent && is_prop(type)) {
            expr not_type;
            if (blast::is_not(type, not_type)) m_lemma_map.insert(not_type, hidx);
            else m_lemma_map.insert(get_app_builder().mk_not(type), hidx);
        }
    }

    virtual void hypothesis_deleted(hypothesis const & , hypothesis_idx ) override {
        /* We discard opportunistically when we encounter a hypothesis that is dead. */
    }

public:
    list<hypothesis_idx> const * find_lemmas(expr const & e) { return m_lemma_map.find(e); }
    template<typename P> void filter_lemmas(expr const & e, P && p) { return m_lemma_map.filter(e, p); }
    hypothesis_idx const * find_fact(expr const & e) { return m_fact_map.find(e); }
    void erase_fact(expr const & e) { return m_fact_map.erase(e); }

    optional<expr> find_live_fact_in_disjunction(expr const & e) {
        expr A, B;
        if (is_or(e, A, B)) {
            if (auto A_fact = find_live_fact_in_disjunction(A)) {
                return some_expr(get_app_builder().mk_app(get_or_intro_left_name(), {A, B, *A_fact}));
            } else if (auto B_fact = find_live_fact_in_disjunction(B)) {
                return some_expr(get_app_builder().mk_app(get_or_intro_right_name(), {A, B, *B_fact}));
            } else {
                return none_expr();
            }
        } else {
            hypothesis_idx const * fact_hidx = find_fact(e);
            if (fact_hidx) {
                hypothesis const & fact_h = curr_state().get_hypothesis_decl(*fact_hidx);
                if (fact_h.is_dead()) {
                    erase_fact(e);
                    return none_expr();
                } else {
                    return some_expr(fact_h.get_self());
                }
            } else {
                return none_expr();
            }
        }
    }
};

void initialize_unit_action() {
    g_ext_id = register_branch_extension(new unit_branch_extension());
}

void finalize_unit_action() { }

static unit_branch_extension & get_extension() {
    return static_cast<unit_branch_extension&>(curr_state().get_extension(g_ext_id));
}

action_result unit_pi(expr const & _type, expr const & proof) {
    unit_branch_extension & ext = get_extension();
    bool missing_argument = false;
    bool has_antecedent = false;
    expr type = _type;
    expr new_hypothesis = proof;
    expr local;

    while (is_pi(type) && is_prop(binding_domain(type)) && closed(binding_body(type))) {
        has_antecedent = true;
        optional<expr> fact = ext.find_live_fact_in_disjunction(binding_domain(type));
        if (fact) {
            new_hypothesis = mk_app(new_hypothesis, *fact);
        } else {
            if (missing_argument) return action_result::failed();
            local = mk_fresh_local(binding_domain(type));
            new_hypothesis = mk_app(new_hypothesis, local);
            missing_argument = true;
        }
        type = binding_body(type);
    }

    if (!has_antecedent) {
        return action_result::failed();
    } else if (!missing_argument) {
        curr_state().mk_hypothesis(type, new_hypothesis);
        return action_result::new_branch();
    } else if (is_prop(type)) {
        expr not_type;
        if (blast::is_not(type, not_type)) {
            optional<expr> fact = ext.find_live_fact_in_disjunction(not_type);
            if (fact) {
                // TODO(dhs): if classical, use double negation elimination
                curr_state().mk_hypothesis(get_app_builder().mk_not(infer_type(local)),
                                           Fun(local, mk_app(new_hypothesis, *fact)));
                return action_result::new_branch();
            } else {
                return action_result::failed();
            }
        } else {
            not_type = get_app_builder().mk_not(type);
            optional<expr> fact = ext.find_live_fact_in_disjunction(not_type);
            if (fact) {
                curr_state().mk_hypothesis(get_app_builder().mk_not(infer_type(local)),
                                           Fun(local, mk_app(*fact, new_hypothesis)));
                return action_result::new_branch();
            } else {
                return action_result::failed();
            }
        }
    } else {
        return action_result::failed();
    }
    lean_unreachable();
}

action_result unit_fact(expr const & type) {
    unit_branch_extension & ext = get_extension();
    list<hypothesis_idx> const * lemmas = ext.find_lemmas(type);
    if (!lemmas) return action_result::failed();
    bool success = false;
    ext.filter_lemmas(type, [&](hypothesis_idx const & hidx) {
            hypothesis const & h = curr_state().get_hypothesis_decl(hidx);
            if (h.is_dead()) {
                return false;
            } else {
                action_result r = unit_pi(whnf(h.get_type()), h.get_self());
                success = success || (r.get_kind() == action_result::NewBranch);
                return true;
            }
        });
    if (success) return action_result::new_branch();
    else return action_result::failed();
}

action_result unit_action(unsigned _hidx) {
    hypothesis const & h = curr_state().get_hypothesis_decl(_hidx);
    expr type = whnf(h.get_type());
    if (is_pi(type)) return unit_pi(type, h.get_self());
    else if (is_prop(type)) return unit_fact(type);
    else return action_result::failed();
}
}}